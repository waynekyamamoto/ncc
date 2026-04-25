#!/bin/bash
# Build Doom with ncc2 (ncc compiled by ncc1, which was compiled by clang)
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
NCC1=$ROOT/ncc
SRCDIR=$ROOT/src
DOOMDIR=$ROOT/tests/doom
BUILDDIR=$ROOT/build/doom_ncc2

mkdir -p "$BUILDDIR"

echo "=== Step 1: Build ncc2 (ncc1 compiles ncc source) ==="

NCC_SRCS=(
  alloc.c codegen_arm64.c hashmap.c main.c parse.c
  preprocess.c tokenize.c type.c unicode.c
)

NCC2_OBJS=()
for f in "${NCC_SRCS[@]}"; do
  obj="$BUILDDIR/ncc2_${f%.c}.o"
  echo "  ncc1: $f"
  "$NCC1" -c -o "$obj" "$SRCDIR/$f"
  NCC2_OBJS+=("$obj")
done

NCC2=$BUILDDIR/ncc2
echo "  link → $NCC2"
clang -o "$NCC2" "${NCC2_OBJS[@]}"
echo "  ncc2 built: $NCC2"

echo ""
echo "=== Step 2: Build Doom with ncc2 ==="

DOOM_C_SRCS=(
  am_map.c d_event.c d_items.c d_iwad.c d_loop.c d_main.c d_mode.c d_net.c
  doomdef.c doomgeneric.c doomstat.c dstrings.c dummy.c
  f_finale.c f_wipe.c g_game.c gusconf.c
  hu_lib.c hu_stuff.c
  i_cdmus.c i_endoom.c i_input.c i_joystick.c i_scale.c i_sound.c
  i_system.c i_timer.c i_video.c icon.c info.c
  m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c
  m_menu.c m_misc.c m_random.c memio.c mus2mid.c
  p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c
  p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c
  p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c
  r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c
  s_sound.c sha1.c sounds.c
  st_lib.c st_stuff.c statdump.c tables.c
  v_video.c w_checksum.c w_file.c w_file_stdc.c w_main.c w_wad.c
  wi_stuff.c z_zone.c
)

DOOM_OBJS=()
FAILED=()
for f in "${DOOM_C_SRCS[@]}"; do
  obj="$BUILDDIR/${f%.c}.o"
  echo "  ncc2: $f"
  if "$NCC2" -c -I "$DOOMDIR" -I "$ROOT/include" -o "$obj" "$DOOMDIR/$f" 2>"$BUILDDIR/${f%.c}.err"; then
    DOOM_OBJS+=("$obj")
  else
    echo "    FAILED: $f"
    cat "$BUILDDIR/${f%.c}.err"
    FAILED+=("$f")
  fi
done

echo ""
echo "  clang: doomgeneric_macos.m"
clang -ObjC -c -I "$DOOMDIR" -o "$BUILDDIR/doomgeneric_macos.o" "$DOOMDIR/doomgeneric_macos.m"
DOOM_OBJS+=("$BUILDDIR/doomgeneric_macos.o")

if [ ${#FAILED[@]} -gt 0 ]; then
  echo ""
  echo "=== Compile failures (${#FAILED[@]}) ==="
  for f in "${FAILED[@]}"; do echo "  $f"; done
  echo "Attempting link with successfully compiled files..."
fi

echo ""
echo "=== Step 3: Link ==="
clang -o "$BUILDDIR/doom" "${DOOM_OBJS[@]}" \
  -framework AppKit \
  -framework CoreGraphics \
  -framework AudioToolbox \
  -framework CoreAudio \
  -framework CoreMIDI \
  -framework CoreFoundation \
  -framework Foundation

echo ""
if [ ${#FAILED[@]} -eq 0 ]; then
  echo "SUCCESS: $BUILDDIR/doom"
  echo "  All ${#DOOM_C_SRCS[@]} C files compiled by ncc2, doomgeneric_macos.m by clang"
else
  echo "PARTIAL: $BUILDDIR/doom (${#FAILED[@]} files fell back)"
fi
