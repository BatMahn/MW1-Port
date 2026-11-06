#!/usr/bin/env python3
"""Unpack PC-98 FAT12 floppy images (FDI with 4 KB header, or raw HDM) into a folder.
usage: fdi_extract.py OUTDIR IMAGE [IMAGE...]"""
import os, struct, sys

def extract(path, outdir):
    d = open(path, 'rb').read()
    if len(d) > 32 and struct.unpack_from('<I', d, 0)[0] == 0:
        hs, ds = struct.unpack_from('<II', d, 8)
        if 0 < hs and hs + ds <= len(d): d = d[hs:]
    bps, spc, res, nfat, nroot, tot, med, spf = struct.unpack_from('<HBHBHHBH', d, 11)
    if bps not in (512, 1024): bps, spc, res, nfat, nroot, spf = 1024, 1, 1, 2, 192, 2
    spc = spc or 1
    fat = d[res * bps:(res + spf) * bps]
    root = (res + nfat * spf) * bps; data = root + nroot * 32; cs = spc * bps
    def nxt(c):
        o = c + c // 2; w = fat[o] | fat[o + 1] << 8
        return w >> 4 if c & 1 else w & 0xFFF
    n = 0
    def walk(off, count, clus, out):
        nonlocal n
        os.makedirs(out, exist_ok=True)
        while True:
            o = data + (clus - 2) * cs if clus else off
            cnt = cs // 32 if clus else count
            for i in range(cnt):
                e = d[o + i * 32:o + i * 32 + 32]
                if len(e) < 32 or e[0] == 0: return
                if e[0] in (0xE5, 0x2E) or e[11] & 8 or e[11] == 0x0F: continue
                name = e[:8].decode('latin1').strip() + ('.' + e[8:11].decode('latin1').strip() if e[8:11].strip() else '')
                name = name.upper()
                first = struct.unpack_from('<H', e, 26)[0]; size = struct.unpack_from('<I', e, 28)[0]
                if e[11] & 0x10: walk(0, 0, first, os.path.join(out, name)); continue
                buf = bytearray(); c = first
                while len(buf) < size and 2 <= c < 0xFF0:
                    buf += d[data + (c - 2) * cs:data + (c - 1) * cs]; c = nxt(c)
                open(os.path.join(out, name), 'wb').write(bytes(buf[:size])); n += 1
            if not clus: return
            clus = nxt(clus)
            if clus < 2 or clus >= 0xFF0: return
    walk(root, nroot, 0, outdir)
    return n

if __name__ == '__main__':
    out = sys.argv[1]
    for img in sorted(sys.argv[2:], key=str.lower):
        print('%s: %d files' % (os.path.basename(img), extract(img, out)))
