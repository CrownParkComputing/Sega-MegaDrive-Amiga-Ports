/*
 * md_amiga.c - 8-bit RTG presenter for the Mega Drive core.
 *
 * Keyboard: arrows move, Ctrl=A, Space/Fire=B, Alt=C, 1=Start, Esc=quit.
 * CD32 pad: d-pad moves, Yellow/Green/L=A, Red=B, Blue/R=C, Play=Start.
 */

#include <exec/exec.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/md_machine.h"

struct IntuitionBase *IntuitionBase = 0;
struct GfxBase *GfxBase = 0;
struct Device *TimerBase = 0;

#define RTG_W 320
#define RTG_H 240
#define FPS_SCALE 2

#define RK_1      0x01
#define RK_P      0x19
#define RK_SPACE  0x40
#define RK_ESC    0x45
#define RK_F5     0x54
#define RK_F6     0x55
#define RK_F7     0x56
#define RK_LCTRL  0x63
#define RK_LALT   0x64
#define RK_RALT   0x65
#define RK_UP     0x4C
#define RK_DOWN   0x4D
#define RK_RIGHT  0x4E
#define RK_LEFT   0x4F

#define CIAA_PRA   (*(volatile unsigned char *)0xbfe001UL)
#define CIAA_DDRA  (*(volatile unsigned char *)0xbfe201UL)
#define JOY1DAT    (*(volatile unsigned short *)0xdff00cUL)
#define POTINP     (*(volatile unsigned short *)0xdff016UL)
#define POTGO      (*(volatile unsigned short *)0xdff034UL)
#define PORT1_FIRE 0x80
#define PORT1_DATRY 0x4000
#define POTGO_PORT1 0x6f00
#define POTGO_RESET 0xff00

#define CD32_BLUE      0x80
#define CD32_RED       0x40
#define CD32_YELLOW    0x20
#define CD32_GREEN     0x10
#define CD32_RSHOULDER 0x08
#define CD32_LSHOULDER 0x04
#define CD32_PLAY      0x02

static struct Screen *scr;
static struct Window *win;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static uint8_t *rtg_frame;
static uint32_t loadrgb[1 + 252 * 3 + 1];
static uint8_t keydown[128];
static ULONG eclock_rate, frame_ticks, next_tick;
static ULONG fps_last_tick;
static int disp_x, disp_y, disp_w, disp_h;
static int colmap[RTG_W], rowmap[RTG_H];
static unsigned fps_frames, fps_value;
static unsigned frontend_frames;
static int quit_requested;
static int paused;
static int show_fps = 1;
static uint8_t keyprev[128];
static int cd32_pause_prev;
static int cd32_fps_prev;

extern void md_audio_amiga_open(void);
extern void md_audio_amiga_frame(void);
extern void md_audio_amiga_close(void);
extern void md_audio_amiga_pause(int paused);

static void close_timer(void)
{
    if (timer_io) {
        if (TimerBase)
            CloseDevice((struct IORequest *)timer_io);
        DeleteIORequest((struct IORequest *)timer_io);
        timer_io = 0;
    }
    if (timer_port) {
        DeleteMsgPort(timer_port);
        timer_port = 0;
    }
    TimerBase = 0;
}

static void open_timer(void)
{
    struct EClockVal ev;
    timer_port = CreateMsgPort();
    if (!timer_port)
        return;
    timer_io = (struct timerequest *)CreateIORequest(timer_port, sizeof *timer_io);
    if (!timer_io) {
        close_timer();
        return;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)timer_io, 0) != 0) {
        close_timer();
        return;
    }
    TimerBase = timer_io->tr_node.io_Device;
    eclock_rate = ReadEClock(&ev);
    frame_ticks = (eclock_rate + 30) / 60;
    if (frame_ticks < 1)
        frame_ticks = 1;
    next_tick = ev.ev_lo;
    fps_last_tick = ev.ev_lo;
}

static int frame_pace(void)
{
    struct EClockVal ev;
    ULONG now;
    if (!TimerBase || !frame_ticks)
        return 0;
    ReadEClock(&ev);
    now = ev.ev_lo;
    if ((LONG)(now - next_tick) > (LONG)frame_ticks) {
        next_tick = now;
        return 1;
    }
    next_tick += frame_ticks;
    do {
        ReadEClock(&ev);
        now = ev.ev_lo;
    } while ((LONG)(now - next_tick) < 0);
    return 0;
}

static void shutdown_rtg(void)
{
    close_timer();
    if (win) {
        CloseWindow(win);
        win = 0;
    }
    if (rtg_frame) {
        FreeMem(rtg_frame, (size_t)RTG_W * RTG_H);
        rtg_frame = 0;
    }
    if (scr) {
        CloseScreen(scr);
        scr = 0;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = 0;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
    }
}

static void center_display_region(void)
{
    int i;
    int scale_x = RTG_W / MD_SCREEN_W;
    int scale_y = RTG_H / MD_SCREEN_H;
    int scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1)
        scale = 1;

    disp_w = MD_SCREEN_W * scale;
    disp_h = MD_SCREEN_H * scale;
    if (disp_w > RTG_W) {
        disp_w = RTG_W;
        disp_h = (MD_SCREEN_H * disp_w) / MD_SCREEN_W;
    }
    if (disp_h > RTG_H) {
        disp_h = RTG_H;
        disp_w = (MD_SCREEN_W * disp_h) / MD_SCREEN_H;
    }

    disp_x = (RTG_W - disp_w) / 2;
    disp_y = (RTG_H - disp_h) / 2;
    for (i = 0; i < disp_w; i++)
        colmap[i] = (i * MD_SCREEN_W) / disp_w;
    for (i = 0; i < disp_h; i++)
        rowmap[i] = (i * MD_SCREEN_H) / disp_h;
}

static int open_rtg(void)
{
    ULONG mode;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 40);
    if (!IntuitionBase || !GfxBase)
        return 0;

    mode = BestModeID(BIDTAG_NominalWidth, RTG_W,
                      BIDTAG_NominalHeight, RTG_H,
                      BIDTAG_DesiredWidth, RTG_W,
                      BIDTAG_DesiredHeight, RTG_H,
                      BIDTAG_Depth, 8,
                      TAG_DONE);
    if (mode != INVALID_ID)
        scr = OpenScreenTags(0, SA_DisplayID, mode, SA_Width, RTG_W, SA_Height, RTG_H,
                             SA_Depth, 8, SA_Quiet, 1, SA_Type, CUSTOMSCREEN,
                             SA_ShowTitle, 0, TAG_END);
    if (!scr)
        scr = OpenScreenTags(0, SA_Width, RTG_W, SA_Height, RTG_H, SA_Depth, 8,
                             SA_Quiet, 1, SA_Type, CUSTOMSCREEN, SA_ShowTitle, 0,
                             TAG_END);
    if (!scr)
        return 0;

    rtg_frame = (uint8_t *)AllocMem((size_t)RTG_W * RTG_H, MEMF_FAST | MEMF_CLEAR);
    if (!rtg_frame)
        rtg_frame = (uint8_t *)AllocMem((size_t)RTG_W * RTG_H, MEMF_PUBLIC | MEMF_CLEAR);
    if (!rtg_frame)
        return 0;

    win = OpenWindowTags(0, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, RTG_W, WA_Height, RTG_H, WA_Backdrop, TRUE,
                         WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
                         WA_IDCMP, IDCMP_RAWKEY, TAG_END);
    if (!win)
        return 0;
    ScreenToFront(scr);
    ActivateWindow(win);
    open_timer();

    center_display_region();
    memset(rtg_frame, 0, (size_t)RTG_W * RTG_H);
    WriteChunkyPixels(win->RPort, 0, 0, RTG_W - 1, RTG_H - 1, rtg_frame, RTG_W);
    return 1;
}

static void upload_palette(void)
{
    uint32_t pal[64];
    int i;
    const int entries = 252;

    md_vdp_palette(pal);
    loadrgb[0] = ((uint32_t)entries << 16) | 0;
    for (i = 0; i < entries; i++) {
        uint32_t rgb = (i < 64) ? pal[i] : 0;
        if (i == 250)
            rgb = 0x000000;
        else if (i == 251)
            rgb = 0xFFFFFF;
        loadrgb[1 + i * 3 + 0] = ((rgb >> 16) & 0xFF) * 0x01010101u;
        loadrgb[1 + i * 3 + 1] = ((rgb >> 8) & 0xFF) * 0x01010101u;
        loadrgb[1 + i * 3 + 2] = (rgb & 0xFF) * 0x01010101u;
    }
    loadrgb[1 + entries * 3] = 0;
    LoadRGB32(&scr->ViewPort, loadrgb);
    md_palette_dirty = 0;
}

static const uint8_t font3x5_digits[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 },
    { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
    { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 },
    { 7, 5, 7, 1, 7 }
};

static const uint8_t font3x5_f[5] = { 7, 4, 6, 4, 4 };
static const uint8_t font3x5_p[5] = { 6, 5, 6, 4, 4 };
static const uint8_t font3x5_s[5] = { 7, 4, 7, 1, 7 };

static void draw_glyph3x5(int x, int y, const uint8_t rows[5], uint8_t pen)
{
    int gy, gx, sy, sx;
    for (gy = 0; gy < 5; gy++) {
        uint8_t bits = rows[gy];
        for (gx = 0; gx < 3; gx++) {
            if (!(bits & (1 << (2 - gx))))
                continue;
            for (sy = 0; sy < FPS_SCALE; sy++) {
                uint8_t *dst = rtg_frame + (size_t)(y + gy * FPS_SCALE + sy) * RTG_W +
                               x + gx * FPS_SCALE;
                for (sx = 0; sx < FPS_SCALE; sx++)
                    dst[sx] = pen;
            }
        }
    }
}

static void draw_fps_overlay(void)
{
    char buf[8];
    unsigned v = fps_value;
    int i, x = disp_x + 8, y = disp_y + 8;
    if (v > 999)
        v = 999;
    buf[0] = 'F';
    buf[1] = 'P';
    buf[2] = 'S';
    buf[3] = ' ';
    buf[4] = (char)('0' + (v / 100) % 10);
    buf[5] = (char)('0' + (v / 10) % 10);
    buf[6] = (char)('0' + v % 10);
    buf[7] = 0;

    for (i = 0; buf[i]; i++, x += 4 * FPS_SCALE) {
        const uint8_t *glyph = 0;
        if (buf[i] == ' ')
            continue;
        if (buf[i] == 'F') glyph = font3x5_f;
        else if (buf[i] == 'P') glyph = font3x5_p;
        else if (buf[i] == 'S') glyph = font3x5_s;
        else glyph = font3x5_digits[buf[i] - '0'];

        draw_glyph3x5(x + 2, y + 2, glyph, 250);
        draw_glyph3x5(x, y, glyph, 251);
    }
}

static void update_fps_counter(void)
{
    struct EClockVal ev;
    ULONG now, elapsed;
    if (!TimerBase || !eclock_rate)
        return;
    fps_frames++;
    ReadEClock(&ev);
    now = ev.ev_lo;
    elapsed = now - fps_last_tick;
    if (elapsed >= eclock_rate) {
        fps_value = (unsigned)(((unsigned long)fps_frames * eclock_rate + elapsed / 2) / elapsed);
        fps_frames = 0;
        fps_last_tick = now;
    }
}

static void scale_frame(void)
{
    int y;
    if (disp_w == MD_SCREEN_W && disp_h == MD_SCREEN_H) {
        for (y = 0; y < MD_SCREEN_H; y++) {
            uint8_t *dst = rtg_frame + (size_t)(disp_y + y) * RTG_W + disp_x;
            memcpy(dst, md_screen[y], MD_SCREEN_W);
        }
        return;
    }

    if (disp_w == MD_SCREEN_W * 2 && disp_h == MD_SCREEN_H * 2) {
        for (y = 0; y < MD_SCREEN_H; y++) {
            const uint8_t *src = md_screen[y];
            uint8_t *dst0 = rtg_frame + (size_t)(disp_y + y * 2) * RTG_W + disp_x;
            uint8_t *dst1 = dst0 + RTG_W;
            uint16_t *dw = (uint16_t *)dst0;
            int x;
            for (x = 0; x < MD_SCREEN_W; x++) {
                uint8_t p = src[x];
                dw[x] = (uint16_t)((p << 8) | p);
            }
            memcpy(dst1, dst0, disp_w);
        }
        return;
    }

    for (y = 0; y < disp_h; y++) {
        const uint8_t *src = md_screen[rowmap[y]];
        uint8_t *dst = rtg_frame + (size_t)(disp_y + y) * RTG_W + disp_x;
        int x;
        for (x = 0; x < disp_w; x++)
            dst[x] = src[colmap[x]];
    }
}

static void present_frame(void)
{
    scale_frame();
    update_fps_counter();
    if (show_fps)
        draw_fps_overlay();
    WriteChunkyPixels(win->RPort, disp_x, disp_y, disp_x + disp_w - 1, disp_y + disp_h - 1,
                      rtg_frame + (size_t)disp_y * RTG_W + disp_x, RTG_W);
}

static unsigned read_cd32_port1(void)
{
    unsigned out = 0;
    int i;

    CIAA_DDRA |= PORT1_FIRE;
    CIAA_PRA &= (unsigned char)~PORT1_FIRE;
    POTGO = POTGO_PORT1;
    for (i = 7; i >= 0; i--) {
        volatile unsigned char t;
        t = CIAA_PRA; t = CIAA_PRA; t = CIAA_PRA;
        t = CIAA_PRA; t = CIAA_PRA; t = CIAA_PRA; (void)t;
        if (!(POTINP & PORT1_DATRY))
            out |= (1u << i);
        CIAA_PRA |= PORT1_FIRE;
        CIAA_PRA &= (unsigned char)~PORT1_FIRE;
    }

    CIAA_DDRA &= (unsigned char)~PORT1_FIRE;
    POTGO = POTGO_RESET;
    CIAA_PRA |= 0xc0;
    if (out == 0xff)
        out = 0;
    return out;
}

static void poll_input(void)
{
    struct IntuiMessage *m;
    uint16_t pad = 0;
    unsigned joy, cd32;
    int fire1, fire2;
    int right, left, down, up;
    int cd32_pause;
    int cd32_fps;
    int state_ok;

    while (win && win->UserPort && (m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cls = m->Class;
        UWORD raw = m->Code;
        ReplyMsg((struct Message *)m);
        if (cls == IDCMP_RAWKEY)
            keydown[raw & 0x7F] = (raw & 0x80) ? 0 : 1;
    }

    if (keydown[RK_ESC]) {
        quit_requested = 1;
        return;
    }

    cd32 = read_cd32_port1();
    cd32_pause = ((cd32 & (CD32_YELLOW | CD32_GREEN)) == (CD32_YELLOW | CD32_GREEN));
    cd32_fps = ((cd32 & (CD32_RED | CD32_BLUE)) == (CD32_RED | CD32_BLUE));

    if ((keydown[RK_P] && !keyprev[RK_P]) || (cd32_pause && !cd32_pause_prev)) {
        paused = !paused;
        md_audio_amiga_pause(paused);
        printf(paused ? "paused\n" : "resumed\n");
        fflush(stdout);
    }
    if ((keydown[RK_F7] && !keyprev[RK_F7]) || (cd32_fps && !cd32_fps_prev)) {
        show_fps = !show_fps;
        printf(show_fps ? "fps on\n" : "fps off\n");
        fflush(stdout);
    }
    if (keydown[RK_F5] && !keyprev[RK_F5]) {
        state_ok = md_save_state("DH1:md_state.bin");
        if (!state_ok)
            state_ok = md_save_state("md_state.bin");
        printf(state_ok ? "state saved\n" : "state save failed\n");
        fflush(stdout);
    }
    if (keydown[RK_F6] && !keyprev[RK_F6]) {
        state_ok = md_load_state("DH1:md_state.bin");
        if (!state_ok)
            state_ok = md_load_state("md_state.bin");
        if (state_ok && md_palette_dirty)
            upload_palette();
        printf(state_ok ? "state loaded\n" : "state load failed\n");
        fflush(stdout);
    }
    cd32_pause_prev = cd32_pause;
    cd32_fps_prev = cd32_fps;

    joy = JOY1DAT;
    right = (joy >> 1) & 1;
    left = (joy >> 9) & 1;
    down = ((joy >> 1) ^ joy) & 1;
    up = ((joy >> 9) ^ (joy >> 8)) & 1;
    fire1 = !(CIAA_PRA & PORT1_FIRE);
    fire2 = !(POTINP & PORT1_DATRY);

    if (keydown[RK_RIGHT]) right = 1;
    if (keydown[RK_LEFT]) left = 1;
    if (keydown[RK_DOWN]) down = 1;
    if (keydown[RK_UP]) up = 1;

    if (up) pad |= 0x01;
    if (down) pad |= 0x02;
    if (left) pad |= 0x04;
    if (right) pad |= 0x08;
    if (keydown[RK_LCTRL] || (cd32 & (CD32_YELLOW | CD32_GREEN | CD32_LSHOULDER)))
        pad |= 0x10;                                             /* A */
    if (keydown[RK_SPACE] || fire1 || (cd32 & CD32_RED))
        pad |= 0x20;                                             /* B */
    if (keydown[RK_LALT] || keydown[RK_RALT] || fire2 || (cd32 & (CD32_BLUE | CD32_RSHOULDER)))
        pad |= 0x40;                                             /* C */
    if (keydown[RK_1] || (cd32 & CD32_PLAY))
        pad |= 0x80;                                             /* Start */
    if (frontend_frames < 960) {
        if (keydown[RK_SPACE] || fire1 || (cd32 & CD32_RED))
            pad |= 0x80;                                         /* fire starts from title */
        if (frontend_frames >= 900 && frontend_frames < 960)
            pad |= 0x80;                                         /* auto-start after intro */
    }

    if ((cd32 & (CD32_BLUE | CD32_RED | CD32_YELLOW | CD32_GREEN)) ==
        (CD32_BLUE | CD32_RED | CD32_YELLOW | CD32_GREEN)) {
        quit_requested = 1;
        return;
    }

    md_pad[0] = pad;
    md_pad[1] = 0;
    memcpy(keyprev, keydown, sizeof keyprev);
}

int main(int argc, char **argv)
{
    const char *rom_path;

    if (argc < 2) {
        printf("usage: shinobi3_md <Shinobi III .bin/.gen/.smd>\n");
        return 20;
    }
    rom_path = argv[1];
    if (!md_load_rom(rom_path))
        return 20;

    md_machine_init();
    if (!open_rtg()) {
        printf("failed to open RTG screen\n");
        shutdown_rtg();
        return 20;
    }
    upload_palette();
    md_audio_amiga_open();

    while (!quit_requested) {
        poll_input();
        if (quit_requested)
            break;
        if (!paused) {
            md_run_frame();
            md_audio_amiga_frame();
            frontend_frames++;
            if (md_palette_dirty)
                upload_palette();
        }
        present_frame();
        frame_pace();
    }

    md_audio_amiga_close();
    shutdown_rtg();
    return 0;
}
