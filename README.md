# Sega Mega Drive — Amiga Native Ports

Native Amiga ports of Sega Mega Drive games. Not an emulator — the Mega Drive
hardware is reimagined as native Amiga code that runs directly on the 68k CPU
with RTG graphics and Paula audio.

## Games

| Game | Type |
|------|------|
| Revenge of Shinobi | Single-game cartridge |
| Road Rash | Single-game cartridge |
| Road Rash 2 | Single-game cartridge |
| Road Rash 3 | Single-game cartridge |
| Sonic the Hedgehog | Single-game cartridge |
| Sonic the Hedgehog 2 | Single-game cartridge |
| Sonic 3 & Knuckles | Single-game cartridge |
| OutRun | Single-game cartridge |
| OutRun 2019 | Single-game cartridge |
| OutRunners | Single-game cartridge |
| OutRun Trilogy | Multi-game loader (OutRun + OutRun 2019 + OutRunners) |

## How This Works — Not an Emulator

This project does not emulate the Mega Drive. An emulator creates a virtual
machine that runs the original console's firmware and interprets its CPU
instructions at runtime. This project takes a fundamentally different approach:

**The Mega Drive's CPU IS the Amiga's CPU.** Both machines use Motorola 68k
family processors. The Mega Drive has a 68000; the Amiga has a 68000 (A500/A1000)
or 68020+ (A1200). The game code is already 68k machine code — it runs natively
on the Amiga's own processor. No instruction translation, no virtual machine,
no emulation layer.

What the project actually reimplements is the Mega Drive's **supporting
hardware** — the custom chips around the CPU that don't exist on Amiga:

- **VDP (Video Display Processor)** — the Mega Drive's graphics chip. Mode 5
  tile planes, sprites, DMA, CRAM/VSRAM/VRAM, per-scanline rendering. Reimplemented
  in C as a software renderer that writes directly to an RTG framebuffer.
- **Z80 sound CPU** — the Mega Drive's secondary CPU dedicated to audio. Emulated
  as a software Z80 interpreter (the Z80 is a different ISA, so this one does
  need interpretation, but it's a tiny CPU running a fixed sound driver, not the
  game logic).
- **YM2612 / OPN2** — the Mega Drive's FM synthesis chip. Three interchangeable
  cores: ymfm (BSD-3, default, accurate), GPGX (dev reference), and Nuked-OPN2
  (LGPL, cycle-accurate but too heavy for the target).
- **SN76489 PSG** — the simple square-wave tone generator, reimplemented in C.

So the 68000 game code runs at full native speed on the Amiga's own CPU. The
VDP is replaced by a software renderer painting to a Picasso96 RTG screen. The
Z80 and YM2612 are interpreted to produce audio, which is mixed into a Paula
ring buffer. Everything is native Amiga code — Exec, Intuition, graphics,
audio, the game logic itself.

The result: Mega Drive games running as Amiga applications, not inside an
emulator sandbox. The game's 68k code executes directly on the Amiga's 68k,
with the Mega Drive's custom chips replaced by native Amiga equivalents.

## Architecture

```
Game ROM (68000 machine code)
    │
    ├─ runs natively on Amiga 68k CPU ────────────── no emulation
    │
    ├─ md_machine.c  ─ Mega Drive bus, I/O, interrupts, frame timing
    ├─ md_vdp.c      ─ VDP renderer → RTG framebuffer (software)
    ├─ md_audio      ─ Z80 interpreter → YM2612/PSG → Paula ring buffer
    ├─ md_amiga.c    ─ Amiga RTG presenter, input (keyboard/CD32 pad)
    └─ rom_loader.c  ─ .bin/.gen/.smd cartridge loader
```

The **mdloader** (`shared/tools/mdloader.c`) is a native Amiga RTG menu that
lets the player choose between multiple games from a single HDF. It shows
a background image, plays intro music via Paula, and launches the chosen
cartridge with save/resume support.

## Repository Structure

```
shared/                         Shared Mega Drive interpreter
├── build_megadrive.sh          Build → bin/shinobi3_md
├── package_rtg_hdf.sh          Package single game → bootable HDF + .uae
├── package_trilogy_hdf.sh      Package multi-game loader set → bootable HDF + .uae
├── md_amiga.c                  RTG presenter, input, frame loop
├── md_machine.c                68k bus, I/O, interrupts
├── md_vdp.c                    VDP renderer (planes, sprites, DMA, palette)
├── md_audio_amiga.c            Paula ring-buffer playback
├── md_audio_stub.c             Z80 sound bus, PSG, YM2612 dispatch
├── rom_loader.c                Cartridge loader (.bin/.gen/.smd)
├── mdl_music.c                 VGM intro music engine for loader
├── cores/
│   ├── musashi/                Musashi 68000 (debug/host builds only)
│   ├── ymfm/                   ymfm YM2612 FM core (BSD-3, default)
│   ├── gpgx_ym2612/            GPGX YM2612 (dev reference)
│   ├── nuked_opn2/              Nuked-OPN2 YM3438 (LGPL, accuracy)
│   ├── ym/                     Fast FM approximation
│   ├── z80.c                   Z80 interpreter
│   └── tables.h                Z80 lookup tables
├── hal/
│   └── md_machine.h            Machine interface header
└── tools/
    ├── mdloader.c              RTG game chooser menu
    ├── make_loader_img.py      Loader background image converter
    ├── mdl_music.c             VGM music engine (platform-agnostic)
    ├── mdl_segapcm.c           SegaPCM for loader music
    └── md_host_probe.c         Host-side debug/profiling tool

games/                          Per-game configs and packaging
├── Revenge_of_Shinobi/
├── Road_Rash/
├── Road_Rash_2/
├── Road_Rash_3/
├── Sonic_the_Hedgehog/
├── Sonic_the_Hedgehog_2/
├── Sonic_3_and_Knuckles/
├── OutRun/
├── OutRun_2019/
└── OutRunners/
```

Each game folder has a `package.sh` that calls the shared packager with the
game's ROM and title. The OutRun trilogy is packaged via
`shared/package_trilogy_hdf.sh` which builds a multi-game HDF with the mdloader
menu, per-game save states, and optional loader artwork/music.

## Controls

| Input | Mega Drive |
|-------|-----------|
| Arrows / joystick | D-pad |
| Ctrl | A |
| Space / fire | B |
| Alt | C |
| 1 | Start |
| P | Pause |
| F5 | Save state |
| F6 | Load state |
| F7 | FPS toggle |
| Esc | Quit |
| CD32 Yellow/Green/L | A |
| CD32 Red | B |
| CD32 Blue/R | C |
| CD32 Play | Start |
| Hold 4 face buttons | Quit |

## Build Options

| Variable | Default | Description |
|----------|---------|-------------|
| MD_ACCURATE_OPN2 | 0 | Use Nuked-OPN2 for accurate YM2612 (slow) |
| MD_FAST_FM | 1 | Fast FM approximation |
| MD_ENABLE_AUDIO | 1 | Enable audio output |
| MD_RUN_Z80_SOUND | 1 | Run Z80 sound CPU |
| OUTPUT_NAME | shinobi3_md | Binary name |

## License

- Mega Drive interpreter code: MIT
- ymfm YM2612 core: BSD-3-Clause (see shared/cores/ymfm/LICENSE)
- Nuked-OPN2: LGPL-2.1+ (see shared/cores/nuked_opn2/LICENSE)
- Musashi 68000: see shared/cores/musashi/ for license
- ROM images are not included — supply your own legally obtained cartridges