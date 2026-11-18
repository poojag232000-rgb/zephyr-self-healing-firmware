/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware-only health supervision milestone for STM32L4P5 + Zephyr RTOS.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/version.h>

LOG_MODULE_REGISTER(zephyr_health, LOG_LEVEL_INF);

#define HEALTH_MAGIC            0x5A455048u /* "ZEPH" */
#define WORKER_A_FAILURE_BIT    BIT(0)
#define WORKER_B_FAILURE_BIT    BIT(1)
#define TASK_WDT_TIMEOUT_MS     2000U
#define HEALTH_SAMPLE_PERIOD_MS 1000U
#define CRASH_RECORD_MAGIC      0x43525348u /* "CRSH" */
#define CRASH_RECORD_VERSION    2U
#define CRASH_RECORD_NVS_ID     1U
#define NVS_SECTOR_COUNT        8U
#define SAFE_MODE_FAILURE_THRESHOLD 3U
#define STABLE_RUNTIME_MS           60000U

#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET PARTITION_OFFSET(NVS_PARTITION)
#define NVS_PARTITION_SIZE   PARTITION_SIZE(NVS_PARTITION)

struct health_snapshot {
	uint32_t magic;
	uint32_t uptime_ms;
	uint32_t worker_a_heartbeats;
	uint32_t worker_b_heartbeats;
	uint32_t watchdog_events;
	uint32_t failure_mask;
};

struct persistent_crash_record {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t boot_count;
	uint32_t recovery_count;
	uint32_t last_failure_mask;
	int32_t last_watchdog_channel;
	uint32_t failure_uptime_ms;
	uint32_t last_reset_cause;
	uint32_t consecutive_failures;
	uint32_t safe_mode_entries;
	uint32_t safe_mode_latched;
};

struct persistent_crash_record_v1 {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t boot_count;
	uint32_t recovery_count;
	uint32_t last_failure_mask;
	int32_t last_watchdog_channel;
	uint32_t failure_uptime_ms;
	uint32_t last_reset_cause;
};

/* Intentionally global and volatile so it is inspectable in a debugger. */
volatile struct health_snapshot g_health_snapshot;

static atomic_t worker_a_heartbeats;
static atomic_t worker_b_heartbeats;
static atomic_t watchdog_events;
static atomic_t watchdog_failure_mask;
static atomic_t worker_b_fault_requested;
static atomic_t recovery_in_progress;
static atomic_t failed_watchdog_channel = ATOMIC_INIT(-1);

static struct nvs_fs crash_fs;
static struct persistent_crash_record crash_record;
static bool crash_store_ready;
static bool safe_mode_active;
static bool stable_boot_confirmed;

static atomic_t pm_stop_entries[4];
static atomic_t pm_wakeups;
static atomic_t pm_last_substate;
static atomic_t rtc_alarm_wakeups;
static atomic_t power_demo_runs;

static const struct device *rtc_counter = DEVICE_DT_GET(DT_NODELABEL(rtc));

static int worker_a_wdt_channel = -1;
static int worker_b_wdt_channel = -1;
static const uint32_t worker_failure_bits[] = {
	WORKER_A_FAILURE_BIT,
	WORKER_B_FAILURE_BIT,
};

K_SEM_DEFINE(worker_a_ready, 0, 1);
K_SEM_DEFINE(worker_b_ready, 0, 1);
K_SEM_DEFINE(recovery_requested, 0, 1);
K_SEM_DEFINE(rtc_alarm_fired, 0, 1);
K_MUTEX_DEFINE(crash_store_lock);

static void rtc_wakeup_callback(const struct device *dev, uint8_t channel,
				uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);

	atomic_inc(&rtc_alarm_wakeups);
	k_sem_give(&rtc_alarm_fired);
}

static void power_state_entry(enum pm_state state, uint8_t substate_id)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE && substate_id <= 3U) {
		atomic_inc(&pm_stop_entries[substate_id]);
		atomic_set(&pm_last_substate, substate_id);
	}
}

static void power_state_exit(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		atomic_inc(&pm_wakeups);
	}
}

static struct pm_notifier power_notifier = {
	.substate_entry = power_state_entry,
	.substate_exit = power_state_exit,
	.report_substate = true,
};

static int crash_record_write_locked(void)
{
	ssize_t written;

	written = nvs_write(&crash_fs, CRASH_RECORD_NVS_ID, &crash_record, sizeof(crash_record));
	if (written < 0) {
		return (int)written;
	}

	return written == sizeof(crash_record) || written == 0 ? 0 : -EIO;
}

static int crash_store_init(uint32_t reset_cause)
{
	struct flash_pages_info page_info;
	struct persistent_crash_record_v1 old_record;
	ssize_t read_len;
	int ret;

	crash_fs.flash_device = NVS_PARTITION_DEVICE;
	if (!device_is_ready(crash_fs.flash_device)) {
		return -ENODEV;
	}

	crash_fs.offset = NVS_PARTITION_OFFSET;
	ret = flash_get_page_info_by_offs(crash_fs.flash_device, crash_fs.offset, &page_info);
	if (ret != 0) {
		return ret;
	}

	crash_fs.sector_size = page_info.size;
	crash_fs.sector_count = NVS_SECTOR_COUNT;
	if ((crash_fs.sector_size * crash_fs.sector_count) > NVS_PARTITION_SIZE) {
		return -EINVAL;
	}

	ret = nvs_mount(&crash_fs);
	if (ret != 0) {
		return ret;
	}

	read_len = nvs_read(&crash_fs, CRASH_RECORD_NVS_ID, &crash_record, sizeof(crash_record));
	if (read_len == sizeof(old_record) && crash_record.magic == CRASH_RECORD_MAGIC &&
	    crash_record.version == 1U && crash_record.size == sizeof(old_record)) {
		memcpy(&old_record, &crash_record, sizeof(old_record));
		memset(&crash_record, 0, sizeof(crash_record));
		crash_record.magic = CRASH_RECORD_MAGIC;
		crash_record.version = CRASH_RECORD_VERSION;
		crash_record.size = sizeof(crash_record);
		crash_record.boot_count = old_record.boot_count;
		crash_record.recovery_count = old_record.recovery_count;
		crash_record.last_failure_mask = old_record.last_failure_mask;
		crash_record.last_watchdog_channel = old_record.last_watchdog_channel;
		crash_record.failure_uptime_ms = old_record.failure_uptime_ms;
		crash_record.last_reset_cause = old_record.last_reset_cause;
	} else if (read_len != sizeof(crash_record) ||
		   crash_record.magic != CRASH_RECORD_MAGIC ||
		   crash_record.version != CRASH_RECORD_VERSION ||
		   crash_record.size != sizeof(crash_record)) {
		memset(&crash_record, 0, sizeof(crash_record));
		crash_record.magic = CRASH_RECORD_MAGIC;
		crash_record.version = CRASH_RECORD_VERSION;
		crash_record.size = sizeof(crash_record);
		crash_record.last_watchdog_channel = -1;
	}

	crash_record.boot_count++;
	crash_record.last_reset_cause = reset_cause;
	safe_mode_active =
		crash_record.consecutive_failures >= SAFE_MODE_FAILURE_THRESHOLD;
	if (safe_mode_active && crash_record.safe_mode_latched == 0U) {
		crash_record.safe_mode_latched = 1U;
		crash_record.safe_mode_entries++;
	}
	ret = crash_record_write_locked();
	if (ret == 0) {
		crash_store_ready = true;
	}

	return ret;
}

static void watchdog_expired(int channel_id, void *user_data)
{
	const uint32_t failure_bit = *(const uint32_t *)user_data;
	atomic_val_t previous_failures;

	previous_failures = atomic_or(&watchdog_failure_mask, (atomic_val_t)failure_bit);
	if ((previous_failures & (atomic_val_t)failure_bit) == 0) {
		atomic_set(&failed_watchdog_channel, channel_id);
		atomic_set(&recovery_in_progress, 1);
		atomic_inc(&watchdog_events);
		LOG_ERR("Watchdog detected failure on channel %d (mask 0x%02x)", channel_id,
			failure_bit);
		k_sem_give(&recovery_requested);
	}
}

static void recovery_worker(void)
{
	while (true) {
		int ret = -ENODEV;

		k_sem_take(&recovery_requested, K_FOREVER);

		if (crash_store_ready) {
			k_mutex_lock(&crash_store_lock, K_FOREVER);
			crash_record.recovery_count++;
			crash_record.consecutive_failures++;
			crash_record.last_failure_mask =
				(uint32_t)atomic_get(&watchdog_failure_mask);
			crash_record.last_watchdog_channel =
				(int32_t)atomic_get(&failed_watchdog_channel);
			crash_record.failure_uptime_ms = (uint32_t)k_uptime_get();
			ret = crash_record_write_locked();
			k_mutex_unlock(&crash_store_lock);
		}

		if (ret == 0) {
			LOG_ERR("Crash record saved; hardware watchdog will reboot the MCU");
		} else {
			LOG_ERR("Crash record save failed (%d); hardware watchdog will reboot",
				ret);
		}

		/* All supervised feeds stop once recovery_in_progress is set. The
		 * independent hardware watchdog performs the actual recovery reset.
		 */
		k_sleep(K_FOREVER);
	}
}

static void worker_a(void)
{
	k_sem_take(&worker_a_ready, K_FOREVER);

	while (true) {
		if (atomic_get(&recovery_in_progress) != 0) {
			k_sleep(K_FOREVER);
		}

		atomic_inc(&worker_a_heartbeats);
		(void)task_wdt_feed(worker_a_wdt_channel);
		k_sleep(K_MSEC(500));
	}
}

static void worker_b(void)
{
	k_sem_take(&worker_b_ready, K_FOREVER);

	while (true) {
		atomic_inc(&worker_b_heartbeats);
		(void)task_wdt_feed(worker_b_wdt_channel);

		if ((IS_ENABLED(CONFIG_APP_INJECT_WORKER_B_FAILURE) &&
		     atomic_get(&worker_b_heartbeats) >= 5) ||
		    atomic_get(&worker_b_fault_requested) != 0) {
			LOG_WRN("Worker B fault injected; watchdog feed stopped");
			k_sleep(K_FOREVER);
		}

		k_sleep(K_MSEC(700));
	}
}

K_THREAD_DEFINE(worker_a_id, 1024, worker_a, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(worker_b_id, 1024, worker_b, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(recovery_worker_id, 1536, recovery_worker, NULL, NULL, NULL, 2, 0, 0);

static int cmd_health(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t failures = (uint32_t)atomic_get(&watchdog_failure_mask);
	const bool worker_b_pending = atomic_get(&worker_b_fault_requested) != 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "System health");
	shell_print(sh, "  System mode:     %s", safe_mode_active ? "SAFE MODE" : "normal");
	shell_print(sh, "  Uptime:          %lld ms", k_uptime_get());
	shell_print(sh, "  Worker A:        %s (heartbeat %u)",
		    (failures & WORKER_A_FAILURE_BIT) != 0 ? "FAILED" : "healthy",
		    (uint32_t)atomic_get(&worker_a_heartbeats));
	if (safe_mode_active) {
		shell_print(sh, "  Worker B:        disabled (safe mode)");
	} else {
		shell_print(sh, "  Worker B:        %s (heartbeat %u)",
			    (failures & WORKER_B_FAILURE_BIT) != 0 ? "FAILED"
			    : worker_b_pending                     ? "fault pending"
								   : "healthy",
			    (uint32_t)atomic_get(&worker_b_heartbeats));
	}
	shell_print(sh, "  Watchdog events: %u", (uint32_t)atomic_get(&watchdog_events));
	shell_print(sh, "  Failure mask:    0x%02x", failures);
	shell_print(sh, "  Recovery:        %s",
		    atomic_get(&recovery_in_progress) != 0 ? "automatic reboot pending" : "idle");

	return 0;
}

static int cmd_system_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "ZEPHYR Self-Healing Platform");
	shell_print(sh, "  Board:           %s", CONFIG_BOARD_TARGET);
	shell_print(sh, "  Zephyr version:  %s", KERNEL_VERSION_STRING);
	shell_print(sh, "  CPU clock:       %u MHz", CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U);
	shell_print(sh, "  Watchdog period: %u ms", TASK_WDT_TIMEOUT_MS);
	shell_print(sh, "  HW fallback:     STM32 IWDG enabled");
	shell_print(sh, "  Crash storage:   Internal Flash NVS");
	shell_print(sh, "  Safe-mode rule:  %u consecutive recoveries",
		    SAFE_MODE_FAILURE_THRESHOLD);
	shell_print(sh, "  Console:         LPUART1 PG7/PG8 @ 115200 baud");

	return 0;
}

static void print_reset_cause(const struct shell *sh, uint32_t cause)
{
	if (cause == 0U) {
		shell_print(sh, "none/unknown");
		return;
	}

	if ((cause & RESET_WATCHDOG) != 0U) {
		shell_print(sh, "hardware watchdog");
	}
	if ((cause & RESET_SOFTWARE) != 0U) {
		shell_print(sh, "software reset");
	}
	if ((cause & RESET_PIN) != 0U) {
		shell_print(sh, "reset pin");
	}
	if ((cause & RESET_POR) != 0U) {
		shell_print(sh, "power-on reset");
	}
	if ((cause & RESET_BROWNOUT) != 0U) {
		shell_print(sh, "brownout");
	}
}

static int cmd_crash_log(const struct shell *sh, size_t argc, char **argv)
{
	struct persistent_crash_record snapshot;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!crash_store_ready) {
		shell_error(sh, "Persistent crash storage is unavailable");
		return -ENODEV;
	}

	k_mutex_lock(&crash_store_lock, K_FOREVER);
	snapshot = crash_record;
	k_mutex_unlock(&crash_store_lock);

	shell_print(sh, "Persistent crash log");
	shell_print(sh, "  Boots:             %u", snapshot.boot_count);
	shell_print(sh, "  Recoveries:        %u", snapshot.recovery_count);
	shell_print(sh, "  Consecutive:       %u", snapshot.consecutive_failures);
	shell_print(sh, "  Safe-mode entries: %u", snapshot.safe_mode_entries);
	shell_print(sh, "  Safe-mode latch:   %s",
		    snapshot.safe_mode_latched != 0U ? "set" : "clear");
	shell_print(sh, "  Last failure mask: 0x%02x", snapshot.last_failure_mask);
	shell_print(sh, "  Last WDT channel:  %d", snapshot.last_watchdog_channel);
	shell_print(sh, "  Failure uptime:    %u ms", snapshot.failure_uptime_ms);
	shell_print(sh, "  Reset cause bits:  0x%08x", snapshot.last_reset_cause);
	shell_fprintf(sh, SHELL_NORMAL, "  Reset cause:       ");
	print_reset_cause(sh, snapshot.last_reset_cause);

	return 0;
}

static int cmd_clear_failures(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!crash_store_ready) {
		shell_error(sh, "Persistent crash storage is unavailable");
		return -ENODEV;
	}

	k_mutex_lock(&crash_store_lock, K_FOREVER);
	crash_record.recovery_count = 0;
	crash_record.consecutive_failures = 0;
	crash_record.safe_mode_entries = 0;
	crash_record.safe_mode_latched = 0;
	crash_record.last_failure_mask = 0;
	crash_record.last_watchdog_channel = -1;
	crash_record.failure_uptime_ms = 0;
	ret = crash_record_write_locked();
	k_mutex_unlock(&crash_store_lock);

	if (ret != 0) {
		shell_error(sh, "Unable to clear crash record: %d", ret);
		return ret;
	}

	shell_print(sh, "Persistent failure history cleared");
	return 0;
}

static int cmd_fault(const struct shell *sh, size_t argc, char **argv)
{
	if (safe_mode_active) {
		shell_error(sh, "Fault injection is disabled while safe mode is active");
		return -EPERM;
	}

	if (argc != 2 || strcmp(argv[1], "worker_b") != 0) {
		shell_error(sh, "Usage: fault worker_b");
		return -EINVAL;
	}

	if (atomic_get(&worker_b_fault_requested) != 0 ||
	    (atomic_get(&watchdog_failure_mask) & WORKER_B_FAILURE_BIT) != 0) {
		shell_warn(sh, "Worker B fault is already pending or latched");
		return 0;
	}

	atomic_set(&worker_b_fault_requested, 1);
	shell_warn(sh, "Worker B failure requested; detection expected within %u ms",
		   TASK_WDT_TIMEOUT_MS);

	return 0;
}

static int cmd_safe_mode(const struct shell *sh, size_t argc, char **argv)
{
	struct persistent_crash_record snapshot;
	int ret;

	if (!crash_store_ready) {
		shell_error(sh, "Persistent crash storage is unavailable");
		return -ENODEV;
	}

	if (argc == 2 && strcmp(argv[1], "exit") == 0) {
		if (!safe_mode_active) {
			shell_print(sh, "Safe mode is not active");
			return 0;
		}

		k_mutex_lock(&crash_store_lock, K_FOREVER);
		crash_record.consecutive_failures = 0;
		crash_record.safe_mode_latched = 0;
		ret = crash_record_write_locked();
		k_mutex_unlock(&crash_store_lock);
		if (ret != 0) {
			shell_error(sh, "Unable to clear safe mode: %d", ret);
			return ret;
		}

		shell_print(sh, "Safe-mode latch cleared; rebooting normally...");
		k_msleep(50);
		sys_reboot(SYS_REBOOT_COLD);
		return 0;
	}

	if (argc != 1) {
		shell_error(sh, "Usage: safe_mode [exit]");
		return -EINVAL;
	}

	k_mutex_lock(&crash_store_lock, K_FOREVER);
	snapshot = crash_record;
	k_mutex_unlock(&crash_store_lock);
	shell_print(sh, "Safe-mode policy");
	shell_print(sh, "  State:                %s", safe_mode_active ? "ACTIVE" : "normal");
	shell_print(sh, "  Consecutive failures: %u/%u", snapshot.consecutive_failures,
		    SAFE_MODE_FAILURE_THRESHOLD);
	shell_print(sh, "  Entries:              %u", snapshot.safe_mode_entries);
	shell_print(sh, "  Stable-run clear:     %u seconds", STABLE_RUNTIME_MS / 1000U);
	if (safe_mode_active) {
		shell_print(sh, "  Exit command:         safe_mode exit");
	}

	return 0;
}

static int cmd_recover(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting into a clean supervised state...");
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

static const char *swap_type_name(int swap_type)
{
	switch (swap_type) {
	case BOOT_SWAP_TYPE_NONE:
		return "none";
	case BOOT_SWAP_TYPE_TEST:
		return "test";
	case BOOT_SWAP_TYPE_PERM:
		return "permanent";
	case BOOT_SWAP_TYPE_REVERT:
		return "revert";
	default:
		return "unknown/error";
	}
}

static void print_image_slot(const struct shell *sh, const char *name, uint8_t area_id)
{
	struct mcuboot_img_header header;
	int ret;

	ret = boot_read_bank_header(area_id, &header, sizeof(header));
	if (ret != 0 || header.mcuboot_version != 1U) {
		shell_print(sh, "  %-10s empty/invalid (area %u)", name, area_id);
		return;
	}

	shell_print(sh, "  %-10s %u.%u.%u+%u, %u bytes (area %u)", name,
		    header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		    header.h.v1.sem_ver.revision, header.h.v1.sem_ver.build_num,
		    header.h.v1.image_size, area_id);
}

static int cmd_image_status(const struct shell *sh, size_t argc, char **argv)
{
	int swap_type;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	swap_type = mcuboot_swap_type();
	shell_print(sh, "MCUboot image status");
	shell_print(sh, "  Running build: %s", CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION);
	shell_print(sh, "  Active area:   %u", boot_fetch_active_slot());
	shell_print(sh, "  Confirmed:     %s", boot_is_img_confirmed() ? "yes" : "NO (trial)");
	shell_print(sh, "  Next action:   %s (%d)", swap_type_name(swap_type), swap_type);
	print_image_slot(sh, "Primary", DT_FIXED_PARTITION_ID(DT_NODELABEL(slot0_partition)));
	print_image_slot(sh, "Secondary", DT_FIXED_PARTITION_ID(DT_NODELABEL(slot1_partition)));
	return 0;
}

static int cmd_image_test(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret != 0) {
		shell_error(sh, "Unable to request trial upgrade: %d", ret);
		return ret;
	}

	shell_print(sh, "Trial upgrade requested; rebooting through MCUboot...");
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int cmd_image_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (boot_is_img_confirmed()) {
		shell_print(sh, "Running image is already confirmed");
		return 0;
	}

	ret = boot_write_img_confirmed();
	if (ret != 0) {
		shell_error(sh, "Unable to confirm running image: %d", ret);
		return ret;
	}

	shell_print(sh, "Running trial image confirmed; rollback disabled");
	return 0;
}

static int cmd_power_stats(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t stop0 = (uint32_t)atomic_get(&pm_stop_entries[1]);
	uint32_t stop1 = (uint32_t)atomic_get(&pm_stop_entries[2]);
	uint32_t stop2 = (uint32_t)atomic_get(&pm_stop_entries[3]);
	int last_substate = (int)atomic_get(&pm_last_substate);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Zephyr power-management statistics");
	shell_print(sh, "  Policy:          watchdog-aware idle; deep STOP locked");
	shell_print(sh, "  RTC companion:   ready (internal LSI clock)");
	shell_print(sh, "  RTC frequency:   %u Hz", counter_get_frequency(rtc_counter));
	shell_print(sh, "  RTC alarm wakes: %u", (uint32_t)atomic_get(&rtc_alarm_wakeups));
	shell_print(sh, "  Demo runs:       %u", (uint32_t)atomic_get(&power_demo_runs));
	shell_print(sh, "  STOP0 entries:   %u", stop0);
	shell_print(sh, "  STOP1 entries:   %u", stop1);
	shell_print(sh, "  STOP2 entries:   %u", stop2);
	shell_print(sh, "  Total STOP:      %u", stop0 + stop1 + stop2);
	shell_print(sh, "  Wake transitions:%u", (uint32_t)atomic_get(&pm_wakeups));
	if (last_substate == 0) {
		shell_print(sh, "  Last substate:   none yet");
	} else {
		shell_print(sh, "  Last substate:   STOP%d", last_substate - 1);
	}
	shell_print(sh, "  IWDG supervision: active");
	return 0;
}

static int cmd_power_demo(const struct shell *sh, size_t argc, char **argv)
{
	struct counter_alarm_cfg alarm_cfg = { 0 };
	uint32_t before[4];
	uint32_t after[4];
	uint32_t rtc_before;
	uint32_t rtc_after;
	char *end;
	unsigned long seconds;
	int64_t started;
	int64_t elapsed;
	int ret;

	ARG_UNUSED(argc);

	seconds = strtoul(argv[1], &end, 10);
	if (*argv[1] == '\0' || *end != '\0' || seconds < 1UL || seconds > 30UL) {
		shell_error(sh, "Usage: power_demo <1-30 seconds>");
		return -EINVAL;
	}

	for (size_t i = 1; i <= 3; i++) {
		before[i] = (uint32_t)atomic_get(&pm_stop_entries[i]);
	}

	while (k_sem_take(&rtc_alarm_fired, K_NO_WAIT) == 0) {
	}
	(void)counter_cancel_channel_alarm(rtc_counter, 0);
	ret = counter_get_value(rtc_counter, &rtc_before);
	if (ret != 0) {
		shell_error(sh, "Unable to read RTC counter: %d", ret);
		return ret;
	}

	alarm_cfg.callback = rtc_wakeup_callback;
	alarm_cfg.ticks = counter_us_to_ticks(rtc_counter, seconds * USEC_PER_SEC);
	ret = counter_set_channel_alarm(rtc_counter, 0, &alarm_cfg);
	if (ret != 0) {
		shell_error(sh, "Unable to arm RTC wake alarm: %d", ret);
		return ret;
	}

	shell_print(sh, "Entering watchdog-aware idle for %lu seconds...", seconds);
	started = k_uptime_get();
	ret = k_sem_take(&rtc_alarm_fired, K_SECONDS(seconds + 2UL));
	elapsed = k_uptime_get() - started;
	if (ret != 0) {
		(void)counter_cancel_channel_alarm(rtc_counter, 0);
		shell_error(sh, "RTC wake alarm timed out: %d", ret);
		return ret;
	}
	(void)counter_get_value(rtc_counter, &rtc_after);
	atomic_inc(&power_demo_runs);

	for (size_t i = 1; i <= 3; i++) {
		after[i] = (uint32_t)atomic_get(&pm_stop_entries[i]);
	}

	shell_print(sh, "Power demo complete");
	shell_print(sh, "  Requested:      %lu ms", seconds * 1000UL);
	shell_print(sh, "  Measured:       %lld ms", elapsed);
	shell_print(sh, "  RTC ticks:      %u", rtc_after - rtc_before);
	shell_print(sh, "  Wake source:    internal RTC alarm");
	shell_print(sh, "  STOP0 entries:  %u", after[1] - before[1]);
	shell_print(sh, "  STOP1 entries:  %u", after[2] - before[2]);
	shell_print(sh, "  STOP2 entries:  %u", after[3] - before[3]);
	shell_print(sh, "  Watchdog state: supervised workers remained active");
	return 0;
}

SHELL_CMD_REGISTER(health, NULL, "Show supervised-thread and watchdog health", cmd_health);
SHELL_CMD_REGISTER(system_info, NULL, "Show Zephyr and target information", cmd_system_info);
SHELL_CMD_ARG_REGISTER(fault, NULL, "Inject a fault: fault worker_b", cmd_fault, 2, 0);
SHELL_CMD_REGISTER(recover, NULL, "Reboot and restore the supervised workers", cmd_recover);
SHELL_CMD_REGISTER(crash_log, NULL, "Show persistent reset and recovery history", cmd_crash_log);
SHELL_CMD_REGISTER(clear_failures, NULL, "Clear persistent failure history", cmd_clear_failures);
SHELL_CMD_ARG_REGISTER(safe_mode, NULL, "Show policy or exit safe mode: safe_mode [exit]",
		       cmd_safe_mode, 1, 1);
SHELL_CMD_REGISTER(image_status, NULL, "Show signed MCUboot slot and trial state",
		   cmd_image_status);
SHELL_CMD_REGISTER(image_test, NULL, "Trial-boot the signed image in the secondary slot",
		   cmd_image_test);
SHELL_CMD_REGISTER(image_confirm, NULL, "Confirm a trial image and prevent rollback",
		   cmd_image_confirm);
SHELL_CMD_REGISTER(power_stats, NULL, "Show Zephyr STOP-state and RTC wake statistics",
		   cmd_power_stats);
SHELL_CMD_ARG_REGISTER(power_demo, NULL, "Run measured low-power demo: power_demo <seconds>",
		       cmd_power_demo, 2, 0);

int main(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	uint32_t reset_cause = 0;
	int ret;

	/* Keep every deep STOP substate locked. Normal CPU idle and explicit RTC
	 * alarms are still available, but no power transition may starve the
	 * two-second worker and hardware-watchdog supervision deadlines.
	 */
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, 1);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, 2);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, 3);

	g_health_snapshot.magic = HEALTH_MAGIC;
	(void)hwinfo_get_reset_cause(&reset_cause);
	(void)hwinfo_clear_reset_cause();

	ret = crash_store_init(reset_cause);
	if (ret != 0) {
		LOG_ERR("Persistent crash storage initialization failed: %d", ret);
	}

	__ASSERT(device_is_ready(hw_wdt), "hardware watchdog is not ready");
	__ASSERT(device_is_ready(rtc_counter), "RTC counter is not ready");
	ret = counter_start(rtc_counter);
	__ASSERT(ret == 0 || ret == -EALREADY, "RTC counter start failed: %d", ret);

	ret = task_wdt_init(hw_wdt);
	__ASSERT(ret == 0, "task_wdt_init failed: %d", ret);

	worker_a_wdt_channel = task_wdt_add(TASK_WDT_TIMEOUT_MS, watchdog_expired,
					    (void *)&worker_failure_bits[0]);
	__ASSERT(worker_a_wdt_channel >= 0, "worker A watchdog setup failed");

	k_thread_name_set(worker_a_id, "health_worker_a");
	k_thread_name_set(worker_b_id, "health_worker_b");
	pm_notifier_register(&power_notifier);
	k_sem_give(&worker_a_ready);

	if (safe_mode_active) {
		LOG_WRN("SAFE MODE active after %u consecutive recoveries; Worker B disabled",
			crash_record.consecutive_failures);
	} else {
		worker_b_wdt_channel = task_wdt_add(TASK_WDT_TIMEOUT_MS, watchdog_expired,
						    (void *)&worker_failure_bits[1]);
		__ASSERT(worker_b_wdt_channel >= 0, "worker B watchdog setup failed");
		k_sem_give(&worker_b_ready);
		LOG_INF("Self-healing supervision started; IWDG and persistent crash log active");
	}

	while (true) {
		g_health_snapshot.uptime_ms = (uint32_t)k_uptime_get();
		g_health_snapshot.worker_a_heartbeats = (uint32_t)atomic_get(&worker_a_heartbeats);
		g_health_snapshot.worker_b_heartbeats = (uint32_t)atomic_get(&worker_b_heartbeats);
		g_health_snapshot.watchdog_events = (uint32_t)atomic_get(&watchdog_events);
		g_health_snapshot.failure_mask = (uint32_t)atomic_get(&watchdog_failure_mask);

		if (!safe_mode_active && !stable_boot_confirmed &&
		    k_uptime_get() >= STABLE_RUNTIME_MS) {
			stable_boot_confirmed = true;
			if (crash_store_ready) {
				k_mutex_lock(&crash_store_lock, K_FOREVER);
				if (crash_record.consecutive_failures != 0U) {
					crash_record.consecutive_failures = 0;
					crash_record.safe_mode_latched = 0;
					ret = crash_record_write_locked();
					if (ret == 0) {
						LOG_INF("Stable runtime confirmed; consecutive failures cleared");
					} else {
						LOG_ERR("Unable to store stable-runtime state: %d", ret);
					}
				}
				k_mutex_unlock(&crash_store_lock);
			}
		}

		k_sleep(K_MSEC(HEALTH_SAMPLE_PERIOD_MS));
	}

	return 0;
}
