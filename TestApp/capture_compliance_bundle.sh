#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${1:-$SCRIPT_DIR/evidence/compliance_$TS}"
BIN="$SCRIPT_DIR/build/fxe_compliance_test"

mkdir -p "$OUT_DIR"

echo "[fxe] writing bundle to: $OUT_DIR"

{
  echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "hostname=$(hostname)"
  echo "uname=$(uname -a)"
  if command -v sw_vers >/dev/null 2>&1; then
    sw_vers
  fi
  if command -v git >/dev/null 2>&1; then
    git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null | awk '{print "git_head=" $1}' || true
    git -C "$ROOT_DIR" status --short 2>/dev/null || true
  fi
} > "$OUT_DIR/environment.txt"

if [ ! -x "$BIN" ]; then
  echo "[fxe] building compliance test binary"
  clang -framework IOKit -framework IOSurface -framework CoreFoundation \
    -o "$BIN" "$SCRIPT_DIR/fxe_compliance_test.c"
fi

echo "[fxe] running compliance test"
"$BIN" > "$OUT_DIR/compliance_test_stdout.jsonl" 2> "$OUT_DIR/compliance_test_stderr.txt" || true

echo "[fxe] capturing boot logs"
log show --last boot --style compact \
  --predicate 'eventMessage CONTAINS[c] "FakeIrisXE"' \
  > "$OUT_DIR/boot_fakeirisxe.log" || true

echo "[fxe] extracting phase + GuC proof lines"
awk '
  /\[PHASE\]/ || /\[GUC\]\[STATE\]/ || /\[GUC\]\[SUMMARY\]/ {
    print
  }
' "$OUT_DIR/boot_fakeirisxe.log" > "$OUT_DIR/phase_and_guc_proof.log" || true

cat > "$OUT_DIR/manifest.txt" <<EOF
bundle_dir=$OUT_DIR
environment=$OUT_DIR/environment.txt
test_stdout=$OUT_DIR/compliance_test_stdout.jsonl
test_stderr=$OUT_DIR/compliance_test_stderr.txt
boot_log=$OUT_DIR/boot_fakeirisxe.log
phase_and_guc=$OUT_DIR/phase_and_guc_proof.log
EOF

echo "[fxe] done"
echo "[fxe] manifest: $OUT_DIR/manifest.txt"
