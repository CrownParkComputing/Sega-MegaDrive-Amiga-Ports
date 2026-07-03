#include "hal/md_machine.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static FILE *audio_file;

static void write_ppm(const char *path)
{
    FILE *f;
    uint32_t pal[256];
    int y, x;

    if (!path || !path[0])
        return;

    md_vdp_palette_dyn(pal);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", MD_SCREEN_W, MD_SCREEN_H);
    for (y = 0; y < MD_SCREEN_H; y++) {
        for (x = 0; x < MD_SCREEN_W; x++) {
            uint32_t rgb = pal[md_screen[y][x]];
            fputc((int)((rgb >> 16) & 0xFF), f);
            fputc((int)((rgb >> 8) & 0xFF), f);
            fputc((int)(rgb & 0xFF), f);
        }
    }

    fclose(f);
}

static double now_sec(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    int frames = 1;
    int start_from = 0;
    int start_to = 0;
    uint16_t start_mask = 0x80;
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: md_host_probe cart.bin [frames] [start_from start_to] [dump.ppm] [audio.raw] [start_mask_hex]\n");
        return 2;
    }
    if (argc > 2)
        frames = atoi(argv[2]);
    if (argc > 4) {
        start_from = atoi(argv[3]);
        start_to = atoi(argv[4]);
    }
    if (argc > 7)
        start_mask = (uint16_t)strtoul(argv[7], NULL, 0);

    if (!md_load_rom(argv[1]))
        return 1;
    md_machine_init();
    if (argc > 6) {
        audio_file = fopen(argv[6], "wb");
        if (!audio_file)
            perror(argv[6]);
    }

    for (i = 0; i < frames; i++) {
        double t0 = now_sec();
        unsigned checksum = 0;
        signed char aud[MD_SND_RATE / 60];
        long aud_sum = 0;
        int aud_peak = 0;
        int nonzero = 0;
        int p;
        md_pad[0] = (i + 1 >= start_from && i + 1 <= start_to) ? start_mask : 0;
        md_run_frame();
        md_audio_render(aud, (int)sizeof aud);
        if (audio_file)
            fwrite(aud, 1, sizeof aud, audio_file);
        for (p = 0; p < (int)sizeof aud; p++) {
            int v = aud[p];
            int av = v < 0 ? -v : v;
            aud_sum += av;
            if (av > aud_peak)
                aud_peak = av;
        }
        const uint8_t *screen = &md_screen[0][0];
        for (p = 0; p < MD_SCREEN_W * MD_SCREEN_H; p++) {
            uint8_t v = screen[p];
            checksum = (checksum * 33u) ^ v;
            if (v)
                nonzero++;
        }
        printf("frame %d %.3fs pc=%06x vdp0=%02x vdp1=%02x nz=%d sum=%08x aud_avg=%ld aud_peak=%d\n",
               i + 1, now_sec() - t0,
               m68k_get_reg(NULL, M68K_REG_PC),
               md_vdp_reg[0], md_vdp_reg[1], nonzero, checksum,
               aud_sum / (long)sizeof aud, aud_peak);
        fflush(stdout);
    }

    printf("vdp regs:");
    for (i = 0; i < 24; i++)
        printf(" %02x", md_vdp_reg[i]);
    printf("\n");
    {
        int zreset, zbus, zpc;
        unsigned long zcycles, ymw, psgw, keyw;
        unsigned active;
        md_audio_debug(&zreset, &zbus, &zpc, &zcycles, &ymw, &psgw, &keyw, &active);
        printf("audio debug: zreset=%d zbus=%d zpc=%04x zcycles=%lu ym_writes=%lu psg_writes=%lu key_writes=%lu active=%02x\n",
               zreset, zbus, zpc & 0xFFFF, zcycles, ymw, psgw, keyw, active);
        {
            unsigned long zramw, bankw, statr;
            unsigned zaddr, zval, zbank, yreg, yval;
            md_audio_debug_ext(&zramw, &zaddr, &zval, &bankw, &zbank, &statr, &yreg, &yval);
            printf("audio extra: zram_writes=%lu last_zram=%04x:%02x bank_writes=%lu last_bank=%03x ym_status_reads=%lu last_ym=%02x:%02x\n",
                   zramw, zaddr, zval, bankw, zbank, statr, yreg, yval);
        }
    }
    if (argc > 5)
        write_ppm(argv[5]);
    if (audio_file)
        fclose(audio_file);
    return 0;
}
