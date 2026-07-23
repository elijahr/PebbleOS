# Bangle.js 2

This page tells you how to flash the relinked PebbleOS firmware onto a physical
Bangle.js 2 over Bluetooth, and how to recover the watch if a flash goes wrong.

The procedure uses only the built-in Bluetooth of the host computer. You do not
open the case. You do not use a hardware probe.

## Overview

The Bangle.js 2 (nRF52840) starts the PebbleOS firmware as a Nordic Secure DFU
application at flash base `0x26000`. This address is above the dormant stock
SoftDevice (S140 v6.1.1). The stock SoftDevice forwards the reset to the
application.

An application-only DFU package does not write the MBR, the SoftDevice, or the
bootloader. The bootloader stays available at all times. This is the recovery
lifeline: you can always re-enter DFU mode and put the stock firmware back.

```{note}
The DFU package is built in pure Python. No `nrfutil` (or `adafruit-nrfutil`)
is used or installed. The only Python packages needed are `protobuf` and
`intelhex`, which the PebbleOS virtual environment supplies.
```

## Flash the firmware

### Step 1 — Build the firmware

Configure and build the firmware:

```bash
./pbl configure --board bangle2
./pbl build
```

The application links at `0x26000`. The linker reserves the app FLASH
region to end at or below `0xF7000` (the stock bootloader start), enforced
by a static `ASSERT`; and the build fails if the firmware image does not
fit within that region. The build makes `build/pebbleos.hex`.

### Step 2 — Package the DFU zip

Package the application-only DFU zip from the built hex:

```bash
python3 tools/make_dfu_package.py build/pebbleos.hex pebbleos_banglejs2_app_dfu.zip
```

The tool reads a hex file and writes a zip file. The two arguments are optional:
the hex defaults to `build/pebbleos.hex` and the zip defaults to
`pebbleos_banglejs2_app_dfu.zip`. A third optional argument gives an alternative
golden init packet for the field check.

The zip is a Nordic Secure DFU application-only package. It contains three
members:

- `pebbleos_banglejs2_app.bin` — the contiguous application image.
- `pebbleos_banglejs2_app.dat` — the dfu-cc init packet.
- `manifest.json` — the package manifest.

The package is structurally matched to the stock Espruino 2v27 package. Before
the tool writes the zip, it checks the init packet against a vendored golden
packet (`tools/dfu/golden/espruino_2v27_banglejs2_app.dat`). If the hardware,
the SoftDevice requirement, or the image type is wrong, the tool rejects the
package here. It does not write a bad zip.

### Step 3 — Enter DFU mode on the watch

1. Press and hold BTN1 for about 10 seconds, until the screen goes blank.
2. Watch for the `====` countdown on the screen.
3. Release BTN1 during the `====` countdown.

The watch now advertises over Bluetooth as `DfuTarg`.

### Step 4 — Upload the zip

Use the Bangle App Loader on the host computer. The host uses its built-in
Bluetooth.

1. Open <https://banglejs.com/apps> in Chrome or Edge.
2. Go to **Firmware Update**.
3. Open the **Advanced** section.
4. Select your `pebbleos_banglejs2_app_dfu.zip` file.
5. Click **Upload**.

```{note}
The App Loader uses Web Bluetooth. Chrome and Edge support Web Bluetooth.
Safari does not.
```

## Recover the stock firmware

If a flash goes wrong, you can always put the stock firmware back. The
application-only package never touches the MBR, the SoftDevice, or the
bootloader, so the bootloader stays available.

```{important}
Save the stock package **before** you flash for the first time. Download
<https://www.espruino.com/binaries/espruino_2v27_banglejs2.zip> and keep it in a
safe place.
```

1. Enter DFU mode with the same BTN1 long-press (see Step 3 above).
2. Open <https://banglejs.com/apps> in Chrome or Edge.
3. Go to **Firmware Update**, then the **Advanced** section.
4. Select the stock `espruino_2v27_banglejs2.zip` file.
5. Click **Upload**.

The stock package uses the same application version (`0xff`). The bootloader
version test is "greater than or equal to", so it always accepts the stock
package. A stock restore is always possible.

## Safety and scope

- The application base is `0x26000`, above the dormant stock S140 v6.1.1
  SoftDevice. The SoftDevice forwards the reset to the application.
- A structurally wrong or hash-mismatched package is rejected by the bootloader
  before it commits. Such a package cannot brick the watch.
- The packaging tool also checks the init-packet fields and the image hash
  before it writes the zip, so a bad package does not leave your computer.

```{warning}
Some behavior is confirmed only on hardware, not in the emulator:

- **On-device DFU acceptance.** The first hardware flash is the acceptance
  confirmation.
- **The Bluetooth radio.** This build ships with Bluetooth stubbed
  (`CONFIG_BT_FW_STUB`). The DFU transport is the stock bootloader's own
  Bluetooth, not the firmware's.
- **The watchdog timing margin.** The 5-second margin is confirmed on the first
  hardware flash.

The Renode emulator (`harness_check.py`) is the primary verification. The first
hardware flash confirms the parts that the emulator cannot prove.
```

## Optional — SWD visibility for debugging

You do not need a hardware probe to flash the watch. For debugging, a low-cost
SWD probe on the charge-cable SWD pins gives GDB and RTT access. Use a
Raspberry Pi as an OpenOCD probe, or a CMSIS-DAP or ST-Link clone. The emulator
stays the primary verification tool.
