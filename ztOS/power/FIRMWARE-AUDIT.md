> Wi-Fi update, 2026-10-16: kernel #27 restores PCIe host/PHY/iATU state
> during resume_noirq and restores the two WLAN GPIO controllers before
> PCI config access. Native AP-sleep recovery passed without the temporary
> recovery module, including reconnect and three successful router pings.
> This fixes post-sleep PCIe recovery; the diagnostic still detaches
> brcmfmac before sleep because firmware D3 acknowledgment is unresolved.
> Details: [Wi-Fi resume](suspend/pcie-recovery/README.md).

> Correction, 2026-10-16: historical trials below interpreted the debug
> `mif_always_on` field incorrectly. Live firmware disassembly shows
> 0x0204f95c=0 disables MIF sequencing; 1 enables it. Trials that cleared this
> field did not test full MIF sleep. AP/SoC hardware sleep and RTC resume are
> now verified, but full MIF resume is not. See suspend/README.md for the
> guarded test result and missing Samsung initialization prerequisites.

# Samsung power audit, 2026-10-15

Firmware examined: `SM-G960N_G960NKSU1CSE3_G960NOKR1CSE3_Korea_KOO_9.0_6File_(by_firmwareplus.com).zip`.
The AP archive's `boot.img.lz4` was decompressed, including its Android
ramdisk and six DTBH board device trees (Star revisions 16, 17, 18, 20, 23,
26). This is a power-management audit of that boot image and the matching
local vendor sources, not a complete audit of every Android feature or of
the SYSTEM/VENDOR filesystem images.

The kernel SHA-256 is
`cbeac83e93e9ab94691dcca4d7c2529bbeed01dbc4b3fe21c310a6baae5138aa`.
It differs from the previously saved `stock/BOOT.img` kernel
(`e597cec19f292ae18becc525b42260275e5ca68ccdb586fe17a2350a1834de12`).
The complete policy evidence is in [samsung-power-audit.json](samsung-power-audit.json).
Reproduce it with:

```sh
python3 scripts/dev/samsung-power-audit.py /path/to/firmware.zip
```

## Current deep-sleep work

Kernel #24 (`out/ztos-sleep-input.img`, SHA-256
`21c7be4d08cfd0c071504d8b849bd49e66182e07a71d49857bd94836dc8e9670`)
is installed with all eight CPUs enrolled in C2 (`ZRN_CPUIDLE_CPUS=0xff`).
Runtime SICD and experimental system SICD remain disabled by default.
RECOVERY is still TWRP. Always warm boot from TWRP for sensor-hub tests.

A missed MCT comparator deadline was found after the original battery tests:
the old driver reported success even when MMIO programming finished after
the equality deadline, leaving the broadcast force mask set and CPUs polling.
The minimum interval is now 200 us; an expired deadline returns `-ETIME` so
the clockevent core retries. The original battery A/B results below **cannot
establish C2 savings**, because counters had stopped advancing during that
configuration. The extracted real set-next C function passes delayed-MMIO
regression checks (`power/idle/test-mct-deadline.py`); 100 hardware idle-state
toggles completed with every online CPU's C2 counter still advancing.

The spare MCT G1 comparator provides a one-shot, oscillator-backed test
wake alarm (`alarm_seconds`). Its IRQ calls `pm_system_wakeup()` and does
not depend on frozen userspace or CPU-local ticks. Comparator1's write-status
bits are 4/5, unlike comparator0's 0/1.

System-PM tests exposed three device issues:

* BCM4361 firmware does not acknowledge the normal PCI D3 suspend handshake.
  The diagnostic detaches brcmfmac before suspend and rebinds it on wake;
  this restarts Wi-Fi and is a workaround, not working Wi-Fi D3 support.
* UFS Hibern8 system resume fails even after preserving device reset/PHY
  retention. Explicit system PM level 5 (power-down/link-off), followed by
  the supported full host/device reset on wake, passes. Runtime clock
  gating remains separate. The diagnostic restores the prior PM level.
* Touch supplies are fixed always-on regulators. A system resume cannot
  expect a fresh boot-complete event. The patch uses application sleep
  with runtime-PM state preserved. A subsequent correction marks the
  already-powered device active at probe so the PM core sends sleep.

With kernel #22, the device-stage dry run passed with no suspend/resume
failures. A genuine 20-second s2idle trial also passed: `success` increased
from 1 to 2, the MCT wake count increased from 0 to 1, USB returned,
and the sensor hub stayed ready. Wall time advanced 20.77 seconds while
monotonic time advanced 0.89 seconds. Firmware SoC/MIF counters stayed zero:
this proves timed system suspend and wake, **not SoC or memory deep sleep**.

The next SICD trial made 13 attempts, all returning `-EOPNOTSUPP` from
`cpu_suspend`, with zero successful SICD entries and unchanged firmware
counters. Non-boot cluster power-down is now under separate testing using
Samsung's MAILBOX_APM2AP bit24 handshake, CPU_INFORM=2, and PSCI state
`0x01000000`; it is restricted to system suspend and disabled by default.
DFD-enabled configurations are refused until their extra reset handling is
implemented. No result is inferred from the attempt count alone.

`power/firmware-state.py` reads the exact FLEXPMU debug layout from
`exynos-flexpmu-dbg.c`, at physical `0x0204f800` (size `0x400`, confirmed in
the supplied firmware DTs). It never writes SRAM. AP/MIF SLEEP and SICD
counters are the evidence for actual hardware sequencing. The current
firmware has MIF-always-on=1 and request-master bits `0x13`. A bounded test
cleared only the documented MIF-always-on debug control and restored its
original value afterward; counters still did not advance. Request-master
flags and sequencer state were never overwritten.

`power/suspend-test.py` records counters and PM failures before/after a timed
trial. It requires the display off and sensor hub ready, restores its PM
parameters and device bindings after return, and is run as a detached
systemd unit so USB suspension cannot kill the test.

Kernel #24 also balances runtime-PM references in touch input open/close;
without those references an open input device could stay asleep after PM
resume. Its device-stage test increased PM success from 0 to 1 with zero
failures, left touch `runtime_status=active`, `runtime_usage=1`, and sensor
hub ready=1. Earlier #22/#23 device tests did not establish this reference
balance. The #24 BOOT hash was verified by reading back the image bytes.

CPD trials now have successful CPU context-resume returns (`cpd_entries`),
including a combined CPD/SICD trial with the 20-second alarm firing. SICD
still has zero successful context resumes and zero firmware counters;
a later duration-instrumented test measured only 32 us per attempted SICD
call. CPD is therefore progress, not proof of complete-system sleep.

To verify the vendor source against the actual supplied firmware, the
kernel's kallsyms were recovered with [vmlinux-to-elf](https://github.com/marin-m/vmlinux-to-elf).
Its `psci_suspend_customized_finisher` at `ffffff80089ee738` uses states
`0x01000000` for index 128, `0x1` for index 129, and `0x03000000` for index
133, matching our source audit. Its `acpm_get_mifdn_count` at
`ffffff800850d298` loads offset `0x17c`; `acpm_get_mif_request` at
`ffffff800850d2b0` loads offset `0xac`, bits 16..23. These confirm the SRAM
counter layout used by the diagnostic.

The corrected `battery-suspend-test.sh` run completed after cable removal;
logs are retained under `/var/lib/ztos-power-test/20261015/`. The 20-second
s2idle baseline returned after 8.7 seconds of wall time, with PM success
increasing and no recorded PM failures, but its MCT G1 alarm did not fire
(`alarm_fires` stayed zero; the last wake IRQ was 105). The
30-second CPD/SICD run returned after 2.2 seconds; CPD context resume
succeeded once, while SICD had two attempts, zero entries, and `-EOPNOTSUPP`.
The CPU0-restricted run also returned after 2.2 seconds and CPD resumed
successfully; SICD had zero entries. AP/MIF sleep and SICD firmware counters
remained zero in all snapshots. These early returns do not verify a timed
MCT wake or deep sleep. A live awake-state check then armed G1 for five
seconds; IRQ 108 fired and `alarm_fires` incremented, so the comparator works
while awake. A focused 10-second s2idle check also returned in 1.49 seconds
with no alarm increment. `pm_wakeup_irq` was 105 both before and after, and
debugfs wake-source counters were unchanged. A subsequent 10-second test,
with a 15-second settling pause after unplug, returned in 2.51 seconds:
IRQ 105 (`gpa1` pin 7, driver action `ttyBCM`) increased five times while
the MCT alarm IRQ 108 did not fire. Samsung's device tree assigns `gpa1-7`
to BCM4773 `ssp-host-req`; the vendor SPI driver requests that line as
`ttyBCM` and enables it as a wake IRQ while open. This identifies the
sensor-hub host-request line as the source interrupt during the failed trial;
the specific userspace request that caused the hub to assert it remains
unknown. The helper fails if the requested MCT alarm did not fire, so early
wakes cannot be counted as timed success. Wi-Fi binding and temporary UFS/MIF
settings were restored; sensor hub stayed ready=1 and the display stayed off
after USB reconnect. The next power-management task is to make the BCM4773
quiet in its screen-off state while preserving required sensor wake behavior.

## Firmware policy and implementation status

| Samsung mechanism | Evidence and ztOS status |
| --- | --- |
| Per-core C2 | Already implemented by zrn-cpuidle. This session corrected RCU/context tracking and discovered/fixed the kernel suspend-context hash collision described below. |
| Non-boot-cluster CPD | Enabled in Samsung's trees, 10,000 us residency. A separate cluster power-down sequence is not implemented in zrn-cpuidle. Per-core C2 is not proof that CPD works. |
| SICD | Enabled in every extracted tree, 10,000 us residency. The pending module implements the firmware call and wake masks; this session made device checks strict by default, corrected deadline sampling, initialized its spinlock, serialized trials, and restored register masks after resume/abort. Full system sleep remains experimental. |
| Device-idle reporting | Samsung drivers call `exynos_update_ip_idle_status()`; these are software-maintained PMU bits, not simply autonomous hardware observations. Equivalent integration across active mainline devices is missing. Zero status bits must block SICD; `relax=1` bypasses this protection and is not a production fix. |
| UFS Hibern8 / idle clock gating | Present in the Samsung kernel and local port. Enabled in deployed kernel #19 using `ufs_exynos9810.clkgate=1`; `clkgate_enable=1`, delay 150 ms. Eight fsync/cache-eviction/idle/read rounds passed without storage errors. SCSI logical-unit runtime suspend is still separate and not enabled by this change. |
| PCIe low-power/SICD coordination | Both Samsung PCIe DT nodes have `use-sicd="true"`; only PCIe0 is enabled. Kernel #27 now restores PCIe and Wi-Fi after AP hardware sleep, verified for two cycles. System-idle coordination remains unimplemented; Wi-Fi radio-off alone does not prove the PCIe PHY is asleep. |
| USB low-power coordination | Samsung provides USB-L2 suspend policy. USB controller runtime PM is forced on in the tested ztOS baseline. USB networking must be preserved while attached. |
| Memory/interconnect DVFS and system suspend | Samsung has additional devfreq/power-mode infrastructure. ztOS's ACPM PMIC/CPU/GPU support does not supply the complete memory/interconnect policy or Samsung's full suspend path. No arbitrary DRAM/PLL register writes were enabled. |
| Panel sleep / Wi-Fi inactivity | Panel sleep was already implemented. The daemon's pending Wi-Fi change now handles unplugging while the display is off, wakes its poll loop at the deadline, tracks command failures, preserves a manually disabled radio, and retains display state across service restarts. |

All six firmware trees agree on SICD:

* PSCI CPU_SUSPEND: `0xc4000001`.
* IDLE_IP masks: `00000000 00000000 ff9003c0 fffffffe`.
* Wake masks at PMU offsets `610`, `614`, `618`, `644`:
  `400001e0 00000000 00000000 00000000`.
* The EINT mask register is at PMU offset `0x60c`, not `0x06c`.

## Suspend-context corruption

The live kernel reports A55 hardware MPIDRs with MT set, with core number
in Aff1. M3 hardware MPIDRs have core number in Aff0. However, DT/PSCI
identifiers are normalized as `0..3, 0x100..0x103`. The upstream suspend
hash is built from those normalized IDs and then applied to raw hardware
MPIDRs in `sleep.S`.

| CPU | Raw hardware MPIDR | Old hash slot | Corrected slot |
| --- | --- | --- | --- |
| 0 | `81000000` | 0 | 0 |
| 1 | `81000100` | 4 | 1 |
| 2 | `81000200` | 0 | 2 |
| 3 | `81000300` | 4 | 3 |
| 4 | `80000100` | 4 | 4 |
| 5 | `80000101` | 5 | 5 |
| 6 | `80000102` | 6 | 6 |
| 7 | `80000103` | 7 | 7 |

That explains why CPU2/3 alone appeared stable while enrolling CPU4
corrupted CPU3's saved stack. A CPU4 test on kernel #17 panicked immediately
with a stack-protector failure. The watchdog returned to TWRP; its pstore
log was saved on the phone under
`/var/lib/ztos-power-test/20261015/cpu4-pstore.txt`.

[patch-suspend-mpidr.py](../kernel/patch-suspend-mpidr.py) normalizes only
Exynos9810 MT core IDs before hashing, in both the save and resume assembly.
The ordinary behavior is preserved on other machines. The build script
applies this patch automatically. A wider module CPU mask is rejected by
the wrapper when the patched kernel symbol is absent.

Validation:

* The actual patched assembly ran under qemu-aarch64 with both register
  assignments: all eight slots are distinct, and platform-off behavior is
  unchanged. Run `python3 kernel/test-suspend-mpidr.py`.
* Kernel #18 with `cpus=0x1c`, SICD disabled, booted from TWRP and completed
  over 1,000 CPU4 C2 transitions without the previous panic.
* The same kernel with `cpus=0x1f` completed over 10,000 CPU0 transitions
  and thousands on CPU1..4 without the old stack corruption. Screen-off
  policy brought the online set back to CPU0..4. SICD remained gated.
* Sensor hub and sensor services were active after the TWRP warm boot.
  Guards left by the known panic were archived rather than mistaken for
  fresh sensor/hotplug failures.
* Wi-Fi ownership and command/query failure tests pass. Run the command at
  the top of `ztmetro/perfd/tests/wifi-radio.c`.

## Deployment and measurement limits

The initial hash-fixed image is `out/ztos-power-mpidr.img`, SHA-256
`038c1fca3c8b5274a14588c0c4201764d42e7e34a293ba962cdabe2bd6fcf95a`.
The existing BOOT was backed up on the phone to
`/var/lib/ztos-power-test/20261015/boot-before-mpidr.img`; its hash matched
the partition before flashing. The new image's hash matched a readback.
RECOVERY/TWRP was not overwritten.

The subsequently deployed UFS-enabled image is `out/ztos-power-ufs.img`,
SHA-256 `156f3780d8f483dacefe4d67c76c18a4ccd479d62ca91272d078a1081d4b54ae`.
Readback matched. The prior hash-fixed BOOT is also backed up at
`/var/lib/ztos-power-test/20261015/boot-hash-fixed-before-ufs.img`.
Reproduce this image with:

```sh
scripts/build-boot.sh ztos-power-ufs --fallback recovery --power --touch \
  --gpu --decon --wlan --cpufreq --cmdline ufs_exynos9810.clkgate=1
```

`python3 power/ufs-idle-test.py` performs the repeated UFS file I/O test on
the phone, using an automatically removed temporary file, never raw disk
writes. It verifies data integrity after idle opportunities; without trace
evidence it is not a count of proven hardware Hibern8 transitions.

The recovery marker now has an independent systemd confirmation timer;
it survives the SSH session that loaded the module. An existing mark is
still respected. Sensor testing requires a warm reboot from TWRP, as
reported by the user; a normal mainline reboot is not equivalent.

The phone's persistent service drop-in is
`/etc/systemd/system/zrn-cpuidle.service.d/power.conf`: `CPUS=0x1f`,
`SICD=0`, `RELAX=0` (using the `ZRN_CPUIDLE_` environment prefix).
SICD remains disabled: diagnostics confirmed timer/CPU eligibility blocks,
but did not establish a successful complete-system sleep or wake. The
new device blocker counter distinguishes that gate from the other two;
zero entries is not a power-saving result.

The first completed battery-only run sampled for 390 seconds after unplug.
The first 10 seconds reported the display on and must be discarded. After
that the daemon reported the display off. Mean current over selected settled
windows was 261.4 mA (C2 enabled, seconds 20..60), 244.9 mA (C2 disabled,
seconds 80..120), 246.7 mA (C2 enabled again, seconds 150..240), and 254.2 mA
(C2 enabled, Wi-Fi sleeping, seconds 320..380). These short unmatched
windows show no clear incremental saving from C2 or radio-off. They do not
isolate UFS's effect or establish overnight battery life. Wi-Fi sleep was
observed at 310.7 seconds. Battery capacity went from 74% to 73%.

The original diagnostic restored the screen at 390 seconds, before the
requested eight-minute wait; this explains the screen being on at the end.
DRM was subsequently checked during an explicit off request: connector
disabled, CRTC `enable=0`, `active=0`. The diagnostic now requires both
daemon-off and DRM-disabled state before sampling, records validity for
every sample, and leaves the display asleep when it finishes on battery.
The historical ~350 mA floor in README.md is not a matched control for this
run or proof that a particular clock/rail accounts for all of it.

The corrected follow-up run completed all 390 seconds with **78/78 samples
confirmed screen-off** by both daemon state and DRM output state. The screen
was left asleep afterward. Results (positive numbers below mean draw):

| Window | Mean mA | Median mA |
| --- | ---: | ---: |
| C2 enabled, seconds 20..60 | 242.9 | 243.4 |
| C2 disabled, seconds 80..120 | 253.3 | 245.0 |
| C2 enabled again, seconds 150..240 | 255.2 | 247.0 |
| C2 enabled, Wi-Fi sleeping, seconds 320..380 | 258.3 | 253.4 |

Wi-Fi sleep was observed at 300.8 seconds and the radio returned on USB
reconnect. Battery capacity went from 73% to 72%, temperature stayed at
30.8 C, and all five CPU C2 disable flags returned to zero. Sensor and
performance services stayed active; no kernel panic, kernel warning,
storage I/O error, or ext4 error matching the final diagnostic query was
reported. This confirms the diagnostic and recovery behavior. The short
windows still do not establish a meaningful C2/radio saving, and the
remaining idle draw is roughly 250 mA with this configuration. The complete
follow-up log is retained on the phone at
`/var/lib/ztos-power-test/20261015/idle-ab-verified.jsonl`.

`idle-ab.py` provides a detached battery-only test: wait for USB unplug,
sample 390 seconds, disable C2 only during seconds 60..120, and observe
the daemon's normal five-minute Wi-Fi timer. It restores the initial idle
settings on completion, signal, or reconnect. For clean comparisons,
discard samples while charging, with the display on, and immediately
after phase changes.

The bounded BCM4773 host-request A/B now confirms IRQ 105 is causing the
premature s2idle returns. With its wake reference temporarily suppressed
(handler and sensor hub left running), a 10-second battery-only trial lasted
10.84 seconds, the MCT alarm fired, IRQ 105 counted two events without waking
the AP, and IRQ 108 recorded the expected alarm. Sensor-hub readiness
remained 1. The unmasked comparison had IRQ 105 events and returned before
the alarm.
The test module was unloaded afterward, restoring IRQ 105 wake. This proves
that the host-request wake causes the short return, but firmware AP/MIF sleep
counters still read zero, so it does not prove deep-sleep entry. Keep the IRQ
mask diagnostic-only until sensor wake requirements are handled. The snapshot
helper now records `pm_wakeup_irq` as unavailable when the kernel node returns
`ENODATA`, preserving the rest of the after-snapshot for alarm-driven wakes.
The vendor `bcm_spi_open()` enables IRQ 105 as a wake source while `/dev/ttyBCM`
is open. Its suspend callback disables the IRQ handler and drains queued work,
but leaves the wake reference enabled until userspace closes the device. That
is a concrete path for host-request activity to wake the AP during suspend;
the bounded CPD/SICD trial with MIF sleep allowed confirmed CPD entry and a
timed MCT wake while IRQ 105 was masked. Pinning SICD to CPU 0 left CPU2
outside C2 at the aggregate gate (`last_block=cpu2 up`). An unpinned retry
allowed SICD attempts (2 to 4) but no entries; the final blocker was CPU4.
CPD entered, IRQ 105 had no events during the masked interval, the MCT alarm
fired, and sensor-hub readiness stayed 1. CPUs 0-4 were online (CPUs 5-7 were
already offline), so the next bounded test was set to temporarily offline
CPU4. The hotplug guard refused because CPU4 is the last online CPU in its
cluster (`zrn-hotplug: cpu4 is the last of its cluster and stays`); no suspend
trial ran and CPU4 remained online. The replacement test keeps CPU0 and CPU4
online and temporarily offlines only CPUs1-3, restoring them through an exit
trap. AP/MIF sleep counters remain zero. A permanent driver change still
needs a sensor-wake-preserving policy.

Further unplug trials are stopped pending source analysis. The raw SICD
`last_smc_result=0` must be distinguished from `last_error=-95`: the local
kernel's `arch/arm64/kernel/suspend.c` converts a finisher that returns zero
directly into `-EOPNOTSUPP`, because successful power-down must resume through
`cpu_resume`. Thus `-95` is not evidence that firmware rejected the request
as unsupported. The unresolved issue is why this request returns directly
rather than completing the power-down/resume path. Later `last_block=cpuN up`
messages describe separate gate checks and do not explain those SMC returns.
The pending reduced-CPU service was stopped; CPUs 0-4 are online, the sensor
hub is ready, the IRQ test module is unloaded, and s2idle CPD/SICD overrides
are disabled.

### Connected SICD investigation

Three connected 10-second trials completed automatically with USB returning
and sensor-hub readiness remaining 1. They did not require unplugging, did
not suppress IRQ 105 wake, and counted no IRQ 105 events. The first used the
normal online set, the second temporarily used CPUs 0 and 4 with MIF-always-on
cleared, and the third used CPUs 0-4 with MIF-always-on cleared. SICD attempts
advanced 4 -> 5 -> 6 -> 14; entries remained zero. MCT alarm fires advanced
5 -> 6 -> 7 -> 8. The reduced CPU set did not resolve the direct SMC return.
The CPU set and temporary MIF setting were restored after every trial.

`power/idle/diagnostic/sicd-return` is a separate read-only CPU-PM notifier.
It captures GICC HPPIR, enabled pending GIC interrupts, BID group 4, and MCT
timestamps after a C2 interval spanning a SICD attempt. Caller snapshots
reported HPPIR 1023 (no pending interrupt), while other cores showed SGI 0/1
(rescheduling/function calls). The final wake snapshot reported hardware IRQ
499, matching the MCT alarm. This observation is at CPU_PM_EXIT, after the
idle driver has already cleared BID group 4 on an early return: it cannot
rule out a transient pending interrupt at the exact SMC boundary. The first
sample from the first module build was discarded because registration could
occur while a CPU was already in C2; the corrected build initializes every
CPU's attempt baseline before registration. The diagnostic was unloaded after
collecting evidence, without unloading cpuidle or restarting the sensor hub.

There is also a separate missing system-suspend implementation: the live
`/sys/power/mem_sleep` exposes only `[s2idle]`. Stock `exynos-pm.c` registers
platform suspend operations and prepares `SYS_SLEEP` (or `SYS_SLEEP_USBL2`)
through a syscore callback, calls customized PSCI index 133, and restores the
matching PMUCAL state. Stock `drivers/firmware/psci.c` packs index 133 as
`0x03000000`, whereas SICD index 129 packs as `0x00000001`. The SICD PMUCAL
enter/save/exit arrays are empty; the SYS_SLEEP save/restore arrays are not.
The current port's cpuidle SICD callback does not provide that platform
suspend path. Porting full system sleep therefore requires its CPU-hotplug,
register save/restore, wake-mask and device-resume integration; issuing the
different SMC alone is not an implementation. This does not yet explain the
SICD early return or establish that all prerequisites for full sleep are met.

The archive's bootloader is CSE3, while the saved live TWRP DT identifies
G960NKSU5FVA2. Extracted BL binaries under `build/power-analysis` are useful
references, but must not be assumed to be the monitor currently running.
The connected evidence is saved at
`build/power-analysis/connected-sicd-evidence.log`.

The timed test now fails explicitly when SICD was requested but its suspend
entry count did not advance, or when a software entry lacks an AP firmware
sequence-down increment. `--validate-only` checks an invocation without
hardware access. The reduced-CPU wrapper validates its exact command before
waiting or offlining CPUs; both battery and connected invocations were
checked on the host, including rejection of the previous missing-unplug
argument. `--connected` allows further controlled diagnostics without manual
cable cycles; it is not a battery-power validation.
