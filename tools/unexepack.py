#!/usr/bin/env python3
"""Microsoft EXEPACK unpacker -> plain MZ executable."""
import struct, sys
def unpack(d):
    hdrp = struct.unpack_from('<H', d, 8)[0]*16
    cs, ip = struct.unpack_from('<H', d, 0x16)[0], struct.unpack_from('<H', d, 0x14)[0]
    img = bytearray(d[hdrp:])
    eh = cs*16
    rip, rcs, _, xsize, rsp, rss, dlen, skip, sig = struct.unpack_from('<9H', img, eh)
    assert sig == 0x4252, 'not EXEPACK'
    hl = 18
    comp_end = eh - (skip-1)*16
    out = bytearray(dlen*16 if dlen*16 > comp_end else comp_end)
    out[:comp_end] = img[:comp_end]
    src = comp_end - 1
    while src >= 0 and out[src] == 0xff: src -= 1
    dst = dlen*16 - 1
    buf = bytes(out[:comp_end])
    out = bytearray(dlen*16)
    while True:
        cmd = buf[src]; cnt = buf[src-2] | buf[src-1] << 8; src -= 3
        if cmd & 0xfe == 0xb0:
            b = buf[src]; src -= 1
            for _ in range(cnt): out[dst] = b; dst -= 1
        elif cmd & 0xfe == 0xb2:
            for _ in range(cnt): out[dst] = buf[src]; dst -= 1; src -= 1
        else: raise ValueError('bad cmd %02x' % cmd)
        if cmd & 1: break
    # remaining head of buffer is literal (src+1 bytes), already identical in place
    out[:src+1] = buf[:src+1]
    # relocations: after unpacker stub; find 'Packed file is corrupt' message end
    stub = bytes(img[eh:eh+xsize])
    m = stub.find(b'Packed file is corrupt')
    p = m + len(b'Packed file is corrupt')
    relocs = []
    for seg in range(16):
        n = struct.unpack_from('<H', stub, p)[0]; p += 2
        for _ in range(n):
            relocs.append((seg*0x1000, struct.unpack_from('<H', stub, p)[0])); p += 2
    # build MZ
    nrel = len(relocs); hsize = (28 + nrel*4 + 511)//512*512
    tot = hsize + len(out)
    h = bytearray(hsize)
    minalloc = struct.unpack_from('<H', d, 0x0a)[0]
    struct.pack_into('<14H', h, 0, 0x5a4d, tot % 512, (tot+511)//512, nrel, hsize//16,
                     max(0, minalloc - (dlen - (len(img)//16))), 0xffff, rss, rsp, 0, rip, rcs, 28, 0)
    for i,(s,o) in enumerate(relocs): struct.pack_into('<HH', h, 28+i*4, o, s)
    return bytes(h+out)
if __name__ == '__main__':
    r = unpack(open(sys.argv[1],'rb').read()); open(sys.argv[2],'wb').write(r); print('ok', len(r))
