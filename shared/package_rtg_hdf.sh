#!/bin/bash
# Package the Mega Drive runner and a cartridge into an RTG boot set.
# Each game folder calls this with its own ROM and title.
#
# Usage:
#   ./package_rtg_hdf.sh /path/to/rom.zip GameTitle
#
# Environment overrides:
#   BOOT_TEMPLATE  - RTG boot HDF template (default: ~/Amiberry/HardDrives/RTG1.hdf)
#   KICKSTART      - Kickstart ROM path (default: ~/Amiberry/ROMs/kick40068.A1200.rom)
#   BINARY         - Pre-built interpreter binary (default: builds via shared/build_megadrive.sh)

set -euo pipefail
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GAME_DIR="${GAME_DIR:-$(pwd)}"
DIST_DIR="$GAME_DIR/dist"
BUILD_DIR="$GAME_DIR/build/package"

BOOT_TEMPLATE="${BOOT_TEMPLATE:-/home/jon/Amiberry/HardDrives/RTG1.hdf}"
KICKSTART="${KICKSTART:-/home/jon/Amiberry/ROMs/kick40068.A1200.rom}"

ROM_SRC="${1:?Usage: $0 <rom-file-or-zip> <title>}"
TITLE="${2:?Usage: $0 <rom-file-or-zip> <title>}"

BINARY="${BINARY:-$REPO_DIR/bin/shinobi3_md}"

mkdir -p "$DIST_DIR" "$BUILD_DIR"

# Build if binary doesn't exist
if [ ! -f "$BINARY" ]; then
  echo "Binary not found, building..."
  "$REPO_DIR/shared/build_megadrive.sh"
fi

STAGED_ROM="$BUILD_DIR/cart.bin"
STARTUP="$BUILD_DIR/startup-sequence"
BOOT_HDF="$DIST_DIR/${TITLE}_Boot.hdf"
GAME_HDF="$DIST_DIR/${TITLE}.hdf"
UAE="$DIST_DIR/${TITLE}.uae"

case "${ROM_SRC,,}" in
  *.zip)
    ROM_MEMBER="$(unzip -Z1 "$ROM_SRC" | grep -Ei '\.(bin|gen|smd|md)$' | head -1 || true)"
    if [ -z "$ROM_MEMBER" ]; then
      echo "No .bin/.gen/.smd/.md cartridge found in $ROM_SRC" >&2
      exit 1
    fi
    unzip -p "$ROM_SRC" "$ROM_MEMBER" > "$STAGED_ROM"
    ;;
  *)
    cp -f "$ROM_SRC" "$STAGED_ROM"
    ;;
esac

cat > "$STARTUP" <<'EOF'
; Mega Drive RTG direct boot
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
Echo "launching shinobi3_md" >DH1:boot.log
DH1:shinobi3_md DH1:cart.bin >>DH1:boot.log
Echo "shinobi3_md returned" >>DH1:boot.log
C:Wait 10
C:UAEquit
EndCLI >NIL:
EOF

cp -f "$BOOT_TEMPLATE" "$BOOT_HDF"
xdftool "$BOOT_HDF" delete S/startup-sequence + write "$STARTUP" S/startup-sequence

rm -f "$GAME_HDF"
xdftool "$GAME_HDF" create size=8M + format MDGAME ffs \
  + write "$BINARY" shinobi3_md \
  + write "$STAGED_ROM" cart.bin

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