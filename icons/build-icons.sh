#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_PNG="${ROOT_DIR}/libera-link.png"
ICONSET_DIR="$(mktemp -d "${TMPDIR:-/tmp}/libera-link.XXXXXX.iconset")"

cleanup() {
  rm -rf "${ICONSET_DIR}"
}
trap cleanup EXIT

if [[ ! -f "${SOURCE_PNG}" ]]; then
  echo "Missing source PNG: ${SOURCE_PNG}" >&2
  exit 1
fi

for size in 16 32 128 256 512; do
  magick "${SOURCE_PNG}" -resize "${size}x${size}" "${ICONSET_DIR}/icon_${size}x${size}.png"
done

for size in 32 64 256 512 1024; do
  base_size=$((size / 2))
  magick "${SOURCE_PNG}" -resize "${size}x${size}" "${ICONSET_DIR}/icon_${base_size}x${base_size}@2x.png"
done

iconutil -c icns "${ICONSET_DIR}" -o "${ROOT_DIR}/libera-link.icns"
magick "${SOURCE_PNG}" -define icon:auto-resize=256,128,96,64,48,32,16 "${ROOT_DIR}/libera-link.ico"
