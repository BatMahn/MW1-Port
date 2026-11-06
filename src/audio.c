/* Audio output. Samples are generated against emulated time and queued to
 * SDL, so sound stays in step with the game's own clock. */
#include "machine.h"
#include "runtime.h"
#include <SDL.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "../third_party/opl3.h"
#include "pc98.h"
void opn_init(void); void opn_generate(s16 *buf, int n, int out_rate);

#define RATE 44100
static SDL_AudioDeviceID dev;
FILE *wav_out;   /* --wav: raw 44.1k stereo s16 capture of everything played */
static opl3_chip opl;
static int spk_on; static double spk_freq, spk_phase;
static double gen_ms;   /* emulated time up to which audio has been generated */
int audio_device = 1;   /* unused: each sound device switches itself on when the game first talks to it */
static int used_adlib, used_tandy, used_mt32;
const char *mt32_dir;   /* folder with the MT-32 ROMs (set from the data layout) */
int mt32_open(const char *romdir, int rate); void mt32_byte(unsigned char b); void mt32_mix(short *buf, int n); int mt32_active(void);

/* SN76496 */
static u16 sn_tone[3]; static u8 sn_vol[4] = { 15, 15, 15, 15 }; static u8 sn_noise, sn_latch;
static double sn_ph[3]; static u16 sn_lfsr = 0x4000; static double sn_nph;

void audio_init(void)
{
    SDL_AudioSpec want = { 0 }, have;
    want.freq = RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    OPL3_Reset(&opl, RATE);
    if (mt32_dir) mt32_open(mt32_dir, RATE);
    opn_init();
    if (dev) SDL_PauseAudioDevice(dev, 0);
}

void audio_speaker(int on, double freq) { spk_on = on; spk_freq = freq; }
static long adlib_writes;
long audio_adlib_count(void) { return adlib_writes; }
void audio_adlib_write(int reg, u8 val) { used_adlib = 1; adlib_writes++; OPL3_WriteRegBuffered(&opl, reg, val); }
void audio_tandy_write(u8 v)
{
    used_tandy = 1;
    if (v & 0x80) {
        sn_latch = (v >> 4) & 7;
        int ch = sn_latch >> 1;
        if (sn_latch & 1) sn_vol[ch] = v & 15;
        else if (ch < 3) sn_tone[ch] = (sn_tone[ch] & 0x3F0) | (v & 15);
        else { sn_noise = v & 7; sn_lfsr = 0x4000; }
    } else {
        int ch = sn_latch >> 1;
        if (sn_latch & 1) sn_vol[ch] = v & 15;
        else if (ch < 3) sn_tone[ch] = (sn_tone[ch] & 15) | ((v & 0x3F) << 4);
    }
}
static long mpu_bytes;
void audio_mpu_data(u8 v) { used_mt32 = 1; mpu_bytes++; mt32_byte(v); }
long audio_mpu_count(void) { return mpu_bytes; }

static const float sn_db[16] = { 1.0f, .794f, .631f, .501f, .398f, .316f, .251f, .200f, .158f, .126f, .100f, .079f, .063f, .050f, .040f, 0 };

void audio_advance(double emu_ms)
{
    if (!dev && !wav_out) { gen_ms = emu_ms; return; }
    int n = (int)((emu_ms - gen_ms) * RATE / 1000.0);
    if (n <= 0) return;
    if (n > RATE / 4) n = RATE / 4;
    gen_ms += n * 1000.0 / RATE;
    /* don't let the queue grow without bound (e.g. after a pause) */
    if (dev && SDL_GetQueuedAudioSize(dev) > RATE * 4 / 5 && !wav_out) return;
    static s16 buf[RATE / 4 * 2];
    if (used_adlib && !machine_pc98) OPL3_GenerateStream(&opl, buf, n);
    else memset(buf, 0, n * 4);
    if (machine_pc98) opn_generate(buf, n, RATE);
    if (used_mt32) mt32_mix(buf, n);
    if (wav_out) fwrite(buf, 4, n, wav_out);
    for (int i = 0; i < n; i++) {
        int s = 0;
        if (spk_on && spk_freq > 20 && spk_freq < 20000) {
            spk_phase += spk_freq / RATE; spk_phase -= floor(spk_phase);
            s += spk_phase < 0.5 ? 4000 : -4000;
        }
        if (used_tandy) {
            float acc = 0;
            for (int c = 0; c < 3; c++) {
                double f = sn_tone[c] ? 3579545.0 / 32.0 / sn_tone[c] : 0;
                if (f > 20000) f = 0;
                sn_ph[c] += f / RATE; sn_ph[c] -= floor(sn_ph[c]);
                acc += (sn_ph[c] < 0.5 ? 1 : -1) * sn_db[sn_vol[c]];
            }
            double nf = (sn_noise & 3) == 3 ? (sn_tone[2] ? 3579545.0 / 32.0 / sn_tone[2] : 0) : 3579545.0 / 32.0 / (0x10 << (sn_noise & 3));
            sn_nph += nf / RATE;
            while (sn_nph >= 1) { sn_nph -= 1; int fb = (sn_noise & 4) ? ((sn_lfsr ^ (sn_lfsr >> 1)) & 1) : (sn_lfsr & 1); sn_lfsr = (sn_lfsr >> 1) | (fb << 14); }
            acc += ((sn_lfsr & 1) ? 1 : -1) * sn_db[sn_vol[3]];
            s += (int)(acc * 2500);
        }
        int l = buf[i * 2] + s, r = buf[i * 2 + 1] + s;
        buf[i * 2] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        buf[i * 2 + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    }
    { extern int audio_muted; if (dev && !audio_muted) SDL_QueueAudio(dev, buf, n * 4); }
}


#include "snap.h"
void audio_snap(FILE *f, int save) { S(gen_ms); S(spk_on); S(spk_freq); S(used_adlib); S(used_tandy); opn_snap(f, save); }
