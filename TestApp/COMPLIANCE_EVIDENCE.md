# FakeIrisXE Compliance Evidence Bundle

This captures a reproducible evidence bundle for the compliance stage without unloading/reloading the kext.

## Prerequisites

- Build kext: `xcodebuild -project FakeIrisXE.xcodeproj -scheme FakeIrisXE -configuration Debug build`
- Build compliance test: `clang -framework IOKit -framework IOSurface -framework CoreFoundation -o TestApp/build/fxe_compliance_test TestApp/fxe_compliance_test.c`

## Capture

From `TestApp/`:

```bash
./capture_compliance_bundle.sh
```

Optional custom output directory:

```bash
./capture_compliance_bundle.sh /tmp/fxe_bundle_001
```

## Bundle contents

- `environment.txt`: host + git snapshot for reproducibility
- `compliance_test_stdout.jsonl`: selector 0/3/4/7 test output
- `compliance_test_stderr.txt`: runtime errors from test run
- `boot_fakeirisxe.log`: `log show --last boot` lines filtered to `FakeIrisXE`
- `phase_and_guc_proof.log`: extracted `[PHASE]`, `[GUC][STATE]`, `[GUC][SUMMARY]`
- `manifest.txt`: index of all evidence files

## What to verify quickly

- `compliance_test_stdout.jsonl` contains steps: `Connect`, `GetVersion`, `BindSurface`, `Present`, `FenceTest`
- `phase_and_guc_proof.log` shows deterministic phase markers from init/teardown
- `phase_and_guc_proof.log` includes GuC proof lines indicating attempted/ready/stage/error state
