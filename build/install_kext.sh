#!/bin/bash
rm -rf /Library/Extensions/FakeIrisXE.kext
cp -R /Users/becoolio/Documents/Github/Untitled/FakeIrisXE/build/FakeIrisXE.kext /Library/Extensions/
chown -R root:wheel /Library/Extensions/FakeIrisXE.kext
echo "Done"
