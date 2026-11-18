# Complete Project Notes: Zephyr Self-Healing Firmware Platform

These notes explain the complete project in very simple language first, and
then add the technical details needed for an interview.

## 1. The project in one sentence

This project is firmware for an STM32L4P5 microcontroller that watches its own
software, detects when an important task stops working, remembers what failed,
restarts itself safely, prevents endless restart loops, and can install signed
firmware updates with automatic rollback.

No external sensor, display, Wi-Fi module, or UART converter is required. The
NUCLEO board, its on-board ST-LINK USB connection, and the STM32's internal
hardware are enough.

## 2. A child-friendly story

Imagine a small factory with two workers:

- Worker A presses a button every 500 milliseconds.
- Worker B presses a button every 700 milliseconds.
- A supervisor expects each worker to press the button at least once every two
  seconds.

Pressing the button means, "I am alive and doing my job." In embedded systems,
this is called a **heartbeat** or **watchdog feed**.

If Worker B becomes stuck, it stops pressing its button. The supervisor notices
that Worker B has been silent for two seconds. Before restarting the factory,
another worker writes the following information into a notebook:

- which worker failed;
- when the failure happened;
- how many failures have happened;
- why the system restarted.

The notebook is stored in Flash memory, so the information remains even after
power loss or a restart.

The hardware watchdog then restarts the microcontroller. After booting again,
we can open the notebook with the `crash_log` command.

If the same problem causes three restarts in a row, the system enters **safe
mode**. In safe mode, Worker B stays disabled so it cannot keep crashing the
system. Worker A, the diagnostic shell, the watchdog, and the crash log continue
working. This is similar to a computer starting in safe mode after repeated
boot problems.

The project also has two firmware rooms:

- one room contains the firmware currently in use;
- the other room holds a new firmware update.

MCUboot checks the new firmware's digital signature. It allows a trial run, but
keeps the old working firmware available. If the trial firmware crashes or is
not accepted, MCUboot brings the old firmware back automatically.

That is why the project is called **self-healing**: it detects trouble,
preserves evidence, recovers automatically, and avoids repeating a dangerous
failure forever.

## 3. What problem does this solve?

Embedded devices often run without a person watching them. Examples include:

- industrial controllers;
- medical equipment;
- smart electricity meters;
- automotive control units;
- remote monitoring stations;
- elevators and building controllers;
- agricultural equipment;
- robots and drones.

A device may freeze because of a software bug, bad memory access, unexpected
timing, electrical noise, or a rare race condition. A normal program might stay
frozen forever. A reliable embedded product should instead:

1. detect that something stopped working;
2. identify the failed service;
3. save useful diagnostic evidence;
4. recover without human help;
5. avoid an endless reboot loop;
6. reject untrusted firmware;
7. return to an older firmware version if an update fails.

This project demonstrates all seven ideas on real STM32 hardware.

## 4. Hardware and software used

### Hardware

- MCU family: STM32L4P5
- Development board: NUCLEO-L4P5ZG
- CPU core: Arm Cortex-M4
- Internal Flash: 1 MiB
- CPU clock used by the project: 80 MHz
- Debug/programming connection: on-board ST-LINK
- Serial terminal: ST-LINK Virtual COM Port at 115200 baud
- Hardware watchdog: STM32 Independent Watchdog, or IWDG
- Time source: internal RTC clocked by the internal LSI oscillator

### Software

- Zephyr RTOS 4.4.0
- Zephyr kernel threads, semaphores, mutexes, atomics, shell, logging, task
  watchdog, NVS, power management, Counter API, and Devicetree
- MCUboot for authenticated boot, trial updates, confirmation, and rollback
- ECDSA-P256 digital signatures
- West and CMake for building
- STM32CubeProgrammer for programming the board
- RealTerm for the serial shell during hardware testing

## 5. Why Zephyr RTOS is used

A bare-metal program normally has one large loop and many manually managed
interrupts. That can work for a small product, but the design becomes difficult
to organize as features grow.

Zephyr provides tested operating-system services:

- **Threads** separate independent jobs.
- **Scheduler** decides which ready thread runs.
- **Semaphores** wake a thread when an event occurs.
- **Mutexes** protect shared data from simultaneous access.
- **Atomic variables** safely update small shared values.
- **Devicetree** describes the board's hardware without hard-coding everything
  inside application logic.
- **Kconfig** selects the firmware features included in a build.
- **Task watchdog** supervises individual threads.
- **NVS** stores records safely in Flash.
- **Shell** provides commands through UART.
- **Logging** reports important runtime events.
- **Power management** controls CPU idle and power states.
- **Counter API** provides a common interface to the STM32 RTC.
- **MCUboot integration** supports signed and recoverable firmware updates.

Therefore this is not simply C code running on an STM32. It is a project that
uses Zephyr's architecture and services throughout the design.

## 6. High-level architecture

```text
                         +--------------------------+
USB serial commands ---> | Zephyr Shell + Logging   |
                         +-------------+------------+
                                       |
            +--------------------------+--------------------------+
            |                          |                          |
            v                          v                          v
  +------------------+      +------------------+      +------------------+
  | Worker A thread  |      | Worker B thread  |      | Main/health loop |
  | heartbeat 500 ms |      | heartbeat 700 ms |      | snapshot 1000 ms |
  +--------+---------+      +--------+---------+      +------------------+
           |                         |
           v                         v
  +---------------------------------------------------+
  | Zephyr task watchdog: one channel for each worker |
  | Timeout: 2000 ms                                  |
  +--------------------------+------------------------+
                             |
                       failure callback
                             |
                             v
                 +--------------------------+
                 | Recovery thread          |
                 | Save crash record to NVS |
                 +------------+-------------+
                              |
                     stop watchdog feeds
                              |
                              v
                 +--------------------------+
                 | STM32 hardware IWDG      |
                 | resets the MCU           |
                 +------------+-------------+
                              |
                              v
                 +--------------------------+
                 | MCUboot verifies image   |
                 | boot, trial, or rollback |
                 +--------------------------+
```

## 7. Custom Zephyr board support

The project contains a custom board definition under:

```text
boards/st/zephyr_l4p5/
```

The board files tell Zephyr:

- which STM32 SoC is present;
- how the CPU clock is generated;
- which UART pins connect to ST-LINK;
- which UART is the console and shell;
- which watchdog device is used;
- which RTC device is used;
- how the 1 MiB Flash is divided;
- where the application is allowed to execute.

Important Devicetree choices are:

- HSI is the main internal oscillator.
- PLL produces an 80 MHz system clock.
- LSI is the low-speed internal clock for the RTC and watchdog.
- LPUART1 uses PG7 and PG8.
- Serial speed is 115200 baud.
- `watchdog0` points to the STM32 IWDG.
- `zephyr,code-partition` points to MCUboot slot 0.

This makes the board a first-class Zephyr target called:

```text
zephyr_l4p5/stm32l4p5xx
```

## 8. The supervised threads

### Worker A

Worker A:

1. waits until initialization is complete;
2. increments its heartbeat counter;
3. feeds its watchdog channel;
4. sleeps for 500 ms;
5. repeats forever.

### Worker B

Worker B does the same work with a 700 ms period. It also contains the
controlled fault-injection point.

When the command below is sent:

```text
fault worker_b
```

Worker B deliberately sleeps forever and stops feeding its watchdog. This is
not an accidental bug. It is a safe and repeatable test used to prove that the
supervisor really detects a stopped thread.

### Recovery thread

The recovery thread normally sleeps on a semaphore. The watchdog callback gives
that semaphore when a failure occurs. The recovery thread then:

1. increments recovery counters;
2. records the failure mask;
3. records the watchdog channel;
4. records the uptime at failure;
5. writes the record to Flash NVS;
6. stops permanently.

All supervised feeds stop, so the independent hardware watchdog performs the
actual reset. Using the hardware watchdog is stronger than calling a normal
software reset because it can recover the MCU even when ordinary software is
badly damaged.

## 9. How the two watchdog layers work

The project uses two related watchdog layers.

### Zephyr task watchdog

The task watchdog knows about individual software services. Worker A and Worker
B each receive a separate channel. This lets the firmware answer, "Which worker
failed?"

The timeout is 2000 ms. Worker A normally feeds every 500 ms and Worker B every
700 ms, so both have plenty of time during healthy operation.

### STM32 independent hardware watchdog

The IWDG is an actual STM32 hardware peripheral. It is clocked independently.
Zephyr's task watchdog uses it as the hardware fallback.

The task watchdog provides detailed software identification. The IWDG provides
the final recovery reset. Together they give both diagnosis and strong recovery.

## 10. Failure masks

A bit mask identifies the failed worker:

| Mask | Meaning |
|---:|---|
| `0x00` | No worker has failed |
| `0x01` | Worker A failed |
| `0x02` | Worker B failed |
| `0x03` | Both failure bits are set |

For example, the hardware test produced `0x02`, correctly showing that Worker B
was the failed service.

## 11. Thread-safety tools used

### Atomic variables

Heartbeat counts, failure bits, watchdog event counts, and power statistics use
Zephyr atomic variables. An atomic update cannot be interrupted halfway through
and observed in a broken state.

### Semaphores

Semaphores are used for four events:

- start Worker A after initialization;
- start Worker B after initialization;
- wake the recovery thread after a watchdog failure;
- wake the power demo after the RTC alarm fires.

### Mutex

The crash-record mutex ensures that only one thread reads or changes the shared
Flash record at a time.

These are real RTOS synchronization concepts, not simulated variables.

## 12. Health snapshot

The global `g_health_snapshot` structure contains:

- magic value `0x5A455048`, which represents `ZEPH`;
- uptime in milliseconds;
- Worker A heartbeat count;
- Worker B heartbeat count;
- watchdog event count;
- failure mask.

The main loop refreshes it every second. Because it is global and volatile, a
debugger can inspect it while the firmware is running.

## 13. Persistent crash logging with NVS

RAM is cleared by a restart, so ordinary variables cannot explain an earlier
crash. The project stores a crash record in the final Flash partition using
Zephyr NVS.

NVS means **non-volatile storage**. Non-volatile data survives a reset and power
loss.

The persistent record stores:

- format magic and version;
- structure size;
- total boot count;
- total recovery count;
- last failure mask;
- last failed watchdog channel;
- uptime when the last failure occurred;
- last hardware reset-cause bits;
- consecutive failure count;
- number of safe-mode entries;
- safe-mode latch state.

NVS provides wear-aware Flash storage and data CRC support is enabled. The code
also understands the older version-1 record and migrates it into the version-2
format. This demonstrates backward-compatible persistent data design.

## 14. Reset-cause detection

At startup, the application asks Zephyr's hardware-information API why the MCU
reset. It recognizes:

- hardware watchdog reset;
- software reset;
- reset-pin reset;
- power-on reset;
- brownout reset.

The reset cause is added to the persistent record and then the hardware flag is
cleared, ready for the next boot.

## 15. Safe mode

One successful reboot is useful, but a permanently broken service could create
this bad cycle:

```text
boot -> fail -> reboot -> fail -> reboot -> fail -> ...
```

This is called a reboot loop.

The project counts consecutive supervised failures. After three consecutive
recoveries, safe mode is latched.

In safe mode:

- Worker A continues running and remains supervised.
- Worker B is not started.
- Worker B does not receive a watchdog channel.
- fault injection is blocked.
- the UART shell remains available.
- crash history remains available.
- the user can diagnose the board instead of losing access to endless resets.

Use:

```text
safe_mode
```

to inspect the policy, and:

```text
safe_mode exit
```

to clear the latch and reboot into normal mode.

During normal mode, a stable run of 60 seconds clears the consecutive-failure
counter. Total recovery and safe-mode-entry counters remain as historical
diagnostic information unless `clear_failures` is used.

## 16. Flash-memory layout

The STM32 has 1 MiB of internal Flash. The custom Devicetree divides it like
this:

| Region | Offset | Size | Purpose |
|---|---:|---:|---|
| MCUboot | `0x00000` | 64 KiB | Secure bootloader |
| Slot 0 | `0x10000` | 448 KiB | Primary/running firmware |
| Slot 1 | `0x80000` | 448 KiB | Secondary/update firmware |
| Storage | `0xF0000` | 64 KiB | NVS crash and recovery history |

The corresponding absolute MCU addresses begin at `0x08000000`:

```text
0x08000000  MCUboot
0x08010000  slot 0
0x08080000  slot 1
0x080F0000  persistent storage
```

The project uses MCUboot's swap-using-offset mode. When manually staging a
signed update with STM32CubeProgrammer, the image payload is written at
`0x08081000`, not directly at `0x08080000`, because the first 4 KiB sector of
the secondary area is reserved by the offset-swap layout.

## 17. Secure boot and digital signatures

A checksum only detects accidental corruption. An attacker could change a file
and calculate a new checksum. A digital signature also proves that the firmware
was signed by the expected owner.

This project uses ECDSA-P256:

1. A private development key signs the application image.
2. MCUboot contains the matching public key.
3. At boot, MCUboot verifies the signature.
4. An invalid or modified image is rejected.

The private `.pem` key is ignored by Git and was not uploaded to GitHub.
Production keys should be held in a protected signing service or hardware
security module, not stored in a normal source repository.

## 18. Trial upgrade, confirmation, and rollback

### Normal state

The confirmed firmware runs from the primary slot. A signed candidate is placed
in the secondary slot.

### Request a trial

The command:

```text
image_test
```

asks MCUboot to try the secondary image once and reboots the board.

### Trial state

The candidate runs but is not yet trusted permanently. `image_status` shows:

```text
Confirmed: NO (trial)
Next action: revert
```

This is a safety window. The new firmware can be tested on real hardware.

### Successful candidate

After health, watchdog, RTC, and shell tests pass, the command:

```text
image_confirm
```

marks the running image as accepted. Rollback is disabled for that image.

### Failed or unconfirmed candidate

If the trial crashes, the watchdog resets the MCU. If the image is rebooted
without confirmation, MCUboot restores the previous confirmed firmware. This
behavior was validated during development when unsafe power experiments caused
watchdog resets: the board automatically returned to the working v2 firmware.

The final validated candidate is version `3.1.0+0`.

## 19. Power-management design

The project enables:

- Zephyr power management;
- tickless kernel operation;
- STM32 RTC Counter driver;
- RTC subsecond support;
- Zephyr power statistics.

### Why deep STOP is locked

The workers must meet a two-second supervision deadline. Early experiments
allowed STM32 STOP modes, but the RTC/timer behavior did not reliably preserve
all watchdog and thread deadlines in this board configuration. The hardware
watchdog correctly reset the MCU and MCUboot correctly rolled the trial image
back.

The final engineering decision is safety-first:

- STOP0 is locked.
- STOP1 is locked.
- STOP2 is locked.
- normal Cortex-M CPU idle remains available;
- tickless scheduling remains enabled;
- the internal RTC remains available;
- watchdog supervision remains active.

This is an important engineering result. A good embedded engineer does not use
a deeper sleep state merely because it exists. A power state is enabled only
after every safety deadline is proven to work.

### RTC power demo

The command:

```text
power_demo 1
```

does the following:

1. reads the current RTC counter;
2. programs a relative one-second RTC alarm;
3. waits on a Zephyr semaphore;
4. lets normal scheduler idle occur between active work;
5. wakes when the RTC callback gives the semaphore;
6. compares requested time, kernel elapsed time, and RTC ticks;
7. confirms that worker supervision stayed active.

The STOP-entry counters should remain zero. Zero is the expected success result
because the safety lock prevented deep STOP entry.

This demo validates watchdog-aware idle and RTC alarm scheduling. It does not
claim deep-STOP current consumption, and no external current-measurement tool is
used.

## 20. Boot sequence

The complete startup sequence is:

1. Reset occurs.
2. MCUboot starts first.
3. MCUboot validates the selected image signature.
4. MCUboot decides whether to boot primary, trial, or rollback firmware.
5. Zephyr initializes the kernel and drivers.
6. The application locks deep STOP substates.
7. It reads and clears the STM32 reset cause.
8. It mounts NVS and loads or creates the persistent crash record.
9. It increments the boot counter.
10. It decides whether safe mode must be active.
11. It checks that the IWDG and RTC devices are ready.
12. It starts the RTC counter.
13. It initializes the Zephyr task watchdog with IWDG fallback.
14. It creates Worker A's watchdog channel and starts Worker A.
15. In normal mode, it creates Worker B's channel and starts Worker B.
16. In safe mode, Worker B remains disabled.
17. The main loop publishes a health snapshot every second.
18. After 60 stable seconds in normal mode, consecutive failures are cleared.

## 21. Shell commands

Connect through the ST-LINK Virtual COM Port using 115200 baud, 8 data bits, no
parity, and 1 stop bit.

| Command | Purpose |
|---|---|
| `health` | Shows mode, uptime, heartbeats, watchdog events, and failure mask |
| `system_info` | Shows board, Zephyr, clock, watchdog, storage, and console details |
| `kernel threads` | Uses Zephyr's built-in shell to list kernel threads |
| `fault worker_b` | Deliberately stops Worker B's watchdog feed |
| `recover` | Performs a clean software reboot |
| `crash_log` | Displays persistent boot, failure, safe-mode, and reset history |
| `clear_failures` | Clears stored failure-history fields |
| `safe_mode` | Displays the safe-mode policy and state |
| `safe_mode exit` | Clears the safe-mode latch and reboots |
| `image_status` | Shows MCUboot version, slots, confirmation, and next action |
| `image_test` | Requests a one-time trial upgrade and reboots |
| `image_confirm` | Permanently accepts the running trial image |
| `power_stats` | Shows RTC alarms and power-state counters |
| `power_demo 1` | Runs a measured one-second RTC alarm/idle demonstration |

## 22. Meaning of important command output

### Healthy system

```text
System mode:     normal
Worker A:        healthy
Worker B:        healthy
Watchdog events: 0
Failure mask:    0x00
Recovery:        idle
```

This means both workers are feeding their watchdog channels on time.

### Worker B failure

```text
Watchdog detected failure on channel 1 (mask 0x02)
Crash record saved; hardware watchdog will reboot the MCU
```

This proves that the per-thread watchdog identified Worker B, the recovery
thread saved evidence, and the IWDG was allowed to reset the MCU.

### Safe mode

```text
State:                ACTIVE
Consecutive failures: 3/3
Worker B:             disabled
```

This proves the reboot-loop protection is active.

### Trial firmware

```text
Running build: 3.1.0+0
Confirmed:     NO (trial)
Next action:   revert
```

This means the new image is running but the old image remains available for
rollback.

### Confirmed firmware

```text
Running build: 3.1.0+0
Confirmed:     yes
Next action:   none
```

This means the update was accepted permanently.

## 23. Development phases completed

### Phase 1: RTOS supervision foundation

- Custom Zephyr board support
- Devicetree and Kconfig configuration
- Multiple supervised threads
- Independent heartbeat counters
- Zephyr task-watchdog channels
- Interactive Zephyr shell
- Runtime fault injection
- Failure identification by bit mask
- Manual software recovery
- Real-hardware validation

### Phase 2: automatic recovery and persistent evidence

- STM32 IWDG hardware fallback
- Automatic watchdog reboot
- NVS crash-record storage
- Boot and recovery counters
- Failure uptime and failed-channel recording
- Hardware reset-cause capture
- Persistent `crash_log` command
- Failure-history clearing
- Record-format validation and migration

### Phase 3: repeated-failure protection

- Consecutive-failure tracking
- Safe mode after three recoveries
- Worker B isolation in safe mode
- Continued Worker A supervision
- Fault-injection lockout in safe mode
- Persistent safe-mode latch and entry counter
- Manual safe-mode exit
- Stable 60-second runtime clearing policy

### Phase 4: secure and recoverable updates

- MCUboot integration
- ECDSA-P256 image signatures
- Primary and secondary firmware slots
- Swap-using-offset upgrade mode
- Trial-image boot
- Explicit image confirmation
- Automatic rollback
- Preserved NVS crash partition
- Successful v2 to v3 candidate validation

### Phase 5: watchdog-aware power management

- Zephyr PM and tickless kernel
- Internal LSI-clocked RTC Counter API
- RTC subsecond support
- Explicit RTC alarm wake callback
- Power-state entry and wake statistics
- Measured idle demonstration
- Permanent deep-STOP safety locks
- Worker and IWDG supervision preserved during the test
- Final signed firmware version `3.1.0+0`

## 24. Project file guide

| Path | Purpose |
|---|---|
| `src/main.c` | Application threads, watchdogs, recovery, shell, MCUboot, and RTC logic |
| `prj.conf` | Zephyr features enabled for the application |
| `Kconfig` | Project-specific fault-injection option |
| `CMakeLists.txt` | Adds the application source to the Zephyr build |
| `VERSION` | Firmware version `3.1.0+0` |
| `boards/st/zephyr_l4p5/` | Custom board and Devicetree files |
| `sysbuild.conf` | Enables MCUboot and ECDSA-P256 policy |
| `sysbuild/mcuboot.conf` | MCUboot-specific configuration |
| `configs/fault_injection.conf` | Optional automatic Worker B failure build |
| `configs/trial_image.conf` | Earlier trial-image demonstration configuration |
| `keys/README.md` | Development-key generation instructions |
| `west.yml` | Fetches Zephyr 4.4.0 and required modules |
| `.gitignore` | Excludes builds, SDK sources, virtual environment, and private keys |
| `README.md` | Repository summary and build instructions |

## 25. How the project was validated

The implementation was not only compiled. It was tested on the physical
NUCLEO-L4P5ZG board.

Validation included:

- observing both heartbeat counters increase at independent rates;
- confirming zero watchdog events during healthy operation;
- injecting a Worker B failure through the shell;
- observing watchdog channel 1 and failure mask `0x02`;
- observing the IWDG reboot the board;
- reading the saved failure after reboot;
- repeating failures until safe mode activated;
- confirming Worker B was disabled in safe mode;
- exiting safe mode and restoring normal operation;
- staging signed MCUboot candidate images;
- testing unconfirmed-image rollback;
- confirming a successful image;
- observing rollback after unsafe STOP-mode trials;
- validating the final watchdog-aware RTC design;
- performing a clean Zephyr and MCUboot build for v3.1.0.

## 26. Why this project is advanced

Blinking an LED proves that a toolchain and GPIO work. This project goes much
further because it combines several production ideas:

- RTOS concurrency;
- per-service liveness supervision;
- hardware-enforced recovery;
- persistent post-crash diagnosis;
- backward-compatible stored-data migration;
- repeated-failure containment;
- secure boot;
- signed updates;
- A/B-style firmware slots;
- trial and rollback policy;
- power-versus-safety decision making;
- real-hardware fault injection and validation.

The value is not the number of features alone. The features cooperate as one
reliability system.

## 27. Where this design can be used

The same architecture can be adapted for:

- a motor-control task that must never stop silently;
- a communication task that may become stuck;
- a medical-monitoring service requiring recovery evidence;
- a remote sensor that must recover without a technician;
- an automotive body controller with safe fallback behavior;
- an industrial gateway receiving field firmware updates;
- a robot that isolates a failing non-critical subsystem;
- a smart meter that needs signed firmware and rollback.

In a real product, Worker A and Worker B would perform useful services instead
of only incrementing heartbeat counters.

## 28. Honest limitations

Every engineering project should clearly state what it does not yet provide.

- The workers are demonstration services, not real sensor or control tasks.
- Updates are staged through the programmer, not downloaded over Wi-Fi, BLE,
  Ethernet, USB DFU, or CAN.
- The signing key is a development key, not a production-managed key.
- Safe mode uses a fixed policy rather than a configurable service manager.
- The crash record stores structured status but not a full CPU register dump or
  stack backtrace.
- Deep STM32 STOP modes remain locked because they were not proven compatible
  with the current two-second watchdog policy.
- No external power analyzer was used, so the project does not claim a measured
  current reduction.
- The shell has no authentication and is intended for development access.

These limitations do not invalidate the project. They define clear next steps.

## 29. Good future improvements

Possible next milestones are:

1. Export health telemetry to a PC dashboard.
2. Add MCUboot image upload through USB DFU, BLE, Ethernet, or UART management.
3. Capture CPU fault registers and a compact backtrace.
4. Add a configurable service-supervision table.
5. Add unit tests and Zephyr Twister hardware tests.
6. Measure energy with a power analyzer and design a proven watchdog-aware STOP
   wake strategy.
7. Store signing keys in a protected CI/CD signing service.
8. Add anti-rollback security counters.
9. Authenticate or disable the production diagnostic shell.
10. Publish crash telemetry before entering recovery.

## 30. Interview explanation in 30 seconds

> I built a self-healing firmware platform on the STM32L4P5 using Zephyr RTOS.
> Two independent threads are supervised by separate Zephyr task-watchdog
> channels backed by the STM32 IWDG. If a task stops feeding, the firmware
> identifies it, saves a persistent NVS crash record, and lets the hardware
> watchdog reset the MCU. Three consecutive recoveries activate safe mode and
> isolate the failing service. I also integrated MCUboot with ECDSA-P256 signed
> images, trial boot, confirmation, and automatic rollback. The final phase adds
> RTC alarm scheduling and watchdog-aware power policy, with unsafe deep STOP
> modes deliberately locked after real-hardware testing.

## 31. Common interview questions and answers

### Why did you create this project?

To learn and demonstrate how reliable embedded products detect failures,
recover automatically, preserve diagnostic evidence, prevent reboot loops, and
perform safe firmware updates.

### Why not use only one hardware watchdog?

A single hardware watchdog can reset the MCU, but it usually cannot identify
which software service failed. Separate Zephyr task-watchdog channels provide
service-level identification, while the STM32 IWDG guarantees the final reset.

### Why save the crash record before rebooting?

RAM disappears after reset. Saving the failure mask, channel, uptime, and reset
history in NVS makes post-crash diagnosis possible.

### Why use NVS instead of writing one fixed Flash address repeatedly?

NVS manages records, detects data with CRC support, and distributes writes more
safely than repeatedly erasing and writing one raw location.

### Why use a mutex around the crash record?

The shell and recovery logic can access the same structure. The mutex prevents
simultaneous updates from corrupting the in-memory record or producing an
inconsistent Flash write.

### Why are heartbeat counters atomic?

Several threads and callbacks read or modify them. Atomic operations prevent a
partial or lost update without requiring a larger lock.

### What happens when Worker B fails?

Its watchdog channel expires after two seconds. The callback sets bit `0x02`,
wakes the recovery thread, and stops normal recovery feeds. The recovery thread
saves the failure to NVS. The IWDG resets the MCU. The next boot reports the
hardware-watchdog reset and the saved Worker B failure.

### What is safe mode?

It is a restricted boot after three consecutive recoveries. Worker B is
isolated, while Worker A, diagnostics, persistent storage, and supervision stay
available. This breaks an endless reboot loop.

### Why use MCUboot?

MCUboot authenticates firmware before execution and manages safe trial,
confirmation, and rollback behavior using two Flash slots.

### What is the difference between a checksum and a signature?

A checksum detects accidental changes. A digital signature also proves that
the image came from someone holding the trusted private key.

### Why is the private key not on GitHub?

Anyone with the private key could sign unauthorized firmware. The repository
ignores `.pem` files and only contains instructions for generating a local
development key.

### Why are there two firmware slots?

The old working firmware must remain available while the new firmware is being
tested. If the candidate fails, MCUboot restores the old image.

### Why must a trial image be confirmed?

Booting successfully once does not prove the firmware is healthy. Confirmation
is an explicit decision made only after validation. Without it, rollback stays
available.

### Why did you lock deep STOP modes?

Real-hardware trials showed that the current deep-STOP wake configuration could
starve the two-second watchdog deadline. Safety is more important than an
unproven power saving, so the final policy keeps deep STOP locked while retaining
normal idle, tickless scheduling, and RTC alarms.

### Is locking STOP a failure?

No. The experiments discovered a system constraint, rollback protected the
board, and the final design enforces the safe operating policy. Recording and
acting on a failed validation is good engineering.

### How do you know the project works on hardware?

The heartbeats, fault detection, failure mask, crash record, reset cause, safe
mode, MCUboot trial, rollback, confirmation, and RTC behavior were observed on
the NUCLEO-L4P5ZG through the serial shell and programmer.

### What Zephyr concepts are demonstrated?

Threads, priorities, scheduler sleeps, semaphores, mutexes, atomics, Devicetree,
Kconfig, shell commands, deferred logging, task watchdog, hardware watchdog,
NVS, Flash map, hardware information, MCUboot APIs, system PM, PM notifiers,
tickless kernel, and the Counter API.

## 32. Resume-ready project description

**Zephyr Self-Healing Firmware Platform — STM32L4P5**

- Developed a custom Zephyr 4.4 board target and multithreaded health supervisor
  for the STM32L4P5 using per-thread task-watchdog channels backed by the STM32
  independent hardware watchdog.
- Implemented fault injection, service-level failure masks, automatic hardware
  reset, reset-cause capture, and versioned persistent NVS crash records.
- Designed a repeated-failure safe mode that isolates a failing service after
  three consecutive recoveries while preserving diagnostics and supervision.
- Integrated MCUboot with ECDSA-P256 signing, dual 448 KiB slots, trial boot,
  explicit confirmation, and automatic rollback.
- Added tickless Zephyr PM, internal RTC alarm measurement, and a safety policy
  that blocks unvalidated deep-STOP states from violating watchdog deadlines.
- Validated healthy operation, injected failures, recovery, safe mode, rollback,
  and signed v3.1.0 firmware on a physical NUCLEO-L4P5ZG board.

## 33. Final simple summary

The board runs two jobs and checks that both remain alive. If one job stops, the
firmware knows which one failed, saves the evidence, and lets a hardware timer
restart the chip. If failures repeat, the system starts in a safer restricted
mode. Firmware updates must carry a valid digital signature, run as a trial,
and can automatically return to the previous working version. The final power
policy uses the internal RTC while refusing unsafe sleep states that could break
watchdog supervision.

In short:

```text
watch -> detect -> record -> reset -> isolate -> diagnose -> update safely
```

That complete chain is the main achievement of the project.
