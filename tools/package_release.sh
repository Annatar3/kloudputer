#!/usr/bin/env bash
# Names the built firmware for a release and writes its checksums.
# Usage: tools/package_release.sh <tag, e.g. v1.7> <outdir>   (run from the repo root after `pio run -e cardputer-adv`)
# Fails if the tag doesn't match kVersion in src/main.cpp, so a release can never carry a mislabeled binary.
set -euo pipefail
TAG=${1:?tag, e.g. v1.7}
OUT=${2:?output directory}
VER=$(grep -oP 'kVersion = "\K[^"]+' src/main.cpp)
if [ "$TAG" != "$VER" ]; then
  echo "tag $TAG does not match kVersion $VER in src/main.cpp" >&2
  exit 1
fi
BUILD=.pio/build/cardputer-adv
mkdir -p "$OUT"
cp "$BUILD/firmware.factory.bin" "$OUT/kloudputer-$TAG.bin"
cp "$BUILD/firmware.bin" "$OUT/kloudputer-$TAG-app.bin"
(cd "$OUT" && sha256sum kloudputer-"$TAG".bin kloudputer-"$TAG"-app.bin > SHA256SUMS.txt)
ls -l "$OUT"
