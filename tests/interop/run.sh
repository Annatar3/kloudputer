#!/bin/sh
# Rerunnable: the Node connector against the Cardputer's own protocol code (src/agent_proto.h, built here as
# proto_tool on OpenSSL), through the mock ntfy relay from the connector's tests. Nothing leaves this machine.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
JSON="$ROOT/.pio/libdeps/cardputer-adv/ArduinoJson/src"
[ -d "$JSON" ] || { echo "ArduinoJson is missing: run 'pio pkg install -e cardputer-adv' first" >&2; exit 2; }
BUILD="$ROOT/tests/build/interop"
mkdir -p "$BUILD"
g++ -std=gnu++20 -O1 -w -I"$ROOT/src" -I"$ROOT/tests/host" -isystem "$JSON" -o "$BUILD/proto_tool" "$HERE/proto_tool.cpp" -lcrypto
PROTO_TOOL="$BUILD/proto_tool" node "$HERE/interop.mjs"
