/*
 * md_ymfm2612.cpp - C wrapper around ymfm's YM2612 (BSD-3-Clause core) for
 * the Mega Drive interpreter's commercial-clean FM path.
 *
 * The chip is placement-new'd at init (family lesson: no global C++ ctors in
 * these builds) and generates at its native rate, clock/144 = 53267 Hz NTSC.
 * ymfm leaves timer scheduling to the host: we count down in input-clock
 * units, 144 clocks per generated sample, and fire engine_timer_expired()
 * exactly like MAME does — so timer A/B status flags and CSM behave.
 * No exceptions, no RTTI, no heap: compile with
 *   -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
 */
#include <stdint.h>
#include <string.h>
#include <new>

#include "ymfm_opn.h"

namespace {

class MdYmInterface : public ymfm::ymfm_interface
{
public:
    int64_t timer_clocks[2];

    MdYmInterface() { timer_clocks[0] = timer_clocks[1] = -1; }

    virtual void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override
    {
        if (tnum < 2)
            timer_clocks[tnum] = duration_in_clocks;   /* <0 disables */
    }

    void clock_timers(int32_t clocks)
    {
        for (int t = 0; t < 2; t++) {
            if (timer_clocks[t] >= 0) {
                timer_clocks[t] -= clocks;
                if (timer_clocks[t] < 0) {
                    timer_clocks[t] = -1;
                    m_engine->engine_timer_expired(t);
                }
            }
        }
    }
};

alignas(8) unsigned char intf_buf[sizeof(MdYmInterface)];
alignas(8) unsigned char chip_buf[sizeof(ymfm::ym2612)];
MdYmInterface *intf;
ymfm::ym2612 *chip;

} /* namespace */

extern "C" {

void ymfm2612_init(void)
{
    if (chip)
        return;
    intf = new (intf_buf) MdYmInterface();
    chip = new (chip_buf) ymfm::ym2612(*intf);
    chip->reset();
}

void ymfm2612_reset(void)
{
    if (!chip)
        ymfm2612_init();
    intf->timer_clocks[0] = intf->timer_clocks[1] = -1;
    chip->reset();
}

void ymfm2612_write(unsigned int reg, unsigned int val)
{
    if (chip)
        chip->write(reg & 3, (uint8_t)val);
}

unsigned int ymfm2612_status(void)
{
    return chip ? chip->read_status() : 0;
}

/* interleaved L/R into out, n samples at the chip's native rate */
void ymfm2612_generate(int *out, int n)
{
    ymfm::ym2612::output_data od;
    int i;
    if (!chip) {
        memset(out, 0, (size_t)n * 2 * sizeof(int));
        return;
    }
    for (i = 0; i < n; i++) {
        chip->generate(&od, 1);
        intf->clock_timers(144);        /* one sample = 144 input clocks */
        /* x1.5: level-match the GPGX/Nuked mixing the rest of the stub
         * was balanced against (measured: ymfm RMS ~2/3 of GPGX's) */
        *out++ = od.data[0] + (od.data[0] >> 1);
        *out++ = od.data[1] + (od.data[1] >> 1);
    }
}

} /* extern "C" */

/* (the shared minimal C++ runtime for Amiga builds lives in md_cxx_stubs.cpp) */
