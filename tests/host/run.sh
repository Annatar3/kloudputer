#!/bin/sh
# Rerunnable: builds the firmware's logic (tts helpers, the session store, phone setup and settings, the captive
# network, the agent protocol on OpenSSL, notes, approvals and the rest) for this machine with sanitizers on, runs
# its checks, then runs the setup page's time zone script under node.
#   tests/host/run.sh [src-dir]     src-dir defaults to ../../src; the mutation runner passes a scratch copy
# Needs g++, OpenSSL headers, node, and the libraries PlatformIO fetched (run `pio run` or `pio pkg install` once).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="${1:-$ROOT/src}"
JSON="$ROOT/.pio/libdeps/cardputer-adv/ArduinoJson/src"
[ -d "$JSON" ] || { echo "ArduinoJson is missing: run 'pio pkg install -e cardputer-adv' first" >&2; exit 2; }
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cd "$OUT"
python3 -I "$HERE/extract.py" "$SRC"
g++ -std=gnu++20 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -O1 -fsanitize=address,undefined \
  -I"$OUT" -I"$HERE" -I"$SRC" -isystem "$JSON" -o test "$HERE/test.cpp" -lcrypto
./test
node "$HERE/tz_test.js" page_for_node.html
