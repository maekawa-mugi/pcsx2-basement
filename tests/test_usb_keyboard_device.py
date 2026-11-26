#!/usr/bin/env python3
"""Compile the production device factory and check guest USB identity/layout."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / 'pcsx2/USB/usb-hid/usb-hid.cpp').read_text()

def function(signature):
    start=text.index(signature)
    brace=text.index('{', start)
    depth=1
    end=brace+1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}')
        end += 1
    return text[start:end]+'\n'

code = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using u32=uint32_t;
#define WBVAL(x) ((x)&255), (((x)>>8)&255)
using USBDescStrings=const char*[16];
struct USBDescDevice { std::vector<uint8_t> device, config; };
struct USBDesc { USBDescDevice* full{}; const char* const* str{}; };
struct USBDevice {
 struct { void (*handle_attach)(); void (*handle_reset)(USBDevice*); void (*handle_control)();
 void (*handle_data)(); void (*unrealize)(USBDevice*); USBDesc* usb_desc{};
 const char* product_desc{}; } klass{};
 int speed{};
};
struct UsbHIDState {
 USBDevice dev{}; USBDesc desc{}; USBDescDevice desc_dev{};
 std::array<uint8_t,34> keyboard_config{};
 bool physical_keyboard=false; int* intr{}; int hid{};
 explicit UsbHIDState(u32) {}
 void SetKeycodeMapping() {}
};
static constexpr int USB_SPEED_FULL=1, USB_TOKEN_IN=1, HID_KEYBOARD=1;
static void usb_desc_attach() {}
static void usb_hid_handle_reset(USBDevice*) {}
static void usb_hid_handle_control() {}
static void usb_hid_handle_data() {}
static void usb_hid_changed() {}
static void usb_hid_unrealize(USBDevice* dev) { delete reinterpret_cast<UsbHIDState*>(dev); }
static int usb_desc_parse_dev(const uint8_t* p,size_t n,USBDesc&,USBDescDevice& d) {
 d.device.assign(p,p+n); return 0;
}
static int usb_desc_parse_config(const uint8_t* p,size_t n,USBDescDevice& d) {
 d.config.assign(p,p+n); return 0;
}
static void usb_desc_init(USBDevice*) {}
static void usb_ep_init(USBDevice*) {}
static int* usb_ep_get(USBDevice*,int,int) { return nullptr; }
static void hid_init(int*,int,void(*)()) {}
"""
for name in ('beatmania_dadada_desc_strings', 'pcsx2_keyboard_desc_strings',
             'pcsx2_keyboard_dev_desc', 'beatmania_dev_desc', 'beatmania_config_desc'):
    start=text.index('static const USBDescStrings '+name) if 'strings' in name else text.index('static const uint8_t '+name)
    end=text.index('};',start)+2
    code += text[start:end]+'\n'
code += function('static USBDevice* CreateKeyboardDevice(')
code += r"""
int main() {
 for(bool pcsx2: {false,true}) for(bool jis: {false,true}) {
  USBDevice* dev=CreateKeyboardDevice(0,pcsx2,jis);
  assert(dev);
  auto* s=reinterpret_cast<UsbHIDState*>(dev);
  auto& d=s->desc_dev.device;
  auto& c=s->desc_dev.config;
  assert(d.size()==18 && c.size()==34);
  unsigned vid=d[8]|(d[9]<<8), pid=d[10]|(d[11]<<8);
  assert(vid==(pcsx2?0:0x510) && pid==(pcsx2?1:2));
  assert(std::strcmp(dev->klass.product_desc,pcsx2?"PCSX2 Keyboard":"USB JIS Mini Keyboard")==0);
  assert(std::strcmp(s->desc.str[1],pcsx2?"PCSX2":"KONAMI CPJ1")==0);
  assert(c[22]==(pcsx2?(jis?15:33):15));
  assert(c[14]==3 && c[15]==1 && c[16]==1); // HID boot keyboard
  assert(c[29]==0x81 && c[31]==8 && c[33]==10);
#ifdef _WIN32
  assert(s->physical_keyboard==pcsx2);
#else
  assert(!s->physical_keyboard);
#endif
  dev->klass.unrealize(dev);
 }
 std::cout << "PCSX2/Konami identities, JIS/US descriptors and native-input isolation passed\n";
}
"""
with tempfile.TemporaryDirectory(prefix='pcsx2-keyboard-device-') as temp:
    source=Path(temp)/'test.cpp'
    binary=Path(temp)/'test'
    source.write_text(code)
    for flags in ([], ['-D_WIN32']):
        subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',
                        *flags,str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
