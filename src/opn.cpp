// YM2203 (OPN) on the PC-9801-26K sound board, via ymfm.
// Timers run on the emulated clock so music tempo follows game time.
#include "../third_party/ymfm/ymfm_opn.h"
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" double emu_time_ms(void);

namespace {
struct Intf : ymfm::ymfm_interface {
    double expire[2] = { -1, -1 };
    double clock = 3993600.0;
    bool irq = false;
    void ymfm_set_timer(uint32_t tnum, int32_t duration) override {
        expire[tnum] = duration < 0 ? -1 : emu_time_ms() + duration * 1000.0 / clock;
        if (getenv("MW_OPNDBG")) fprintf(stderr, "[%9.1f] opn timer %u -> %d clocks\n", emu_time_ms(), tnum, duration);
    }
    void ymfm_update_irq(bool asserted) override { irq = asserted; }
    // SSG I/O port A on the PC-9801-26K reports the board's interrupt jumper:
    // bit 7 set = INT5 (IRQ12, INT 14h), the factory setting. The PC-98 bus
    // mouse owns IRQ13, so the board must not sit there. Joystick lines idle high.
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override {
        if (type == ymfm::ACCESS_IO) return address == 0 ? 0xBF : 0xFF;
        return 0;
    }
    void service(double now) {
        for (int t = 0; t < 2; t++)
            if (expire[t] >= 0 && now >= expire[t]) { expire[t] = -1; m_engine->engine_timer_expired(t); }
    }
};
Intf intf;
ymfm::ym2203 *chip;
ymfm::ym2203::output_data out;
uint32_t native_rate;
double frac;
int32_t prev[2], cur[2];
}

extern "C" {
void opn_init(void)
{
    chip = new ymfm::ym2203(intf);
    chip->reset();
    native_rate = chip->sample_rate((uint32_t)intf.clock);
}
void opn_write(int off, uint8_t v) { if (chip) chip->write(off & 1, v); }
uint8_t opn_read(int off) { return chip ? chip->read(off & 1) : 0; }
void opn_service(double now_ms) { intf.service(now_ms); }
int opn_irq(void) { return intf.irq; }
/* generate n stereo samples at out_rate, added into buf */
void opn_generate(int16_t *buf, int n, int out_rate)
{
    if (!chip) return;
    double step = (double)native_rate / out_rate;
    for (int i = 0; i < n; i++) {
        frac += step;
        while (frac >= 1.0) {
            frac -= 1.0;
            chip->generate(&out, 1);
            prev[0] = cur[0];
            /* output 0 = FM, 1..3 = SSG channels */
            cur[0] = (out.data[0] + (out.data[1] + out.data[2] + out.data[3]) / 2) * 5 / 2;   /* board output gain */
        }
        int32_t s = (int32_t)(prev[0] + (cur[0] - prev[0]) * frac);
        for (int c = 0; c < 2; c++) {
            int32_t v = buf[i * 2 + c] + s;
            buf[i * 2 + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
    }
}
}

#include <vector>
extern "C" void opn_snap(FILE *f, int save)
{
    std::vector<uint8_t> buf;
    if (save) {
        ymfm::ymfm_saved_state st(buf, true); chip->save_restore(st);
        uint32_t n = buf.size(); fwrite(&n, 4, 1, f); fwrite(buf.data(), 1, n, f);
        fwrite(intf.expire, sizeof intf.expire, 1, f); fwrite(&intf.irq, sizeof intf.irq, 1, f);
    } else {
        uint32_t n = 0; if (fread(&n, 4, 1, f) != 1) return; buf.resize(n); if (fread(buf.data(), 1, n, f) != n) return;
        ymfm::ymfm_saved_state st(buf, false); chip->save_restore(st);
        if (fread(intf.expire, sizeof intf.expire, 1, f) != 1 || fread(&intf.irq, sizeof intf.irq, 1, f) != 1) return;
    }
}
