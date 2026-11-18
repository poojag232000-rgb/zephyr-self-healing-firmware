# ZEPHYR Self-Healing Firmware Platform

Firmware-only reliability project for the STM32L4P5ZGT using Zephyr RTOS.

## Milestone 1: supervised kernel threads

The current application starts two Zephyr threads and supervises each one with
an independent task-watchdog channel. A debugger-readable health snapshot is
published in `g_health_snapshot` once per second.

No external sensor, display, UART adapter, or communication module is required.
Hardware watchdog reset is deliberately deferred until the base image has been
validated on the physical board.

The Zephyr Shell is available at 115200 baud through the Nucleo's on-board
ST-LINK Virtual COM Port. Useful commands are:

```text
health
system_info
kernel threads
fault worker_b
recover
crash_log
clear_failures
safe_mode
```

When a supervised task fails, Zephyr records the failure in the final 64 KiB
of internal Flash. Task feeds then stop and the STM32 independent watchdog
automatically resets the MCU. `crash_log` reports the failure and reset cause
after boot.

## Milestone 3: repeated-failure safe mode

Three consecutive watchdog recoveries latch a restricted safe mode. In this
mode Worker A continues to supervise the platform, Worker B remains disabled,
fault injection is blocked, and the diagnostic shell remains available. This
prevents a permanently failing service from causing an endless reboot loop.

Use these commands to inspect or leave the mode:

```text
safe_mode
health
crash_log
safe_mode exit
```

A normal boot that runs for 60 seconds clears the consecutive-failure counter.
The cumulative recovery and safe-mode entry counters remain available as
diagnostic history. The Phase 2 NVS record is migrated to the new format during
the first Phase 3 boot.

## Milestone 4: authenticated boot and rollback

MCUboot occupies the first 64 KiB of internal Flash and verifies ECDSA-P256
signatures before executing firmware. Two 448 KiB slots support trial upgrades,
explicit confirmation, and automatic rollback. The final 64 KiB NVS crash
partition is unchanged.

The application exposes these commands:

```text
image_status
image_test
image_confirm
```

`image_test` requests a one-time boot of the signed secondary image. Rebooting
an unconfirmed trial causes MCUboot to restore the previous image. Calling
`image_confirm` accepts the trial and disables rollback for that version.

The repository ignores the development private key. Production keys must be
stored outside source control in a protected signing service or HSM.

## Milestone 5: Watchdog-aware RTC power management

Zephyr power management and tickless idle are enabled. Because the independent
hardware watchdog has a two-second supervision deadline, STM32L4 STOP0, STOP1,
and STOP2 are deliberately locked. This prevents a deep-sleep transition from
silently starving the supervised worker threads or the watchdog feed path.

The internal LSI-clocked RTC remains available through Zephyr's Counter API.
The application can schedule an explicit RTC alarm while normal Cortex-M idle
continues, so RTC wake-up timing is tested without weakening fault supervision.

Use the application commands to inspect and exercise the policy:

```text
power_stats
power_demo 10
```

The demo reports requested time, kernel elapsed time, RTC elapsed ticks, alarm
wake count, and watchdog state. STOP transition counters are expected to remain
zero, proving that the safety policy guard stayed active. Worker heartbeats and
the independent hardware watchdog remain active throughout the test.

## Hardware validation

Validated on a NUCLEO-L4P5ZG through its on-board ST-LINK:

- Target voltage: 3.22 V
- Device ID: `0x471` (STM32L4P/Q family)
- Flash: 1 MiB
- Snapshot after 6001 ms: Worker A = 12, Worker B = 9
- Watchdog events = 0 and failure mask = 0

The observed heartbeat counts match the configured 500 ms and 700 ms thread
periods.

The controlled fault-injection image was also validated on hardware:

- Snapshot after 9001 ms: Worker A = 18, Worker B = 5
- Watchdog events = 1
- Failure mask = `0x2`, correctly identifying Worker B
- The normal image was restored and revalidated with both workers active and
  no watchdog failures.

## Build

Initialize a West workspace directly from this repository, install Zephyr's
Python requirements, and generate a local development signing key:

```powershell
west init -m https://github.com/YOUR_ACCOUNT/YOUR_REPOSITORY zephyr-workspace
cd zephyr-workspace
west update
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r zephyr\scripts\requirements.txt
python bootloader\mcuboot\scripts\imgtool.py keygen `
  -k ZEPHYR_PROJECT\keys\dev-signing.pem -t ecdsa-p256
west zephyr-export
```

Then build MCUboot and the signed application from the workspace root:

```powershell
$env:ZEPHYR_BASE = "$PWD\zephyr"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "gnuarmemb"
$env:GNUARMEMB_TOOLCHAIN_PATH = "C:\path\to\arm-gnu-toolchain"
$keyPath = (Resolve-Path "ZEPHYR_PROJECT\keys\dev-signing.pem").Path.Replace('\', '/')
$keyArg = '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"' + $keyPath + '\"'
west build --sysbuild -p always -d build-mcuboot `
  -b zephyr_l4p5 ZEPHYR_PROJECT -- `
  -DBOARD_ROOT="$PWD\ZEPHYR_PROJECT" `
  $keyArg
```

Build the controlled Worker B failure test separately:

```powershell
west build --sysbuild -d build-fault -p always `
  -b zephyr_l4p5 ZEPHYR_PROJECT -- `
  -DBOARD_ROOT="$PWD\ZEPHYR_PROJECT" `
  $keyArg `
  -DEXTRA_CONF_FILE="$PWD\ZEPHYR_PROJECT\configs\fault_injection.conf"
```

## Debug evidence

Inspect `g_health_snapshot` after the target has run for several seconds:

- `magic` is `0x5A455048` (`ZEPH`).
- `uptime_ms` increases every second.
- Both heartbeat counters increase independently.
- `watchdog_events` and `failure_mask` remain zero during healthy operation.

## Next milestones

1. Add telemetry export and host-side reliability visualization.

