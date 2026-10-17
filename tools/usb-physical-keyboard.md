# Physical keyboard input for the Konami USB keyboard

The existing HID Keyboard (Konami) device now has an opt-in setting:
**Physical Keyboard Input (Windows)**. Leave it disabled for the existing
logical Keyboard binding. Enable it for a guest OS that decodes physical
US/JIS key positions itself. Restart the VM after changing the option.

The setting is device-specific. For USB port 1 in PCSX2.ini:

```ini
[USB1]
Type = hidkbd
hidkbd_PhysicalKeyInput = true
```

This sends native Windows set-1 make positions through the existing QEMU
keyboard/HID report queue. It bypasses the logical Keyboard binding for this
device, including Qt's US-oriented Shift+symbol normalization. The device
identity and USB protocol remain Konami's. It does not claim or forward a
physical USB device through libusb/WinUSB. Other keyboard bindings/hotkeys
retain their existing behavior.

JIS Ro/Yen/Kana/Henkan/Muhenkan positions are mapped to the corresponding
USB HID usages. The guest still selects its layout: NetBSD PS2's native
console recognizes this Konami model as JIS. It implements the ASCII subset;
an IME is not supplied. Native E0 variants are normalized, fake extended
Shift is ignored, and E1/Pause is currently unsupported. Non-Windows
frontends keep the existing input path and do not expose this option.

Host autorepeat events are ignored; the guest handles repetition. Focus
loss and application deactivation clear held keys/modifiers, discard queued
presses, and enqueue a no-op so that USB polling receives a real all-released
report. Returning focus permits fresh presses. Loading a state clears the
host physical-key state in this mode. Deterministic agent sessions reject
physical input and focus-loss mutations, like the existing logical path.

Validation on 2026-10-17:

- Clang AVX2 build succeeded (the existing dual WinMain entry warning remains).
- `wsl.exe -d Ubuntu --exec python3 /mnt/c/Users/mugi/Desktop/pcsx2-basement/tests/test_usb_keyboard_physical.py`
  compiles the production HID queue, scan mapping and new physical-state
  methods and checks JIS keys, modifiers, duplicate presses and all-released
  delivery even with a full queue. It does not simulate Qt focus events.
- The user manually confirmed JIS punctuation in NetBSD, then confirmed
  repetition stops on leaving the PCSX2 window and ordinary input works
  after returning. Runs 41/42 in the NetBSD development captures retain
  EE SIO output; the SIO input capture is empty for these manual operations.
- No Computer Use automation was used. Other existing local PCSX2 changes
  were preserved. The physical input feature is committed independently.

The first focus-clear implementation emptied the HID queue without sending
an event. `hid_has_events()` then returned false and USB polling NAKed, so
NetBSD never saw key release. The final version deliberately leaves a queued
no-op until the all-released report has been polled. Root-hub hot-unplug and
IOP-to-EE interrupt forwarding are separate from host-window focus handling.

## Local AgentConsole integration

A separate follow-up commit adds deterministic host-input guards in USB.cpp.
It depends on the local AgentConsole interface (AgentConsole.h). The other
pre-existing AgentConsole implementation/build changes are outside these
keyboard commits. The running development build includes both the feature
and its integration guards, as used during validation.
