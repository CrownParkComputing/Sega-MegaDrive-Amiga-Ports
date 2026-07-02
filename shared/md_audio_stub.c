/*
 * md_audio_stub.c - Mega Drive sound-side bus, Z80, and SN76489 PSG.
 *
 * PSG behavior follows JGenesis' SN76489 model: latch/data writes, /16 PSG
 * divider, 10-bit tone counters, 15-bit noise LFSR, and 2dB attenuation table.
 * Default output keeps the sound CPU and PSG running but does not mix the cheap
 * YM approximation; it was fast but too harsh for normal play. Define
 * MD_FAST_FM=1 for that approximation, or MD_ACCURATE_OPN2=1 to use Nuked-OPN2
 * clocked with JGenesis' Genesis YM divider of 42 master clocks per internal tick.
 */

#include "hal/md_machine.h"
#include "cores/z80emu.h"

#ifndef MD_ACCURATE_OPN2
#define MD_ACCURATE_OPN2 0
#endif
#ifndef MD_FAST_FM
#define MD_FAST_FM 0
#endif
#ifndef MD_RUN_Z80_SOUND
#define MD_RUN_Z80_SOUND 1
#endif
#ifndef MD_AUDIO_DEBUG
#define MD_AUDIO_DEBUG 0
#endif

#if MD_ACCURATE_OPN2
#include "nuked_opn2/ym3438.h"
#endif

#include <string.h>

#define MD_MASTER_CLOCK 53693175u
#define MD_Z80_CLOCK    (MD_MASTER_CLOCK / 15u)
#define MD_PSG_CLOCK    (MD_MASTER_CLOCK / 15u)
#define MD_PSG_TICK     (MD_PSG_CLOCK / 16u)
#define MD_YM_DIVIDER   42u

enum {
    PSG_TONE0 = 0,
    PSG_VOL0,
    PSG_TONE1,
    PSG_VOL1,
    PSG_TONE2,
    PSG_VOL2,
    PSG_NOISE,
    PSG_VOL3
};

typedef struct {
    uint16_t counter;
    uint16_t tone;
    uint8_t output;
    uint8_t attenuation;
} PsgTone;

typedef struct {
    uint16_t counter;
    uint16_t lfsr;
    uint8_t counter_output;
    uint8_t lfsr_output;
    uint8_t reload_mode;
    uint8_t white_noise;
    uint8_t attenuation;
} PsgNoise;

static MY_LITTLE_Z80 z80;
#if MD_ACCURATE_OPN2
static ym3438_t opn2;
#endif
static uint8_t ym_addr[2];
static uint8_t ym_regs[2][256];
static uint8_t ym_busy;
static uint8_t ym_status_latch;
static uint16_t ym_status_decay;
static uint16_t ym_timer_a_interval;
static uint16_t ym_timer_a_counter;
static uint8_t ym_timer_a_enabled;
static uint8_t ym_timer_a_flag_enabled;
static uint8_t ym_timer_a_flag;
static uint8_t ym_timer_b_interval;
static uint8_t ym_timer_b_counter;
static uint8_t ym_timer_b_divider;
static uint8_t ym_timer_b_enabled;
static uint8_t ym_timer_b_flag_enabled;
static uint8_t ym_timer_b_flag;
static PsgTone psg_tone[3];
static PsgNoise psg_noise;
static uint8_t psg_latch;
static uint16_t z80_bank;
static int z80_bus_held;
static int z80_reset;
static int z80_irq_pending;
static uint32_t z80_cycle_acc;
static uint32_t psg_cycle_acc;
static uint32_t ym_cycle_acc;
static int16_t ym_last_sample;
#if MD_AUDIO_DEBUG
static unsigned long z80_cycles_total;
static unsigned long ym_write_count;
static unsigned long psg_write_count;
static unsigned long fast_key_count;
static unsigned long z80_ram_write_count;
static unsigned long z80_bank_write_count;
static unsigned long ym_status_read_count;
static uint16_t z80_ram_last_addr;
static uint8_t z80_ram_last_value;
static uint16_t z80_bank_last;
static uint8_t ym_last_reg;
static uint8_t ym_last_value;
#endif

typedef struct {
    uint32_t phase;
    uint32_t step;
    uint8_t key;
    uint8_t pan;
    uint8_t level;
} FastYmChannel;

static FastYmChannel fast_ym[6];
static uint8_t fast_dac_enabled;
static int16_t fast_dac_sample;

typedef struct {
    int status;
    unsigned char regs[14];
    unsigned short wregs[7];
    unsigned short alternates[4];
    int i, r, pc, iff1, iff2, im;
    unsigned char memory[1 << 16];
    uint8_t ym_addr[2];
    uint8_t ym_regs[2][256];
    uint8_t ym_busy;
    uint8_t ym_status_latch;
    uint16_t ym_status_decay;
    uint16_t ym_timer_a_interval;
    uint16_t ym_timer_a_counter;
    uint8_t ym_timer_a_enabled;
    uint8_t ym_timer_a_flag_enabled;
    uint8_t ym_timer_a_flag;
    uint8_t ym_timer_b_interval;
    uint8_t ym_timer_b_counter;
    uint8_t ym_timer_b_divider;
    uint8_t ym_timer_b_enabled;
    uint8_t ym_timer_b_flag_enabled;
    uint8_t ym_timer_b_flag;
    PsgTone tone[3];
    PsgNoise noise;
    uint8_t latch;
    uint16_t bank;
    int bus_held;
    int reset;
    int irq_pending;
    uint32_t z80_acc;
    uint32_t psg_acc;
    uint32_t ym_acc;
    int16_t ym_last;
    FastYmChannel fast[6];
    uint8_t dac_enabled;
    int16_t dac_sample;
} AudioState;

static const int16_t psg_volume[16] = {
    4096, 3254, 2584, 2053, 1631, 1295, 1029, 817,
     649,  516,  410,  325,  258, 205, 163, 0
};

static void psg_reset(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        psg_tone[i].counter = 1;
        psg_tone[i].tone = 0;
        psg_tone[i].output = 0;
        psg_tone[i].attenuation = 0x0F;
    }
    psg_noise.counter = 0;
    psg_noise.lfsr = 0x8000;
    psg_noise.counter_output = 0;
    psg_noise.lfsr_output = 0;
    psg_noise.reload_mode = 0;
    psg_noise.white_noise = 0;
    psg_noise.attenuation = 0x0F;
    psg_latch = PSG_TONE0;
}

static void psg_write_low(uint8_t data)
{
    switch (psg_latch) {
        case PSG_TONE0:
        case PSG_TONE1:
        case PSG_TONE2: {
            int ch = psg_latch >> 1;
            psg_tone[ch].tone = (uint16_t)((psg_tone[ch].tone & 0x03F0) | (data & 0x0F));
            break;
        }
        case PSG_VOL0:
        case PSG_VOL1:
        case PSG_VOL2:
            psg_tone[psg_latch >> 1].attenuation = data & 0x0F;
            break;
        case PSG_NOISE:
            psg_noise.reload_mode = data & 0x03;
            psg_noise.white_noise = (data & 0x04) ? 1 : 0;
            psg_noise.lfsr = 0x8000;
            break;
        case PSG_VOL3:
            psg_noise.attenuation = data & 0x0F;
            break;
    }
}

static void psg_write_high(uint8_t data)
{
    switch (psg_latch) {
        case PSG_TONE0:
        case PSG_TONE1:
        case PSG_TONE2: {
            int ch = psg_latch >> 1;
            psg_tone[ch].tone = (uint16_t)((psg_tone[ch].tone & 0x000F) | ((data & 0x3F) << 4));
            break;
        }
        default:
            psg_write_low(data);
            break;
    }
}

static void psg_shift_noise(void)
{
    uint8_t bit0 = psg_noise.lfsr & 1;
    uint8_t input;
    psg_noise.lfsr_output = bit0;
    input = psg_noise.white_noise ? (uint8_t)(((psg_noise.lfsr >> 0) ^ (psg_noise.lfsr >> 3)) & 1)
                                  : bit0;
    psg_noise.lfsr = (uint16_t)((psg_noise.lfsr >> 1) | ((uint16_t)input << 15));
}

static void psg_tick(void)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (--psg_tone[i].counter == 0) {
            psg_tone[i].counter = psg_tone[i].tone ? psg_tone[i].tone : 1;
            psg_tone[i].output ^= 1;
        }
    }

    if (psg_noise.counter)
        psg_noise.counter--;
    if (!psg_noise.counter) {
        uint16_t reload;
        switch (psg_noise.reload_mode) {
            default:
            case 0: reload = 0x10; break;
            case 1: reload = 0x20; break;
            case 2: reload = 0x40; break;
            case 3: reload = psg_tone[2].tone ? psg_tone[2].tone : 1; break;
        }
        psg_noise.counter = reload;
        psg_noise.counter_output ^= 1;
        if (psg_noise.counter_output)
            psg_shift_noise();
    }
}

static int16_t psg_sample(void)
{
    int i;
    int sample = 0;
    for (i = 0; i < 3; i++) {
        int vol = psg_volume[psg_tone[i].attenuation & 0x0F];
        sample += psg_tone[i].output ? vol : -vol;
    }
    {
        int vol = psg_volume[psg_noise.attenuation & 0x0F];
        sample += psg_noise.lfsr_output ? vol : -vol;
    }
    return (int16_t)(sample >> 2);
}

static int ym_channel_from_reg(int port, uint8_t reg)
{
    int c = reg & 3;
    if (c == 3)
        return -1;
    return (port & 1) ? c + 3 : c;
}

static void fast_ym_update_channel(int ch)
{
    int port, c, fnum, block, tl_min = 127, i;
    uint32_t hz;
    if (ch < 0 || ch >= 6)
        return;

    port = ch >= 3;
    c = ch - port * 3;
    fnum = ym_regs[port][0xA0 + c] | ((ym_regs[port][0xA4 + c] & 7) << 8);
    block = (ym_regs[port][0xA4 + c] >> 3) & 7;
    if (!fnum || !fast_ym[ch].key) {
        fast_ym[ch].step = 0;
    } else {
        hz = ((uint32_t)fnum * (1u << block) * 53267u) >> 20;
        if (hz < 1)
            hz = 1;
        fast_ym[ch].step = (uint32_t)(((uint64_t)hz << 24) / MD_SND_RATE);
    }

    for (i = 0; i < 4; i++) {
        int r = 0x40 + c + i * 4;
        int tl = ym_regs[port][r] & 0x7F;
        if (tl < tl_min)
            tl_min = tl;
    }
    fast_ym[ch].level = (uint8_t)((127 - tl_min) >> 1);
    if (fast_ym[ch].key && fnum && fast_ym[ch].level < 16)
        fast_ym[ch].level = 24;
    fast_ym[ch].pan = ym_regs[port][0xB4 + c] & 0xC0;
}

static void fast_ym_key_write(uint8_t v)
{
    int c = v & 3;
    int ch;
    if (c == 3)
        return;
    ch = c + ((v & 4) ? 3 : 0);
    fast_ym[ch].key = (v & 0xF0) ? 1 : 0;
    fast_ym_update_channel(ch);
}

static int16_t fast_ym_sample(void)
{
    int ch;
    int sample = 0;
    for (ch = 0; ch < 6; ch++) {
        int v, tri;
        if (fast_dac_enabled && ch == 5) {
            sample += fast_dac_sample >> 1;
            continue;
        }
        if (!fast_ym[ch].step || !fast_ym[ch].level)
            continue;
        fast_ym[ch].phase += fast_ym[ch].step;
        v = (fast_ym[ch].phase >> 16) & 0xFF;
        tri = (v < 128) ? v : 255 - v;
        sample += ((tri - 64) * fast_ym[ch].level) >> 1;
    }
    ym_last_sample = (int16_t)(((int)ym_last_sample * 3 + sample) >> 2);
    return ym_last_sample;
}

static uint8_t ym_read_status(uint16_t a)
{
    uint8_t status;

#if MD_AUDIO_DEBUG
    ym_status_read_count++;
#endif

    if ((a & 3) && ym_status_decay)
        return ym_status_latch;

    status = (uint8_t)((ym_busy ? 0x80 : 0x00) |
                       (ym_timer_b_flag ? 0x02 : 0x00) |
                       (ym_timer_a_flag ? 0x01 : 0x00));
    ym_status_latch = status;
    ym_status_decay = 12000;
    return status;
}

static void ym_timer_write(uint8_t reg, uint8_t v)
{
    switch (reg) {
        case 0x24:
            ym_timer_a_interval = (uint16_t)((ym_timer_a_interval & 3) | ((uint16_t)v << 2));
            break;
        case 0x25:
            ym_timer_a_interval = (uint16_t)((ym_timer_a_interval & ~3u) | (v & 3));
            break;
        case 0x26:
            ym_timer_b_interval = v;
            break;
        case 0x27:
            if (v & 0x10)
                ym_timer_a_flag = 0;
            if (v & 0x20)
                ym_timer_b_flag = 0;
            if (!ym_timer_a_enabled && (v & 0x01))
                ym_timer_a_counter = ym_timer_a_interval;
            if (!ym_timer_b_enabled && (v & 0x02)) {
                ym_timer_b_counter = ym_timer_b_interval;
                ym_timer_b_divider = 16;
            }
            ym_timer_a_enabled = v & 0x01;
            ym_timer_b_enabled = (v >> 1) & 0x01;
            ym_timer_a_flag_enabled = (v >> 2) & 0x01;
            ym_timer_b_flag_enabled = (v >> 3) & 0x01;
            break;
    }
}

static void ym_tick_status(void)
{
    if (ym_busy)
        ym_busy--;
    if (ym_status_decay)
        ym_status_decay--;

    if (ym_timer_a_enabled) {
        ym_timer_a_counter++;
        if (ym_timer_a_counter >= 1024) {
            ym_timer_a_counter = ym_timer_a_interval;
            if (ym_timer_a_flag_enabled)
                ym_timer_a_flag = 1;
        }
    }

    if (ym_timer_b_divider)
        ym_timer_b_divider--;
    if (!ym_timer_b_divider) {
        ym_timer_b_divider = 16;
        if (ym_timer_b_enabled) {
            uint8_t old = ym_timer_b_counter++;
            if (ym_timer_b_counter < old) {
                ym_timer_b_counter = ym_timer_b_interval;
                if (ym_timer_b_flag_enabled)
                    ym_timer_b_flag = 1;
            }
        }
    }
}

static void ym_write(int port, uint8_t reg, uint8_t v)
{
    int ch;
#if MD_AUDIO_DEBUG
    ym_write_count++;
    ym_last_reg = reg;
    ym_last_value = v;
#endif
    ym_busy = 32;
    ym_regs[port & 1][reg] = v;
    if ((port & 1) == 0 && reg >= 0x24 && reg <= 0x27)
        ym_timer_write(reg, v);
#if MD_ACCURATE_OPN2
    OPN2_Write(&opn2, (uint32_t)((port & 1) << 1), reg);
    OPN2_Write(&opn2, (uint32_t)(((port & 1) << 1) | 1), v);
#else
    if ((port & 1) == 0 && reg == 0x28) {
#if MD_AUDIO_DEBUG
        fast_key_count++;
#endif
        fast_ym_key_write(v);
        return;
    }
    if ((port & 1) == 0 && reg == 0x2A) {
        fast_dac_sample = ((int)v - 128) << 5;
        return;
    }
    if ((port & 1) == 0 && reg == 0x2B) {
        fast_dac_enabled = v & 0x80;
        return;
    }
    ch = ym_channel_from_reg(port, reg);
    if (ch >= 0 && ((reg >= 0x40 && reg <= 0x4E) ||
                    (reg >= 0xA0 && reg <= 0xA6) ||
                    (reg >= 0xB4 && reg <= 0xB6)))
        fast_ym_update_channel(ch);
#endif
}

static int16_t ym_sample(void)
{
#if MD_ACCURATE_OPN2
    int clocks = 0;
    int sample_acc = 0;
    Bit16s pins[2];

    ym_cycle_acc += MD_MASTER_CLOCK;
    while (ym_cycle_acc >= (MD_SND_RATE * MD_YM_DIVIDER)) {
        ym_cycle_acc -= (MD_SND_RATE * MD_YM_DIVIDER);
        OPN2_Clock(&opn2, pins);
        sample_acc += ((int)pins[0] + (int)pins[1]) >> 1;
        clocks++;
    }
    if (clocks)
        ym_last_sample = (int16_t)(sample_acc / clocks);
    return ym_last_sample;
#else
#if MD_FAST_FM
    return fast_ym_sample();
#else
    return 0;
#endif
#endif
}

static void run_z80_samples(int n)
{
    unsigned long total;
    int cycles;
#if !MD_RUN_Z80_SOUND
    (void)n;
    z80_irq_pending = 0;
    return;
#endif
    if (n <= 0 || z80_reset || z80_bus_held)
        return;
    if (z80_irq_pending) {
        Z80Interrupt(&z80.state, 0xFF, &z80);
        z80_irq_pending = 0;
    }
    total = (unsigned long)z80_cycle_acc + (unsigned long)n * MD_Z80_CLOCK;
    cycles = (int)(total / MD_SND_RATE);
    z80_cycle_acc = (uint32_t)(total % MD_SND_RATE);
    if (cycles > 0)
        Z80Emulate(&z80.state, cycles, &z80);
#if MD_AUDIO_DEBUG
    z80_cycles_total += (unsigned long)cycles;
#endif
}

void md_audio_init(void)
{
}

void md_audio_reset(void)
{
    memset(&z80, 0, sizeof z80);
    memset(ym_addr, 0, sizeof ym_addr);
    memset(ym_regs, 0, sizeof ym_regs);
    ym_busy = 0;
    ym_status_latch = 0;
    ym_status_decay = 0;
    ym_timer_a_interval = 0;
    ym_timer_a_counter = 0;
    ym_timer_a_enabled = 0;
    ym_timer_a_flag_enabled = 0;
    ym_timer_a_flag = 0;
    ym_timer_b_interval = 0;
    ym_timer_b_counter = 0;
    ym_timer_b_divider = 16;
    ym_timer_b_enabled = 0;
    ym_timer_b_flag_enabled = 0;
    ym_timer_b_flag = 0;
    memset(fast_ym, 0, sizeof fast_ym);
    Z80Reset(&z80.state);
#if MD_ACCURATE_OPN2
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&opn2);
#endif
    psg_reset();
    z80_bus_held = 0;
    z80_reset = 1;
    z80_irq_pending = 0;
    z80_bank = 0;
    z80_cycle_acc = 0;
    psg_cycle_acc = 0;
    ym_cycle_acc = 0;
    ym_last_sample = 0;
#if MD_AUDIO_DEBUG
    z80_cycles_total = 0;
    ym_write_count = 0;
    psg_write_count = 0;
    fast_key_count = 0;
    z80_ram_write_count = 0;
    z80_bank_write_count = 0;
    ym_status_read_count = 0;
    z80_ram_last_addr = 0;
    z80_ram_last_value = 0;
    z80_bank_last = 0;
    ym_last_reg = 0;
    ym_last_value = 0;
#endif
    fast_dac_enabled = 0;
    fast_dac_sample = 0;
}

unsigned md_audio_state_size(void)
{
    return sizeof(AudioState);
}

void md_audio_state_save(void *out)
{
    AudioState *s = (AudioState *)out;
    s->status = z80.state.status;
    memcpy(s->regs, z80.state.registers.byte, sizeof s->regs);
    memcpy(s->wregs, z80.state.registers.word, sizeof s->wregs);
    memcpy(s->alternates, z80.state.alternates, sizeof s->alternates);
    s->i = z80.state.i;
    s->r = z80.state.r;
    s->pc = z80.state.pc;
    s->iff1 = z80.state.iff1;
    s->iff2 = z80.state.iff2;
    s->im = z80.state.im;
    memcpy(s->memory, z80.memory, sizeof s->memory);
    memcpy(s->ym_addr, ym_addr, sizeof s->ym_addr);
    memcpy(s->ym_regs, ym_regs, sizeof s->ym_regs);
    s->ym_busy = ym_busy;
    s->ym_status_latch = ym_status_latch;
    s->ym_status_decay = ym_status_decay;
    s->ym_timer_a_interval = ym_timer_a_interval;
    s->ym_timer_a_counter = ym_timer_a_counter;
    s->ym_timer_a_enabled = ym_timer_a_enabled;
    s->ym_timer_a_flag_enabled = ym_timer_a_flag_enabled;
    s->ym_timer_a_flag = ym_timer_a_flag;
    s->ym_timer_b_interval = ym_timer_b_interval;
    s->ym_timer_b_counter = ym_timer_b_counter;
    s->ym_timer_b_divider = ym_timer_b_divider;
    s->ym_timer_b_enabled = ym_timer_b_enabled;
    s->ym_timer_b_flag_enabled = ym_timer_b_flag_enabled;
    s->ym_timer_b_flag = ym_timer_b_flag;
    memcpy(s->tone, psg_tone, sizeof s->tone);
    s->noise = psg_noise;
    s->latch = psg_latch;
    s->bank = z80_bank;
    s->bus_held = z80_bus_held;
    s->reset = z80_reset;
    s->irq_pending = z80_irq_pending;
    s->z80_acc = z80_cycle_acc;
    s->psg_acc = psg_cycle_acc;
    s->ym_acc = ym_cycle_acc;
    s->ym_last = ym_last_sample;
    memcpy(s->fast, fast_ym, sizeof s->fast);
    s->dac_enabled = fast_dac_enabled;
    s->dac_sample = fast_dac_sample;
}

void md_audio_state_load(const void *in)
{
    const AudioState *s = (const AudioState *)in;
    Z80Reset(&z80.state);
    z80.state.status = s->status;
    memcpy(z80.state.registers.byte, s->regs, sizeof s->regs);
    memcpy(z80.state.registers.word, s->wregs, sizeof s->wregs);
    memcpy(z80.state.alternates, s->alternates, sizeof s->alternates);
    z80.state.i = s->i;
    z80.state.r = s->r;
    z80.state.pc = s->pc;
    z80.state.iff1 = s->iff1;
    z80.state.iff2 = s->iff2;
    z80.state.im = s->im;
    memcpy(z80.memory, s->memory, sizeof s->memory);
    memcpy(ym_addr, s->ym_addr, sizeof ym_addr);
    memcpy(ym_regs, s->ym_regs, sizeof ym_regs);
    ym_busy = s->ym_busy;
    ym_status_latch = s->ym_status_latch;
    ym_status_decay = s->ym_status_decay;
    ym_timer_a_interval = s->ym_timer_a_interval;
    ym_timer_a_counter = s->ym_timer_a_counter;
    ym_timer_a_enabled = s->ym_timer_a_enabled;
    ym_timer_a_flag_enabled = s->ym_timer_a_flag_enabled;
    ym_timer_a_flag = s->ym_timer_a_flag;
    ym_timer_b_interval = s->ym_timer_b_interval;
    ym_timer_b_counter = s->ym_timer_b_counter;
    ym_timer_b_divider = s->ym_timer_b_divider ? s->ym_timer_b_divider : 16;
    ym_timer_b_enabled = s->ym_timer_b_enabled;
    ym_timer_b_flag_enabled = s->ym_timer_b_flag_enabled;
    ym_timer_b_flag = s->ym_timer_b_flag;
    memcpy(psg_tone, s->tone, sizeof psg_tone);
    psg_noise = s->noise;
    psg_latch = s->latch;
    z80_bank = s->bank;
    z80_bus_held = s->bus_held;
    z80_reset = s->reset;
    z80_irq_pending = s->irq_pending;
    z80_cycle_acc = s->z80_acc;
    psg_cycle_acc = s->psg_acc;
    ym_cycle_acc = s->ym_acc;
    ym_last_sample = s->ym_last;
    memcpy(fast_ym, s->fast, sizeof fast_ym);
    fast_dac_enabled = s->dac_enabled;
    fast_dac_sample = s->dac_sample;
#if MD_ACCURATE_OPN2
    OPN2_Reset(&opn2);
#endif
}

void md_audio_render(signed char *out, int n)
{
    int i;
    if (!out || n <= 0)
        return;

#if !MD_ACCURATE_OPN2 && !MD_FAST_FM
    run_z80_samples(n);
#endif

    for (i = 0; i < n; i++) {
        int sample;

#if MD_ACCURATE_OPN2 || MD_FAST_FM
        if (!z80_reset && !z80_bus_held) {
            int cycles;
            if (z80_irq_pending) {
                Z80Interrupt(&z80.state, 0xFF, &z80);
                z80_irq_pending = 0;
            }
            z80_cycle_acc += MD_Z80_CLOCK;
            cycles = (int)(z80_cycle_acc / MD_SND_RATE);
            z80_cycle_acc %= MD_SND_RATE;
            if (cycles > 0) {
                Z80Emulate(&z80.state, cycles, &z80);
#if MD_AUDIO_DEBUG
                z80_cycles_total += (unsigned long)cycles;
#endif
            }
        }
#endif

        psg_cycle_acc += MD_PSG_TICK;
        ym_tick_status();
        while (psg_cycle_acc >= MD_SND_RATE) {
            psg_cycle_acc -= MD_SND_RATE;
            psg_tick();
        }

        sample = ((int)psg_sample() << 1) + (int)ym_sample();
        sample >>= 7;
        if (sample > 127) sample = 127;
        if (sample < -128) sample = -128;
        out[i] = (signed char)sample;
    }
}

uint8_t md_z80_area_read(uint16_t a)
{
    a &= 0x7FFF;
    if (a < 0x4000)
    {
        if ((a & 0x1FFF) == 0x1FFD)
            return (uint8_t)(z80.memory[a & 0x1FFF] & 0x7F);
        return z80.memory[a & 0x1FFF];
    }
#if MD_ACCURATE_OPN2
    if (a >= 0x4000 && a <= 0x5FFF)
        return OPN2_Read(&opn2, a & 3);
#else
    if (a >= 0x4000 && a <= 0x5FFF)
        return ym_read_status(a);
#endif
    return 0xFF;
}

void md_z80_area_write(uint16_t a, uint8_t v)
{
    a &= 0x7FFF;
    if (a < 0x4000) {
        z80.memory[a & 0x1FFF] = v;
        /* OutRun's 68k command byte lands at 1c0a, while its uploaded Z80
         * driver polls 1c4f before starting the DAC stream. */
        if ((a & 0x1FFF) == 0x1C0A)
            z80.memory[0x1C4F] = v;
#if MD_AUDIO_DEBUG
        z80_ram_write_count++;
        z80_ram_last_addr = a & 0x1FFF;
        z80_ram_last_value = v;
#endif
        return;
    }
    if (a >= 0x4000 && a <= 0x5FFF) {
        int reg = a & 3;
        if (reg == 0)
            ym_addr[0] = v;
        else if (reg == 2)
            ym_addr[1] = v;
        else
            ym_write(reg >> 1, ym_addr[reg >> 1], v);
        return;
    }
    if (a >= 0x6000 && a <= 0x60FF) {
        z80_bank = (uint16_t)((z80_bank >> 1) | ((uint16_t)(v & 1) << 8));
#if MD_AUDIO_DEBUG
        z80_bank_write_count++;
        z80_bank_last = z80_bank;
#endif
        return;
    }
    if (a == 0x7F11)
        md_psg_write(v);
}

void md_z80_busreq(int held)
{
    z80_bus_held = held ? 1 : 0;
}

void md_z80_set_reset(int asserted)
{
    int was_reset = z80_reset;
    z80_reset = asserted ? 1 : 0;
    if (z80_reset && !was_reset) {
        Z80Reset(&z80.state);
        memset(ym_addr, 0, sizeof ym_addr);
        memset(ym_regs, 0, sizeof ym_regs);
        memset(fast_ym, 0, sizeof fast_ym);
#if MD_ACCURATE_OPN2
        OPN2_Reset(&opn2);
#endif
        ym_cycle_acc = 0;
        ym_last_sample = 0;
        fast_dac_enabled = 0;
        fast_dac_sample = 0;
    }
}

int md_z80_bus_free(void)
{
    return z80_bus_held ? 0 : 1;
}

void md_psg_write(uint8_t v)
{
#if MD_AUDIO_DEBUG
    psg_write_count++;
#endif
    if (v & 0x80) {
        psg_latch = (uint8_t)((v >> 4) & 7);
        psg_write_low(v);
    } else {
        psg_write_high(v);
    }
}

void md_audio_vblank(void)
{
    z80_irq_pending = 1;
}

void md_audio_debug(int *reset, int *bus, int *pc,
                    unsigned long *cycles,
                    unsigned long *ym_writes,
                    unsigned long *psg_writes,
                    unsigned long *key_writes,
                    unsigned *active_mask)
{
    int i;
    unsigned mask = 0;
    if (reset) *reset = z80_reset;
    if (bus) *bus = z80_bus_held;
    if (pc) *pc = z80.state.pc;
#if MD_AUDIO_DEBUG
    if (cycles) *cycles = z80_cycles_total;
    if (ym_writes) *ym_writes = ym_write_count;
    if (psg_writes) *psg_writes = psg_write_count;
    if (key_writes) *key_writes = fast_key_count;
#else
    if (cycles) *cycles = 0;
    if (ym_writes) *ym_writes = 0;
    if (psg_writes) *psg_writes = 0;
    if (key_writes) *key_writes = 0;
#endif
    for (i = 0; i < 6; i++) {
        if (fast_ym[i].key && fast_ym[i].step && fast_ym[i].level)
            mask |= 1u << i;
    }
    if (active_mask) *active_mask = mask;
}

void md_audio_debug_ext(unsigned long *z80_ram_writes,
                        unsigned *last_z80_ram_addr,
                        unsigned *last_z80_ram_value,
                        unsigned long *z80_bank_writes,
                        unsigned *last_z80_bank,
                        unsigned long *ym_status_reads,
                        unsigned *last_ym_reg,
                        unsigned *last_ym_value)
{
#if MD_AUDIO_DEBUG
    if (z80_ram_writes) *z80_ram_writes = z80_ram_write_count;
    if (last_z80_ram_addr) *last_z80_ram_addr = z80_ram_last_addr;
    if (last_z80_ram_value) *last_z80_ram_value = z80_ram_last_value;
    if (z80_bank_writes) *z80_bank_writes = z80_bank_write_count;
    if (last_z80_bank) *last_z80_bank = z80_bank_last;
    if (ym_status_reads) *ym_status_reads = ym_status_read_count;
    if (last_ym_reg) *last_ym_reg = ym_last_reg;
    if (last_ym_value) *last_ym_value = ym_last_value;
#else
    if (z80_ram_writes) *z80_ram_writes = 0;
    if (last_z80_ram_addr) *last_z80_ram_addr = 0;
    if (last_z80_ram_value) *last_z80_ram_value = 0;
    if (z80_bank_writes) *z80_bank_writes = 0;
    if (last_z80_bank) *last_z80_bank = 0;
    if (ym_status_reads) *ym_status_reads = 0;
    if (last_ym_reg) *last_ym_reg = 0;
    if (last_ym_value) *last_ym_value = 0;
#endif
}

uint8_t machine_rd(MY_LITTLE_Z80 *ctx, unsigned int a)
{
    (void)ctx;
    a &= 0xFFFF;
    if (a < 0x4000)
        return z80.memory[a & 0x1FFF];
    if (a >= 0x4000 && a <= 0x5FFF)
        return md_z80_area_read((uint16_t)a);
    if (a >= 0x8000) {
        uint32_t m68k_addr = ((uint32_t)z80_bank << 15) | (a & 0x7FFF);
        if ((m68k_addr & 0xFF0000) == 0xA00000 || m68k_addr >= 0xE00000)
            return 0xFF;
        return md_read8(m68k_addr);
    }
    return 0xFF;
}

void machine_wr(MY_LITTLE_Z80 *ctx, unsigned int a, unsigned char v)
{
    (void)ctx;
    a &= 0xFFFF;
    if (a < 0x4000) {
        z80.memory[a & 0x1FFF] = v;
        return;
    }
    if (a >= 0x4000 && a <= 0x5FFF) {
        md_z80_area_write((uint16_t)a, v);
        return;
    }
    if (a >= 0x6000 && a <= 0x60FF) {
        z80_bank = (uint16_t)((z80_bank >> 1) | ((uint16_t)(v & 1) << 8));
#if MD_AUDIO_DEBUG
        z80_bank_write_count++;
        z80_bank_last = z80_bank;
#endif
        return;
    }
    if (a == 0x7F11) {
        md_psg_write(v);
        return;
    }
    if (a >= 0x8000) {
        uint32_t m68k_addr = ((uint32_t)z80_bank << 15) | (a & 0x7FFF);
        if ((m68k_addr & 0xFF0000) != 0xA00000)
            md_write8(m68k_addr, v);
    }
}

unsigned char in_impl(MY_LITTLE_Z80 *ctx, int port)
{
    (void)ctx;
    (void)port;
    return 0xFF;
}

void out_impl(MY_LITTLE_Z80 *ctx, int port, unsigned char v)
{
    (void)ctx;
    if ((port & 0xFF) == 0x7F)
        md_psg_write(v);
}
