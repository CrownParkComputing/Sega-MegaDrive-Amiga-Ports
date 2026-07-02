# Revenge of Shinobi MD - Amiga RTG Port

## Building

From the repo root:

```bash
./shared/build_megadrive.sh
```

This produces `bin/shinobi3_md` — the shared Mega Drive interpreter binary.

## Packaging

```bash
cd games/Revenge_of_Shinobi
./package.sh "/path/to/Revenge of Shinobi.zip"
```

Outputs in `dist/`:
- `RevengeOfShinobi_MD_Boot.hdf` — boot partition
- `RevengeOfShinobi_MD.hdf` — game partition (interpreter + ROM)
- `RevengeOfShinobi_MD.uae` — Amiberry config

## Running

```bash
amiberry -f dist/RevengeOfShinobi_MD.uae
```

Or copy `shinobi3_md` + your legally supplied ROM to an Amiga RTG environment:

```
shinobi3_md "Revenge of Shinobi (USA).bin"
```

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