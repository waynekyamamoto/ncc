#!/bin/bash
# Build Doom with ncc2 (the bootstrap-validated compiler)
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
NCC2=$ROOT/ncc2
DOOMDIR=$ROOT/tests/doom
BUILDDIR=$ROOT/build/doom_ncc2

mkdir -p "$BUILDDIR"

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

echo "=== Building Doom with ncc2 (${#DOOM_C_SRCS[@]} files) ==="

DOOM_OBJS=()
FAILED=()
for f in "${DOOM_C_SRCS[@]}"; do
  obj="$BUILDDIR/${f%.c}.o"
  if "$NCC2" -c -I "$DOOMDIR" -o "$obj" "$DOOMDIR/$f" 2>"$BUILDDIR/${f%.c}.err"; then
    echo "  OK: $f"
    DOOM_OBJS+=("$obj")
  else
    echo "  FAIL: $f"
    cat "$BUILDDIR/${f%.c}.err"
    FAILED+=("$f")
  fi
done

echo ""
echo "  clang: doomgeneric_macos.m"
clang -ObjC -c -I "$DOOMDIR" -o "$BUILDDIR/doomgeneric_macos.o" "$DOOMDIR/doomgeneric_macos.m"
DOOM_OBJS+=("$BUILDDIR/doomgeneric_macos.o")

echo ""
echo "=== Linking ==="
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
  echo "  All ${#DOOM_C_SRCS[@]} C files compiled by ncc2"
else
  echo "PARTIAL: $BUILDDIR/doom (${#FAILED[@]} compile failures)"
fi
