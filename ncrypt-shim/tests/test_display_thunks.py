#!/usr/bin/env python3
"""Execute the compiled x86 display patches, including their C helpers.

Requires Python packages `unicorn` and `pefile`. No Wine session or login needed.
Usage: python test_display_thunks.py --dll /path/to/ncrypt32.dll --game /path/to/BinaryTrU.exe
Only the imported Win32 APIs are mocked; the installer and patched instructions
run from the actual PE images. In particular, this catches incorrectly encoded
relative jumps, custom calling convention violations, and stack drift.
"""

import argparse
import struct
import unittest
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EIP, UC_X86_REG_FPCW,
)

ARGS = None
DATA = 0x20000000
STACK = 0x30100000 - 0x100
STOP = 0x70010000
APP, HOLDER, OPTION, WINDOW = (DATA + n for n in (0, 0x100, 0x200, 0x300))
OUT_X, OUT_Y, EVENT = DATA + 0x500, DATA + 0x504, DATA + 0x600
REGS = (UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
        UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)


class Game:
    def __init__(self, corrupt=None):
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.apis = {}
        self.client_rect_ok = True
        self.client_width, self.client_height = 1920, 1080
        for path in (ARGS.game, ARGS.dll):
            pe = pefile.PE(str(path))
            base = pe.OPTIONAL_HEADER.ImageBase
            self.u.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
            self.u.mem_write(base, pe.get_memory_mapped_image())
            if path == ARGS.dll:
                self.dll_base = base
                self.entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
                self.imports = pe.DIRECTORY_ENTRY_IMPORT
        self.u.mem_map(DATA, 0x10000)
        self.u.mem_map(0x30000000, 0x100000)
        self.u.mem_map(0x70000000, 0x20000)
        for desc in self.imports:
            for imp in desc.imports:
                addr = 0x70000000 + len(self.apis) * 16
                self.apis[addr] = imp.name.decode() if imp.name else str(imp.ordinal)
                self.put(imp.address, addr)
        self.u.hook_add(UC_HOOK_CODE, self.api_call)
        self.u.reg_write(UC_X86_REG_FPCW, 0x37f)
        if corrupt is not None:
            self.u.mem_write(corrupt, b'\xcc')
        self.invoke(self.entry, (self.dll_base, 1, 0))
        self.put(0x012baa04, APP)
        self.put(APP, WINDOW)
        self.put(APP + 4, HOLDER)
        self.put(APP + 0x1c, OPTION)
        self.put(WINDOW + 4, 1)
        self.put(WINDOW + 0x88, 1920)
        self.put(WINDOW + 0x8c, 1080)
        self.put(WINDOW + 0xc8, 1440)
        self.put(WINDOW + 0xcc, 900)
        self.put(OPTION + 0x14, 2)
        self.dimensions(1280, 800)

    def put(self, addr, value):
        self.u.mem_write(addr, struct.pack('<I', value & 0xffffffff))

    def get(self, addr):
        return struct.unpack('<I', self.u.mem_read(addr, 4))[0]

    def floats(self, addr, *values):
        self.u.mem_write(addr, struct.pack('<' + 'f' * len(values), *values))

    def dimensions(self, width, height):
        self.put(HOLDER + 0x10, width)
        self.put(HOLDER + 0x14, height)
        self.put(OPTION + 8, width)
        self.put(OPTION + 12, height)

    def api_call(self, u, addr, size, _):
        if addr not in self.apis:
            return
        name = self.apis[addr]
        counts = {
            'GetModuleHandleW': 1, 'GetModuleFileNameW': 3,
            'GetCurrentProcessId': 0, 'GetCurrentThreadId': 0,
            'CreateFileA': 7, 'AddVectoredExceptionHandler': 2,
            'VirtualProtect': 4, 'FlushInstructionCache': 3,
            'GetCurrentProcess': 0, 'DisableThreadLibraryCalls': 1,
            'GetEnvironmentVariableW': 3, 'GetClientRect': 2,
            'lstrlenW': 1,
        }
        if name not in counts:
            raise AssertionError('Unexpected Win32 call: ' + name)
        sp = u.reg_read(UC_X86_REG_ESP)
        args = [self.get(sp + 4 + i * 4) for i in range(counts[name])]
        result = 1
        if name == 'GetModuleHandleW':
            result = 0  # Neither D3D module is loaded in this fixture.
        elif name == 'GetModuleFileNameW':
            text = 'C:\\Game\\BinaryTrU.exe'
            u.mem_write(args[1], (text + '\0').encode('utf-16-le'))
            result = len(text)
        elif name in ('GetCurrentProcessId', 'GetCurrentThreadId'):
            result = 42
        elif name in ('CreateFileA', 'GetCurrentProcess'):
            result = 0xffffffff  # Disable file logging in the emulator.
        elif name == 'GetEnvironmentVariableW':
            result = 0
        elif name == 'VirtualProtect':
            self.put(args[3], 0x20)
        elif name == 'GetClientRect':
            u.mem_write(args[1], struct.pack('<4i', 0, 0, self.client_width, self.client_height))
            result = int(self.client_rect_ok)
        elif name == 'lstrlenW':
            result = 0
            while u.mem_read(args[0] + result * 2, 2) != b'\0\0':
                result += 1
        # A legal stdcall implementation may destroy all these registers.
        u.reg_write(UC_X86_REG_ECX, 0xcccccccc)
        u.reg_write(UC_X86_REG_EDX, 0xdddddddd)
        u.reg_write(UC_X86_REG_EAX, result)
        u.reg_write(UC_X86_REG_EIP, self.get(sp))
        u.reg_write(UC_X86_REG_ESP, sp + 4 + counts[name] * 4)

    def invoke(self, addr, args=()):
        self.u.reg_write(UC_X86_REG_ESP, STACK)
        self.put(STACK, STOP)
        for i, arg in enumerate(args):
            self.put(STACK + 4 + i * 4, arg)
        self.u.emu_start(addr, STOP, count=100000)

    def registers(self):
        return [self.u.reg_read(r) for r in REGS]

    def mouse(self, x, y):
        for r, value in zip(REGS, (0xbabababa, WINDOW, OUT_X, OUT_Y, 0xdadadada, 0xebebebeb)):
            self.u.reg_write(r, value)
        before = self.registers()
        self.invoke(0x00d36ae0, (x, y))
        return before, struct.unpack('<2f', self.u.mem_read(OUT_X, 8))

    def wheel(self, x, y, delta=-1.0):
        self.floats(EVENT + 8, x, y, delta)
        for r, value in zip(REGS, (WINDOW, DATA + 0x800, EVENT, 0xbebebebe, 0xdadadada, 0xebebebeb)):
            self.u.reg_write(r, value)
        before = self.registers()
        self.u.reg_write(UC_X86_REG_ESP, STACK)
        self.put(STACK, 0xfeedface)
        self.u.emu_start(0x00d38be5, 0x00d38b09, count=100000)
        return before, struct.unpack('<3f', self.u.mem_read(EVENT + 8, 12))


class DisplayThunks(unittest.TestCase):
    def test_mouse_custom_abi_and_signed_coordinates(self):
        g = Game()
        before, (x, y) = g.mouse(-120, 540)
        self.assertEqual(g.registers(), before)
        self.assertEqual(g.u.reg_read(UC_X86_REG_ESP), STACK + 12)  # return + ret 8
        self.assertAlmostEqual(x, -80)
        self.assertAlmostEqual(y, 400)

    def test_wheel_branch_and_custom_abi(self):
        g = Game()
        before, (x, y, delta) = g.wheel(960, 540)
        self.assertEqual(g.u.reg_read(UC_X86_REG_EIP), 0x00d38b09)
        self.assertEqual(g.registers(), before)
        self.assertEqual(g.u.reg_read(UC_X86_REG_ESP), STACK)
        self.assertEqual(g.get(STACK), 0xfeedface)
        self.assertAlmostEqual(x, 640)
        self.assertAlmostEqual(y, 400)
        self.assertEqual(delta, -1)

    def test_repeated_resolution_mouse_and_wheel_changes(self):
        g = Game()
        for i in range(1000):
            width, height = ((1280, 800), (1440, 900), (1920, 1080))[i % 3]
            g.dimensions(width, height)
            before, (x, y) = g.mouse(960, 540)
            self.assertEqual(g.registers(), before)
            self.assertAlmostEqual(x, width / 2)
            self.assertAlmostEqual(y, height / 2)
            before, (x, y, delta) = g.wheel(960, 540)
            self.assertEqual(g.registers(), before)
            self.assertAlmostEqual(x, width / 2)
            self.assertAlmostEqual(y, height / 2)

    def test_borderless_client_rect_fallback(self):
        g = Game()
        g.client_rect_ok = False
        _, (x, y) = g.mouse(960, 540)
        self.assertAlmostEqual(x, 640)
        self.assertAlmostEqual(y, 400)

    def test_windowed_client_rect_fallback(self):
        g = Game()
        g.client_rect_ok = False
        g.put(OPTION + 0x14, 0)
        _, (x, y) = g.mouse(720, 450)
        self.assertAlmostEqual(x, 640)
        self.assertAlmostEqual(y, 400)

    def test_does_not_overwrite_modified_wheel_site(self):
        g = Game(corrupt=0x00d38be5)
        self.assertEqual(bytes(g.u.mem_read(0x00d36ae0, 4)), b'\x55\x8b\xec\x51')
        self.assertEqual(bytes(g.u.mem_read(0x00d36b00, 4)), b'\x1a\xdb\x45\x0c')
        self.assertEqual(bytes(g.u.mem_read(0x00d38be5, 1)), b'\xcc')

    def test_does_not_overwrite_modified_cave(self):
        g = Game(corrupt=0x00d36b00)
        self.assertEqual(bytes(g.u.mem_read(0x00d36ae0, 4)), b'\x55\x8b\xec\x51')
        self.assertEqual(bytes(g.u.mem_read(0x00d38be5, 5)), b'\xe9\x1f\xff\xff\xff')
        self.assertEqual(bytes(g.u.mem_read(0x00d36b00, 1)), b'\xcc')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', required=True, type=Path)
    parser.add_argument('--game', required=True, type=Path)
    ARGS, rest = parser.parse_known_args()
    unittest.main(argv=[__file__] + rest)
