# OutRun 2019 MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/OutRun_2019
./package.sh "/path/to/OutRun 2019.zip"
```

Outputs in `dist/`:
- `OutRun2019_MD_Boot.hdf` — boot partition
- `OutRun2019_MD.hdf` — game partition (interpreter + ROM)
- `OutRun2019_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/OutRun2019_MD.uae
```

## Controls

Same as all Mega Drive ports — see [games/Revenge_of_Shinobi/README.md](../Revenge_of_Shinobi/README.md).
