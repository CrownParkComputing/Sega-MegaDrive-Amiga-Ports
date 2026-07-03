# Sega Mega Drive - Amiga RTG Ports

Native Amiga RTG interpreter for Sega Mega Drive cartridge ROMs. Built with
m68k-amigaos-gcc, targeting A1200 AGA + Picasso96 RTG graphics.

## Games

| Game | Folder |
|------|--------|
| Revenge of Shinobi | [games/Revenge_of_Shinobi](games/Revenge_of_Shinobi/) |
| Road Rash | [games/Road_Rash](games/Road_Rash/) |
| Road Rash 2 | [games/Road_Rash_2](games/Road_Rash_2/) |
| Road Rash 3 | [games/Road_Rash_3](games/Road_Rash_3/) |
| Sonic the Hedgehog | [games/Sonic_the_Hedgehog](games/Sonic_the_Hedgehog/) |
| Sonic the Hedgehog 2 | [games/Sonic_the_Hedgehog_2](games/Sonic_the_Hedgehog_2/) |
| Sonic 3 & Knuckles | [games/Sonic_3_and_Knuckles](games/Sonic_3_and_Knuckles/) |
| OutRun | [games/OutRun](games/OutRun/) |
| OutRun 2019 | [games/OutRun_2019](games/OutRun_2019/) |
| OutRunners | [games/OutRunners](games/OutRunners/) |

All games share a single Mega Drive interpreter binary (`shinobi3_md`).
Each game folder contains its own packaging script and Amiberry config.

## Repository Structure

```
shared/                         # Shared Mega Drive interpreter (all games)
├── build_megadrive.sh          # Build script → bin/shinobi3_md
├── package_rtg_hdf.sh          # Package binary + ROM → RTG boot HDFs
├── md_amiga.c                  # Amiga RTG presenter and input loop
├── md_machine.c                # 68k bus, I/O, interrupts, frame timing
├── md_vdp.c                    # VDP ports, DMA, planes, sprites, palette
├── md_audio_amiga.c            # Paula ring-buffer playback
├── md_audio_stub.c             # Z80 sound bus, PSG, YM2612 FM
├── rom_loader.c                # .bin/.gen/.smd cartridge loader
├── cores/
│   ├── musashi/                # Musashi 68000 CPU emulator (vendored)
│   ├── ymfm/                   # ymfm YM2612 core (BSD-3, default FM)
│   ├── gpgx_ym2612/            # Genesis Plus GX YM2612 (non-commercial, dev A/B only)
│   ├── nuked_opn2/             # Nuked-OPN2 YM3438/YM2612 (LGPL-2.1+, dev only)
│   ├── ym/                     # Fast FM synthesis (deprecated)
│   ├── z80.c                   # Z80 emulator
│   └── md_m68k_interface.c     # Musashi memory callbacks
├── hal/
│   └── md_machine.h            # Mega Drive machine interface
└── tools/
    └── md_host_probe.c         # Host capability probe

games/                          # Per-game configs and packaging
├── Revenge_of_Shinobi/
│   ├── package.sh              # Builds + packages this game
│   ├── README.md
│   └── dist/                   # Output HDFs + .uae (gitignored)
└── OutRun/
    ├── package.sh
    ├── README.md
    └── dist/

bin/                            # Build output (gitignored)
build/                          # Object files (gitignored)
```

## Building

Prerequisites:
- `m68k-amigaos-gcc` (amiga-amigaos toolchain)
- `xdftool` (for packaging HDF images)

```bash
./shared/build_megadrive.sh
```

Output: `bin/shinobi3_md`

## Packaging a Game

```bash
cd games/Revenge_of_Shinobi
./package.sh "/path/to/ROM.zip"
```

Or specify the ROM and title explicitly via the shared script:

```bash
./shared/package_rtg_hdf.sh "/path/to/ROM.zip" "MyGame_MD"
```

## Running

```bash
amiberry -f games/Revenge_of_Shinobi/dist/RevengeOfShinobi_MD.uae
```

## Build Options

| Variable | Default | Description |
|----------|---------|-------------|
| `MD_YMFM_FM` | 1 | ymfm YM2612 (BSD-3-Clause) — hardware-accurate FM at the chip's native 53267 Hz, box-averaged to the Paula rate. License-clean for commercial builds |
| `MD_GPGX_FM` | 0 | Genesis Plus GX YM2612 — accuracy A/B reference. NON-COMMERCIAL license: never ship in a sold build |
| `MD_ACCURATE_OPN2` | 0 | Use Nuked-OPN2 for reference-grade YM2612 (too slow for the Amiga build) |
| `MD_FAST_FM` | 0 | Old fast FM approximation (deprecated) |
| `MD_ENABLE_AUDIO` | 1 | Enable audio output |
| `MD_RUN_Z80_SOUND` | 1 | Run Z80 sound CPU |
| `OUTPUT_NAME` | shinobi3_md | Binary name |
| `BUILD_DIR` | ./build | Object file directory |
| `BIN_DIR` | ./bin | Output binary directory |

## References

- Sega Megadrive hardware manual
- [Genesis Programming Guide](https://cgfm2.emuuniversity.com/)
- [JGenesis](https://github.com/jsgroth/jgenesis) — VDP/audio behavior reference
- [ymfm](https://github.com/aaronsgiles/ymfm) — YM2612 core (Aaron Giles, BSD-3-Clause; the shipping FM core)
- [Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX) — YM2612 accuracy reference (Eke-Eke; non-commercial, dev builds only)
- [Nuked-OPN2](https://github.com/nukeykt/Nuked-OPN2) — YM3438/YM2612 core
- Musashi 68k CPU emulator

## License

- Mega Drive interpreter code: MIT
- Musashi 68000 core: see shared/cores/musashi/ for license
- Genesis Plus GX YM2612: non-commercial license (see shared/cores/gpgx_ym2612/LICENSE.txt)
- Nuked-OPN2: LGPL-2.1+ (see shared/cores/nuked_opn2/LICENSE)
- ROM images are not included — you must supply your own legally obtained cartridges