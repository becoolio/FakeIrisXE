#!/bin/bash
# View FakeIrisXe logs

echo "Showing FakeIrisXe logs (last 5 minutes)..."
echo ""
echo "=========================================="
sudo log show --predicate 'sender == "FakeIrisXE"' --last 5m --style compact
