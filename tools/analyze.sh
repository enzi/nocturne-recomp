#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
exe="${1:-$root/data/install/Program_Executable_Files/nocturne.exe}"
ghidra="${GHIDRA_HOME:-/opt/ghidra}"
out="$root/data/ghidra"

mkdir -p "$out"
python3 -I "$root/tools/fix_watcom_pe.py" "$exe" "$out/nocturne.exe"
rm -rf "$out/nocturne.gpr" "$out/nocturne.rep"
"$ghidra/support/analyzeHeadless" "$out" nocturne \
    -import "$out/nocturne.exe" \
    -scriptPath "$root/tools/ghidra" \
    -postScript SeedFunctionsFromRelocs.java \
    -postScript ExportRecompInfo.java "$out/nocturne.recomp.txt" \
    > "$out/analyze.log" 2>&1
grep -E 'round [0-9]+:' "$out/analyze.log" | sed -E 's/.*> //; s/ \(GhidraScript\).*//'
echo "wrote $out/nocturne.recomp.txt"
