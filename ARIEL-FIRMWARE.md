# Ariel's pendant firmware (custom, installed over Bluetooth, no cable)

Fork of Omi's MIT CV1 firmware (see UPSTREAM.md).

## Why no cable is needed
The CV1 bootloader trusts MCUboot's public default signing key (its embedded public-key
hash `fc5701dc...` equals the hash of `omi/firmware/bootloader/mcuboot/root-rsa-2048.pem`,
the mcu-tools default). Any app image signed with that key installs over Bluetooth (SMP).
Our build of the unmodified upstream source reproduces Omi's pinned bootloader hash
`6cfd4a70...35cad1` byte for byte, so the toolchain is the production one.

## What changed (vs upstream 3.0.24)
| Behaviour | Where |
|---|---|
| 4 quick clicks = recording on, 5 = off (persisted mute), short haptic confirm | `ariel_ui.c`, `ariel_pendant.c`, `button.c` |
| Hold 10 s then release = 60 s pairing window (blue blink); hold 20 s = power off (3 s glow first); holds under 10 s do nothing (stock powered off at 3 s) | same |
| LED: 3 s white glow at power on/off, blue blink while pairing, 3 red flashes when pairing fails or times out, otherwise dark | `ariel_ui.c` policy, `ariel_pendant.c` driver |
| Bonded-only: every app GATT attribute needs an encrypted link; pairing accepted only in the window (or when no bond exists at all); a link that never encrypts is dropped after 20 s | `ariel_pendant.c`, perms in `transport.c`, `button.c`, `storage.c`, `accel.c`, `haptic.c` |
| Up to 2 bonds (phone + laptop); a third replaces the oldest | `omi.conf` |
| Firmware updates (SMP) only over an encrypted link | `omi.conf` `MCUMGR_TRANSPORT_BT_PERM_RW_ENCRYPT` |
| One time after install: old offline recordings on the SD ring are cleared | `ariel_pendant.c` first-boot wipe |
| Version 3.5.0 (above every official release, so downgrade prevention allows it) | `omi.conf` |

Kept untouched on purpose: bootloader, network core (radio) image, partitions, SAFE
(recovery) mode except one line (`bt_set_bondable(false)` so recovery pairing never stores a
bond). Existing phone bond is kept, so the phone stays paired across the install.

## Recovery without a cable
- Crash loop (3 failed boots) or a 10 s hold at power-on **while on the charger** starts
  SAFE mode: Bluetooth + firmware update only. Pair (no bond is stored) and upload any
  signed image, including official firmware re-signed at a higher version.
- Return to stock: re-sign Omi's official app image at a version above ours
  (MCUboot `imgtool` re-sign) and upload it.
- Not covered without a cable: a hang before SAFE can start, or a stock-bootloader unit
  whose update copy is corrupted by the SD/flash bus fault (shipped 3.0.21 units). Read the
  pendant's version first; a ~$5 Pi Pico programmer is the backstop.

## Not done yet
- Hardware-free Bluetooth test (BabbleSim / Bumble virtual pendant) and the audit rounds.
- Phone-app side: install flow (sync recordings, upload, re-check), new button codes
  6 = recording on, 7 = recording off, 8 = pairing window opened.
- Boot-region hash in diagnostics (tells us which bootloader the unit has).
- Pre-reset SD quieting before a DFU reboot (protects stock-bootloader units).

## Build
`scripts/build-custom.sh` (Docker, Omi's pinned CI image). Host tests:
`cc -std=c11 -Wall -Wextra -Werror -iquote omi/firmware/omi/src/lib/core omi/firmware/omi/src/lib/core/ariel_ui.c omi/firmware/omi/tests/host/ariel_ui_test.c -o /tmp/t && /tmp/t`
