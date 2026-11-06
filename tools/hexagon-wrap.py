#!/usr/bin/env python3
"""
hexagon-wrap.py IN OUT LOAD_ADDR: wrap a raw Qualcomm DSP firmware segment as a
one-segment ELF32 so LLVM can disassemble it.

The SLPI/ADSP images are split Qualcomm files: slpi.mdt is the ELF32 header and
the segments live in slpi.bNN. `readelf -l slpi.mdt` gives each segment's load
address; slpi.b15, for example, is the segment at paddr 0x8A000000. Wrap it:

    tools/hexagon-wrap.py fw-slpi/slpi.b15 /tmp/slpi15.elf 0x8A000000
    llvm-objdump -d --arch-name=hexagon /tmp/slpi15.elf > /tmp/slpi15.dis

Then a string at file offset 0x142f62 is at address LOAD_ADDR + 0x142f62. Hexagon
code often reaches strings through a table base plus an offset, so a direct
"search for the address" does not always find the reference.
"""
import struct
import sys

if len(sys.argv) != 4:
    sys.exit(__doc__)

src, out, addr = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
data = open(src, 'rb').read()

# ELF32, little-endian, EM_QDSP6 (164), one PT_LOAD (read+execute) at `addr`.
payload_off = 0x1000
e_ident = b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
ehdr = e_ident + struct.pack('<HHIIIIIHHHHHH', 2, 164, 1, addr, 52, 0, 0x66,
                             52, 32, 1, 0, 0, 0)
phdr = struct.pack('<IIIIIIII', 1, payload_off, addr, addr, len(data), len(data), 5, 0x1000)
blob = ehdr + phdr
blob += b'\0' * (payload_off - len(blob))
blob += data
open(out, 'wb').write(blob)
print('hexagon-wrap: %s -> %s (%u bytes) at %#x' % (src, out, len(blob), addr))
