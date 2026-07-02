/*
 * md_audio_amiga.c - Paula playback for the Mega Drive audio stream.
 *
 * AUD0/AUD1 play one mono ring buffer in phase. The ring is kept several frames
 * ahead of Paula so RTG frame jitter does not cause tiny stale loops.
 */

#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdint.h>

#include "hal/md_machine.h"

#ifndef MD_ENABLE_AUDIO
#define MD_ENABLE_AUDIO 0
#endif

#if MD_ENABLE_AUDIO
#define CUSTOM    ((volatile uint16_t *)0xdff000)
#define R_DMACON  (0x096/2)
#define R_AUD0LCH (0x0a0/2)
#define R_AUD0LEN (0x0a4/2)
#define R_AUD0PER (0x0a6/2)
#define R_AUD0VOL (0x0a8/2)
#define R_AUD1LCH (0x0b0/2)
#define R_AUD1LEN (0x0b4/2)
#define R_AUD1PER (0x0b6/2)
#define R_AUD1VOL (0x0b8/2)
#define R_AUD2LEN (0x0c4/2)
#define R_AUD2VOL (0x0c8/2)
#define R_AUD3LEN (0x0d4/2)
#define R_AUD3VOL (0x0d8/2)

#define MD_PAULA_PER (3546895 / MD_SND_RATE)
#define MD_SPF       (MD_SND_RATE / 60 + 64)
#define MD_LEAD_FR   3
#define MD_RING_FR   24
#define MD_LEAD      (MD_LEAD_FR * MD_SPF)
#define MD_RING      (MD_RING_FR * MD_SPF)

static signed char *ring;
static unsigned long p_play;
static unsigned long p_wrote;
static int audio_paused;

static void aud_setup(volatile uint16_t *c)
{
    uint32_t a = (uint32_t)ring;
    c[R_AUD0LCH] = (uint16_t)(a >> 16);
    c[R_AUD0LCH + 1] = (uint16_t)a;
    c[R_AUD1LCH] = (uint16_t)(a >> 16);
    c[R_AUD1LCH + 1] = (uint16_t)a;
    c[R_AUD0LEN] = MD_RING / 2;
    c[R_AUD1LEN] = MD_RING / 2;
    c[R_AUD0PER] = MD_PAULA_PER;
    c[R_AUD1PER] = MD_PAULA_PER;
    c[R_AUD0VOL] = 48;
    c[R_AUD1VOL] = 48;
}

static void ring_render(unsigned long n)
{
    while (n) {
        unsigned long pos = p_wrote % MD_RING;
        unsigned long chunk = MD_RING - pos;
        if (chunk > n)
            chunk = n;
        md_audio_render(ring + pos, (int)chunk);
        p_wrote += chunk;
        n -= chunk;
    }
}

void md_audio_amiga_close(void)
{
    volatile uint16_t *c = CUSTOM;
    c[R_DMACON] = 0x000f;
    c[R_AUD0VOL] = 0;
    c[R_AUD1VOL] = 0;
    c[R_AUD2VOL] = 0;
    c[R_AUD3VOL] = 0;
    for (volatile int i = 0; i < 20000; i++) { }
    if (ring) {
        FreeMem(ring, MD_RING);
        ring = 0;
    }
    p_play = p_wrote = 0;
    audio_paused = 0;
}

void md_audio_amiga_open(void)
{
    volatile uint16_t *c = CUSTOM;
    if (ring)
        md_audio_amiga_close();

    ring = (signed char *)AllocMem(MD_RING, MEMF_CHIP | MEMF_CLEAR);
    if (!ring)
        return;

    p_play = p_wrote = 0;
    audio_paused = 0;
    c[R_DMACON] = 0x000f;
    c[R_AUD0VOL] = 0;
    c[R_AUD1VOL] = 0;
    c[R_AUD2VOL] = 0;
    c[R_AUD3VOL] = 0;
    c[R_AUD2LEN] = 0;
    c[R_AUD3LEN] = 0;
    aud_setup(c);
    c[R_DMACON] = 0x8203;
}

void md_audio_amiga_frame(void)
{
    unsigned long target, cap;
    if (!ring || audio_paused)
        return;

    p_play += MD_SND_RATE / 60;

    target = p_play + MD_LEAD;
    cap = p_play + (MD_RING - MD_SPF);
    if (target > cap)
        target = cap;
    if ((long)(target - p_wrote) > 0) {
        ring_render(target - p_wrote);
        CacheClearU();
    }
}

void md_audio_amiga_pause(int paused)
{
    volatile uint16_t *c = CUSTOM;
    audio_paused = paused ? 1 : 0;
    if (!ring)
        return;
    c[R_AUD0VOL] = audio_paused ? 0 : 48;
    c[R_AUD1VOL] = audio_paused ? 0 : 48;
    if (!audio_paused) {
        p_wrote = p_play;
        ring_render(MD_LEAD);
        CacheClearU();
    }
}
#else
void md_audio_amiga_close(void) { }
void md_audio_amiga_open(void) { }
void md_audio_amiga_frame(void) { }
void md_audio_amiga_pause(int paused) { (void)paused; }
#endif
