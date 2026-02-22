#!/bin/bash
# Get FakeIrisXE boot logs and save to file

LOGFILE="/Users/becoolio/Documents/Github/Untitled/FakeIrisXE/build/boot_logs.txt"

echo "Fetching FakeIrisXE logs from the last 15 minutes..."
echo ""

# Try to get logs - if sudo fails, try without
if sudo -k 2>/dev/null; then
    # Try to use sudo with password from environment or prompt
    sudo -S log show --predicate 'eventMessage contains "FakeIrisXE"' --last 15m 2>/dev/null > "$LOGFILE" <<< "shibby" || \
    sudo log show --predicate 'eventMessage contains "FakeIrisXE"' --last 15m > "$LOGFILE" 2>&1 <<< "shibby"
else
    log show --predicate 'eventMessage contains "FakeIrisXE"' --last 15m > "$LOGFILE" 2>&1
fi

echo "Logs saved to: $LOGFILE"
echo ""
echo "=========================================="
echo "Recent output (last 100 lines):"
echo "=========================================="
tail -100 "$LOGFILE"
