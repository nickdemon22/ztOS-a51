# Exynos9810 RTC wake source

The stock `samsung,exynos8-rtc` block at 0x141e0000 is running but was absent
from the mainline board setup. `zrn-exynos-rtc.c` exposes its calendar and
one-shot alarm through the RTC class. Stock IRQ resource 0 is SPI31 / GIC63
(alarm); resource 1 is SPI32 / GIC64 (tick). The calendar uses BCD and supports
2000-2099. The corrected source rejects invalid BCD dates and suppresses the
synchronous RTC_HCTOSYS read during dynamic registration so it cannot replace
an established system clock while probing.

The first live prototype registered an invalid year (0xff) and initially
used the tick IRQ. The system clock was restored immediately from the host,
and `initialize-calendar.py` initialized only the invalid hardware calendar
from corrected UTC. Kernel alarmtimer pins the first wake-capable RTC module,
so it cannot be replaced live. `diagnostic/zrn_rtc_irq_bridge.c` routes the
correct alarm IRQ to that prototype's existing handler without text/data
patches. It is a temporary current-boot correction; the corrected main driver
does not need it after the next boot. The bridge's handler address must come
from the currently loaded RTC module and must never be reused across boots.

Two awake alarm deliveries were confirmed, with GIC63 interrupt counts and
the driver's alarm_fires counter advancing. RTC wake from system suspend
and full SYS_SLEEP still require verification. Initial s2idle trials were
interrupted before the alarm by IRQ105 and the power-key IRQ; these trials
were correctly reported as failures. Power-key wake remains enabled.

The original prototype and temporary IRQ bridge remain loaded on the live
phone to provide the wake source. They were loaded from /tmp, with no new
persistent boot service. The RTC calendar is now valid and system UTC was
verified after correction. Sensor-hub services were not stopped or restarted.
