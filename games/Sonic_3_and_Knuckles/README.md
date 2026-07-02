# Sonic 3 & Knuckles MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/Sonic_3_and_Knuckles
./package.sh "/path/to/Sonic 3 & Knuckles.zip"
```

Outputs in `dist/`:
- `Sonic3Knuckles_MD_Boot.hdf` — boot partition
- `Sonic3Knuckles_MD.hdf` — game partition (interpreter + ROM)
- `Sonic3Knuckles_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/Sonic3Knuckles_MD.uae
```

## Controls

Same as all Mega Drive ports — see [games/Revenge_of_Shinobi/README.md](../Revenge_of_Shinobi/README.md).
