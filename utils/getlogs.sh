#!/usr/bin/env zsh
#
# Usage:
#   ./utils/getlogs.sh               — download all *.log files
#   ./utils/getlogs.sh 2026-04-11    — download logs whose name starts with 2026-04-11
#
# The argument is treated as a filename prefix; a wildcard is appended automatically,
# so no quoting is needed.

if [[ -n "$1" ]]; then
    pattern="${1}*.log"
else
    pattern="*.log"
fi

scp "playground:/home/tom/gpsdo_rewrite/${pattern}" $(dirname "$0")/../logs/
