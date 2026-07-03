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
#include <string.h>

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

#define MD_PAULA_PER  (3546895 / MD_SND_RATE)
#define MD_PAULA_RATE (3546895 / MD_PAULA_PER)   /* true consumption rate ~16574 Hz */
#define MD_SPF        (MD_SND_RATE / 60 + 64)
#define MD_LEAD_FR    8
#define MD_RING_FR    24
#define MD_LEAD       (MD_LEAD_FR * MD_SPF)
#define MD_RING       (MD_RING_FR * MD_SPF)

static signed char *ring;
static unsigned long p_play;
static unsigned long p_wrote;
static int audio_paused;
static unsigned long last_eclk;
static unsigned long long eclk_acc;
static int have_clk;

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
    have_clk = 0;
    eclk_acc = 0;
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

/* Advance the play cursor by WALL CLOCK, not by displayed frames: Paula
 * consumes at a fixed real rate, so frame-counted pacing garbles audio (stale
 * ring loops) the moment the loop drops below 60 fps. eclk_rate==0 falls back
 * to the old frame-counted advance. */
void md_audio_amiga_frame(unsigned long eclk_now, unsigned long eclk_rate)
{
    unsigned long target, cap, delta, adv;
    if (!ring || audio_paused) {
        have_clk = 0;
        return;
    }

    if (eclk_rate) {
        delta = have_clk ? (eclk_now - last_eclk) : eclk_rate / 60;
        if (delta > eclk_rate / 4)      /* stall/debugger clamp: 0.25s max */
            delta = eclk_rate / 60;
        last_eclk = eclk_now;
        have_clk = 1;
        eclk_acc += (unsigned long long)delta * MD_PAULA_RATE;
        adv = (unsigned long)(eclk_acc / eclk_rate);
        eclk_acc %= eclk_rate;
    } else {
        adv = MD_SND_RATE / 60;
    }
    p_play += adv;

    /* deep underrun: samples before p_play were already consumed. Silence the
     * ring before resyncing — otherwise Paula audibly replays last lap's
     * stale content ("looping music") until fresh data lands. */
    if ((long)(p_play - p_wrote) > 0) {
        memset(ring, 0, MD_RING);
        p_wrote = p_play;
    }

    target = p_play + MD_LEAD;
    cap = p_play + (MD_RING - MD_SPF);
    if (target > cap)
        target = cap;
    if ((long)(target - p_wrote) > 0)
        ring_render(target - p_wrote);

    /* silence the band just ahead of the write head: when a long frame lets
     * Paula graze past it, the transient plays as a clean micro-dropout
     * instead of 24-frame-old ring content (audible as "reverb" on speech) */
    {
        unsigned long gpos = p_wrote % MD_RING;
        unsigned long glen = MD_SPF;
        if (gpos + glen <= MD_RING) {
            memset(ring + gpos, 0, glen);
        } else {
            memset(ring + gpos, 0, MD_RING - gpos);
            memset(ring, 0, glen - (MD_RING - gpos));
        }
    }
    /* data-cache flush only: CacheClearU also clears the instruction
     * cache, which forces UAE JIT to retranslate everything each frame */
    CacheClearE(ring, MD_RING, CACRF_ClearD);
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
        CacheClearE(ring, MD_RING, CACRF_ClearD);
    }
}
#else
void md_audio_amiga_close(void) { }
void md_audio_amiga_open(void) { }
void md_audio_amiga_frame(unsigned long eclk_now, unsigned long eclk_rate) { (void)eclk_now; (void)eclk_rate; }
void md_audio_amiga_pause(int paused) { (void)paused; }
#endif
