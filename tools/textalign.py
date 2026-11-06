#!/usr/bin/env python3
"""Pair PC-98 (Japanese) strings with DOS (English) strings from the same source.

Strings are aligned in data-segment order with dynamic programming. The score
for pairing two strings uses only language-independent evidence:
  * identical non-text bytes around them (tables, numbers) - strongest anchor
  * the same control codes / formatting prefix (line breaks, colour bytes)
  * shared ASCII words and digits once full-width characters are normalised
  * plausible length ratio
Output: "pc98_offset dos_offset confidence" per line. No text is written.

usage: textalign.py DOS.EXE DOS_SEG PC98.EXE PC98_SEG out.map
"""
import sys, re, struct, unicodedata, difflib

def load(f):
    d = open(f, 'rb').read()
    if d[:2] != b'MZ': return d
    lp, pg = struct.unpack_from('<HH', d, 2); h = struct.unpack_from('<H', d, 8)[0] * 16
    return d[h:(pg - 1) * 512 + (lp or 512)]

EN = re.compile(rb'[ -~\r\n\x01-\x1f]{2,}\x00')
SJ = re.compile(rb'(?:[\x81-\x9f\xe0-\xef][\x40-\x7e\x80-\xfc]|[ -~\r\n\x01-\x1f\xa1-\xdf])+\x00')

def collect(img, base, sj):
    seg = img[base:base + 0x10000]
    rx = SJ if sj else EN
    # string starts named by pointer tables in this segment: runs of 3+ consecutive words,
    # increasing, each landing just after a terminator (or another table entry's end)
    words = set()
    W = [struct.unpack_from('<H', seg, i)[0] for i in range(0, len(seg) - 1)]
    for par in (0, 1):
        run = []
        for i in range(par, len(W), 2):
            v = W[i]
            if run and 0 < v - run[-1] < 0x200: run.append(v); continue
            if len(run) >= 3: words.update(run)
            run = [v] if 0 < v < len(seg) else []
        if len(run) >= 3: words.update(run)
    out = []
    for m in rx.finditer(seg):
        st = m.start()
        # the text pattern can swallow the tail of a preceding pointer table whose bytes happen
        # to look like characters; if the run isn't preceded by a terminator, move the start to
        # the first position within it that something actually points at
        if st > 0 and seg[st - 1] != 0:
            for k in range(1, min(10, m.end() - st - 2)):
                t = st + k
                # the swallowed prefix is the tail of a pointer table: it contains the high
                # byte of the pointers, which point at (roughly) this very string
                if t in words and (t >> 8) in seg[st:t]:
                    st = t; break
        s = seg[st:m.end() - 1]
        if sj and not re.search(rb'[\x81-\x9f\xe0-\xef]', s): continue
        if not sj and not re.search(rb'[A-Za-z]{2}', s): continue
        before = seg[max(0, st - 6):st]
        after = seg[m.end():m.end() + 6]
        out.append((base + st, s, before, after))
    return out

def ctrl_sig(s):
    sig = []
    for i, c in enumerate(s):
        if c < 0x20: sig.append(c)
    lead = s[:2] if len(s) > 1 and s[1] == 0x0d and 0x20 <= s[0] < 0x7f else b''
    return tuple(sig), lead

def words(s, sj):
    t = s.decode('cp932', 'replace') if sj else s.decode('latin1')
    t = unicodedata.normalize('NFKC', t).upper()
    return set(re.findall(r'[A-Z]{2,}|\d+', t))

KANA = {}
_rows = {'': 'aiueo', 'k': 'kakikukeko', 's': 'sashisuseso', 't': 'tachitsuteto', 'n': 'naninuneno', 'h': 'hahihuheho',
         'm': 'mamimumemo', 'r': 'rarirurero', 'g': 'gagigugego', 'z': 'zajizuzezo', 'd': 'dajizudedo', 'b': 'babibubebo', 'p': 'papipupepo'}
def _build():
    kata = ('アイウエオ', 'カキクケコ', 'サシスセソ', 'タチツテト', 'ナニヌネノ', 'ハヒフヘホ', 'マミムメモ', 'ラリルレロ',
            'ガギグゲゴ', 'ザジズゼゾ', 'ダヂヅデド', 'バビブベボ', 'パピプペポ')
    cons = ('', 'k', 's', 't', 'n', 'h', 'm', 'r', 'g', 'z', 'd', 'b', 'p')
    for row, c in zip(kata, cons):
        for k, v in zip(row, 'aiueo'): KANA[k] = c + v
    KANA.update({'ヤ': 'ya', 'ユ': 'yu', 'ヨ': 'yo', 'ワ': 'wa', 'ヲ': 'o', 'ン': 'n', 'ヴ': 'bu', 'ッ': '', 'ー': '',
                 'ャ': 'ya', 'ュ': 'yu', 'ョ': 'yo', 'ァ': 'a', 'ィ': 'i', 'ゥ': 'u', 'ェ': 'e', 'ォ': 'o', '・': ' '})
_build()
def skeleton_jp(s):
    t = s.decode('cp932', 'replace')
    r = ''.join(KANA.get(ch, '') for ch in t)
    return re.sub(r'[aeiouy ]', '', r).replace('l', 'r')
def skeleton_en(s):
    t = s.decode('latin1').lower()
    t = re.sub(r'[^a-z]', '', t).replace('l', 'r').replace('v', 'b').replace('th', 's').replace('c', 'k').replace('ph', 'f').replace('f', 'h').replace('x', 'ks').replace('w', '').replace('q', 'k')
    return re.sub(r'[aeiouy]', '', t)

def score(a, b):
    sa, la = ctrl_sig(a[1]); sb, lb = ctrl_sig(b[1])
    sc = 0.0
    if la and la == lb: sc += 3
    elif la != lb: sc -= 2
    if sa == sb: sc += 1 + min(len(sa), 4) * 0.5
    else: sc -= 1
    wa, wb = words(a[1], True), words(b[1], False)
    if wa: sc += 4 * len(wa & wb) / len(wa) - (1 if not (wa & wb) else 0)
    if a[2][-4:] == b[2][-4:] and a[2][-4:] != b'\0\0\0\0': sc += 2
    if a[3][:4] == b[3][:4]: sc += 1
    # japanese bytes vs english characters: roughly 0.6x-3x
    kj = skeleton_jp(a[1])
    if len(kj) >= 2:
        ke = skeleton_en(b[1])
        if ke:
            rr = difflib.SequenceMatcher(None, kj, ke).ratio()
            if rr > 0.55: sc += 6 * rr
    if a[1][:1] and b[1][:1] and a[1][0] < 0x20 and a[1][0] == b[1][0]: sc += 2
    r = (len(b[1]) + 1) / (len(a[1]) + 1)
    if r < 0.25 or r > 6: sc -= 2
    return sc

def align(J, E, gap=-0.8):
    n, m = len(J), len(E)
    import array
    # banded DP around the diagonal to keep it fast
    band = max(160, abs(n - m) + 80)
    NEG = -1e9
    S = [dict() for _ in range(n + 1)]; P = [dict() for _ in range(n + 1)]
    S[0][0] = 0.0
    for j in range(1, min(m, band) + 1): S[0][j] = S[0][j - 1] + gap; P[0][j] = 'l'
    for i in range(1, n + 1):
        c = int(i * m / n)
        lo, hi = max(0, c - band), min(m, c + band)
        Si, Sp = S[i], S[i - 1]
        for j in range(lo, hi + 1):
            best, how = Sp.get(j, NEG) + gap, 'u'
            v = Si.get(j - 1, NEG) + gap
            if v > best: best, how = v, 'l'
            if j > 0 and (j - 1) in Sp:
                v = Sp[j - 1] + score(J[i - 1], E[j - 1])
                if v > best: best, how = v, 'd'
            Si[j] = best; P[i][j] = how
    i, j = n, max(S[n], key=lambda k: S[n][k]) if m not in S[n] else m
    pairs = []
    while i > 0 and j >= 0:
        how = P[i].get(j)
        if how == 'd': pairs.append((J[i - 1], E[j - 1])); i -= 1; j -= 1
        elif how == 'u': i -= 1
        elif how == 'l': j -= 1
        else: break
    return pairs[::-1]

def main():
    dos_f, dseg, p98_f, pseg, out = sys.argv[1:6]
    dos, p98 = load(dos_f), load(p98_f)
    E = collect(dos, int(dseg, 16) * 16, False); J = collect(p98, int(pseg, 16) * 16, True)
    pairs = align(J, E)
    with open(out, 'w') as f:
        for a, b in pairs: f.write('%05x %05x %.1f\n' % (a[0], b[0], score(a, b)))
    hi = sum(1 for a, b in pairs if score(a, b) >= 2)
    print('%s: japanese %d, english %d, paired %d, confident %d' % (out, len(J), len(E), len(pairs), hi))

if __name__ == '__main__':
    main()
