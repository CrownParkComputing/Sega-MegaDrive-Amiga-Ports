#!/bin/bash
# Package the OutRun trilogy (OutRun / OutRun 2019 / OutRunners) behind the
# mdloader menu with per-game save states, as one RTG boot set.
#
# Usage:
#   ./shared/package_trilogy_hdf.sh
#
# Environment overrides:
#   ROM_OUTRUN     - path to OutRun ROM (default: looks in games/OutRun/build/package/cart.bin)
#   ROM_2019       - path to OutRun 2019 ROM
#   ROM_RUNNERS    - path to OutRunners ROM
#   LOADER_ART     - artwork PNG for loader background (optional)
#   LOADER_MP3     - intro music MP3 (converted to raw PCM, optional)
#   LOADER_VGZ     - intro music VGZ/VGM (optional, fallback to MP3)
#   BOOT_TEMPLATE  - RTG boot HDF template
#   KICKSTART      - Kickstart ROM path

set -euo pipefail
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GAME_DIR="${GAME_DIR:-$REPO_DIR/games/OutRun}"
DIST_DIR="$GAME_DIR/dist/trilogy"
BUILD_DIR="$GAME_DIR/build/trilogy"

BIN="$REPO_DIR/bin/shinobi3_md"
LOADER="$REPO_DIR/bin/mdloader"

BOOT_TEMPLATE="${BOOT_TEMPLATE:-/home/jon/Amiberry/HardDrives/RTG1.hdf}"
KICKSTART="${KICKSTART:-/home/jon/Amiberry/ROMs/kick40068.A1200.rom}"

ROM_OUTRUN="${ROM_OUTRUN:-$REPO_DIR/games/OutRun/build/package/cart.bin}"
ROM_2019="${ROM_2019:-$REPO_DIR/games/OutRun_2019/build/package/cart.bin}"
ROM_RUNNERS="${ROM_RUNNERS:-$REPO_DIR/games/OutRunners/build/package/cart.bin}"

TITLE="OutRunTrilogy_MD"
BOOT_HDF="$DIST_DIR/${TITLE}_Boot.hdf"
GAME_HDF="$DIST_DIR/${TITLE}.hdf"
UAE="$DIST_DIR/${TITLE}.uae"

mkdir -p "$DIST_DIR" "$BUILD_DIR"

# Build interpreter if missing
if [ ! -f "$BIN" ]; then
  "$REPO_DIR/shared/build_megadrive.sh"
fi

# Build loader if missing
if [ ! -f "$LOADER" ]; then
  echo "Building mdloader..."
  m68k-amigaos-gcc -m68040 -noixemul -O3 -fomit-frame-pointer -DNDEBUG \
    -I "$REPO_DIR/shared" -I "$REPO_DIR/shared/hal" -I "$REPO_DIR/shared/cores" \
    -I "$REPO_DIR/shared/cores/musashi" -I "$REPO_DIR/shared/cores/musashi/softfloat" \
    -o "$LOADER" "$REPO_DIR/shared/tools/mdloader.c" \
    -Wl,--start-group -lamiga -lm -lgcc -Wl,--end-group
fi

# Loader background image
LOADER_ART="${LOADER_ART:-}"
if [ -n "$LOADER_ART" ] && [ -f "$LOADER_ART" ]; then
  python3 "$REPO_DIR/shared/tools/make_loader_img.py" "$LOADER_ART" "$BUILD_DIR/loader.img"
fi

# Loader intro music
LOADER_MP3="${LOADER_MP3:-}"
LOADER_VGZ="${LOADER_VGZ:-}"
rm -f "$BUILD_DIR/intro.pcm" "$BUILD_DIR/intro.vgm"
if [ -n "$LOADER_MP3" ] && [ -f "$LOADER_MP3" ]; then
  ffmpeg -y -loglevel error -i "$LOADER_MP3" -ac 1 -ar 16574 -f s8 "$BUILD_DIR/intro.pcm"
elif [ -n "$LOADER_VGZ" ] && [ -f "$LOADER_VGZ" ]; then
  case "$LOADER_VGZ" in
    *.vgz) gzip -dc "$LOADER_VGZ" > "$BUILD_DIR/intro.vgm" ;;
    *)     cp -f "$LOADER_VGZ" "$BUILD_DIR/intro.vgm" ;;
  esac
fi

cat > "$BUILD_DIR/games.cfg" <<'EOF'
OutRun|DH1:carts/OutRun.bin|DH1:states/OutRun.state
OutRun 2019|DH1:carts/OutRun2019.bin|DH1:states/OutRun2019.state
OutRunners|DH1:carts/OutRunners.bin|DH1:states/OutRunners.state
EOF

cat > "$BUILD_DIR/startup-sequence" <<'EOF'
; Mega Drive trilogy loader boot
C:SetPatch QUIET
C:Version >NIL:
FailAt 21

C:MakeDir RAM:T RAM:Clipboards RAM:ENV RAM:ENV/Sys
C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ

Assign >NIL: ENV: RAM:ENV
Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Assign >NIL: REXX: S:
Assign >NIL: PRINTERS: DEVS:Printers
Assign >NIL: KEYMAPS: DEVS:Keymaps
Assign >NIL: LOCALE: SYS:Locale
Assign >NIL: LIBS: SYS:Classes ADD
Assign >NIL: HELP: LOCALE:Help DEFER

BindDrivers
C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)

IF EXISTS DEVS:Monitors
  IF EXISTS DEVS:Monitors/VGAOnly
    DEVS:Monitors/VGAOnly
  EndIF
  C:List >NIL: DEVS:Monitors/~(#?.info|VGAOnly) TO T:M LFORMAT "DEVS:Monitors/%s"
  Execute T:M
  C:Delete >NIL: T:M
EndIF

SetEnv Language "english"
C:AddDataTypes REFRESH QUIET
C:IPrefs
C:ConClip
Path >NIL: RAM: C: SYS:Utilities SYS:Rexxc SYS:System S: SYS:Prefs SYS:WBStartup SYS:Tools SYS:Tools/Commodities
SetMap gb

Stack 500000
CD DH1:
Echo "launching mdloader" >DH1:boot.log
DH1:mdloader >>DH1:boot.log
Echo "mdloader returned" >>DH1:boot.log
C:Wait 5
C:UAEquit
EndCLI >NIL:
EOF

cp -f "$BOOT_TEMPLATE" "$BOOT_HDF"
xdftool "$BOOT_HDF" delete DEVS/Monitors/PicassoIV >/dev/null 2>&1 || true
xdftool "$BOOT_HDF" delete DEVS/Monitors/PicassoIV.info >/dev/null 2>&1 || true
xdftool "$BOOT_HDF" delete S/startup-sequence + write "$BUILD_DIR/startup-sequence" S/startup-sequence

rm -f "$GAME_HDF"
xdftool "$GAME_HDF" create size=8M + format MDGAMES ffs \
  + write "$BIN" shinobi3_md \
  + write "$LOADER" mdloader \
  + write "$BUILD_DIR/games.cfg" games.cfg \
  $( [ -f "$BUILD_DIR/loader.img" ] && echo "+ write $BUILD_DIR/loader.img loader.img" ) \
  $( [ -f "$BUILD_DIR/intro.pcm" ] && echo "+ write $BUILD_DIR/intro.pcm intro.pcm" ) \
  $( [ -f "$BUILD_DIR/intro.vgm" ] && echo "+ write $BUILD_DIR/intro.vgm intro.vgm" ) \
  + makedir carts \
  + write "$ROM_OUTRUN" carts/OutRun.bin \
  + write "$ROM_2019" carts/OutRun2019.bin \
  + write "$ROM_RUNNERS" carts/OutRunners.bin \
  + makedir states

cat > "$UAE" <<EOF
[config]
config_description=$TITLE Mega Drive RTG
config_version=8.2.0
config_hardware=1
config_host=1
use_gui=no

chipset=aga
chipset_compatible=A1200
chipset_refreshrate=49.920410
cpu_type=68040
cpu_model=68040
cpu_compatible=false
cpu_cycle_exact=false
cpu_memory_cycle_exact=false
cpu_data_cache=false
cpu_speed=max
cpu_throttle=0.0
cpu_24bit_addressing=false
fpu_model=68040
fpu_strict=false
cachesize=16384
comp_trustbyte=direct
comp_trustword=direct
comp_trustlong=direct
comp_trustnaddr=direct
comp_nf=true
comp_constjump=true
comp_flushmode=hard
comp_catchfault=true
compfpu=false
cycle_exact=false
immediate_blits=true
collision_level=playfields
display_optimizations=full
ntsc=false

chipmem_size=4
fastmem_size=8
z3mem_size=512
z3mem_start=0x40000000
bogomem_size=0

gfxcard_size=16
gfxcard_type=ZorroIII
gfxcard_hardware_vblank=false
gfxcard_hardware_sprite=false
gfxcard_multithread=false
gfxcard_zerocopy=true
rtg_nocustom=false
rtg_noautomodes=false
rtg_modes=0x3ffe

kickstart_rom_file=$KICKSTART

nr_floppies=0
floppy0=
floppy0type=-1
floppy1=
floppy1type=-1
floppy2=
floppy2type=-1
floppy3=
floppy3type=-1

hardfile2=rw,DH0:$BOOT_HDF,32,1,2,512,0,,uae0,0
uaehf0=hdf,rw,DH0:$BOOT_HDF,32,1,2,512,0,,uae0,0
hardfile2=rw,DH1:$GAME_HDF,32,1,2,512,-128,,uae1,0
uaehf1=hdf,rw,DH1:$GAME_HDF,32,1,2,512,-128,,uae1,0

gfx_display=0
gfx_display_rtg=0
gfx_framerate=1
gfx_width=640
gfx_height=480
gfx_x_windowed=64
gfx_y_windowed=48
gfx_width_windowed=640
gfx_height_windowed=480
gfx_width_fullscreen=640
gfx_height_fullscreen=480
gfx_fullscreen=0
gfx_fullscreen_amiga=false
gfx_fullscreen_picasso=false
gfx_lores=false
gfx_resolution=hires
gfx_linemode=none
gfx_center_horizontal=smart
gfx_center_vertical=smart
gfx_keep_aspect=true
gfx_colour_mode=32bit
gfx_api=sdl3
gfx_api_options=hardware
gfx_backbuffers=2
gfx_backbuffers_rtg=1
gfx_vsync=false
gfx_vsync_picasso=false

joyport0=none
joyport0autofire=none
joyport1=joy0
joyport1mode=cd32joy
joyport1autofire=none
input.config=0
input.joymouse_speed_analog=100
input.joymouse_speed_digital=10
input.joymouse_deadzone=33
input.joystick_deadzone=33
input.mouse_speed=100
input.autofire_speed=600
input.autoswitch=false

sound_output=exact
sound_channels=stereo
sound_frequency=44100
sound_interpol=anti
sound_filter=emulated
sound_filter_type=enhanced
EOF

echo "Packaged:"
echo "  Boot HDF: $BOOT_HDF"
echo "  Game HDF: $GAME_HDF"
echo "  UAE:      $UAE"