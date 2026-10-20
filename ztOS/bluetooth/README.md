# Bluetooth on the Galaxy S9: working (2026-10-18)

Up at boot: `zrn-bluetooth.service` loads `driver/zrn-bt.ko` and runs
`btattach` (Broadcom protocol); `zrn-bluetooth-addr.service` gives the
controller the phone's own address from EFS (`50:77:05:8E:75:B9` here,
cached in `/var/lib/zrn-bluetooth`) before bluetoothd starts. bluetoothd shows
it as "ztOS Bluetooth Device". The firmware patch is
`brcm/BCM.samsung,starlte.hcd` (= `third_party/s9plus/firmware/bcm4361B2_semco.hcd`);
the chip reports BCM4361B2 "Crown E32A ANT1", version 0110.0232. Scanning
finds devices; ztMETRO's Bluetooth quick action switches it.

What it took, beyond the plan below:

* **The UART is behind a USI v2 wrapper** (stock: `samsung,usi-serial-v2`),
  held in reset: every register write was dropped until `USI_CON` (+0xc4) is
  cleared, with `USI_OPTION` (+0xc8) = 2 (CLKREQ_ON) first.
* **Its clock is 200 MHz with the PERIC1 divider at 1:1** (SHARED2 800 MHz,
  /2 in CMU_TOP's mux, /2 in its divider). Not 1:2, as Samsung's LUT
  parameter suggested.
* **btbcm needs the transmit power quirk** (`patch-btbcm.py`): the firmware
  answers LE Read Transmit Power with one byte and setup failed with -38.
  Upstream sets the quirk from DMI only.
* **Nothing may talk to the controller during its setup** (about 11 s, the
  firmware download): management requests then leave it unregistered for
  good. `zrn-bluetooth-addr` waits for setup's last kernel log line, then
  for the index, before it sets the address.
* Re-registering the UART in the same boot (rmmod/insmod zrn-bt) does not
  work: samsung_tty keeps the first port. Reboot instead.
* The `exynos850-uart` variant miscounts the receive FIFO in loopback (each
  byte repeated); HCI traffic is fine. To look at if it ever matters.

Tools used to find this, in the scratchpad of the session: raw register
pokes over /dev/mem, a loopback test, and reading the UART pins as GPIO
inputs with pull-downs (the chip drives its TX high once powered).

## The original plan (2026-10-18, morning)

The BCM4361 is one chip for Wi-Fi and Bluetooth. Wi-Fi works over PCIe
(brcmfmac); Bluetooth is its second function, on a UART, and has nothing
under it yet. Everything it needs is known; what is left needs someone at
the phone, because two of the steps write to clock and UART registers, and a
mistake there on this SoC is a silent bus hang that only the keys undo.

## The hardware (stock tree, `stock/twrp-live.dts` 13798, 9385, 6679)

| Piece | Where |
| --- | --- |
| UART | `UART_BT` at `0x10840000` ("uart1"), a dedicated UART in PERIC1, not a USI; 4-wire (`gpq0-0..3`, function 2, RTS/CTS), FIFO 256 |
| Its clocks | CMU_PERIC1 `0x10800000`: gates `+0x20c4` (IPCLK), `+0x20c8` (PCLK), `+0x201c` (RSTnSYNC), `+0x2034` (GATE_CLK_PERIC1_UART_BT); divider `+0x1814`; Q-channel `+0x3028`. 200 MHz (Samsung's LUT) |
| BT_EN | pin 7 of the PCAL6524 expander, I2C `0x22` on hsi2c@10930000; bit-banged here on `gpp6-2` SCL, `gpp6-3` SDA. Expander reset: `gpg3-6` |
| BT_WAKE | `gpg2-0`, output |
| HOST_WAKE | `gpa2-3`, interrupt |
| Firmware | `third_party/s9plus/firmware/bcm4361B2_semco.hcd` |

Read on the phone with `probe/zrn_btprobe.ko` (read-only; it unloads itself):
the expander answers; BT_EN is an output, driven low (Bluetooth off).

## Kernel side

The running kernel has no Bluetooth (`CONFIG_BT` unset). `build/linux-7.1/out-bt`
is its configuration with only these added, as modules or inside them:
`BT`, `BT_HCIUART` (+ `SERDEV`, `H4`, `BCM`), `BT_BCM`, `BT_RFCOMM`, `BT_HIDP`;
the crypto it needs (`ECDH`, `AES`, `CMAC`) is already modular. Built-ins are
identical to `out`, so the modules load on the flashed kernel.

## The bring-up, in order (with someone at the phone)

1. Open the four PERIC1 gates above (as the `s9p,clock-gates` node does for
   the touch bus). Only then is `0x10840000` safe to read.
2. A device-tree overlay (as zrn-modem applies its own): a
   `samsung,exynos850-uart`-style node at `0x10840000` with a 200 MHz
   fixed clock for both `uart` and `clk_uart_baud0`, `samsung,uart-fifosize
   = <256>`, the `gpq0` pins in function 2, and a serdev child
   `bluetooth { compatible = "brcm,bcm4345c5"; }` (hci_bcm has no 4361
   entry; any BCM43xx one drives it) with `shutdown-gpios` = expander pin 7,
   `device-wakeup-gpios` = gpg2-0, `host-wakeup-gpios` = gpa2-3, and
   `max-speed = <3000000>`.
3. The expander as a gpiochip: `pca953x` (mainline knows `nxp,pcal6524`) on
   the bit-banged bus, so `shutdown-gpios` can point at it.
4. Firmware where btbcm looks: `brcm/BCM4361B2.hcd` (and the board-specific
   name it logs on first load).
5. `bluetoothctl power on`; then a quick-settings tile in ztMETRO (it is
   there, drawn faint).
