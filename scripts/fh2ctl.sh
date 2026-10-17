#!/usr/bin/env bash
# Send a command to the running game's debug channel (a named pipe "fh2ctl" in
# its working directory). Results appear as [CTL] lines in the log.
#   scripts/fh2ctl.sh key F12               press a bound key
#   scripts/fh2ctl.sh env FH2_DIRECT_CUBE 0  live renderer switch
#   scripts/fh2ctl.sh capture 2              renderer frame log of 2 frames
#   scripts/fh2ctl.sh peek 4020A294 4        read guest memory
# Environment: FH2_PLAY_DIR (default <repo>/play) -- the game's working directory.
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
p=${FH2_PLAY_DIR:-$repo/play}/fh2ctl
[ -p "$p" ] || { echo "no pipe at $p (game not running?)" >&2; exit 1; }
timeout 3 sh -c 'printf "%s\n" "$1" > "$2"' _ "$*" "$p" || { echo "game not reading the pipe" >&2; exit 1; }
