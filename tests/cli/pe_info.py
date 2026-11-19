#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""What a PE file declares, read from the file itself.

  pe_info.py FILE subsystem   the subsystem version, as MAJOR.MINOR
  pe_info.py FILE dlls        the DLLs it imports from, lower case, one a line
  pe_info.py FILE imports     the functions it imports by name, one a line
  pe_info.py FILE exports     the names it exports, one a line
"""
import struct
import sys


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        pe = self.u32(0x3C)
        if self.d[:2] != b"MZ" or self.d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path} is not a PE file")
        nsections, = struct.unpack_from("<H", self.d, pe + 6)
        optsize, = struct.unpack_from("<H", self.d, pe + 20)
        self.opt = pe + 24
        magic, = struct.unpack_from("<H", self.d, self.opt)
        self.plus = magic == 0x20B                    # PE32+
        self.dirs = self.opt + (112 if self.plus else 96)
        self.sections = []
        for i in range(nsections):
            s = self.opt + optsize + 40 * i
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", self.d, s + 8)
            self.sections.append((va, max(vsize, rawsize), raw))

    def u32(self, off):
        return struct.unpack_from("<I", self.d, off)[0]

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"RVA {rva:#x} is in no section")

    def text(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("ascii")

    def subsystem(self):
        major, minor = struct.unpack_from("<HH", self.d, self.opt + 48)
        return f"{major}.{minor}"

    def imported(self):
        """(dll, function) pairs; an import by ordinal has no function name."""
        rva = self.u32(self.dirs + 8)
        if not rva:
            return
        d = self.off(rva)
        width, fmt, by_ordinal = (8, "<Q", 1 << 63) if self.plus else (4, "<I", 1 << 31)
        while True:
            lookup, _, _, name, thunks = struct.unpack_from("<IIIII", self.d, d)
            if not name:
                break
            dll = self.text(name)
            t = self.off(lookup or thunks)
            while True:
                entry, = struct.unpack_from(fmt, self.d, t)
                if not entry:
                    break
                yield dll, None if entry & by_ordinal else self.text(entry + 2)
                t += width
            d += 20

    def exported(self):
        rva = self.u32(self.dirs)
        if not rva:
            return
        e = self.off(rva)
        count, names = self.u32(e + 24), self.off(self.u32(e + 32))
        for i in range(count):
            yield self.text(self.u32(names + 4 * i))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pe, what = PE(sys.argv[1]), sys.argv[2]
    if what == "subsystem":
        out = [pe.subsystem()]
    elif what == "dlls":
        out = sorted({dll.lower() for dll, _ in pe.imported()})
    elif what == "imports":
        out = [fn for _, fn in pe.imported() if fn]
    elif what == "exports":
        out = list(pe.exported())
    else:
        sys.exit(__doc__)
    print("\n".join(out))


if __name__ == "__main__":
    main()
