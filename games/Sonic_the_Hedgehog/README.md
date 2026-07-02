# Sonic the Hedgehog MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/Sonic_the_Hedgehog
./package.sh "/path/to/Sonic the Hedgehog.zip"
```

Outputs in `dist/`:
- `Sonic1_MD_Boot.hdf` — boot partition
- `Sonic1_MD.hdf` — game partition (interpreter + ROM)
- `Sonic1_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/Sonic1_MD.uae
```

## Controls

Same as all Mega Drive ports — see [games/Revenge_of_Shinobi/README.md](../Revenge_of_Shinobi/README.md).
