#!/usr/bin/env python3
"""Disassemble a function of a vendor kernel Image, by symbol name, with its calls and data resolved.

The phone's own kernel is the only source for several of its drivers' register tables and
sequences (docs/cellular.md), and it is not in this repository: `boot_a` holds an uncompressed
ARM64 Image, and the running Android's /proc/kallsyms (with kptr_restrict=0) gives the symbols
and _text. A symbol's file offset in the Image is its live address minus _text; this script
takes that offset, feeds the bytes to llvm-mc and annotates every bl/b target and every
adrp/add data address with the nearest symbol (and a string, when the data is one).

    adb exec-out su -c 'sh -c "echo 0 > /proc/sys/kernel/kptr_restrict; cat /proc/kallsyms"' \
        > kallsyms.txt
    tools/kdisas.py --image boot_a-kernel.bin --kallsyms kallsyms.txt --text 0xffffff9e92080000 \
        --function sdx55m_setup_hw
"""
import argparse
import bisect
import struct
import subprocess

LINK_BASE = 0xffffff8010080000  # the Image's link address (pointers in its data use it)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", required=True, help="the kernel Image (uncompressed ARM64 Image)")
    ap.add_argument("--kallsyms", required=True, help="/proc/kallsyms from the same kernel")
    ap.add_argument("--text", required=True, help="_text of that boot (KASLR), e.g. 0xffffff9e92080000")
    ap.add_argument("--function", required=True, help="the symbol to disassemble")
    ap.add_argument("--limit", type=lambda s: int(s, 0), default=0x1000, help="bytes at most")
    args = ap.parse_args()

    text = int(args.text, 16)
    data = open(args.image, "rb").read()
    syms = []
    for line in open(args.kallsyms):
        p = line.split()
        if len(p) >= 3:
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    syms.sort()
    keys = [s[0] for s in syms]

    def nearest(a):
        i = bisect.bisect_right(keys, a) - 1
        return "?" if i < 0 else "%s+%#x" % (syms[i][1], a - syms[i][0])

    start = None
    for i, (a, n) in enumerate(syms):
        if n == args.function:
            start = a
            end = syms[i + 1][0] if i + 1 < len(syms) else a + args.limit
            break
    if start is None:
        raise SystemExit("symbol not found: %s" % args.function)
    end = min(end, start + args.limit)

    code = data[start - text:start - text + (end - start)]
    words = [int.from_bytes(code[i:i + 4], "little") for i in range(0, len(code) - 3, 4)]
    lines = [" ".join("0x%02x" % b for b in w.to_bytes(4, "little")) for w in words]
    out = subprocess.run(["llvm-mc", "--disassemble", "--triple=aarch64"],
                         input=("\n".join(lines) + "\n").encode(), stdout=subprocess.PIPE)
    ins = [l.strip() for l in out.stdout.decode(errors="replace").splitlines()
           if l.strip() and not l.strip().startswith(".")]
    for k, s in enumerate(ins):
        if k >= len(words):
            break
        a, w = start + 4 * k, words[k]
        note = ""
        if (w & 0xfc000000) in (0x94000000, 0x14000000):        # bl / b
            imm = w & 0x3ffffff
            if imm & 0x2000000:
                imm -= 0x4000000
            target = a + (imm << 2)
            note = "  -> %#x %s" % (target, nearest(target))
        if (w & 0x7f800000) == 0x11000000:                        # add xD, xN, #imm
            rn = (w >> 5) & 0x1f
            # resolve through any adrp xN in the previous two instructions
            for back in (1, 2):
                if k - back < 0:
                    continue
                pw = words[k - back]
                prd = pw & 0x1f
                if prd == rn and (pw & 0x9f000000) == 0x90000000:
                    immlo = (pw >> 29) & 3
                    immhi = (pw >> 5) & 0x7ffff
                    immv = (immhi << 2) | immlo
                    if immv & 0x100000:
                        immv -= 0x200000
                    v = ((a - 4 * back) & ~0xfff) + (immv << 12) + ((w >> 10) & 0xfff)
                    note = "  => %#x" % v
                    off = v - text
                    if 0 <= off < len(data):
                        raw = data[off:off + 32].split(b"\0")[0]
                        if 2 <= len(raw) <= 31 and all(32 <= c < 127 for c in raw):
                            note += ' "%s"' % raw.decode()
                    break
        print("%#x: %-36s%s" % (a, s, note))


if __name__ == "__main__":
    main()
