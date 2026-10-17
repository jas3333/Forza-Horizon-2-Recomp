#!/usr/bin/env bash
# Development test run: fresh save (the opening drive plays by itself),
# windowed, time-limited, with frame statistics in the log.
# Usage: scripts/test-run.sh <name> <seconds> [extra game flags...]
#   log: <repo>/testrun/<name>.log; screenshots: <repo>/testrun/screenshots
#   Useful log lines: [FPS], [PRESENT] (late frames), [SPIKE] (slow frames),
#   [RTSTAT] and [DRAWTHREAD] (renderer cost per frame / per draw).
# Environment: FH2_GAME_DIR (default <repo>/../game), GPU (default fh2).
set -u
name=$1; secs=$2; shift 2
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
game=${FH2_GAME_DIR:-$repo/../game}
bin=${FH2_BIN_DIR:-$repo/out/build/linux-amd64-release}
dir=$repo/testrun
mkdir -p "$dir"; cd "$dir" || exit 1
rm -rf user
timeout -s INT "$secs" "$bin/forzahorizon2" \
  --game_data_root="$game" --no-fullscreen --gpu_plugin="${GPU:-fh2}" \
  --vblank_frequency_override=120 --log_fps \
  --log_max_file_size_mb=200 --log_file="$dir/$name.log" "$@" > "$dir/$name.stdout" 2>&1
echo "exit=$? log=$dir/$name.log"
