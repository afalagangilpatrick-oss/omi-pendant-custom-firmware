# Custom Omi CV1 pendant firmware (3.5.0)

Custom firmware for the Omi CV1 wearable pendant (nRF5340, Zephyr / nRF Connect SDK), built on
Omi's open-source MIT firmware. It installs over Bluetooth with no cable and changes how the
pendant is controlled and who can talk to it:

- **Gestures:** 4 quick clicks = recording on, 5 = off; hold 10 s = 60 s pairing window; hold 20 s = power off.
- **LED rules:** short glow at power on/off, blue blink while pairing, red flashes on pairing failure, dark otherwise.
- **Bonded-only Bluetooth:** every app attribute needs an encrypted link; pairing only inside the window;
  firmware updates only over an encrypted link; links that never encrypt are dropped.
- **First-boot wipe** of old offline recordings, and a SAFE recovery mode that needs no cable.

The gesture state machine and LED policy (`omi/firmware/omi/src/lib/core/ariel_ui.c`) are pure C
with host unit tests (`omi/firmware/omi/tests/host/ariel_ui_test.c`):

```bash
cc -std=c11 -Wall -Wextra -Werror -iquote omi/firmware/omi/src/lib/core \
  omi/firmware/omi/src/lib/core/ariel_ui.c omi/firmware/omi/tests/host/ariel_ui_test.c -o /tmp/t && /tmp/t
```

Design notes, changed files and recovery: [ARIEL-FIRMWARE.md](ARIEL-FIRMWARE.md).
Build: `scripts/build-custom.sh` (Omi's pinned Zephyr CI Docker image).

Upstream: [BasedHardware/omi](https://github.com/BasedHardware/omi) `omi/firmware`, MIT (see
[LICENSE-omi](LICENSE-omi) and [UPSTREAM.md](UPSTREAM.md)). Unofficial; flash at your own risk.
