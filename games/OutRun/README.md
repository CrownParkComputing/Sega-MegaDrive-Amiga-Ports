# OutRun MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/OutRun
./package.sh "/path/to/OutRun.zip"
```

Outputs in `dist/`:
- `OutRun_MD_Boot.hdf` — boot partition
- `OutRun_MD.hdf` — game partition (interpreter + ROM)
- `OutRun_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/OutRun_MD.uae
```

Or copy `shinobi3_md` + your legally supplied ROM to an Amiga RTG environment:

```
shinobi3_md "OutRun (USA).bin"
```

## Notes

OutRun uses the same Mega Drive interpreter as Revenge of Shinobi. The VDP
renderer has been aligned with JGenesis for H40 detection, invalid scroll
sizes, and scanline timing — these fixes resolved the Plane A grey-block
corruption that was visible in OutRun with the older renderer.

## Controls

Same as Revenge of Shinobi — see games/Revenge_of_Shinobi/README.md.