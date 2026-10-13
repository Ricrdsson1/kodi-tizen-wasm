#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Makes the symbols a relocatable WebAssembly object defines local to it.

Binary add-ons that are linked into the executable each bring their own copy of
the add-on development kit and of whatever else they share by name, so everything
but an add-on's entry points has to stay private to it. llvm-objcopy cannot change
symbols in WebAssembly objects; this rewrites the "linking" section instead:
defined symbols become local, and COMDAT groups are dropped so that the linker
does not fold the copies of two add-ons into one.

Usage: localize_symbols.py <in.o> <out.o> [<symbol to keep global> ...]
"""

import sys

SECTION_CUSTOM = 0

SUBSECTION_COMDAT_INFO = 7
SUBSECTION_SYMBOL_TABLE = 8

SYMBOL_DATA = 1
SYMBOL_SECTION = 3

FLAG_WEAK = 0x01
FLAG_LOCAL = 0x02
FLAG_UNDEFINED = 0x10
FLAG_EXPLICIT_NAME = 0x40


class Reader:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def byte(self):
        value = self.data[self.pos]
        self.pos += 1
        return value

    def leb(self):
        result = 0
        shift = 0
        while True:
            byte = self.byte()
            result |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                return result

    def take(self, size):
        value = self.data[self.pos:self.pos + size]
        self.pos += size
        return value

    def name(self):
        return self.take(self.leb())

    def done(self):
        return self.pos >= len(self.data)


def leb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def name(value):
    return leb(len(value)) + value


def localize_symbol_table(payload, keep):
    reader = Reader(payload)
    count = reader.leb()
    out = bytearray(leb(count))
    localized = 0

    for _ in range(count):
        kind = reader.byte()
        flags = reader.leb()
        defined = not flags & FLAG_UNDEFINED
        body = bytearray()
        symbol = None

        if kind == SYMBOL_DATA:
            symbol = reader.name()
            body += name(symbol)
            if defined:
                body += leb(reader.leb()) + leb(reader.leb()) + leb(reader.leb())
        elif kind == SYMBOL_SECTION:
            body += leb(reader.leb())
        else:
            body += leb(reader.leb())
            if defined or flags & FLAG_EXPLICIT_NAME:
                symbol = reader.name()
                body += name(symbol)

        if defined and symbol is not None and symbol.decode() not in keep:
            if not flags & FLAG_LOCAL:
                localized += 1
            flags = (flags & ~FLAG_WEAK) | FLAG_LOCAL

        out.append(kind)
        out += leb(flags)
        out += body

    return bytes(out), localized


def localize_linking_section(payload, keep):
    reader = Reader(payload)
    out = bytearray(leb(reader.leb()))  # version
    localized = 0

    while not reader.done():
        kind = reader.byte()
        body = reader.take(reader.leb())
        if kind == SUBSECTION_COMDAT_INFO:
            continue
        if kind == SUBSECTION_SYMBOL_TABLE:
            body, localized = localize_symbol_table(body, keep)
        out.append(kind)
        out += leb(len(body))
        out += body

    return bytes(out), localized


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    source, target = sys.argv[1:3]
    keep = set(sys.argv[3:])

    with open(source, "rb") as file:
        data = file.read()
    if data[:4] != b"\0asm":
        sys.exit(f"{source}: not a WebAssembly file")

    reader = Reader(data, 8)
    out = bytearray(data[:8])
    localized = None

    while not reader.done():
        section = reader.byte()
        payload = reader.take(reader.leb())
        if section == SECTION_CUSTOM:
            custom = Reader(payload)
            if custom.name() == b"linking":
                body, localized = localize_linking_section(payload[custom.pos:], keep)
                payload = name(b"linking") + body
        out.append(section)
        out += leb(len(payload))
        out += payload

    if localized is None:
        sys.exit(f"{source}: no linking section, not a relocatable object")

    with open(target, "wb") as file:
        file.write(out)
    print(f"{target}: {localized} symbols made local")


if __name__ == "__main__":
    main()
