/*
 * md_machine.c - Megadrive 68k bus, I/O, interrupts and frame loop.
 *
 * Memory map (24-bit):
 *   000000-3FFFFF  cartridge ROM (RoS: 512KB, no SRAM)
 *   A00000-A0FFFF  Z80 address space (valid while the 68k holds the bus)
 *   A10000-A1001F  I/O: version, pad data/ctrl
 *   A11100         Z80 BUSREQ, A11200 Z80 RESET
 *   A14000         TMSS 'SEGA' latch (ignored, version reg reports 0)
 *   C00000-C0001F  VDP: data 00/02, control 04/06, HV 08, PSG 11
 *   E00000-FFFFFF  work RAM, 64KB mirrored
 *
 * Interrupts: VINT = level 6 at line 224, HINT = level 4 from the VDP
 * line counter (reg 10). Both level-held until the 68k acks; the ack
 * callback clears the source and re-computes the IPL.
 */

#include "hal/md_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

uint8_t  md_rom[MD_ROM_MAX];
uint32_t md_rom_size;
uint8_t  md_ram[0x10000];
uint8_t  md_screen[MD_SCREEN_H][MD_SCREEN_W];
uint16_t md_pad[2];

/* ---- interrupt state (md_vdp.c reads the enables from md_vdp_reg) ---- */
int md_vint_pending, md_hint_pending;
extern int md_vdp_status_f;

void md_update_ipl(void)
{
    int level = 0;
    if (md_hint_pending && (md_vdp_reg[0] & 0x10)) level = 4;
    if (md_vint_pending && (md_vdp_reg[1] & 0x20)) level = 6;
    m68k_set_irq((unsigned)level);
}

static int md_int_ack(int level)
{
    if (level == 6)      { md_vint_pending = 0; md_vdp_status_f = 0; }
    else if (level == 4) md_hint_pending = 0;
    m68k_set_irq(0);
    md_update_ipl();
    return M68K_INT_ACK_AUTOVECTOR;
}

/* ---- I/O ports (pads) ----
 * 3-button pad: TH out on bit 6 of the data port selects which half of
 * the button set appears on the low 6 bits.
 *   TH=1: [.][TH][C][B][R][L][D][U]   TH=0: [.][0][St][A][0][0][D][U]
 */
static uint8_t io_ctrl[3];      /* A10009/B/D direction regs */
static uint8_t io_data[3];      /* last written data (TH lives here) */

static uint8_t pad_read(int idx)
{
    uint16_t p = md_pad[idx];   /* 1 = pressed */
    uint8_t th = (uint8_t)(io_data[idx] & 0x40);
    uint8_t v;
    if (th) {
        v = (uint8_t)(0xC0
            | ((p & 0x40) ? 0 : 0x20)      /* C */
            | ((p & 0x20) ? 0 : 0x10)      /* B */
            | ((p & 0x08) ? 0 : 0x08)      /* Right */
            | ((p & 0x04) ? 0 : 0x04)      /* Left */
            | ((p & 0x02) ? 0 : 0x02)      /* Down */
            | ((p & 0x01) ? 0 : 0x01));    /* Up */
    } else {
        v = (uint8_t)(0x80
            | ((p & 0x80) ? 0 : 0x20)      /* Start */
            | ((p & 0x10) ? 0 : 0x10)      /* A */
            | ((p & 0x02) ? 0 : 0x02)      /* Down */
            | ((p & 0x01) ? 0 : 0x01));    /* Up */
    }
    /* pins configured as outputs read back the written value */
    return (uint8_t)((v & (uint8_t)~io_ctrl[idx]) | (io_data[idx] & io_ctrl[idx]));
}

static uint8_t io_read(uint32_t a)      /* a = 0xA10000-0xA1001F, odd bytes */
{
    switch (a & 0x1F) {
        case 0x01: return 0xA0;         /* version: export, NTSC, no FDD, v0 */
        case 0x03: return pad_read(0);
        case 0x05: return pad_read(1);
        case 0x07: return 0x7F;         /* exp port, no device */
        case 0x09: return io_ctrl[0];
        case 0x0B: return io_ctrl[1];
        case 0x0D: return io_ctrl[2];
    }
    return 0x00;
}

static void io_write(uint32_t a, uint8_t v)
{
    switch (a & 0x1F) {
        case 0x03: io_data[0] = v; break;
        case 0x05: io_data[1] = v; break;
        case 0x07: io_data[2] = v; break;
        case 0x09: io_ctrl[0] = v; break;
        case 0x0B: io_ctrl[1] = v; break;
        case 0x0D: io_ctrl[2] = v; break;
    }
}

/* ---- Z80 bus control ---- */
static int z80_busreq_held;

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint32_t sr;
    uint32_t usp;
    uint32_t isp;
    uint32_t msp;
    uint32_t sfc;
    uint32_t dfc;
    uint32_t vbr;
} M68kState;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t rom_size;
    int vint_pending;
    int hint_pending;
    int z80_busreq_held;
    int hint_counter;
    M68kState cpu;
} MdStateHeader;

static int hint_cnt;

/* ---- 68k bus ---- */

uint8_t md_read8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < 0x400000)
        return (a < md_rom_size) ? md_rom[a] : 0x00;
    if (a >= 0xE00000)
        return md_ram[a & 0xFFFF];
    if ((a & 0xFF0000) == 0xA00000)
        return md_z80_area_read((uint16_t)a);
    if ((a & 0xFFFFE0) == 0xA10000)
        return io_read(a | 1);
    if ((a & 0xFFFFFE) == 0xA11100)
        return (a & 1) ? 0x00 : (uint8_t)(md_z80_bus_free() ? 0x01 : 0x00);
    if ((a & 0xFFFFF8) == 0xC00000) {           /* VDP data/control, byte */
        uint16_t w = (a & 4) ? md_vdp_read_status() : md_vdp_read_data();
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    if ((a & 0xFFFFFE) == 0xC00008) {
        uint16_t w = md_vdp_hv_counter();
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    return 0x00;
}

uint16_t md_read16(uint32_t a)
{
    a &= 0xFFFFFE;
    if (a < 0x400000) {
        if (a + 1 < md_rom_size)
            return (uint16_t)((md_rom[a] << 8) | md_rom[a + 1]);
        return 0x0000;
    }
    if (a >= 0xE00000) {
        uint32_t o = a & 0xFFFF;
        return (uint16_t)((md_ram[o] << 8) | md_ram[o + 1]);
    }
    if ((a & 0xFFFFF8) == 0xC00000)
        return (a & 4) ? md_vdp_read_status() : md_vdp_read_data();
    if ((a & 0xFFFFFE) == 0xC00008)
        return md_vdp_hv_counter();
    if ((a & 0xFF0000) == 0xA00000) {           /* Z80 space word read: byte doubled */
        uint8_t b = md_z80_area_read((uint16_t)a);
        return (uint16_t)((b << 8) | b);
    }
    if ((a & 0xFFFFE0) == 0xA10000) {
        uint8_t b = io_read(a | 1);
        return (uint16_t)((b << 8) | b);
    }
    if ((a & 0xFFFFFE) == 0xA11100)
        return md_z80_bus_free() ? 0x0100 : 0x0000;
    return 0x0000;
}

void md_write8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { md_ram[a & 0xFFFF] = v; return; }
    if (a < 0x400000) return;                   /* ROM */
    if ((a & 0xFF0000) == 0xA00000) { md_z80_area_write((uint16_t)a, v); return; }
    if ((a & 0xFFFFE0) == 0xA10000) { io_write(a | 1, v); return; }
    if ((a & 0xFFFFFE) == 0xA11100) {
        if (!(a & 1)) { z80_busreq_held = v & 1; md_z80_busreq(z80_busreq_held); }
        return;
    }
    if ((a & 0xFFFFFE) == 0xA11200) {
        if (!(a & 1)) md_z80_set_reset(!(v & 1));
        return;
    }
    if ((a & 0xFFFFF8) == 0xC00000) {           /* byte write doubles onto the bus */
        uint16_t w = (uint16_t)((v << 8) | v);
        if (a & 4) md_vdp_write_control(w); else md_vdp_write_data(w);
        return;
    }
    if ((a & 0xFFFFFE) == 0xC00010 || (a & 0xFFFFFE) == 0xC00012) {
        if (a & 1) md_psg_write(v);
        return;
    }
    /* A14000 TMSS etc: ignore */
}

void md_write16(uint32_t a, uint16_t v)
{
    a &= 0xFFFFFE;
    if (a >= 0xE00000) {
        uint32_t o = a & 0xFFFF;
        md_ram[o] = (uint8_t)(v >> 8);
        md_ram[o + 1] = (uint8_t)v;
        return;
    }
    if (a < 0x400000) return;
    if ((a & 0xFFFFF8) == 0xC00000) {
        if (a & 4) md_vdp_write_control(v); else md_vdp_write_data(v);
        return;
    }
    if ((a & 0xFF0000) == 0xA00000) { md_z80_area_write((uint16_t)a, (uint8_t)(v >> 8)); return; }
    if ((a & 0xFFFFE0) == 0xA10000) { io_write(a | 1, (uint8_t)v); return; }
    if ((a & 0xFFFFFE) == 0xA11100) { z80_busreq_held = (v >> 8) & 1; md_z80_busreq(z80_busreq_held); return; }
    if ((a & 0xFFFFFE) == 0xA11200) { md_z80_set_reset(!((v >> 8) & 1)); return; }
    if ((a & 0xFFFFFE) == 0xC00010 || (a & 0xFFFFFE) == 0xC00012) { md_psg_write((uint8_t)v); return; }
}

/* ---- machine init / frame loop ---- */

void md_machine_reset(void)
{
    memset(md_ram, 0, sizeof md_ram);
    memset(io_ctrl, 0, sizeof io_ctrl);
    memset(io_data, 0x40, sizeof io_data);
    md_vint_pending = md_hint_pending = 0;
    z80_busreq_held = 0;
    hint_cnt = 0;
    md_vdp_reset();
    md_audio_reset();
    m68k_pulse_reset();
}

void md_machine_init(void)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(md_int_ack);
    md_audio_init();
    md_machine_reset();
}

static void cpu_state_save(M68kState *s)
{
    int i;
    for (i = 0; i < 8; i++) {
        s->d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
        s->a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
    }
    s->pc = m68k_get_reg(NULL, M68K_REG_PC);
    s->sr = m68k_get_reg(NULL, M68K_REG_SR);
    s->usp = m68k_get_reg(NULL, M68K_REG_USP);
    s->isp = m68k_get_reg(NULL, M68K_REG_ISP);
    s->msp = m68k_get_reg(NULL, M68K_REG_MSP);
    s->sfc = m68k_get_reg(NULL, M68K_REG_SFC);
    s->dfc = m68k_get_reg(NULL, M68K_REG_DFC);
    s->vbr = m68k_get_reg(NULL, M68K_REG_VBR);
}

static void cpu_state_load(const M68kState *s)
{
    int i;
    for (i = 0; i < 8; i++) {
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), s->d[i]);
        m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), s->a[i]);
    }
    m68k_set_reg(M68K_REG_USP, s->usp);
    m68k_set_reg(M68K_REG_ISP, s->isp);
    m68k_set_reg(M68K_REG_MSP, s->msp);
    m68k_set_reg(M68K_REG_SFC, s->sfc);
    m68k_set_reg(M68K_REG_DFC, s->dfc);
    m68k_set_reg(M68K_REG_VBR, s->vbr);
    m68k_set_reg(M68K_REG_SR, s->sr);
    m68k_set_reg(M68K_REG_PC, s->pc);
}

static int write_block(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n;
}

static int read_block(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n;
}

int md_save_state(const char *path)
{
    MdStateHeader h;
    unsigned vdp_size = md_vdp_state_size();
    unsigned audio_size = md_audio_state_size();
    void *audio_blob;
    unsigned char vdp_blob[64];
    FILE *f;
    int ok = 0;

    if (vdp_size > sizeof vdp_blob)
        return 0;
    audio_blob = malloc(audio_size);
    if (!audio_blob)
        return 0;

    memset(&h, 0, sizeof h);
    memcpy(h.magic, "MDST0001", 8);
    h.version = 1;
    h.rom_size = md_rom_size;
    h.vint_pending = md_vint_pending;
    h.hint_pending = md_hint_pending;
    h.z80_busreq_held = z80_busreq_held;
    h.hint_counter = hint_cnt;
    cpu_state_save(&h.cpu);
    md_vdp_state_save(vdp_blob);
    md_audio_state_save(audio_blob);

    f = fopen(path, "wb");
    if (f) {
        ok = write_block(f, &h, sizeof h)
          && write_block(f, md_ram, sizeof md_ram)
          && write_block(f, md_screen, sizeof md_screen)
          && write_block(f, md_vram, sizeof md_vram)
          && write_block(f, md_cram, sizeof md_cram)
          && write_block(f, md_vsram, sizeof md_vsram)
          && write_block(f, md_vdp_reg, sizeof md_vdp_reg)
          && write_block(f, vdp_blob, vdp_size)
          && write_block(f, audio_blob, audio_size);
        fclose(f);
    }
    free(audio_blob);
    return ok;
}

int md_load_state(const char *path)
{
    MdStateHeader h;
    unsigned vdp_size = md_vdp_state_size();
    unsigned audio_size = md_audio_state_size();
    void *audio_blob;
    unsigned char vdp_blob[64];
    FILE *f;
    int ok;

    if (vdp_size > sizeof vdp_blob)
        return 0;
    audio_blob = malloc(audio_size);
    if (!audio_blob)
        return 0;

    f = fopen(path, "rb");
    if (!f) {
        free(audio_blob);
        return 0;
    }

    ok = read_block(f, &h, sizeof h)
      && memcmp(h.magic, "MDST0001", 8) == 0
      && h.version == 1
      && h.rom_size == md_rom_size
      && read_block(f, md_ram, sizeof md_ram)
      && read_block(f, md_screen, sizeof md_screen)
      && read_block(f, md_vram, sizeof md_vram)
      && read_block(f, md_cram, sizeof md_cram)
      && read_block(f, md_vsram, sizeof md_vsram)
      && read_block(f, md_vdp_reg, sizeof md_vdp_reg)
      && read_block(f, vdp_blob, vdp_size)
      && read_block(f, audio_blob, audio_size);
    fclose(f);

    if (ok) {
        md_vint_pending = h.vint_pending;
        md_hint_pending = h.hint_pending;
        z80_busreq_held = h.z80_busreq_held;
        hint_cnt = h.hint_counter;
        md_vdp_state_load(vdp_blob);
        md_audio_state_load(audio_blob);
        cpu_state_load(&h.cpu);
        md_palette_dirty = 1;
        md_update_ipl();
    }

    free(audio_blob);
    return ok;
}

/* Real hardware is 3420 MCLK per line. The Amiga/Amiberry native build is
 * bottlenecked by nested 68000 interpretation, so the default package uses a
 * trimmed cycle budget to keep wall-clock video close to 60 FPS. */
#ifndef MD_MCLK_PER_LINE
#define MD_MCLK_PER_LINE 2600
#endif

void md_run_frame_ex(int render_video)
{
    int line;
    long target = 0, ran = 0;

    for (line = 0; line < MD_LINES; line++) {
        md_vdp_line = line;

        if (line < MD_ACTIVE_LINES) {
            /* VDP line counter: reload during vblank, count down in active */
            if (hint_cnt == 0) {
                hint_cnt = md_vdp_reg[10];
                if (line != 0) {            /* no HINT on the reload at line 0 */
                    md_hint_pending = 1;
                    md_update_ipl();
                }
            } else {
                hint_cnt--;
            }
        } else {
            hint_cnt = md_vdp_reg[10];
        }

        if (line == MD_ACTIVE_LINES) {      /* enter vblank */
            md_vint_pending = 1;
            md_vdp_status_f = 1;
            md_update_ipl();
            md_audio_vblank();              /* Z80 INT */
        }

        if (render_video && line < MD_ACTIVE_LINES)
            md_vdp_render_line(line, md_screen[line]);

        target += MD_MCLK_PER_LINE;
        {
            int need = (int)(target / 7 - ran);
            if (need > 0) ran += m68k_execute(need);
        }
    }
}

void md_run_frame(void)
{
    md_run_frame_ex(1);
}
