#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/data/install"
dst="$root/data/game"

mkdir -p "$dst/system" "$dst/video"
find "$src/Program_Executable_Files" "$src"/Act_* "$src/_user/English_OS_Independent_Files" -maxdepth 1 -iname '*.pod' -exec ln -sf {} "$dst/" \;
find "$src/_user/Language_Independent_OS_Independent_Files" -maxdepth 1 -type f ! -iname '*.ini' -exec ln -sf {} "$dst/" \;
ln -sf "$src/Program_Executable_Files/nocturne.exe" "$dst/nocturne.exe"
ln -sf "$src/Video/video/opening.avi" "$dst/video/opening.avi"
cp -n "$src/System/SYSTEM/RENDER.INI" "$dst/system/RENDER.INI"
cp -n "$src/_user/Language_Independent_OS_Independent_Files/NOCTURNE.INI" "$dst/system/NOCTURNE.INI"
echo "game directory: $dst"
