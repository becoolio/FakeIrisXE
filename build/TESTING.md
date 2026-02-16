# FakeIrisXe V52 Test Build

## What's New in V52

### Apple-Style DMA Implementation
- Based on reverse engineering of Apple's Intel GPU driver (mac-gfx-research)
- Uses magic DMA trigger value `0xFFFF0011` 
- Implements Apple's status polling method
- Checks for status byte `0xF0` (success) or `0xA0/0x60` (failure)

### Linux i915 Fallback
- Tries Linux DMA method first (START_DMA | UOS_MOVE)
- Falls back to Apple method if Linux fails
- Maximizes compatibility across different GPU generations

### Register Offsets
- Linux-style: 0x5820-0x5834 (standard Intel)
- Apple-style: 0x1C570-0x1C584 (from mac-gfx-research analysis)

## Installation

### Prerequisites
1. macOS with SIP disabled (for testing):
   ```bash
   # In Recovery Mode (Cmd+R on boot)
   csrutil disable
   ```

2. Root privileges

### Loading the Kext

```bash
cd build
sudo ./load_kext.sh
```

### Unloading the Kext

```bash
sudo ./unload_kext.sh
```

### Viewing Logs

```bash
# Real-time logs
sudo log stream --predicate 'sender == "FakeIrisXE"'

# Last 5 minutes
sudo ./view_logs.sh
```

## Expected Behavior

### Successful DMA Upload
```
[V52] Attempting firmware upload with fallback...
[V52] Attempt 1: Linux-style DMA upload
[V52] ✅ Linux-style DMA succeeded!
[V52] ✅ Firmware uploaded to GPU WOPCM via DMA
```

### Fallback to Apple Method
```
[V52] Attempt 1: Linux-style DMA upload
[V52] ⚠️ Linux-style DMA failed, trying Apple-style...
[V52] Attempt 2: Apple-style DMA upload
[V52]     Poll 0: STATUS=0xXXXXXX, byte=0xF0
[V52] ✅ GuC firmware loaded successfully!
```

### Failure Case
```
[V52] ❌ Both DMA methods failed!
[V52] ⚠️ Continuing without DMA (may not work on Gen12+)
```

## Troubleshooting

### Kext Won't Load
1. Check SIP is disabled: `csrutil status`
2. Check permissions: `ls -la FakeIrisXE.kext/`
3. Check for existing kext: `kextstat | grep FakeIrisXE`

### DMA Upload Fails
- Expected on some hardware - driver falls back to non-DMA mode
- Check logs to see which method was tried
- Gen12+ Tiger Lake requires DMA for proper operation

### System Logs
```bash
# All system logs mentioning FakeIrisXE
sudo log show --predicate 'eventMessage contains "FakeIrisXE"' --last 10m

# Kernel logs
sudo dmesg | grep -i fakeiris
```

## Supported Hardware

Tested on:
- Tiger Lake (Gen12) - 0x9A49, 0x46A3
- May work on other Intel GPUs with similar architecture

## Version History

- **V52**: Apple+Linux DMA fallback implementation
- **V51**: Initial Linux-style DMA upload
- **V50**: GuC initialization with execlist fallback

## References

- mac-gfx-research: https://github.com/pawan295/mac-gfx-research
- Linux i915: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/i915/gt/uc
