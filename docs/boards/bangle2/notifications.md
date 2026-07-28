# Phone notifications

This page tells how phone notifications reach the watch. It also tells why they
did not work at first, and how to correct the cause.

Status on 2026-07-28: notifications work. The watch shows a banner. The watch
shows the item in the Notifications list. The phone app is the official Core
Devices app. No change to firmware source was necessary.

The durable correction is complete. The watch has a real PRF image in the safe
firmware region. The watch reports a correct recovery firmware version. The
phone app classifies the watch correctly without help. The debug settings in
the phone app are no longer necessary. For the procedure and the results, see
"The durable correction".

## Summary of the cause

The phone app put no notification on the wire. The app received each
notification and stored it. The app then stopped.

The app classifies the watch as a device in recovery mode. For a device in
recovery mode, the app does not start BlobDB. BlobDB is the transport that
carries notifications. Therefore no notification can reach the watch.

The app makes this decision because the watch reports no recovery firmware
version.

## Evidence

Log line from the app at the time of a test:

```
WatchManager: ConnectedPebbleDeviceInRecovery: ... Pebble 0E74 ...
runningFwVersion=v4.30.0-76-g8526296e ... recoveryFwVersion=null,
platform=UNKNOWN ... isUnfaithful=true
```

Phone side, in `libpebble3` (repository `coredevices/mobileapp`):

- `TransportConnector.kt:228-243` sets `recoveryMode` to true when
  `!ignoreMissingPrfOnThisDevice && watchInfo.recoveryFwVersion == null`.
- `TransportConnector.kt:244-258` builds a `ConnectedInPrf` state. This state
  gives only the PRF services. The function returns before `blobDB.init(...)`.
- `SystemService.kt:282-286` returns null from `FirmwareVersion.from()` when
  the version tag does not agree with the expression at `SystemService.kt:237`:
  `v?N.N(.N)?(-suffix)?`.

Watch side:

- `src/fw/kernel/system_versions.c:100` sends the recovery firmware metadata.
  The function is `version_copy_recovery_fw_metadata`.
- `src/fw/system/version.c:91-95` reads the metadata from
  `FLASH_REGION_SAFE_FIRMWARE_BEGIN`. The parameter `check_crc` is true.
- The Bangle.js 2 has no PRF image in that region. Therefore the metadata is
  not valid, and the phone reads a null version.

## Temporary correction with a phone setting

Do not use this procedure for the Bangle.js 2 on this bench. The durable
correction is complete, and this procedure is no longer necessary. This record
stays for two reasons. Use it for a different watch that has no PRF image. Use
it also to understand the cause.

Do these steps on the phone. No change to firmware is necessary.

1. Open the Pebble app.
2. Go to the Settings tab.
3. Go to `Phone` > `Debug`.
4. Put on `Show debug options`. This item is a normal check box. No special
   action is necessary to show it.
5. Go to `Phone` > `Connectivity`.
6. Put on `Ignore Missing PRF`.
7. Disconnect the watch. Then connect the watch again.

The `Ignore Missing PRF` item is hidden until step 4. In the source, the item
has `isDebugSetting = true` at `WatchSettingsScreen.kt:887`. The filter is at
`WatchSettingsScreen.kt:1892`. The gate item is at
`WatchSettingsScreen.kt:1317-1327`, with the key `showDebugOptions` at
`WatchSettingsScreen.kt:2522-2524`.

### How to make sure the steps worked

After the reconnection, these things must be true:

- The Devices tab shows `Connected`. Before the change it showed
  `Connected (Factory)`.
- The log has no line `ConnectedPebbleDeviceInRecovery`.
- The log shows that BlobDB started: `BlobDB2Command$Version`, then
  `BlobDb version: 1`, then `MarkAllDirty`, then a group of inserts.
- A test notification gives these lines:

```
BlobDB: insert: Notification ... (f6006ad3-57c9-4a87-b0c9-9932eee53129)
PebbleProtocol: sending BlobCommand$InsertCommand
inbound ... BlobResponse$Success
BlobDB: insert: result = Success
```

To make a test notification, use this command:

```
adb -s <device> shell cmd notification post -S bigtext -t 'title' 'tag' 'body'
```

To watch the log, use this command:

```
adb -s <device> logcat -v time,threadtime
```

Useful words to find in the log: `NotificationHandler`, `BlobDB`,
`PebbleProtocol`, `WatchManager`, `ConnectedPebbleDeviceInRecovery`.

## Causes that were examined and refused

Do not spend time on these causes again. Each one has evidence against it.

- Per-application mute in the phone app. The app gives the message
  `NotSentAppMuted` only when the source application is muted. With all
  applications permitted, the app gives no such message and still sends
  nothing.
- Access for the notification listener. The listener is permitted:
  `coredevices.coreapp/io.rebble.libpebblecommon.notification.LibPebbleNotificationListener`.
- Suppression while the screen is on. The behaviour is the same when the
  screen is on and when the screen is off.
- The BLE link. At the time of each test the link was up and encrypted. The
  capabilities include `SupportsBlobDbVersion`.
- Refusal by the watch. Before the phone setting was changed, the phone sent
  nothing. Therefore the watch could refuse nothing.
- The platform identifier 22. The platform identifier and the recovery
  firmware version are independent fields of the same response. A build that
  reported the platform of another board corrected the cosmetic problems. It
  did not correct notifications.

## The durable correction

This procedure was done on 2026-07-28. It was successful. The results are at
the end of this section.

The phone setting is a correction for one device. To remove the need for the
setting, put a real PRF image in the safe firmware region.

`version_copy_recovery_fw_metadata` only reads the image and makes a check of
the CRC. It does not need an image that can start. Therefore a real image in
the region makes the recovery firmware version correct and true immediately,
before a boot loader exists.

Requirements for the region, from `src/fw/system/version.c:59-95`:

- At the start of the region, put a `FirmwareDescription` of 12 bytes. See
  `src/fw/system/firmware_storage.h:15-19`. The field `description_length`
  must be 12. The field `firmware_length` must agree with the number of bytes
  that come after. The field `checksum` must be the standard CRC32 of those
  bytes.
- Make the last 41 bytes of the image a `FirmwareMetadata`. See
  `include/pebbleos/firmware_metadata.h:69-87`.
- The field `version_tag` must agree with the expression in
  `SystemService.kt:237`.

Nothing makes a check of `hw_platform`, `is_recovery_firmware`, the time stamp,
or the ability of the image to start.

### How to put the image in the region

There is no serial link on the bench. Therefore `./pbl image_recovery --tty`
cannot be used. That tool needs a PULSE serial port. See
`tools/pulse_flash_imaging.py:34` and `:62`.

Use the phone instead. The watch does the work:

1. Build the PRF variant in a separate directory, so that the normal build
   stays as it is:

```
WAFLOCK=.lock-waf-prf ./waf configure --out=build-prf --board bangle2 --variant=prf --kconfig-override=CONFIG_BT_FW_NIMBLE=y
```

```
WAFLOCK=.lock-waf-prf ./waf build
```

```
WAFLOCK=.lock-waf-prf ./waf bundle
```

2. Move the recovery `.pbz` file to the phone.
3. In the Pebble app, use the debug screen for the sideload of firmware. See
   `DebugFirmwareSideload.kt`.
4. Make a soft reset of the watch with a long press of a button.

The watch then does these steps:

- `PutBytes` with `ObjectRecovery` puts the bytes in the scratch slot
  (`FIRMWARE_SLOT_1`, external NOR at `0x100000`). See
  `src/fw/services/put_bytes/put_bytes_storage_raw.c:28-41`.
- The COMMIT step writes the `FirmwareDescription` and the CRC. See
  `src/fw/services/put_bytes/put_bytes.c:453-467`.
- The INSTALL step sets `BOOT_BIT_NEW_PRF_AVAILABLE`. See
  `src/fw/services/put_bytes/put_bytes.c:508-513`.
- At the next boot, `check_prf_update` removes the protection from the safe
  firmware region, erases it, and copies the image. See
  `src/fw/main.c:364` and `src/fw/services/prf_update/service.c:70-81`.

`CONFIG_SERVICE_PRF_UPDATE=y` is in the build that is on the watch. Therefore
the watch has this path already.

The function `check_prf_update` is called at `src/fw/main.c:364`. No condition
controls this call. The function `prv_do_update` has only the condition
`#ifndef CONFIG_RECOVERY_FW`. The build on the watch is not a recovery build.
Therefore the copy path is in the build.

`CONFIG_PRF_UNAVAILABLE` is `y` for this board. The name of this symbol is a
trap. Read the help text with care. The symbol prevents a reset INTO the PRF.
The symbol does not prevent the receipt or the installation of a PRF image.
The only use of the symbol is at `src/fw/resource/system_resource.c:28`. There
is no use of the symbol in `prf_update`.

### Cautions for this procedure

- An update of the recovery firmware alone does not make the watch restart.
  Make the reset by hand.
- The boot bits are in RAM that keeps its contents through a soft reset. They
  do not keep their contents through a loss of power. Do not let the battery
  become empty between the transfer and the reset.
- The variant `prf` has no test in CI for this board. The workflow
  `build-prf.yml` includes only asterix, obelix, and getafix. The link step of
  2026-07-28 was the first test since the start of the port. It was successful.
- If the image is not correct, `prf_update` makes a record of a warning and
  does not copy. The recovery firmware version stays null. No damage occurs.
- The watch can stop the BLE link some seconds after the INSTALL step. The
  phone can then show the dialog `BluetoothKeyMissingDialog`, with the error
  `MtuGattError` or `HCI_ERR_KEY_MISSING`. Do not remove the device from the
  Bluetooth settings of the phone. Do not make a new pair. This condition is
  temporary. Make the reset of the watch by hand. The link then comes back, and
  the pair stays correct. A new pair is not necessary and wastes time.

### Results of the procedure on 2026-07-28

The build step was successful. The variant `prf` links for this board. The
image uses 451963 bytes of the 512 KiB region. This is 86.21 per cent. Thus
approximately 60 KiB stays free. The linker keeps the limit of the region. See
`wscript:846-848`. An image that is too large stops the build with an error.

The bundle is `recovery_bangle2_v4.30.0-78-gc6e8cde8.pbz`. The manifest gives
`type=recovery`, `hwrev=bangle2`, `size=451963`, `crc=3212142839`, and
`versionTag=v4.30.0-78-gc6e8cde8`.

The transfer was successful. The log of the phone app gives this sequence. Each
step has an acknowledgement:

```
WaitingToStart -> InProgress -> PutBytes -> PutBytesCommit -> PutBytesResponse
-> PutBytesInstall -> PutBytesResponse -> WaitingForReboot
```

The operator then made the reset by hand with a long press of the button. At
the next boot, `check_prf_update` copied the image into the safe firmware
region.

The watch now reports a correct recovery firmware version:

```
recoveryFwVersion=FirmwareVersion(stringVersion=v4.30.0-78-gc6e8cde8,
timestamp=2026-07-28T08:36:49Z, gitHash=c6e8cde, isRecovery=true)
```

The value `gitHash=c6e8cde` refers to the commit that was the tip of this branch
when the image was built. A later rebase of this branch changed the identifiers
of its commits. Thus this value does not agree with a commit that is in the
branch now. Do not try to find it.

The durable proof of the image is the archived bundle in
`~/Development/bangle2-swd-backups/`. Its SHA-256 is
`6e19d69ce4e452c64242d3dc1838288c8d7712d62fa49fc5290c5e20d6a95377`. This value
is identical to the image that the procedure installed. The archive also keeps
the Kconfig and the linker map of the same build.

The internal flash did not change. The value `runningFwVersion` stays
`v4.30.0-76-g8526296e`. The procedure writes only the external NOR flash.

### Test of the result without the phone setting

The setting `Ignore Missing PRF` was then put off. The watch was disconnected
and connected again. These things are true after this test:

- The log has no line `ConnectedPebbleDeviceInRecovery`.
- BlobDB started without help: `BlobDB2Command$Version`, then
  `BlobDb version: 1`, then `SyncDone`, then `WriteBack`.
- The Devices tab shows `Connected`.
- The field `isUnfaithful` is `false`.
- A notification from the phone reached the watch and the watch showed it. The
  log gives `insert: Notification`, then `BlobCommand$InsertCommand`, then
  `insert: result = Success`.

Therefore the correction is complete. The phone settings are not necessary.

Note for a test with `adb`: a notification from `adb` comes from the
application `com.android.shell`. If that application is muted, the app gives
`NotSentAppMuted` and sends nothing. This condition is not a fault. To test the
full path, use a notification from an application that is not muted.

## Why the firmware must not report false metadata

An alternative correction is to make `version_copy_recovery_fw_metadata` report
metadata that is valid but false. Do not do this.

The report would be false, because the board has no PRF image. A real image in
the region makes the same report true, and needs no change to source. A change
to source would also make a conflict with the work for the boot loader, which
writes the same function.

The danger of a false report is small, but it is not the reason to refuse it.
For the record, the phone takes no automatic action. In `libpebble3`, the field
`recoveryFwVersion` has only three users:

- `TransportConnector.kt:234`, for the decision about recovery mode.
- `WatchesScreen.kt:1553`, `:1643`, and `:1654`, for two manual items in a
  debug menu. Each item has a dialog for confirmation.

`FirmwareUpdateCheck.kt` and `web/FirmwareUpdateManager.kt` use only
`runningFwVersion`. The command `Reset into PRF` sets `BOOT_BIT_FORCE_PRF`. See
`src/fw/kernel/util/fw_reset.c:76-79`. On this board no code reads that bit.
See `boards/bangle2/Kconfig:37-44`. Therefore the command gives a normal
restart.

## Note about the platform identifier

This subject is separate from the recovery firmware version. Do not mix them.

The watch reports platform 22. This value is correct. In `libpebble3`, the
table `WatchHardwarePlatform.kt:17-70` has no entry for 22. Therefore the app
shows `UNKNOWN`, and the type of watch becomes a default value that is not
correct for this board. The check for a new firmware version also fails with
`UpdateCheckFailed(error=Unknown platform)`.

The correction is upstream, in `coredevices/mobileapp`: add an entry for
`BANGLE2(22)`.

One result of `UNKNOWN` is useful. The check for the hardware revision at
`FirmwareUpdater.kt:126` compares the platform of the watch with the platform
of the firmware. Both are `UNKNOWN`, so the check permits the sideload. The
check for the slot does not apply to recovery firmware. Therefore the procedure
above is possible today.

An entry for `BANGLE2(22)` stops this useful result. After the change, the
platform of the watch is 22 and the platform of the firmware stays `UNKNOWN`.
The two values do not agree, and the check refuses the sideload.

The sequence was therefore important. The sideload was done first, on
2026-07-28. Thus this hazard is now in the past for this watch. Two conditions
stay true for other work:

- For a watch that has no PRF image, do the sideload before the upstream
  change.
- If the upstream change comes first, the change must also give a correct
  result for the hardware revision `bangle2`. If it does not, the recovery path
  closes.
