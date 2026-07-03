/*
 * mdloader.c - Mega Drive game chooser for the shared shinobi3_md interpreter.
 *
 * Reads games.cfg ("Title|cartpath|statepath" per line) from the current dir,
 * shows an RTG menu, launches the chosen cart synchronously and loops when the
 * player exits back (the game auto-saves its state on exit). If a state file
 * exists for the chosen game, asks Resume / Start fresh first.
 *
 * Controls: d-pad or keyboard arrows + fire/Red/Return to select,
 * Esc or the Quit entry to leave.
 */
#include <exec/exec.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR_W 320
#define SCR_H 256

/* menu pens: the background image reserves 252-255 for the UI */
#define PEN_PANEL 252
#define PEN_SEL   253
#define PEN_TXT   254
#define PEN_ACC   255

#define CIAA_PRA   (*(volatile unsigned char *)0xbfe001UL)
#define CIAA_DDRA  (*(volatile unsigned char *)0xbfe201UL)
#define JOY1DAT    (*(volatile unsigned short *)0xdff00cUL)
#define POTINP     (*(volatile unsigned short *)0xdff016UL)
#define POTGO      (*(volatile unsigned short *)0xdff034UL)
#define PORT1_FIRE 0x80
#define PORT1_DATRY 0x4000
#define POTGO_PORT1 0x6f00
#define POTGO_RESET 0xff00

#define CD32_RED       0x40
#define CD32_PLAY      0x02

#define RK_UP     0x4C
#define RK_DOWN   0x4D
#define RK_LEFT   0x4F
#define RK_RIGHT  0x4E
#define RK_RETURN 0x44
#define RK_ENTER  0x43
#define RK_ESC    0x45

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

typedef struct {
    char title[40];
    char cart[108];
    char state[108];
} Game;

static Game games[16];
static int ngames;

static struct Screen *scr;
static struct Window *win;

/* intro music: VGM player (mdl_music.c) into a Paula AUD0/1 mono ring */
int  mus_load(const char *path);
void mus_render(signed char *out, int n);
void mus_restart(void);
int  mus_active(void);

#define AUD_RATE   16574
#define AUD_PER    (3546895 / AUD_RATE)
#define AUD_SPF    (AUD_RATE / 50 + 32)
#define AUD_RING   (16 * AUD_SPF)
#define AUD_LEAD   (4 * AUD_SPF)
#define CUSTOMW    ((volatile unsigned short *)0xdff000)
#define R_DMACON   (0x096/2)
#define R_AUD0LCH  (0x0a0/2)
#define R_AUD0LEN  (0x0a4/2)
#define R_AUD0PER  (0x0a6/2)
#define R_AUD0VOL  (0x0a8/2)
#define R_AUD1LCH  (0x0b0/2)
#define R_AUD1LEN  (0x0b4/2)
#define R_AUD1PER  (0x0b6/2)
#define R_AUD1VOL  (0x0b8/2)
#define R_AUD2VOL  (0x0c8/2)
#define R_AUD3VOL  (0x0d8/2)

static signed char *mus_ring;
static unsigned long mus_play, mus_wrote;
static unsigned mus_adv_acc;
static int mus_on;

static void music_ring_fill(unsigned long upto)
{
    while ((long)(upto - mus_wrote) > 0) {
        unsigned long pos = mus_wrote % AUD_RING;
        unsigned long chunk = AUD_RING - pos;
        if (chunk > upto - mus_wrote)
            chunk = upto - mus_wrote;
        mus_render(mus_ring + pos, (int)chunk);
        mus_wrote += chunk;
    }
    CacheClearE(mus_ring, AUD_RING, CACRF_ClearD);
}

static void music_start(void)
{
    volatile unsigned short *c = CUSTOMW;
    unsigned long a;
    if (!mus_on)
        return;
    if (!mus_ring)
        mus_ring = (signed char *)AllocMem(AUD_RING, MEMF_CHIP | MEMF_CLEAR);
    if (!mus_ring)
        return;
    mus_play = mus_wrote = 0;
    mus_adv_acc = 0;
    music_ring_fill(AUD_LEAD);
    a = (unsigned long)mus_ring;
    c[R_DMACON] = 0x000f;
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0; c[R_AUD2VOL] = 0; c[R_AUD3VOL] = 0;
    c[R_AUD0LCH] = (unsigned short)(a >> 16); c[R_AUD0LCH + 1] = (unsigned short)a;
    c[R_AUD1LCH] = (unsigned short)(a >> 16); c[R_AUD1LCH + 1] = (unsigned short)a;
    c[R_AUD0LEN] = AUD_RING / 2; c[R_AUD1LEN] = AUD_RING / 2;
    c[R_AUD0PER] = AUD_PER; c[R_AUD1PER] = AUD_PER;
    c[R_AUD0VOL] = 52; c[R_AUD1VOL] = 52;
    c[R_DMACON] = 0x8203;
}

static void music_stop(void)
{
    volatile unsigned short *c = CUSTOMW;
    c[R_DMACON] = 0x000f;
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0; c[R_AUD2VOL] = 0; c[R_AUD3VOL] = 0;
}

static void music_frame(void)
{
    unsigned adv;
    if (!mus_on || !mus_ring)
        return;
    mus_adv_acc += AUD_RATE;
    adv = mus_adv_acc / 50;
    mus_adv_acc %= 50;
    mus_play += adv;
    if ((long)(mus_play - mus_wrote) > 0)
        mus_wrote = mus_play;
    music_ring_fill(mus_play + AUD_LEAD);
    /* silence the band just past the write head so a long frame plays a
     * micro-dropout, not old ring content ("reverb") */
    {
        unsigned long gpos = mus_wrote % AUD_RING;
        unsigned long glen = AUD_SPF;
        if (gpos + glen <= AUD_RING) {
            memset(mus_ring + gpos, 0, glen);
        } else {
            memset(mus_ring + gpos, 0, AUD_RING - gpos);
            memset(mus_ring, 0, glen - (AUD_RING - gpos));
        }
        CacheClearE(mus_ring, AUD_RING, CACRF_ClearD);
    }
}

/* optional background artwork: loader.img = u16 w, u16 h, 256*3 pal, pixels */
static unsigned char *bg_pix;
static int bg_w, bg_h;
static unsigned char bg_pal[768];
static int bg_have;

static void load_bg(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[4];
    if (!f)
        return;
    if (fread(hdr, 1, 4, f) == 4) {
        bg_w = (hdr[0] << 8) | hdr[1];
        bg_h = (hdr[2] << 8) | hdr[3];
        if (bg_w > 0 && bg_w <= SCR_W && bg_h > 0 && bg_h <= SCR_H &&
            fread(bg_pal, 1, 768, f) == 768) {
            bg_pix = (unsigned char *)malloc((size_t)bg_w * bg_h);
            if (bg_pix && fread(bg_pix, 1, (size_t)bg_w * bg_h, f) ==
                              (size_t)bg_w * bg_h)
                bg_have = 1;
        }
    }
    fclose(f);
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

static int load_config(void)
{
    FILE *f = fopen("games.cfg", "r");
    char line[256];
    if (!f)
        return 0;
    while (ngames < 16 && fgets(line, sizeof line, f)) {
        char *a, *b;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        a = strchr(line, '|');
        if (!a)
            continue;
        *a++ = 0;
        b = strchr(a, '|');
        if (!b)
            continue;
        *b++ = 0;
        strncpy(games[ngames].title, line, sizeof games[ngames].title - 1);
        strncpy(games[ngames].cart, a, sizeof games[ngames].cart - 1);
        strncpy(games[ngames].state, b, sizeof games[ngames].state - 1);
        ngames++;
    }
    fclose(f);
    return ngames;
}

static int state_exists(const char *path)
{
    BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (l) {
        UnLock(l);
        return 1;
    }
    return 0;
}

static void draw_text(int x, int y, int pen, const char *s)
{
    struct RastPort *rp = &scr->RastPort;
    SetAPen(rp, pen);
    SetBPen(rp, PEN_PANEL);
    SetDrMd(rp, JAM2);
    Move(rp, x, y);
    Text(rp, (CONST_STRPTR)s, strlen(s));
}

static int bar_anim, bar_dir;

static const unsigned long bar_hues[8][3] = {
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu },
    { 0xFFFFFFFFu, 0xDDDDDDDDu, 0x66666666u },
    { 0xFFFFFFFFu, 0xAAAAAAAAu, 0x22222222u },
    { 0xFFFFFFFFu, 0x77777777u, 0x77777777u },
    { 0xEEEEEEEEu, 0x55555555u, 0xCCCCCCCCu },
    { 0x88888888u, 0x99999999u, 0xFFFFFFFFu },
    { 0x66666666u, 0xEEEEEEEEu, 0xFFFFFFFFu },
    { 0xBBBBBBBBu, 0xFFFFFFFFu, 0xAAAAAAAAu },
};

static void draw_bar(int sel, int nitems, const char **items)
{
    struct RastPort *rp = &scr->RastPort;
    int w = scr->Width;
    int bar_h = 20;
    int xoff = bar_anim > 0 ? bar_dir * bar_anim * 14 : 0;
    char buf[48];
    (void)nitems;
    SetAPen(rp, PEN_PANEL);
    RectFill(rp, 0, 0, w - 1, bar_h - 1);
    draw_text(6, 13, PEN_TXT, "<");
    draw_text(w - 14, 13, PEN_TXT, ">");
    sprintf(buf, "%.34s", items[sel]);
    draw_text((w - (int)strlen(buf) * 8) / 2 + xoff, 13, PEN_SEL, buf);
}

static void draw_menu(int sel, int nitems, const char **items, const char *title)
{
    struct RastPort *rp = &scr->RastPort;
    int w = scr->Width, h = scr->Height;
    (void)title;
    if (bg_have)
        WriteChunkyPixels(rp, 0, 0, bg_w - 1, bg_h - 1, bg_pix, bg_w);
    else {
        SetAPen(rp, 0);
        RectFill(rp, 0, 0, w - 1, h - 1);
    }
    draw_bar(sel, nitems, items);
}

/* returns selected index, or -1 on Esc */
static int run_menu(int nitems, const char **items, const char *title)
{
    int sel = 0, dirty = 1;
    int hue = 0, hue_div = 0;
    unsigned prev_joy_up = 0, prev_joy_dn = 0, prev_fire = 1, prev_cd32 = 0;

    for (;;) {
        struct IntuiMessage *m;
        unsigned short joy;
        unsigned cd32;
        int up = 0, down = 0, fire = 0;

        if (dirty) { draw_menu(sel, nitems, items, title); dirty = 0; }
        if (bar_anim > 0) { bar_anim--; draw_bar(sel, nitems, items); }
        if (++hue_div >= 4) {
            hue_div = 0;
            hue = (hue + 1) & 7;
            SetRGB32(&scr->ViewPort, PEN_SEL,
                     bar_hues[hue][0], bar_hues[hue][1], bar_hues[hue][2]);
        }
        WaitTOF();
        while (win && win->UserPort &&
               (m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD raw = m->Code;
            ReplyMsg((struct Message *)m);
            if (cls == IDCMP_RAWKEY) {
                if (raw == RK_UP || raw == RK_LEFT) up = 1;
                if (raw == RK_DOWN || raw == RK_RIGHT) down = 1;
                if (raw == RK_RETURN || raw == RK_ENTER) fire = 1;
                if (raw == RK_ESC) return -1;
            }
        }

        joy = JOY1DAT;
        {
            unsigned jup = (((joy >> 9) ^ (joy >> 8)) & 1) | ((joy >> 9) & 1);
            unsigned jdn = (((joy >> 1) ^ joy) & 1) | ((joy >> 1) & 1);
            unsigned jfire = !(CIAA_PRA & PORT1_FIRE);
            if (jup && !prev_joy_up) up = 1;
            if (jdn && !prev_joy_dn) down = 1;
            if (jfire && !prev_fire) fire = 1;
            prev_joy_up = jup; prev_joy_dn = jdn; prev_fire = jfire;
        }
        cd32 = read_cd32_port1();
        if ((cd32 & CD32_RED) && !(prev_cd32 & CD32_RED)) fire = 1;
        if ((cd32 & CD32_PLAY) && !(prev_cd32 & CD32_PLAY)) fire = 1;
        prev_cd32 = cd32;

        music_frame();
        if (up)   { sel = (sel + nitems - 1) % nitems; bar_anim = 12; bar_dir = -1; }
        if (down) { sel = (sel + 1) % nitems; bar_anim = 12; bar_dir = 1; }
        if (fire) return sel;
    }
}

static void run_game(const Game *g, int resume)
{
    char cmd[300];
    BPTR nil_in, nil_out;
    sprintf(cmd, "%s \"%.106s\" \"%.106s\"%s",
            "DH1:shinobi3_md", g->cart, g->state, resume ? " RESUME" : "");
    music_stop();
    nil_in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    nil_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    SystemTags((CONST_STRPTR)cmd,
               SYS_Input, (ULONG)nil_in,
               SYS_Output, (ULONG)nil_out,
               TAG_END);
    if (nil_in) Close(nil_in);
    if (nil_out) Close(nil_out);
    if (scr) ScreenToFront(scr);
    if (mus_on) {
        mus_restart();
        music_start();
    }
}

int main(void)
{
    ULONG mode;

    if (!load_config()) {
        printf("mdloader: no games.cfg found\n");
        return 20;
    }
    printf("cfg ok (%d games)\n", ngames); fflush(stdout);
    load_bg("loader.img");
    printf("bg %d\n", bg_have); fflush(stdout);
    mus_on = mus_load("intro.pcm");
    if (!mus_on)
        mus_on = mus_load("intro.vgm");
    printf("mus %d\n", mus_on); fflush(stdout);

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    if (!IntuitionBase || !GfxBase)
        return 20;

    /* RTG if present, otherwise a plain AGA/ECS PAL HiRes screen — the
     * loader needs nothing beyond Text() so it runs on a stock A1200 too */
    mode = BestModeID(BIDTAG_NominalWidth, SCR_W,
                      BIDTAG_NominalHeight, SCR_H,
                      BIDTAG_Depth, 8, TAG_END);
    if (mode != INVALID_ID)
        scr = OpenScreenTags(0, SA_DisplayID, mode, SA_Width, SCR_W,
                             SA_Height, SCR_H, SA_Depth, 8, SA_Quiet, TRUE,
                             SA_ShowTitle, FALSE, TAG_END);
    if (!scr)
        scr = OpenScreenTags(0, SA_DisplayID, (PAL_MONITOR_ID | LORES_KEY),
                             SA_Width, SCR_W, SA_Height, SCR_H, SA_Depth, 8,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_END);
    if (!scr)
        scr = OpenScreenTags(0, SA_Width, SCR_W, SA_Height, SCR_H, SA_Depth, 8,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_END);
    if (!scr)
        return 20;

    {
        int i;
        if (bg_have) {
            for (i = 0; i < 256; i++)
                SetRGB32(&scr->ViewPort, i,
                         (ULONG)bg_pal[i * 3 + 0] * 0x01010101u,
                         (ULONG)bg_pal[i * 3 + 1] * 0x01010101u,
                         (ULONG)bg_pal[i * 3 + 2] * 0x01010101u);
        } else {
            SetRGB32(&scr->ViewPort, 0, 0, 0, 0);
            SetRGB32(&scr->ViewPort, PEN_PANEL, 0, 0, 0);
            SetRGB32(&scr->ViewPort, PEN_SEL, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
            SetRGB32(&scr->ViewPort, PEN_TXT, 0x88888888, 0x88888888, 0x88888888);
            SetRGB32(&scr->ViewPort, PEN_ACC, 0xFFFFFFFF, 0x99999999, 0x22222222);
        }
    }

    printf("screen %dx%d\n", (int)scr->Width, (int)scr->Height); fflush(stdout);
    win = OpenWindowTags(0, WA_CustomScreen, (ULONG)scr,
                         WA_Left, 0, WA_Top, 0,
                         WA_Width, scr->Width, WA_Height, scr->Height,
                         WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_Activate, TRUE,
                         WA_RMBTrap, TRUE, WA_IDCMP, IDCMP_RAWKEY, TAG_END);

    printf("music start...\n"); fflush(stdout);
    music_start();
    printf("menu\n"); fflush(stdout);

    for (;;) {
        const char *items[18];
        static char labels[16][56];
        int i, sel;

        for (i = 0; i < ngames; i++) {
            sprintf(labels[i], "%.30s%s", games[i].title,
                    state_exists(games[i].state) ? "  [SAVED]" : "");
            items[i] = labels[i];
        }
        items[ngames] = "Quit";
        sel = run_menu(ngames + 1, items, "GAME");
        if (sel < 0 || sel == ngames)
            break;

        if (state_exists(games[sel].state)) {
            static const char *ritems[2] = { "Resume saved game", "Start fresh" };
            int r = run_menu(2, ritems, "SAVE");
            if (r < 0)
                continue;
            run_game(&games[sel], r == 0);
        } else {
            run_game(&games[sel], 0);
        }
    }

    music_stop();
    if (mus_ring) FreeMem(mus_ring, AUD_RING);
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
