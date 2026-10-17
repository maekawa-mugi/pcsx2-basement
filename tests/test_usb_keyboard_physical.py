#!/usr/bin/env python3
"""Compile production keyboard queue/decoding and exercise physical JIS input."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
hid = (root / 'pcsx2/USB/qemu-usb/hid.cpp').read_text()
usb = (root / 'pcsx2/USB/usb-hid/usb-hid.cpp').read_text()

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'

code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <iterator>
#include "pcsx2/USB/qemu-usb/hid.h"
#include "pcsx2/USB/qemu-usb/input-keymap.h"
#include "pcsx2/USB/usb-hid/physical-keyboard.h"
#define HID_USAGE_ERROR_ROLLOVER 1
'''
start = hid.index('static const uint8_t hid_usage_keys[0x100]')
code += hid[start:hid.index('};', start)+2] + '\n'
code += function(hid, 'bool hid_has_events(')
code += function(hid, 'static void hid_keyboard_event(')
code += function(hid, 'static void hid_keyboard_process_keycode(')
code += function(hid, 'int hid_keyboard_poll(')
code += r'''
using u32 = std::uint32_t;
struct USBDevice {};
namespace usb_hid {
struct UsbHIDState {
    USBDevice dev;
    HIDState hid{};
    bool physical_keyboard=false;
    std::array<bool,256> physical_keys{};
    void QueueKeyboardState(KeyValue keycode, bool pressed) {
        InputEvent evt{};
        evt.u.key.key=keycode; evt.u.key.down=pressed;
        hid_keyboard_event(&hid, &evt);
    }
};
#define USB_CONTAINER_OF(p,t,m) reinterpret_cast<t*>(p)
class HIDKbdDevice {
public:
 void PhysicalKeyboardEvent(USBDevice*,u32,bool) const;
 void ClearPhysicalKeyboardState(USBDevice*) const;
};
'''
code += function(usb, 'void HIDKbdDevice::PhysicalKeyboardEvent(')
code += function(usb, 'void HIDKbdDevice::ClearPhysicalKeyboardState(')
code += r'''
}
static unsigned notifications;
static void notify(HIDState*) { notifications++; }
int main() {
 using namespace usb_hid;
 assert(NormalizeWindowsScanCode(3)==3);
 assert(NormalizeWindowsScanCode(0xe01d)==0x9d);
 assert(NormalizeWindowsScanCode(0x11d)==0x9d);
 assert(NormalizeWindowsScanCode(0xe11d)==0);
 assert(NormalizeWindowsScanCode(0xe02a)==0);
 assert(NormalizeWindowsScanCode(0)==0);
 assert(NormalizeWindowsScanCode(0x80)==0);
 UsbHIDState s{}; HIDKbdDevice proxy; s.hid.event=notify;
 auto key=[&](unsigned scan,bool down) {
   proxy.PhysicalKeyboardEvent(&s.dev,NormalizeWindowsScanCode(scan),down);
 };
 uint8_t report[8]{};
 auto poll=[&]() { assert(hid_has_events(&s.hid)); assert(hid_keyboard_poll(&s.hid,report,8)==8); };
 key(3,true); assert(!hid_has_events(&s.hid)); // opt-in only
 s.physical_keyboard=true;
 key(0x2a,true); poll(); assert(report[0]==2);
 key(3,true); poll(); assert(report[0]==2 && report[2]==31); // JIS Shift+2 = quote
 key(3,true); assert(!hid_has_events(&s.hid)); // duplicate/fake press
 key(3,false); poll(); assert(report[2]==0);
 key(0x2a,false); poll(); assert(report[0]==0);
 for(auto pair: {std::pair{0x70u,136u}, {0x73u,135u}, {0x79u,138u}, {0x7bu,139u}, {0x7du,137u}}) {
   key(pair.first,true); poll(); assert(report[2]==pair.second);
   key(pair.first,false); poll(); assert(report[2]==0);
 }
 key(0xe01d,true); poll(); poll(); assert(report[0]==0x10); // RCtrl uses E0 prefix
 key(0x1e,true); poll(); assert(report[2]==4);
 // Clear must produce a real release report even when the old queue is full.
 s.hid.n=QUEUE_LENGTH;
 proxy.ClearPhysicalKeyboardState(&s.dev);
 assert(hid_has_events(&s.hid)); poll();
 for(auto b:report) assert(b==0);
 assert(!hid_has_events(&s.hid));
 for(bool held:s.physical_keys) assert(!held);
 key(0x1e,true); poll(); assert(report[2]==4); // focus return accepts fresh press
 proxy.ClearPhysicalKeyboardState(&s.dev); poll();
 assert(notifications>0);
 std::cout << "Physical native scan/JIS/modifier/duplicate/focus-release queue checks passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='pcsx2-keyboard-') as temp:
    source=Path(temp)/'test.cpp'
    binary=Path(temp)/'test'
    source.write_text(code)
    subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root),str(source),
                    str(root/'pcsx2/USB/qemu-usb/input-keymap-qcode-to-qnum.cpp'),
                    '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
