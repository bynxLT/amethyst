#!/bin/bash
# Build AMETHYST on a Mac (macOS 13+ target, universal binary) and make a .dmg.
# Needs: Xcode (or Command Line Tools) and CMake  ->  brew install cmake
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build-mac -G Xcode -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-mac --config Release -- -quiet

APP="build-mac/VocalDAW_artefacts/Release/AMETHYST.app"
codesign --force --deep --sign - "$APP"
"$APP/Contents/MacOS/AMETHYST" --selftest

rm -rf dmg && mkdir dmg
cp -R "$APP" dmg/
ln -s /Applications dmg/Applications
hdiutil create -volname "AMETHYST" -srcfolder dmg -ov -format UDZO AMETHYST-macOS.dmg
rm -rf dmg
echo "Done: $(pwd)/AMETHYST-macOS.dmg"
