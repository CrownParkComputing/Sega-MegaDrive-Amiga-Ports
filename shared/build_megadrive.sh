#!/bin/bash
# Build script for the Mega Drive native Amiga RTG interpreter.
# Produces a single binary that can run any MD cartridge ROM.
#
# Usage:
#   ./shared/build_megadrive.sh
#   OUTPUT_NAME=outrun_md ./shared/build_megadrive.sh

set -euo pipefail
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"

# Resolve repo root (parent of shared/)
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_DIR/shared"
CORES_DIR="$SRC_DIR/cores"
MUSASHI_DIR="$CORES_DIR/musashi"
BUILD_DIR="${BUILD_DIR:-$REPO_DIR/build}"
BIN_DIR="${BIN_DIR:-$REPO_DIR/bin}"
OUTPUT_NAME="${OUTPUT_NAME:-shinobi3_md}"

MD_ACCURATE_OPN2="${MD_ACCURATE_OPN2:-0}"
MD_FAST_FM="${MD_FAST_FM:-0}"
MD_GPGX_FM="${MD_GPGX_FM:-0}"
MD_YMFM_FM="${MD_YMFM_FM:-1}"
MD_ENABLE_AUDIO="${MD_ENABLE_AUDIO:-1}"
MD_RUN_Z80_SOUND="${MD_RUN_Z80_SOUND:-1}"

echo "=== Building Mega Drive interpreter (Amiga RTG, Musashi 68000) ==="

mkdir -p "$BUILD_DIR" "$BIN_DIR"

CC="m68k-amigaos-gcc"
CXX="m68k-amigaos-g++"
CFLAGS=(
  -m68040 -noixemul -O3 -fomit-frame-pointer -funroll-loops -DNDEBUG
  -DMD_ACCURATE_OPN2="$MD_ACCURATE_OPN2"
  -DMD_FAST_FM="$MD_FAST_FM"
  -DMD_GPGX_FM="$MD_GPGX_FM"
  -DMD_YMFM_FM="$MD_YMFM_FM"
  -DMD_ENABLE_AUDIO="$MD_ENABLE_AUDIO"
  -DMD_RUN_Z80_SOUND="$MD_RUN_Z80_SOUND"
  -I"$SRC_DIR"
  -I"$SRC_DIR/hal"
  -I"$CORES_DIR"
  -I"$MUSASHI_DIR"
  -I"$MUSASHI_DIR/softfloat"
)
LDFLAGS=(-noixemul -Wl,--start-group -lamiga -lm -lgcc -Wl,--end-group)

echo "Compiling core components..."

"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/md_machine.c" -o "$BUILD_DIR/md_machine.o"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/md_vdp.c" -o "$BUILD_DIR/md_vdp.o"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/md_audio_stub.c" -o "$BUILD_DIR/md_audio_stub.o"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/md_audio_amiga.c" -o "$BUILD_DIR/md_audio_amiga.o"
EXTRA_OBJS=()
if [ "$MD_ACCURATE_OPN2" = "1" ]; then
  "$CC" "${CFLAGS[@]}" -c "$CORES_DIR/nuked_opn2/ym3438.c" -o "$BUILD_DIR/ym3438.o"
  EXTRA_OBJS+=("$BUILD_DIR/ym3438.o")
else
  rm -f "$BUILD_DIR/ym3438.o"
fi
if [ "$MD_GPGX_FM" = "1" ] && [ "$MD_ACCURATE_OPN2" != "1" ] && [ "$MD_FAST_FM" != "1" ]; then
  # -O3/no-unroll: JIT translations persist (no CacheClearU), so favour speed
  "$CC" "${CFLAGS[@]}" -O3 -fno-unroll-loops -c "$CORES_DIR/gpgx_ym2612/ym2612.c" -o "$BUILD_DIR/gpgx_ym2612.o"
  EXTRA_OBJS+=("$BUILD_DIR/gpgx_ym2612.o")
else
  rm -f "$BUILD_DIR/gpgx_ym2612.o"
fi
if [ "$MD_YMFM_FM" = "1" ] && [ "$MD_ACCURATE_OPN2" != "1" ] && [ "$MD_FAST_FM" != "1" ] && [ "$MD_GPGX_FM" != "1" ]; then
  CXXFLAGS=(-m68040 -noixemul -O2 -fomit-frame-pointer -DNDEBUG -std=gnu++14
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
            -I"$CORES_DIR/ymfm")
  "$CXX" "${CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/ymfm_opn.cpp" -o "$BUILD_DIR/ymfm_opn.o"
  "$CXX" "${CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/ymfm_ssg.cpp" -o "$BUILD_DIR/ymfm_ssg.o"
  "$CXX" "${CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/ymfm_adpcm.cpp" -o "$BUILD_DIR/ymfm_adpcm.o"
  "$CXX" "${CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/md_ymfm2612.cpp" -o "$BUILD_DIR/md_ymfm2612.o"
  "$CXX" "${CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/md_cxx_stubs.cpp" -o "$BUILD_DIR/md_cxx_stubs.o"
  EXTRA_OBJS+=("$BUILD_DIR/ymfm_opn.o" "$BUILD_DIR/ymfm_ssg.o" "$BUILD_DIR/ymfm_adpcm.o" "$BUILD_DIR/md_ymfm2612.o" "$BUILD_DIR/md_cxx_stubs.o")
fi
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/rom_loader.c" -o "$BUILD_DIR/rom_loader.o"
"$CC" "${CFLAGS[@]}" -c "$SRC_DIR/md_amiga.c" -o "$BUILD_DIR/md_amiga.o"
"$CC" "${CFLAGS[@]}" -c "$CORES_DIR/md_m68k_interface.c" -o "$BUILD_DIR/md_m68k_interface.o"
"$CC" "${CFLAGS[@]}" -DZ80_MAP_GENESIS -c "$CORES_DIR/z80.c" -o "$BUILD_DIR/z80.o"

echo "Compiling Musashi 68000 core..."

"$CC" "${CFLAGS[@]}" -Wno-incompatible-pointer-types -c "$MUSASHI_DIR/m68kcpu.c" -o "$BUILD_DIR/m68kcpu.o"
"$CC" "${CFLAGS[@]}" -Wno-incompatible-pointer-types -c "$MUSASHI_DIR/m68kops.c" -o "$BUILD_DIR/m68kops.o"
"$CC" "${CFLAGS[@]}" -Wno-incompatible-pointer-types -c "$MUSASHI_DIR/m68kdasm.c" -o "$BUILD_DIR/m68kdasm.o"
"$CC" "${CFLAGS[@]}" -Wno-incompatible-pointer-types -c "$MUSASHI_DIR/softfloat/softfloat.c" -o "$BUILD_DIR/softfloat.o"

echo "Linking executable..."

"$CC" -o "$BIN_DIR/$OUTPUT_NAME" \
    "$BUILD_DIR/md_amiga.o" \
    "$BUILD_DIR/md_machine.o" \
    "$BUILD_DIR/md_vdp.o" \
    "$BUILD_DIR/md_audio_stub.o" \
    "$BUILD_DIR/md_audio_amiga.o" \
    "${EXTRA_OBJS[@]}" \
    "$BUILD_DIR/rom_loader.o" \
    "$BUILD_DIR/md_m68k_interface.o" \
    "$BUILD_DIR/z80.o" \
    "$BUILD_DIR/m68kcpu.o" \
    "$BUILD_DIR/m68kops.o" \
    "$BUILD_DIR/m68kdasm.o" \
    "$BUILD_DIR/softfloat.o" \
    "${LDFLAGS[@]}"

echo "Done. Executable at: $BIN_DIR/$OUTPUT_NAME"
ls -l "$BIN_DIR/$OUTPUT_NAME"
echo "Building mdloader (with VGM intro music)..."
LOADER_CXXFLAGS=(-m68020 -noixemul -O2 -DNDEBUG -std=gnu++14
                 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
                 -I"$CORES_DIR/ymfm")
"$CXX" "${LOADER_CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/ymfm_opm.cpp" -o "$BUILD_DIR/ldr_ymfm_opm.o"
"$CXX" "${LOADER_CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/md_ymfm2151.cpp" -o "$BUILD_DIR/ldr_ymfm2151.o"
"$CXX" "${LOADER_CXXFLAGS[@]}" -c "$CORES_DIR/ymfm/md_cxx_stubs.cpp" -o "$BUILD_DIR/ldr_cxx_stubs.o"
"$CC" -m68020 -noixemul -O2 -DNDEBUG -c "$SRC_DIR/tools/mdl_segapcm.c" -o "$BUILD_DIR/ldr_segapcm.o"
"$CC" -m68020 -noixemul -O2 -DNDEBUG -c "$SRC_DIR/tools/mdl_music.c" -o "$BUILD_DIR/ldr_music.o"
"$CC" -m68020 -noixemul -O2 -DNDEBUG -o "$BIN_DIR/mdloader" "$SRC_DIR/tools/mdloader.c" \
    "$BUILD_DIR/ldr_music.o" "$BUILD_DIR/ldr_segapcm.o" "$BUILD_DIR/ldr_ymfm2151.o" \
    "$BUILD_DIR/ldr_ymfm_opm.o" "$BUILD_DIR/ldr_cxx_stubs.o" \
    -Wl,--start-group -lamiga -lm -lgcc -Wl,--end-group
