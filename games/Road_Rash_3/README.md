# Road Rash 3 MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/Road_Rash_3
./package.sh "/path/to/Road Rash 3.zip"
```

Outputs in `dist/`:
- `RoadRash3_MD_Boot.hdf` — boot partition
- `RoadRash3_MD.hdf` — game partition (interpreter + ROM)
- `RoadRash3_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/RoadRash3_MD.uae
```

## Controls

Same as all Mega Drive ports — see [games/Revenge_of_Shinobi/README.md](../Revenge_of_Shinobi/README.md).
