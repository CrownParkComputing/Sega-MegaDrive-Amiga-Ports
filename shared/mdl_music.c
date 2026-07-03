/* mdl_music.c - VGM intro-music engine for the loader (platform-agnostic).
 *
 * Plays arcade-style VGMs (YM2151 via ymfm + SegaPCM) at MD_SND_RATE for the
 * Paula ring. The .vgz must be gunzipped to a plain .vgm at package time.
 * Chips generate at their native rates (clock/64 and clock/128) and are
 * box-averaged down, VGM waits run on a 44100Hz accumulator, and the stream
 * honours the header loop point. Unsupported commands are skipped so a
 * Mega Drive VGM won't crash — it just plays whatever the YM2151 was given.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUS_RATE 16574

void ymfm2151_init(void);
void ymfm2151_reset(void);
void ymfm2151_write(unsigned int reg, unsigned int val);
void ymfm2151_generate(int *out, int n);

void mdl_segapcm_init(const uint8_t *rom, uint32_t romsize, uint32_t iface);
void mdl_segapcm_reset(void);
void mdl_segapcm_write(uint32_t addr, uint8_t v);
void mdl_segapcm_render(int32_t *l, int32_t *r);

static uint8_t *vgm;
static uint32_t vgm_len;
static uint32_t data_off, loop_off;
static uint32_t ym_rate, pcm_rate;      /* native samples per second */
static uint8_t *pcm_rom;
static uint32_t pcm_rom_size;
static uint32_t pcm_iface;

static uint32_t pos;
static uint32_t wait_left;              /* pending wait, 44100Hz ticks */
static uint32_t vgm_acc, ym_acc, pcm_acc;
static int music_ok;

void mus_restart(void);

static uint32_t rd32(uint32_t o)
{
    return (uint32_t)vgm[o] | ((uint32_t)vgm[o + 1] << 8)
         | ((uint32_t)vgm[o + 2] << 16) | ((uint32_t)vgm[o + 3] << 24);
}

static void handle_block(void)
{
    uint8_t type = vgm[pos + 2];
    uint32_t sz = rd32(pos + 3);
    if (type == 0x80 && sz >= 8) {      /* SegaPCM ROM image chunk */
        uint32_t total = rd32(pos + 7);
        uint32_t start = rd32(pos + 11);
        if (!pcm_rom && total && total <= 4u * 1024 * 1024) {
            pcm_rom = (uint8_t *)malloc(total);
            if (pcm_rom) {
                memset(pcm_rom, 0x80, total);   /* silence */
                pcm_rom_size = total;
            }
        }
        if (pcm_rom && start < pcm_rom_size) {
            uint32_t n = sz - 8;
            if (start + n > pcm_rom_size)
                n = pcm_rom_size - start;
            memcpy(pcm_rom + start, vgm + pos + 15, n);
            /* (re)arm the chip now the ROM exists — registers are kept */
            mdl_segapcm_init(pcm_rom, pcm_rom_size, pcm_iface);
        }
    }
    pos += 7 + sz;
}

static void step_commands(void)
{
    while (!wait_left) {
        uint8_t op;
        if (pos >= vgm_len) {
            pos = loop_off;
            if (pos >= vgm_len) { music_ok = 0; return; }
        }
        op = vgm[pos];
        if (op == 0x54) {
            ymfm2151_write(vgm[pos + 1], vgm[pos + 2]);
            pos += 3;
        } else if (op == 0xC0) {
            mdl_segapcm_write((uint32_t)(vgm[pos + 1] | (vgm[pos + 2] << 8)),
                              vgm[pos + 3]);
            pos += 4;
        } else if ((op & 0xF0) == 0x70) {
            wait_left = (uint32_t)(op & 15) + 1;
            pos += 1;
        } else if (op == 0x61) {
            wait_left = (uint32_t)(vgm[pos + 1] | (vgm[pos + 2] << 8));
            pos += 3;
        } else if (op == 0x62) {
            wait_left = 735;
            pos += 1;
        } else if (op == 0x63) {
            wait_left = 882;
            pos += 1;
        } else if (op == 0x66) {
            pos = loop_off;
            if (!loop_off || pos >= vgm_len) { music_ok = 0; return; }
        } else if (op == 0x67) {
            handle_block();
        } else if (op == 0x50) {
            pos += 2;                   /* PSG (unsupported here) */
        } else if (op == 0x52 || op == 0x53) {
            pos += 3;                   /* YM2612 (unsupported here) */
        } else if ((op & 0xF0) == 0x80) {
            wait_left = (uint32_t)(op & 15);    /* YM2612 DAC+wait: wait only */
            pos += 1;
        } else if (op == 0xE0) {
            pos += 5;
        } else if (op == 0x4F) {
            pos += 2;
        } else {
            music_ok = 0;               /* unknown opcode: stop cleanly */
            return;
        }
    }
}

int mus_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t ver, ymclk, pcmclk;
    long n;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x40) { fclose(f); return 0; }
    vgm = (uint8_t *)malloc((size_t)n);
    if (!vgm) { fclose(f); return 0; }
    if (fread(vgm, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    vgm_len = (uint32_t)n;

    if (memcmp(vgm, "Vgm ", 4) != 0)
        return 0;
    ver = rd32(8);
    ymclk = rd32(0x30) & 0x3FFFFFFF;
    data_off = (ver >= 0x150) ? rd32(0x34) + 0x34 : 0x40;
    loop_off = rd32(0x1C);
    loop_off = loop_off ? loop_off + 0x1C : data_off;
    pcmclk = (ver >= 0x151 && vgm_len > 0x40) ? rd32(0x38) & 0x3FFFFFFF : 0;
    pcm_iface = (ver >= 0x151) ? rd32(0x3C) : 0;

    if (!ymclk)
        return 0;                       /* only OPM tunes supported */
    ym_rate = ymclk / 64;
    pcm_rate = pcmclk ? pcmclk / 128 : 0;

    ymfm2151_init();
    mus_restart();
    return 1;
}

void mus_restart(void)
{
    if (!vgm)
        return;
    ymfm2151_reset();
    mdl_segapcm_reset();
    if (pcm_rom)
        mdl_segapcm_init(pcm_rom, pcm_rom_size, pcm_iface);
    pos = data_off;
    wait_left = 0;
    vgm_acc = ym_acc = pcm_acc = 0;
    music_ok = 1;
}

int mus_active(void)
{
    return music_ok;
}

void mus_render(signed char *out, int n)
{
    int i;
    if (!music_ok) {
        memset(out, 0, (size_t)n);
        return;
    }
    for (i = 0; i < n; i++) {
        int fmbuf[2 * 8];
        int32_t sample = 0;
        uint32_t k;

        vgm_acc += 44100;
        while (vgm_acc >= MUS_RATE && music_ok) {
            vgm_acc -= MUS_RATE;
            step_commands();
            if (wait_left)
                wait_left--;
        }

        ym_acc += ym_rate;
        k = ym_acc / MUS_RATE;
        if (k) {
            uint32_t j;
            int32_t acc = 0;
            ym_acc -= k * MUS_RATE;
            if (k > 8) k = 8;
            ymfm2151_generate(fmbuf, (int)k);
            for (j = 0; j < k; j++)
                acc += fmbuf[2 * j] + fmbuf[2 * j + 1];
            sample += acc / (int32_t)(2 * k);
        }

        if (pcm_rate && pcm_rom) {
            int32_t l = 0, r = 0, cnt = 0;
            pcm_acc += pcm_rate;
            while (pcm_acc >= MUS_RATE) {
                pcm_acc -= MUS_RATE;
                mdl_segapcm_render(&l, &r);
                cnt++;
            }
            if (cnt)
                sample += (l + r) / (2 * cnt * 8);
        }

        sample >>= 7;
        if (sample > 127) sample = 127;
        if (sample < -128) sample = -128;
        out[i] = (signed char)sample;
    }
}
