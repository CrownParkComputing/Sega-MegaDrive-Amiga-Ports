/*
 * md_ymfm2151.cpp - C wrapper around ymfm's YM2151 (BSD-3-Clause) for the
 * loader's VGM intro-music player. Pure register feed (no timers, no IRQ),
 * generated at the chip's native rate, clock/64. Same no-ctor/no-heap rules
 * as md_ymfm2612.cpp; the shared C++ runtime stubs live in md_cxx_stubs.cpp.
 */
#include <stdint.h>
#include <string.h>
#include <new>

#include "ymfm_opm.h"

namespace {

class MdOpmInterface : public ymfm::ymfm_interface
{
};

alignas(8) unsigned char intf_buf[sizeof(MdOpmInterface)];
alignas(8) unsigned char chip_buf[sizeof(ymfm::ym2151)];
MdOpmInterface *intf;
ymfm::ym2151 *chip;

} /* namespace */

extern "C" {

void ymfm2151_init(void)
{
    if (chip)
        return;
    intf = new (intf_buf) MdOpmInterface();
    chip = new (chip_buf) ymfm::ym2151(*intf);
    chip->reset();
}

void ymfm2151_reset(void)
{
    if (!chip)
        ymfm2151_init();
    chip->reset();
}

void ymfm2151_write(unsigned int reg, unsigned int val)
{
    if (!chip)
        return;
    chip->write(0, (uint8_t)reg);
    chip->write(1, (uint8_t)val);
}

/* interleaved L/R into out, n samples at clock/64 */
void ymfm2151_generate(int *out, int n)
{
    ymfm::ym2151::output_data od;
    int i;
    if (!chip) {
        memset(out, 0, (size_t)n * 2 * sizeof(int));
        return;
    }
    for (i = 0; i < n; i++) {
        chip->generate(&od, 1);
        *out++ = od.data[0];
        *out++ = od.data[1];
    }
}

} /* extern "C" */
