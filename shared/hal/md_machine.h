/*
 * md_machine.h - Sega Megadrive machine model (Revenge of Shinobi port)
 *
 * Family-standard layout: one global machine, plain arrays for the hot
 * memories (no pointer indirection on the 68030), per-line frame loop.
 *
 * CPU:   Musashi 68000 @ 7.670454 MHz (NTSC MCLK/7), 3420 MCLK per line,
 *        262 lines -> 488.57 68k cycles/line, 59.92 Hz.
 * Video: VDP mode 5, H40 320x224 (H32 256 supported, centred).
 * Sound: Z80 @ 3.579545 MHz + YM2612/OPN2 + SN76489 PSG,
 *        pulled from the Paula ring like 1943/StDragon.
 */

#ifndef MD_MACHINE_H
#define MD_MACHINE_H

#include <stdint.h>

#define MD_ROM_MAX      0x400000        /* full 68k cartridge window */
#define MD_SCREEN_W     320
#define MD_SCREEN_H     224
#define MD_LINES        262             /* NTSC */
#define MD_ACTIVE_LINES 224

/* ---- memories (md_machine.c) ---- */
extern uint8_t  md_rom[MD_ROM_MAX];
extern uint32_t md_rom_size;
extern uint8_t  md_ram[0x10000];        /* 68k work RAM (0xFF0000-0xFFFFFF) */

/* pen buffer written by the per-line renderer: CRAM index 0-63 per pixel */
extern uint8_t  md_screen[MD_SCREEN_H][MD_SCREEN_W];

/* ---- pad input, set by the frontend before each frame ----
 * bit0 Up, 1 Down, 2 Left, 3 Right, 4 A, 5 B, 6 C, 7 Start (1 = pressed) */
extern uint16_t md_pad[2];

/* ---- machine control ---- */
void md_machine_init(void);             /* call after ROM is loaded */
void md_machine_reset(void);
void md_run_frame(void);                /* one video frame (renders md_screen) */
void md_run_frame_ex(int render_video); /* one frame, optionally skip VDP render */
int  md_load_rom(const char *path);      /* loads .bin/.gen, basic .smd */
int  md_save_state(const char *path);
int  md_load_state(const char *path);

/* 68k bus (also used by VDP DMA) */
uint8_t  md_read8(uint32_t a);
uint16_t md_read16(uint32_t a);
void     md_write8(uint32_t a, uint8_t v);
void     md_write16(uint32_t a, uint16_t v);

/* ---- VDP (md_vdp.c) ---- */
extern uint8_t  md_vram[0x10000];
extern uint16_t md_cram[64];
extern uint16_t md_vsram[40];
extern uint8_t  md_vdp_reg[32];
extern int      md_palette_dirty;       /* set on CRAM write; frontend clears */
extern int      md_vdp_line;            /* current scanline (frame loop owns) */

void     md_vdp_reset(void);
void     md_vdp_state_save(void *out);
void     md_vdp_state_load(const void *in);
unsigned md_vdp_state_size(void);
void     md_vdp_write_control(uint16_t v);
void     md_vdp_write_data(uint16_t v);
uint16_t md_vdp_read_data(void);
uint16_t md_vdp_read_status(void);
uint16_t md_vdp_hv_counter(void);
void     md_vdp_render_line(int line, uint8_t *dst);   /* dst = 320 pens */

/* 9-bit BGR CRAM -> 0x00RRGGBB, 64 entries */
void     md_vdp_palette(uint32_t out[64]);

/* ---- audio (md_audio.c) ---- */
#define MD_SND_RATE 16574               /* Paula PAL period 214 */

void md_audio_init(void);
void md_audio_reset(void);
void md_audio_state_save(void *out);
void md_audio_state_load(const void *in);
unsigned md_audio_state_size(void);
/* Render n signed 8-bit samples and advance the sound side (Z80+FM+PSG)
 * by exactly that much time. Called from the Paula ring refill. */
void md_audio_render(signed char *out, int n);

/* 68k-side hooks into the sound subsystem (md_machine.c bus routes here) */
uint8_t md_z80_area_read(uint16_t a);           /* 68k reads 0xA00000+ */
void    md_z80_area_write(uint16_t a, uint8_t v);
void    md_z80_busreq(int held);                /* 0xA11100 */
void    md_z80_set_reset(int asserted);         /* 0xA11200 */
int     md_z80_bus_free(void);                  /* for busreq readback */
void    md_psg_write(uint8_t v);                /* 0xC00011 / Z80 0x7F11 */
void    md_audio_vblank(void);                  /* queue the Z80 vblank INT */
void    md_audio_debug(int *reset, int *bus, int *pc,
                        unsigned long *cycles,
                        unsigned long *ym_writes,
                        unsigned long *psg_writes,
                        unsigned long *key_writes,
                        unsigned *active_mask);
void    md_audio_debug_ext(unsigned long *z80_ram_writes,
                           unsigned *last_z80_ram_addr,
                           unsigned *last_z80_ram_value,
                           unsigned long *z80_bank_writes,
                           unsigned *last_z80_bank,
                           unsigned long *ym_status_reads,
                           unsigned *last_ym_reg,
                           unsigned *last_ym_value);

#endif
