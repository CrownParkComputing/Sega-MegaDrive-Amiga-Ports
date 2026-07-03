/* mdl_segapcm.c - integer SegaPCM (315-5218) for the loader's VGM player.
 * Adapted from the family's pdrift_segapcm.c (in-house, MAME-faithful);
 * bank shift/mask are runtime values taken from the VGM header's interface
 * register (OutRun arcade = shift 12, mask 0x70) instead of compile-time. */
#include <stdint.h>

static const uint8_t *pcm_rom;
static uint32_t pcm_mask;
static uint8_t ram[0x100];
static uint32_t low[16];
static uint32_t bank_shift = 12;
static uint32_t bank_mask = 0x70;

void mdl_segapcm_reset(void)
{
    int i, ch;
    for (i = 0; i < 0x100; i++) ram[i] = 0;
    for (i = 0; i < 16; i++) low[i] = 0;
    for (ch = 0; ch < 16; ch++) ram[8 * ch + 0x86] |= 1;
}

/* iface = VGM header dword 0x3C: low byte = bank shift, MAME default mask
 * 0x70 OR'd with bits 16-23 (BANK_MASKF8-style overrides) */
void mdl_segapcm_init(const uint8_t *rom, uint32_t romsize, uint32_t iface)
{
    uint32_t m = 1;
    pcm_rom = rom;
    while (m < romsize) m <<= 1;
    if (m > romsize) m >>= 1;
    pcm_mask = m ? m - 1 : 0;
    bank_shift = iface & 0xff;
    bank_mask = 0x70 | ((iface >> 16) & 0xfc);
    mdl_segapcm_reset();
}

void mdl_segapcm_write(uint32_t addr, uint8_t v) { ram[addr & 0xff] = v; }

/* One native-rate (clock/128) sample; adds the 16-channel mix into *l,*r. */
void mdl_segapcm_render(int32_t *l, int32_t *r)
{
    int32_t ml = 0, mr = 0;
    int ch;
    if (!pcm_rom) return;
    for (ch = 0; ch < 16; ch++) {
        uint8_t *regs = ram + 8 * ch;
        uint32_t offset, addr, loop;
        uint8_t end;
        int32_t v;
        if (regs[0x86] & 1) continue;

        offset = (uint32_t)(regs[0x86] & bank_mask) << bank_shift;
        addr = ((uint32_t)regs[0x85] << 16) | ((uint32_t)regs[0x84] << 8) | low[ch];
        loop = ((uint32_t)regs[0x05] << 16) | ((uint32_t)regs[0x04] << 8);
        end = (uint8_t)(regs[6] + 1);

        if ((addr >> 16) == end) {
            if (regs[0x86] & 2) { regs[0x86] |= 1; continue; }
            addr = loop;
        }

        v = (int32_t)pcm_rom[(offset + (addr >> 8)) & pcm_mask] - 0x80;
        ml += v * (regs[2] & 0x7f);
        mr += v * (regs[3] & 0x7f);
        addr = (addr + regs[7]) & 0xffffff;

        regs[0x84] = (uint8_t)(addr >> 8);
        regs[0x85] = (uint8_t)(addr >> 16);
        low[ch] = (regs[0x86] & 1) ? 0 : (addr & 0xff);
    }
    *l += ml;
    *r += mr;
}
