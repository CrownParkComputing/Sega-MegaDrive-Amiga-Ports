# Road Rash 2 MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/Road_Rash_2
./package.sh "/path/to/Road Rash 2.zip"
```

Outputs in `dist/`:
- `RoadRash2_MD_Boot.hdf` — boot partition
- `RoadRash2_MD.hdf` — game partition (interpreter + ROM)
- `RoadRash2_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/RoadRash2_MD.uae
```

## Controls

Same as all Mega Drive ports — see [games/Revenge_of_Shinobi/README.md](../Revenge_of_Shinobi/README.md).
