#!/bin/bash
# Save boot logs to file for FakeIrisXE analysis

echo "Saving boot logs..."
log show --style syslog --last 30m > /Users/becoolio/Documents/FakeIrisXE_V140_Log.txt 2>&1

echo "Searching for FakeIrisXE logs..."
grep -i "FakeIris\|V140\|V139\|WOPCM\|GuC\|GUC\|guc" /Users/becoolio/Documents/FakeIrisXE_V140_Log.txt > /Users/becoolio/Documents/FakeIrisXE_V140_Filtered.txt 2>&1

echo "Done. Check FakeIrisXE_V140_Filtered.txt"
