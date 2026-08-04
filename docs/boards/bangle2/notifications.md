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

  Use this one only as an indication, NOT as proof. The state that the tab
  shows can stay behind the true state: on 2026-08-03 the tab gave a button
  `Disconnect`, which means a connection, while the Bluetooth system of the
  phone gave `STATE_DISCONNECTED` for every client and no watch held a
  connection at all. To know the true state, ask the system of the phone:

  ```
  adb -s <device> shell dumpsys bluetooth_manager | grep "mConnectionState"
  ```
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

Step 4 is necessary. Do not omit it. The copy into the safe firmware region
occurs only at boot, in `check_prf_update`. Until the watch starts again, the
image stays in the scratch slot and the recovery firmware version stays null.

A recovery-only installation also leaves the watch in the firmware-update run
level, because that path has no reboot. The update service sets
`RunLevel_FirmwareUpdate` at `src/fw/services/firmware_update/service.c:179`.
It puts the level back to `RunLevel_Normal` at `:218` only when the update
FAILS. The comment there says "If we succeeded, we'll reboot shortly." For a
recovery object there is no reboot: `ObjectRecovery` sets
`BOOT_BIT_NEW_PRF_AVAILABLE` and falls through
(`src/fw/services/put_bytes/put_bytes.c:512-516`).

On this board that state has no effect that a user can see. In the table at
`src/fw/services/services_common/service.c`, only `hrm_manager_enable` has the
mask `R_Normal` in a form that the build compiles. Bluetooth, the accelerometer,
the backlight and the vibration motor all include `R_FirmwareUpdate` in their
masks and stay on.

The touch screen is NOT affected. Its entry is inside
`#if defined(CONFIG_TOUCH) && defined(CONFIG_RECOVERY_FW)`, and
`CONFIG_RECOVERY_FW` is absent from the normal build (see `build/autoconf.h`).
Thus the entry is not in the image, the run level does not manage the touch
screen, and the touch screen continues to work. The heart-rate monitor stops,
but this port computes no rate, so nothing changes for the user.

(Run-level behaviour found by pebble-ble-2, who also corrected an earlier
statement that the touch screen stops. Read the guards around a table, not only
the rows.)

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

- DANGER IF YOU CHANGE THE IMAGE. This procedure sends a recovery image. A
  recovery bundle has no block of resources, so the application sends the
  firmware only. IF YOU USE THE SAME PROCEDURE WITH A NORMAL FIRMWARE BUNDLE,
  THE APPLICATION ALSO SENDS THE RESOURCES, and that operation can destroy the
  only copy of the pack of resources.

  The reason: the installation of resources writes to the bank that is not in
  use. When the watch has a valid pack, that bank is the other one, and the
  choice is safe. When the watch has NO valid pack, the choice becomes random
  (`resource_storage_flash.c:78` uses `rand()`), so there is one chance in two
  that it writes over the good bank. A watch that has a new firmware and an old
  pack has no valid pack. The mono pack at `0x200000` is the only copy, the
  external flash has no backup that anybody can make again, and the panic
  screen does not stop this: Bluetooth and the transfer of bytes continue to
  operate.

  Therefore, if you must change the platform: SEND THE RESOURCES FIRST, while
  the old firmware still starts, THEN write the new firmware. Never the
  opposite order. (Analysis from pebble-color.)

- Do not trust the screen of the application to tell you that a transfer
  failed. A refusal by the safety check goes to the state `Idle` with no
  message on the screen (`FirmwareUpdater.kt:345-347`), and nothing in the
  application reads the record of the failure. `Idle` with no error DOES NOT
  mean that the transfer was successful. The signal that has meaning is the
  progress that goes past 50 per cent, which is the point where the firmware
  ends and the resources begin, and then the message `Waiting for reboot`. Use
  the log for anything else.

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

  There are two different events here. Keep them apart. The first has an
  explanation. The second does not.

  The disconnection is not a fault. The phone log gives `status=19`, which is
  the HCI reason `0x13`, "Remote User Terminated Connection". The watch sent
  that message on purpose. To send it, the host stack must still operate. A
  watch that stops because of an assert cannot send it. An assert gives a
  supervision timeout, which is `status=8`. The two values show different
  conditions, and they almost exclude each other. The order in the log agrees:
  the message "Has no more bearers and is disconnected" comes before the
  status. That is the order of a disconnection that the watch controls, not of
  a link that stopped and was found later. The probable cause is the restart
  that applies the new image. The record of 2026-07-28 gives 3.06 seconds
  between the acknowledgement of INSTALL and the disconnection. Thus this part
  of the sequence is not a defect. It is the installation that completes.

  The failure to connect again IS an open problem. `HCI_ERR_KEY_MISSING` shows
  that the phone gave a pair that the watch could not use at that moment. This
  is a problem of the storage of the pair through a restart. It is not a
  problem of advertisement. Only the manual reset corrects it. The pair is
  correct after the reset.

  If this occurs again, get this evidence. A reset destroys it.

  - Read `reboot_reason` at `0x20000014` over SWD.
  - Read `s_last_reboot_reason_code` at `0x20016553` over SWD.
  - Record if the database of pairs kept its contents through the restart.
    This is the item that has the most importance. The two words above show
    why the watch started again. They do not show why the pair became
    unusable.

  A value of `0x11` in the second word is an assert. Any other value shows a
  restart that the watch made on purpose. Nobody read these words on
  2026-07-28.

  The comparison of `status=19` with `status=8` is from pebble-ble-2. This
  record was offered to them as possible evidence of a different defect, in
  the stop of advertisement, which they had found. They refused the relation,
  and the reason above is theirs. That defect ends in an assert, thus in
  `status=8`, thus it is not the cause here. Their confidence in the refusal
  is high. Their confidence in the restart that applies the image is medium,
  because it is only the most simple explanation and nobody read the words.

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

### A report that is already not true: dictation

The rule above is about a change that was refused. This is a report that the
firmware already makes and that is not true.

At connection the watch sends a field of capabilities. The function
`prv_send_watch_versions` sets bit 7, `voice_api_support`, at
`src/fw/kernel/system_versions.c:122`. The board has no microphone.

Do not read the missing guard as an error of the person who wrote the line. The
function has a rule, and bit 7 obeys it. Every bit for a capability is set
without a condition (:115-123). Only the bits that report the presence of an
application have guards: `APP_ID_SEND_TEXT` at :125-127, `APP_ID_WEATHER` at
:129-131, `APP_ID_REMINDERS` at :132-135, and `APP_ID_WORKOUT` at :136-138.

The fault is in the rule. The rule assumes that every board has the hardware for
every capability. That was true for the boards that existed when the function
was written. It is not true for this board. Thus the correction is a new guard,
not the repair of a guard that somebody forgot.

`CONFIG_MIC` is not in `build/autoconf.h` and not in `boards/bangle2/defconfig`.
Thus the firmware cannot do what the bit says:

- `applib/voice/dictation_session.c:17` puts the implementation inside
  `#ifdef CONFIG_MIC`. The alternative code gives
  `DictationSessionStatusFailureInternalError`.
- `applib/wscript_build:45-46` does not compile `voice/voice_window.c`.
- `services/voice/Kconfig:4-7` makes `SERVICE_VOICE` default to yes only with
  `MIC`, so `services/wscript_build:137-138` does not use the directory
  `voice/`. The directory `voice_endpoint/` is in the image, but its only user
  is in `voice/`. It is dead code.
- `timeline_actions.c:905-912` refuses the option `ReplyOption_Voice`, and the
  menu entry at :943-951 is not in the image.

The phone reads the bit and shows a part for speech recognition in the sequence
for a new watch: `composeApp` `WatchOnboardingScreen.kt:310`. Thus the user sees
a function that the hardware cannot do.

The correction is probably a guard on `CONFIG_MIC` at `system_versions.c:122`.
That file is in the kernel and other boards use it. Do not make that change as
part of the work for notifications. It is recorded here because the examination
of the notifications found it.

Bit 9, `notification_filtering_support`, is NOT the same and is NOT a false
report. The firmware has the function:
`services/notifications/ancs/ancs_filtering.c` is in the image. The function
operates only for notifications from ANCS, which is the protocol of iOS. An
Android phone does not send them, and the phone application does not send the
preferences to the watch for Android
(`LibPebbleModule.android.kt:81` gives `syncNotificationApps = false`).
Therefore the report is true and the function is only not reachable with an
Android phone. Do not "correct" this one.

## Note about the platform identifier

This subject is separate from the recovery firmware version. Do not mix them.

The watch reports platform 22. This value is correct. In `libpebble3`, the
table `WatchHardwarePlatform.kt:17-70` has no entry for 22. Therefore the app
shows `UNKNOWN`, and the type of watch becomes a default value that is not
correct for this board. The check for a new firmware version also fails with
`UpdateCheckFailed(error=Unknown platform)`.

The correction is an entry for `BANGLE2(22)` in the table. Make it on the
operator's copy of the application. No work goes from this project to the
repository of the manufacturer. If the operator sends it there, the operator
does that separately.

One result of `UNKNOWN` is useful. The check for the hardware revision at
`FirmwareUpdater.kt:125-126` compares the platform of the watch with the
platform of the firmware. Before the entry, both values are `UNKNOWN` and they
agree, so the check permits the sideload. The check for the slot does not apply
to recovery firmware.

An earlier version of this page said that the entry stops this useful result.
That is NOT correct, and the reason has importance for anybody who writes such
an entry.

Both values come from the same table. The platform of the watch comes from
`fromProtocolNumber(22)`, which compares the field `protocolNumber`. The
platform of the firmware comes from `fromHWRevision`, which compares the field
`revision` with the field `hwrev` in the manifest of the image. Both archived
images give `hwrev=bangle2`. Therefore an entry that has BOTH the number 22 and
the revision string `bangle2` moves the two values together, from `UNKNOWN` to
`BANGLE2`. They still agree, and the check still permits the sideload.

The entry that is on the operator's copy has both:
`BANGLE2(22u, WatchType.FLINT, "bangle2")`.

The danger is real, but it is conditional. An entry that gives the number and
NOT the revision string moves only one of the two values. The values then
disagree, and the check refuses the sideload. Therefore:

- Give the revision string. It is not optional.
- The revision string must be the same as the field `hwrev` in the manifest of
  the image. For this board that is `bangle2`.
- With a correct entry the sequence of the work does not matter. If you are not
  sure that the entry is correct, do the sideload first.
