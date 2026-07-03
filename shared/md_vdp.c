/*
 * md_vdp.c - Megadrive VDP: ports, DMA, per-line mode-5 renderer.
 *
 * VRAM byte order: word writes land big-endian (vram[even] = MSB), so a
 * pattern byte's HIGH nibble is the LEFT pixel. Odd-address word writes
 * byte-swap, as on real hardware.
 *
 * The renderer emits one 320-pen line per call (pen = CRAM index 0-63).
 * H32 mode renders 256 pixels centred with backdrop borders. Priority is
 * resolved per pixel: Blow < Alow < Slow < Bhigh < Ahigh < Shigh.
 * Not modelled: shadow/highlight, interlace, sprite overflow flicker.
 */

#include "hal/md_machine.h"
#include <string.h>

uint8_t  md_vram[0x10000];
uint16_t md_cram[64];
uint16_t md_vsram[40];
uint8_t  md_vdp_reg[32];
int      md_palette_dirty;
int      md_vdp_line;
int      md_vdp_status_f;               /* F flag: set at vblank by machine */

extern int md_vint_pending, md_hint_pending;
extern void md_update_ipl(void);

static uint16_t vdp_addr;               /* address counter */
static uint8_t  vdp_code;               /* CD5-CD0 */
static int      vdp_pending;            /* first control word latched */
static int      dma_fill_wait;          /* fill DMA armed, waiting for data */

typedef struct {
    uint16_t addr;
    uint8_t code;
    int pending;
    int fill_wait;
    int status_f;
    int line;
    int palette_dirty;
} VdpState;

void md_vdp_reset(void)
{
    memset(md_vram, 0, sizeof md_vram);
    memset(md_cram, 0, sizeof md_cram);
    memset(md_vsram, 0, sizeof md_vsram);
    memset(md_vdp_reg, 0, sizeof md_vdp_reg);
    vdp_addr = 0; vdp_code = 0; vdp_pending = 0; dma_fill_wait = 0;
    md_vdp_status_f = 0;
    md_palette_dirty = 1;
}

unsigned md_vdp_state_size(void)
{
    return sizeof(VdpState);
}

void md_vdp_state_save(void *out)
{
    VdpState *s = (VdpState *)out;
    s->addr = vdp_addr;
    s->code = vdp_code;
    s->pending = vdp_pending;
    s->fill_wait = dma_fill_wait;
    s->status_f = md_vdp_status_f;
    s->line = md_vdp_line;
    s->palette_dirty = md_palette_dirty;
}

void md_vdp_state_load(const void *in)
{
    const VdpState *s = (const VdpState *)in;
    vdp_addr = s->addr;
    vdp_code = s->code;
    vdp_pending = s->pending;
    dma_fill_wait = s->fill_wait;
    md_vdp_status_f = s->status_f;
    md_vdp_line = s->line;
    md_palette_dirty = 1;
}

/* ---- frame-global dynamic palette ----
 * Raster games (OutRunners gradient skies, split-screen palette reloads)
 * rewrite CRAM mid-frame — every scanline, even. A single 64-pen palette
 * can't show that, so mid-frame CRAM deltas allocate NEW pens (up to 250)
 * and the affected lines are remapped through a pen->slot LUT. Games that
 * never touch CRAM mid-frame stay on pens 0-63 with zero extra work. */
static int      cram_line_dirty;        /* CRAM write since last line */
static uint16_t dyn_slot_val[250];      /* CRAM value per allocated slot */
static int      dyn_nslots;
static uint8_t  dyn_lut[64];            /* pen -> slot, for the current line */
static uint16_t dyn_cur[64];            /* CRAM view dyn_lut reflects */
static int      dyn_active;             /* any mid-frame delta this frame */

static void dyn_line_update(int line)
{
    int i;
    if (line == 0) {
        /* The 64 base slots track CRAM at frame start; pool slots 64-249
         * PERSIST across frames so a recurring raster value keeps the same
         * pen — per-frame reshuffling made fading raster screens flash
         * (pixels of frame N-1 shown against frame N's palette). The pool
         * only flushes when it fills (one rough frame per screen change). */
        if (dyn_nslots < 64 || dyn_nslots >= 250)
            dyn_nslots = 64;
        for (i = 0; i < 64; i++) {
            dyn_slot_val[i] = md_cram[i];
            dyn_cur[i] = md_cram[i];
            dyn_lut[i] = (uint8_t)i;
        }
        dyn_active = 0;
        cram_line_dirty = 0;
        return;
    }
    if (!cram_line_dirty)
        return;
    cram_line_dirty = 0;
    for (i = 0; i < 64; i++) {
        uint16_t v = md_cram[i];
        if (v != dyn_cur[i]) {
            int s, slot = -1;
            /* dedupe against POOL slots only: matching a base slot would tie
             * the pen to a value that fades out from under it */
            for (s = 64; s < dyn_nslots; s++) {
                if (dyn_slot_val[s] == v) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0 && dyn_nslots < 250) {
                slot = dyn_nslots++;
                dyn_slot_val[slot] = v;
                md_palette_dirty = 1;
            }
            if (slot >= 0) {
                dyn_lut[i] = (uint8_t)slot;
                dyn_active = 1;
            }
            dyn_cur[i] = v;
        }
    }
}

/* ---- raw memory write helpers ---- */

static void vram_write_word(uint16_t a, uint16_t d)
{
    if (a & 1) d = (uint16_t)((d >> 8) | (d << 8));
    md_vram[a & 0xFFFE] = (uint8_t)(d >> 8);
    md_vram[(a & 0xFFFE) + 1] = (uint8_t)d;
}

static void data_write(uint16_t d)
{
    switch (vdp_code & 0x0F) {
        case 0x01:  /* VRAM */
            vram_write_word(vdp_addr, d);
            break;
        case 0x03:  /* CRAM */
            md_cram[(vdp_addr >> 1) & 63] = (uint16_t)(d & 0x0EEE);
            md_palette_dirty = 1;
            cram_line_dirty = 1;
            break;
        case 0x05:  /* VSRAM */
            {
                unsigned i = (vdp_addr >> 1);
                if ((i % 64) < 40) md_vsram[i % 64] = (uint16_t)(d & 0x7FF);
            }
            break;
        default:
            break;
    }
    vdp_addr = (uint16_t)(vdp_addr + md_vdp_reg[15]);
}

/* ---- DMA ---- */

static void dma_68k_transfer(void)
{
    uint32_t len = (uint32_t)(md_vdp_reg[19] | (md_vdp_reg[20] << 8));
    uint32_t src = ((uint32_t)md_vdp_reg[21] | ((uint32_t)md_vdp_reg[22] << 8)
                  | ((uint32_t)(md_vdp_reg[23] & 0x7F) << 16)) << 1;
    if (!len) len = 0x10000;
    while (len--) {
        data_write(md_read16(src));
        src += 2;
    }
    /* source/length registers reflect the finished transfer */
    md_vdp_reg[19] = md_vdp_reg[20] = 0;
    md_vdp_reg[21] = (uint8_t)(src >> 1);
    md_vdp_reg[22] = (uint8_t)(src >> 9);
    md_vdp_reg[23] = (uint8_t)((md_vdp_reg[23] & 0x80) | ((src >> 17) & 0x7F));
}

static void dma_fill(uint16_t d)
{
    uint32_t len = (uint32_t)(md_vdp_reg[19] | (md_vdp_reg[20] << 8));
    uint8_t  fill = (uint8_t)(d >> 8);
    if (!len) len = 0x10000;
    vram_write_word(vdp_addr, d);       /* the trigger write lands first */
    vdp_addr = (uint16_t)(vdp_addr + md_vdp_reg[15]);
    do {
        md_vram[(vdp_addr ^ 1) & 0xFFFF] = fill;
        vdp_addr = (uint16_t)(vdp_addr + md_vdp_reg[15]);
    } while (--len);
    md_vdp_reg[19] = md_vdp_reg[20] = 0;
}

static void dma_copy(void)
{
    uint32_t len = (uint32_t)(md_vdp_reg[19] | (md_vdp_reg[20] << 8));
    uint16_t src = (uint16_t)(md_vdp_reg[21] | (md_vdp_reg[22] << 8));
    if (!len) len = 0x10000;
    do {
        md_vram[(vdp_addr ^ 1) & 0xFFFF] = md_vram[(src ^ 1) & 0xFFFF];
        src++;
        vdp_addr = (uint16_t)(vdp_addr + md_vdp_reg[15]);
    } while (--len);
    md_vdp_reg[19] = md_vdp_reg[20] = 0;
    md_vdp_reg[21] = (uint8_t)src;
    md_vdp_reg[22] = (uint8_t)(src >> 8);
}

/* ---- ports ---- */

void md_vdp_write_control(uint16_t v)
{
    if (!vdp_pending) {
        if ((v & 0xC000) == 0x8000) {           /* register write */
            uint8_t r = (uint8_t)((v >> 8) & 0x1F);
            if (r < 24) {
                md_vdp_reg[r] = (uint8_t)v;
                if (r == 0 || r == 1) md_update_ipl();
            }
            return;
        }
        vdp_addr = (uint16_t)((vdp_addr & 0xC000) | (v & 0x3FFF));
        vdp_code = (uint8_t)((vdp_code & 0x3C) | ((v >> 14) & 3));
        vdp_pending = 1;
        return;
    }
    vdp_pending = 0;
    vdp_addr = (uint16_t)((vdp_addr & 0x3FFF) | ((v & 3) << 14));
    vdp_code = (uint8_t)((vdp_code & 3) | ((v >> 2) & 0x3C));
    if ((vdp_code & 0x20) && (md_vdp_reg[1] & 0x10)) {   /* CD5 + DMA enable */
        switch (md_vdp_reg[23] >> 6) {
            case 2:  dma_fill_wait = 1; break;           /* fill: wait for data */
            case 3:  dma_copy(); break;
            default: dma_68k_transfer(); break;          /* 68k -> VDP */
        }
    }
}

void md_vdp_write_data(uint16_t v)
{
    vdp_pending = 0;
    if (dma_fill_wait) {
        dma_fill_wait = 0;
        dma_fill(v);
        return;
    }
    data_write(v);
}

uint16_t md_vdp_read_data(void)
{
    uint16_t d = 0;
    vdp_pending = 0;
    switch (vdp_code & 0x0F) {
        case 0x00:  /* VRAM */
            d = (uint16_t)((md_vram[vdp_addr & 0xFFFE] << 8)
                          | md_vram[(vdp_addr & 0xFFFE) + 1]);
            break;
        case 0x08:  /* CRAM */
            d = md_cram[(vdp_addr >> 1) & 63];
            break;
        case 0x04:  /* VSRAM */
            {
                unsigned i = (vdp_addr >> 1) % 64;
                d = (i < 40) ? md_vsram[i] : 0;
            }
            break;
    }
    vdp_addr = (uint16_t)(vdp_addr + md_vdp_reg[15]);
    return d;
}

uint16_t md_vdp_read_status(void)
{
    uint16_t s = 0x3400 | 0x0200;                       /* FIFO empty */
    if (md_vdp_line >= MD_ACTIVE_LINES || !(md_vdp_reg[1] & 0x40))
        s |= 0x0008;                                    /* VBLANK */
    if (md_vdp_status_f) s |= 0x0080;                   /* F */
    vdp_pending = 0;
    md_vdp_status_f = 0;
    return s;
}

uint16_t md_vdp_hv_counter(void)
{
    /* lines execute atomically, so fake a moving H counter to keep
     * H-polling loops from hanging */
    static uint8_t h;
    int v = md_vdp_line;
    if (v > 0xEA) v -= 6;                               /* NTSC jump E A->E5 */
    h = (uint8_t)(h + 0x29);
    return (uint16_t)((v << 8) | h);
}

/* ---- palette ---- */

/* measured VDP DAC response (Genesis Plus levels) */
static const uint8_t md_dac[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };

void md_vdp_palette(uint32_t out[64])
{
    int i;
    for (i = 0; i < 64; i++) {
        uint16_t c = md_cram[i];
        out[i] = ((uint32_t)md_dac[(c >> 1) & 7] << 16)
               | ((uint32_t)md_dac[(c >> 5) & 7] << 8)
               |  (uint32_t)md_dac[(c >> 9) & 7];
    }
}

/* full dynamic-slot palette (250 slots max; see dyn_line_update).
 * Base entries 0-63 read LIVE CRAM, not the line-0 snapshot: games set new
 * palettes during vblank, after the snapshot — resolving from the snapshot
 * uploaded stale colours and, because the dirty flag was already consumed,
 * the correct ones never arrived (permanently wrong colours after any
 * single-shot palette change). Pool slots 64+ are the mid-frame values. */
void md_vdp_palette_dyn(uint32_t out[256])
{
    int i;
    for (i = 0; i < 250; i++) {
        uint16_t c = (i < 64) ? md_cram[i]
                   : (i < dyn_nslots) ? dyn_slot_val[i] : 0;
        out[i] = ((uint32_t)md_dac[(c >> 1) & 7] << 16)
               | ((uint32_t)md_dac[(c >> 5) & 7] << 8)
               |  (uint32_t)md_dac[(c >> 9) & 7];
    }
    out[250] = 0x000000;
    out[251] = 0xFFFFFF;
    out[252] = out[253] = out[254] = out[255] = 0;
}

/* ---- renderer ---- */

static uint16_t scroll_cells(uint8_t bits)
{
    switch (bits & 3) {
        case 1: return 64;
        case 3: return 128;
        default: return 32;
    }
}

/* render one scrolling plane (0=A with window, 1=B) into buf:
 * (pri<<7)|(pal<<4)|colorindex per pixel, colorindex 0 = transparent */
static int vscroll_value(unsigned index)
{
    return (int)(md_vsram[index % 40] & 0x7FF);
}

static uint32_t nametable_addr(uint32_t base, uint16_t width_cells,
                               uint16_t row, uint16_t col)
{
    return base + ((((uint32_t)row * (uint32_t)width_cells + (uint32_t)col) << 1) & 0x1FFF);
}

static void render_plane(int line, int plane, int W, uint8_t *buf)
{
    uint8_t hsize = md_vdp_reg[16] & 3;
    uint8_t vsize = (md_vdp_reg[16] >> 4) & 3;
    uint16_t pw = scroll_cells(hsize);
    uint16_t ph = (hsize == 2) ? 1 : scroll_cells(vsize);
    uint16_t xmask = (uint16_t)(pw * 8 - 1), ymask = (uint16_t)(ph * 8 - 1);
    uint32_t base = plane ? ((uint32_t)(md_vdp_reg[4] & 0x07) << 13)
                          : ((uint32_t)(md_vdp_reg[2] & 0x38) << 10);
    int h40 = (W == 320);

    /* horizontal scroll for this line */
    uint32_t hsbase = (uint32_t)(md_vdp_reg[13] & 0x3F) << 10;
    uint32_t hsoff;
    uint16_t hs;
    switch (md_vdp_reg[11] & 3) {
        case 3:  hsoff = (uint32_t)line * 4; break;             /* per line */
        case 2:  hsoff = (uint32_t)(line & ~7) * 4; break;      /* per cell */
        case 1:  hsoff = (uint32_t)(line & 7) * 4; break;       /* invalid */
        default: hsoff = 0; break;                              /* full */
    }
    hsoff += (uint32_t)plane * 2;
    hs = (uint16_t)(((md_vram[(hsbase + hsoff) & 0xFFFF] << 8)
                    | md_vram[(hsbase + hsoff + 1) & 0xFFFF]) & 0x3FF);

    /* window region (plane A only) */
    int win_row = 0, win_x0 = 0, win_x1 = 0;
    uint32_t wbase = 0;
    uint16_t wcells = 0;
    if (plane == 0) {
        int hp = (md_vdp_reg[17] & 0x1F) * 16;
        int vp = (md_vdp_reg[18] & 0x1F) * 8;
        win_row = (md_vdp_reg[18] & 0x80) ? (line >= vp) : (line < vp);
        if (md_vdp_reg[17] & 0x80) { win_x0 = hp; win_x1 = W; }
        else                       { win_x0 = 0;  win_x1 = hp; }
        if (win_x1 > W) win_x1 = W;
        wbase  = (uint32_t)(md_vdp_reg[3] & (h40 ? 0x3C : 0x3E)) << 10;
        wcells = (uint16_t)(h40 ? 64 : 32);
    }

    int two_cell = md_vdp_reg[11] & 4;
    int vs_full = vscroll_value((unsigned)plane);

    int x;
    for (x = 0; x < W; x++) {
        uint32_t nt;
        uint16_t e;
        uint8_t  b, ci;
        int vx, vy, row, col;

        if (plane == 0 && (win_row || (x >= win_x0 && x < win_x1))) {
            /* window: unscrolled, own nametable */
            nt = nametable_addr(wbase, wcells, (uint16_t)(line >> 3), (uint16_t)(x >> 3));
            vy = line; vx = x;
        } else {
            int vs = two_cell ? vscroll_value((((unsigned)x >> 4) << 1) + (unsigned)plane)
                              : vs_full;
            vy = (line + vs) & ymask;
            vx = (x - hs) & xmask;
            nt = nametable_addr(base, pw, (uint16_t)(vy >> 3), (uint16_t)(vx >> 3));
        }
        e = (uint16_t)((md_vram[nt & 0xFFFF] << 8) | md_vram[(nt + 1) & 0xFFFF]);

        row = vy & 7; if (e & 0x1000) row = 7 - row;    /* vflip */
        col = vx & 7; if (e & 0x0800) col = 7 - col;    /* hflip */
        b = md_vram[(((uint32_t)(e & 0x7FF) << 5) + ((uint32_t)row << 2) + ((uint32_t)col >> 1)) & 0xFFFF];
        ci = (col & 1) ? (uint8_t)(b & 15) : (uint8_t)(b >> 4);
        buf[x] = ci ? (uint8_t)(((e >> 8) & 0x80) | ((e >> 9) & 0x30) | ci) : 0;
    }
}

static void render_sprites(int line, int W, uint8_t *buf)
{
    int h40 = (W == 320);
    uint32_t sat = (uint32_t)(md_vdp_reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
    int total = h40 ? 80 : 64;
    int perline = h40 ? 20 : 16;
    uint8_t list[20];
    int n = 0, i, k;
    unsigned link = 0;

    for (i = 0; i < total; i++) {
        uint32_t off = sat + link * 8;
        int y = ((md_vram[off & 0xFFFF] << 8 | md_vram[(off + 1) & 0xFFFF]) & 0x3FF) - 128;
        int h = (((md_vram[(off + 2) & 0xFFFF]) & 3) + 1) * 8;
        if (line >= y && line < y + h) {
            if (n < perline) list[n++] = (uint8_t)link;
            else break;                                 /* overflow: drop rest */
        }
        link = md_vram[(off + 3) & 0xFFFF] & 0x7F;
        if (!link || link >= (unsigned)total) break;
    }

    for (k = 0; k < n; k++) {
        uint32_t off = sat + (uint32_t)list[k] * 8;
        int y  = ((md_vram[off & 0xFFFF] << 8 | md_vram[(off + 1) & 0xFFFF]) & 0x3FF) - 128;
        uint8_t sz = md_vram[(off + 2) & 0xFFFF];
        int hc = (sz & 3) + 1, wc = ((sz >> 2) & 3) + 1;        /* cells */
        uint16_t attr = (uint16_t)((md_vram[(off + 4) & 0xFFFF] << 8) | md_vram[(off + 5) & 0xFFFF]);
        int xr = ((md_vram[(off + 6) & 0xFFFF] << 8) | md_vram[(off + 7) & 0xFFFF]) & 0x1FF;
        int x = xr - 128;
        int row = line - y;
        int hf = attr & 0x0800, vf = attr & 0x1000;
        uint8_t hipal = (uint8_t)(((attr >> 8) & 0x80) | ((attr >> 9) & 0x30));
        int c, p;

        if (xr == 0 && k > 0) break;    /* x=0 masks lower sprites this line */

        if (vf) row = hc * 8 - 1 - row;
        for (c = 0; c < wc; c++) {
            int cc = hf ? (wc - 1 - c) : c;
            uint32_t tile = (uint32_t)(attr & 0x7FF) + (uint32_t)cc * (uint32_t)hc + ((uint32_t)row >> 3);
            uint32_t taddr = ((tile << 5) + (uint32_t)(row & 7) * 4) & 0xFFFF;
            for (p = 0; p < 8; p++) {
                int sx = x + c * 8 + p;
                int col;
                uint8_t b, ci;
                if (sx < 0 || sx >= W || buf[sx]) continue;
                col = hf ? 7 - p : p;
                b = md_vram[(taddr + ((uint32_t)col >> 1)) & 0xFFFF];
                ci = (col & 1) ? (uint8_t)(b & 15) : (uint8_t)(b >> 4);
                if (ci) buf[sx] = (uint8_t)(hipal | ci);
            }
        }
    }
}

void md_vdp_render_line(int line, uint8_t *dst)
{
    static uint8_t bufA[MD_SCREEN_W], bufB[MD_SCREEN_W], bufS[MD_SCREEN_W];
    uint8_t bg = (uint8_t)(md_vdp_reg[7] & 0x3F);
    int h40 = md_vdp_reg[12] & 0x81;
    int W = h40 ? 320 : 256;
    uint8_t *out = dst + (h40 ? 0 : 32);
    int x;

    dyn_line_update(line);

    if (!(md_vdp_reg[1] & 0x40)) {      /* display disabled */
        memset(dst, dyn_active ? dyn_lut[bg] : bg, MD_SCREEN_W);
        return;
    }
    memset(dst, dyn_active ? dyn_lut[bg] : bg, MD_SCREEN_W);

    render_plane(line, 1, W, bufB);
    render_plane(line, 0, W, bufA);
    memset(bufS, 0, (size_t)W);
    render_sprites(line, W, bufS);

    if (dyn_active) {
        for (x = 0; x < W; x++) {
            uint8_t a = bufA[x], b = bufB[x], s = bufS[x], p = bg;
            if ((b & 15) && !(b & 0x80)) p = b & 0x3F;
            if ((a & 15) && !(a & 0x80)) p = a & 0x3F;
            if ((s & 15) && !(s & 0x80)) p = s & 0x3F;
            if ((b & 15) &&  (b & 0x80)) p = b & 0x3F;
            if ((a & 15) &&  (a & 0x80)) p = a & 0x3F;
            if ((s & 15) &&  (s & 0x80)) p = s & 0x3F;
            out[x] = dyn_lut[p];
        }
        return;
    }

    for (x = 0; x < W; x++) {
        uint8_t a = bufA[x], b = bufB[x], s = bufS[x], p = bg;
        if ((b & 15) && !(b & 0x80)) p = b & 0x3F;
        if ((a & 15) && !(a & 0x80)) p = a & 0x3F;
        if ((s & 15) && !(s & 0x80)) p = s & 0x3F;
        if ((b & 15) &&  (b & 0x80)) p = b & 0x3F;
        if ((a & 15) &&  (a & 0x80)) p = a & 0x3F;
        if ((s & 15) &&  (s & 0x80)) p = s & 0x3F;
        out[x] = p;
    }
}
