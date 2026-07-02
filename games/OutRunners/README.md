# OutRunners MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/OutRunners
./package.sh "/path/to/OutRunners.zip"
```

Outputs in `dist/`:
- `OutRunners_MD_Boot.hdf` — boot partition
- `OutRunners_MD.hdf` — game partition (interpreter + ROM)
- `OutRunners_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/OutRunners_MD.uae
```

## Controls

Same as all Mega Drive ports — see [games/Revenge_of_Shinobi/README.md](../Revenge_of_Shinobi/README.md).
