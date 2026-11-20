#include <cstdlib>
#include <cstdio>
/* Roland MT-32 through Munt's libmt32emu. The ROM files are the player's own (see README). */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include "../third_party/mt32emu/mt32emu.h"

using namespace MT32Emu;
static Synth *synth;
static SampleRateConverter *src;
static MidiStreamParser *parser;
static const ROMImage *ctl_img, *pcm_img;
static FileStream *ctl_fs, *pcm_fs;

namespace {
class Parser : public DefaultMidiStreamParser {
public:
    Parser(Synth &s) : DefaultMidiStreamParser(s) {}
};
}

extern "C" int mt32_open(const char *romdir, int rate)
{
    DIR *d = opendir(romdir);
    if (!d) return 0;
    std::vector<std::string> files;
    while (struct dirent *e = readdir(d)) if (e->d_name[0] != '.') files.push_back(std::string(romdir) + "/" + e->d_name);
    closedir(d);
    /* identify control and PCM ROMs by their contents, whatever the files are called */
    for (const std::string &p : files) {
        FileStream *fs = new FileStream();
        if (!fs->open(p.c_str())) { delete fs; continue; }
        const ROMImage *img = ROMImage::makeROMImage(fs);
        const ROMInfo *info = img ? img->getROMInfo() : NULL;
        if (!info) { if (img) ROMImage::freeROMImage(img); delete fs; continue; }
        bool mt32 = !strncmp(info->shortName, "ctrl_mt32", 9) || !strncmp(info->shortName, "pcm_mt32", 8);
        if (info->type == ROMInfo::Control && (!ctl_img || mt32)) { if (ctl_img) ROMImage::freeROMImage(ctl_img); ctl_img = img; ctl_fs = fs; }
        else if (info->type == ROMInfo::PCM && (!pcm_img || mt32)) { if (pcm_img) ROMImage::freeROMImage(pcm_img); pcm_img = img; pcm_fs = fs; }
        else { ROMImage::freeROMImage(img); delete fs; }
    }
    if (!ctl_img || !pcm_img) { fprintf(stderr, "MT-32: control and PCM ROMs not both found in %s\n", romdir); return 0; }
    synth = new Synth();
    if (!synth->open(*ctl_img, *pcm_img)) { fprintf(stderr, "MT-32: the ROMs in %s did not load\n", romdir); delete synth; synth = NULL; return 0; }
    src = new SampleRateConverter(*synth, rate, SamplerateConversionQuality_GOOD);
    parser = new Parser(*synth);
    fprintf(stderr, "MT-32: %s + %s\n", ctl_img->getROMInfo()->description, pcm_img->getROMInfo()->description);
    return 1;
}

/* The game's music sets MT-32 part channels with duplicates (MECH.MUS assigns parts 7 and 8
 * to the channels of parts 3 and 2; MECHBAR.MUS doubles three parts). Munt layers every part
 * that listens to a channel, and the doubled voices beat against each other: a warble that the
 * reference recordings of the game don't have. So a part whose channel is already taken by a
 * lower part is switched off as the setting goes by. */
static unsigned char sx[512]; static int sxn = -1;
static void dedupe_channels(unsigned char *m, int n)
{
    /* F0 41 dev 16 12 a1 a2 a3 data... sum F7; system area 10 00 xx, channels at 0Dh-15h */
    if (n < 11 || m[1] != 0x41 || m[3] != 0x16 || m[4] != 0x12 || m[5] != 0x10 || m[6] != 0x00) return;
    int base = m[7], ndata = n - 10, changed = 0;
    unsigned char ch[9]; int have[9];
    for (int p = 0; p < 9; p++) { int off = 0x0D + p - base; have[p] = off >= 0 && off < ndata; ch[p] = have[p] ? m[8 + off] : 0xFF; }
    /* the rhythm part (index 8) keeps its channel: a melodic part on the drum channel would
     * play the drum notes as pitched notes (MECHBAR.MUS puts part 6 there) */
    if (have[8] && ch[8] < 0x10)
        for (int p = 0; p < 8; p++) if (have[p] && ch[p] == ch[8]) { m[8 + 0x0D + p - base] = 0x10; ch[p] = 0x10; changed = 1; }
    for (int p = 1; p < 8; p++) {
        if (!have[p] || ch[p] >= 0x10) continue;
        for (int q = 0; q < p; q++) if (have[q] && ch[q] == ch[p]) { m[8 + 0x0D + p - base] = 0x10; ch[p] = 0x10; changed = 1; break; }
    }
    if (changed && getenv("MW_MPULOG")) { fprintf(stderr, "mt32: part channels now"); for (int p = 0; p < 9; p++) fprintf(stderr, " %02x", ch[p]); fprintf(stderr, "\n"); }
    if (changed) { unsigned sum = 0; for (int i = 5; i < n - 2; i++) sum += m[i]; m[n - 2] = (128 - (sum & 127)) & 127; }
}
extern "C" void mt32_byte(unsigned char b)
{
    if (!parser) return;
    if (b == 0xF0) { sxn = 0; sx[sxn++] = b; return; }
    if (sxn >= 0) {
        if (b & 0x80 && b != 0xF7) { sxn = -1; parser->parseStream(&b, 1); return; }   /* aborted sysex */
        if (sxn < (int)sizeof sx) sx[sxn++] = b;
        if (b == 0xF7) { dedupe_channels(sx, sxn); parser->parseStream(sx, sxn); sxn = -1; }
        return;
    }
    parser->parseStream(&b, 1);
}

/* mix n stereo frames into buf */
extern "C" void mt32_mix(short *buf, int n)
{
    if (!src) return;
    static std::vector<Bit16s> tmp;
    tmp.resize(n * 2);
    src->getOutputSamples(tmp.data(), n);
    for (int i = 0; i < n * 2; i++) {
        int v = buf[i] + tmp[i];
        buf[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
}

extern "C" int mt32_active(void) { return synth != NULL; }
