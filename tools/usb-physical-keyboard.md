# PCSX2 Keyboard

Select **PCSX2 Keyboard** as the USB device for a guest operating system.
It is independent of HID Keyboard (Konami); the Konami device keeps its
original logical Keyboard binding and no longer exposes Physical Key Input.

The new device reports manufacturer `PCSX2` and product `PCSX2 Keyboard` to
the guest. Its development VID/PID is 0000:0001, without borrowing Konami's
commercial identity or claiming a registered vendor assignment.

For the current NetBSD development profile:

```ini
[USB1]
Type = pcsx2kbd
pcsx2kbd_JISLayout = true
```

On Windows, native set-1 physical positions are sent automatically, bypassing
Qt's US-oriented Shift+symbol normalization. There is no logical Keyboard
binding for this device on Windows. This does not forward a physical USB
device through libusb/WinUSB. Other frontend bindings/hotkeys retain their
existing behavior. Non-Windows frontends use the logical Keyboard binding
as a fallback; native physical mapping there remains future work.

The Japanese Keyboard (JIS) setting defaults on and reports HID country 15.
Disable it for US country 33; restart the VM after changing the setting.
The guest still chooses its layout. NetBSD's console detects the country
without a product-specific quirk and supports the ASCII subset, without IME.
JIS Ro/Yen/Kana/Henkan/Muhenkan positions use their corresponding HID usages.
E0 variants are normalized, fake extended Shift is ignored, and E1/Pause
is currently unsupported.

Host autorepeat events are ignored; the guest handles repetition. Focus
loss and application deactivation clear held keys/modifiers, discard queued
presses, and enqueue a no-op so that USB polling receives a real all-released
report. Returning focus permits fresh presses. Loading a state clears the
host physical-key state in this mode. Deterministic agent sessions reject
physical input and focus-loss mutations, like the existing logical path.

Validation of the shared input path on 2026-10-17 (then exposed as a
Konami option):

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
AgentConsole implementation/build changes are committed separately from the
keyboard feature. The running development build includes both the feature
and its integration guards, as used during validation.

## Dedicated device validation, 2026-10-18

- Clang AVX2 rebuild passed; the existing dual WinMain linker warning remains.
- `tests/test_usb_keyboard_device.py` compiles the production factory for
  Windows and non-Windows modes and verifies guest identity, HID boot protocol,
  endpoint size, JIS/US country codes, and isolation from the Konami device.
- `tests/test_usb_keyboard_physical.py` still passes native scan/JIS/modifier,
  duplicate press, and focus-release queue/report checks.
- NetBSD run52 enumerates `PCSX2 (0x0000) PCSX2 Keyboard (0x0001)` and attaches
  its JIS console keyboard before reaching the RAM-root shell. No Computer
  Use or synthetic keyboard input was used. Earlier manual focus/typing
  confirmations cover the unchanged shared input delivery path; this run
  confirms the new device's enumeration rather than another manual typing test.
