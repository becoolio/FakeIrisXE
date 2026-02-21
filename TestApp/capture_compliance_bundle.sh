#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${1:-$SCRIPT_DIR/evidence/compliance_$TS}"
BIN="$SCRIPT_DIR/build/fxe_compliance_test"
KEXT_INSTALLED_BIN="/Library/Extensions/FakeIrisXE.kext/Contents/MacOS/FakeIrisXE"
KEXT_BUILT_BIN="$ROOT_DIR/build/Debug/FakeIrisXE.kext/Contents/MacOS/FakeIrisXE"

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
  /\[PHASE\]/ || /\[GUC\]\[STATE\]/ || /\[GUC\]\[SUMMARY\]/ ||
  /\[UC\]/ || /\[IOSurface\]/ || /\[SUMMARY\]/ {
    print
  }
' "$OUT_DIR/boot_fakeirisxe.log" > "$OUT_DIR/phase_and_guc_proof.log" || true

echo "[fxe] capturing binary provenance"
ROOT_DIR_ENV="$ROOT_DIR" python3 - <<'PY' > "$OUT_DIR/binary_provenance.txt"
from pathlib import Path
import hashlib
import os
import time

installed = Path("/Library/Extensions/FakeIrisXE.kext/Contents/MacOS/FakeIrisXE")
built = Path(os.environ["ROOT_DIR_ENV"]) / "build/Debug/FakeIrisXE.kext/Contents/MacOS/FakeIrisXE"

markers = {
    "legacy_submit_log": b"SubmitExeclistFenceTest called",
    "new_fence_not_wired_log": b"[UC][FenceTest] submission path not wired",
    "new_iosurface_disabled_log": b"[IOSurface] DISABLED lookupOK=0 IOSURF=0",
}

def describe(path: Path):
    print(f"path={path}")
    if not path.exists():
        print("exists=0")
        return None

    data = path.read_bytes()
    st = path.stat()
    print("exists=1")
    print(f"size={len(data)}")
    print(f"sha256={hashlib.sha256(data).hexdigest()}")
    print(f"mtime={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}")
    for k, needle in markers.items():
        print(f"{k}={1 if needle in data else 0}")
    return hashlib.sha256(data).hexdigest()

inst_hash = describe(installed)
print("---")
built_hash = describe(built)
print("---")
if inst_hash and built_hash:
    print(f"hash_match={1 if inst_hash == built_hash else 0}")
else:
    print("hash_match=0")
PY

cat > "$OUT_DIR/manifest.txt" <<EOF
bundle_dir=$OUT_DIR
environment=$OUT_DIR/environment.txt
test_stdout=$OUT_DIR/compliance_test_stdout.jsonl
test_stderr=$OUT_DIR/compliance_test_stderr.txt
boot_log=$OUT_DIR/boot_fakeirisxe.log
phase_and_guc=$OUT_DIR/phase_and_guc_proof.log
binary_provenance=$OUT_DIR/binary_provenance.txt
EOF

echo "[fxe] done"
echo "[fxe] manifest: $OUT_DIR/manifest.txt"
