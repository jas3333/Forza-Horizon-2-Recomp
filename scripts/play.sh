#!/usr/bin/env bash
# Play Forza Horizon 2 with the native renderer.
#
# Saves, settings (forzahorizon2.toml, edited in game with F1), the shader
# cache and screenshots live in $FH2_PLAY_DIR (default: <repo>/play). The first
# run seeds the settings with 3x resolution, 60 fps and occlusion queries off.
# Extra arguments are passed to the game and win over the settings for that
# run, e.g.:
#   scripts/play.sh --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
#
# Environment:
#   FH2_GAME_DIR  extracted game (default: <repo>/../game, as in the manifest)
#   FH2_PLAY_DIR  saves / settings / caches (default: <repo>/play)
#   FH2_USER      gamertag (default: Player)
#   WINDOWED=1    run in a window instead of fullscreen
#   OCCLUSION=1   real occlusion queries for this run (buildings may pop in)
#   FH2_*         renderer switches, see README (e.g. FH2_MOTION_BLUR=1)
# Log: $FH2_PLAY_DIR/play.log
set -u
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
game=${FH2_GAME_DIR:-$repo/../game}
play=${FH2_PLAY_DIR:-$repo/play}
bin=${FH2_BIN_DIR:-$repo/out/build/linux-amd64-release}
if [ ! -x "$bin/forzahorizon2" ]; then
  echo "No build at $bin (see README: Build)" >&2
  exit 1
fi
if [ ! -f "$game/default.xex" ]; then
  echo "No game at $game (set FH2_GAME_DIR to your extracted copy)" >&2
  exit 1
fi
mkdir -p "$play"
cd "$play" || exit 1
if [ ! -f forzahorizon2.toml ]; then
  cat > forzahorizon2.toml <<'TOML'
# FH2 settings (edited by the in-game menu, F1)
draw_resolution_scale_x = 3
draw_resolution_scale_y = 3
vblank_frequency_override = 120
occlusion_query_enable = false
TOML
fi
extra_flags=()
[ "${WINDOWED:-0}" = 1 ] && extra_flags+=(--no-fullscreen)
[ "${OCCLUSION:-0}" = 1 ] && extra_flags+=(--occlusion_query_enable=true)
exec "$bin/forzahorizon2" \
  --game_data_root="$game" --gpu_plugin=fh2 --user_name="${FH2_USER:-Player}" \
  --log_max_file_size_mb=50 --log_file="$play/play.log" "${extra_flags[@]}" "$@"
