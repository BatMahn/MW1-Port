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

extern "C" void mt32_byte(unsigned char b) { if (parser) parser->parseStream(&b, 1); }

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
