# Exynos9810 Wi-Fi resume

AP sleep loses the host's ELBI interrupt enables, LTSSM enable and iATU
mapping state. The old host driver had no system PM callbacks. Consequently
PCI children resumed against an uninitialized link, the endpoint was marked
D3cold/inaccessible, and brcmfmac reprobe returned all-ones MMIO and EAGAIN.

`zrn_pcie_recovery.ko` is an explicit diagnostic for the old running kernel.
It requires this board, an unbound BCM4361, the expected host driver and
valid cached PCI configuration/BARs. Default loading only validates.
`recover=1` masks the parent IRQ, stops the link, invokes the existing DWC
resume sequence (host/PHY init, RC/iATU setup and link training), then
restores saved PCI configuration and endpoint D0. It installs no PM hooks.

Live recovery restored the link and Wi-Fi reconnected without rebooting.
The AP-sleep test `ztos-wifi-resume-test.service`, using the optional
`suspend-test.py --pcie-recovery-module /tmp/zrn_pcie_recovery.ko`, exited
successfully. Wi-Fi reconnected and three packets over wlp1s0 reached the
router without loss; sensorhub ready remained 1 and restore_error was 0.
Evidence: `build/power-analysis/wifi-resume-recovered.log` and the original
register snapshot `pcie-after-sleep.json`. Full MIF shutdown was disabled.

The permanent fix in `third_party/s9plus/src/pci/pcie-exynos9810.c` adds
noirq system PM callbacks: suspend masks interrupts and stops the link;
resume rebuilds the host and mappings before PCI children resume. This
is being validated on hardware. The first noirq-only build (#25) timed out
training the link: Samsung pinctrl restored the WLAN GPIOs in resume_early,
after PCI needed them. `kernel/patch-pinctrl-resume.py` and the generated
WLAN DTS opt FSYS1/PERIC0 into resume_noirq; the host also reapplies
WL_REG_ON with its boot-time 200 ms settling delay while PERST is held.
Kernel #27 contains both corrections. The first native AP-sleep test
(`ztos-wifi-native-resume-test-2.service`) passed with no recovery module:
PCIe Gen1 x1 retrained, brcmfmac rebound, NetworkManager reconnected, and
three router pings over wlp1s0 passed without loss. Sensorhub ready stayed
1, hardware sleep entries advanced to 1 and last_restore_error was 0.
A second consecutive cycle (`ztos-wifi-native-resume-test-3.service`) also
passed, advancing entries to 2 with restore_error 0 and hub ready 1.
Three more router pings passed without loss. Evidence:
`build/power-analysis/wifi-native-resume-ordered.log` and
`build/power-analysis/wifi-native-resume-repeat.log`.
The BCM4361 firmware D3 acknowledgment timeout is a separate issue; the
current suspend diagnostic still detaches brcmfmac before sleeping.
