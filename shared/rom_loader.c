/*
 * rom_loader.c - Sega Mega Drive cartridge loader.
 *
 * Normal .bin/.gen dumps are copied directly. Super Magic Drive dumps are
 * accepted when they have a 512-byte header and 16KB interleaved blocks.
 */

#include "hal/md_machine.h"

#include <stdio.h>
#include <string.h>

static int read_file(FILE *f, long offset, uint8_t *dst, long size)
{
    if (fseek(f, offset, SEEK_SET) != 0)
        return 0;
    return fread(dst, 1, (size_t)size, f) == (size_t)size;
}

static void deinterleave_smd(uint8_t *dst, const uint8_t *src, long size)
{
    long block;
    for (block = 0; block + 0x4000 <= size; block += 0x4000) {
        long i;
        const uint8_t *s = src + block;
        uint8_t *d = dst + block;
        for (i = 0; i < 0x2000; i++) {
            d[i * 2 + 0] = s[0x2000 + i];
            d[i * 2 + 1] = s[i];
        }
    }
}

int md_load_rom(const char *path)
{
    FILE *f;
    long size;
    long data_size;
    int is_smd;

    if (!path)
        return 0;

    f = fopen(path, "rb");
    if (!f) {
        printf("cannot open ROM: %s\n", path);
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return 0;
    }

    is_smd = (size > 512 && ((size - 512) & 0x3FFF) == 0);
    data_size = is_smd ? size - 512 : size;
    if (data_size > MD_ROM_MAX) {
        printf("ROM too large: %ld bytes, max %u\n", data_size, (unsigned)MD_ROM_MAX);
        fclose(f);
        return 0;
    }

    memset(md_rom, 0, sizeof md_rom);
    if (is_smd) {
        static uint8_t tmp[MD_ROM_MAX];
        if (!read_file(f, 512, tmp, data_size)) {
            fclose(f);
            return 0;
        }
        deinterleave_smd(md_rom, tmp, data_size);
    } else {
        if (!read_file(f, 0, md_rom, data_size)) {
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    md_rom_size = (uint32_t)data_size;
    printf("loaded %s (%ld bytes%s)\n", path, data_size, is_smd ? ", SMD deinterleaved" : "");
    return 1;
}
