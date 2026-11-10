#!/bin/bash
# =============================================================================
# make-app.sh — The Mac app for people without Xcode: a .dmg (docs/INSTALL.md)
# =============================================================================
# Builds the Release app for Apple Silicon and Intel Macs, makes it a real app
# bundle (External Display.app, with its Info.plist), signs it ad hoc, and packs
# it with a link to Applications and the install guide into a disk image:
#
#   packaging/mac/make-app.sh      →  dist/ExternalDisplay-<version>.dmg
#
# Ad hoc, not notarized: the first start needs "Open Anyway" in System
# Settings → Privacy & Security (docs/INSTALL.md). A Developer ID certificate
# would remove that step (docs/SMOOTHNESS.md, N5).
# =============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION=$(sed -n 's/^project(ExternalDisplayReceiver VERSION \([0-9.]*\).*/\1/p' "$ROOT/windows-receiver/CMakeLists.txt")
BUILD_NUMBER=$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 1)
WORK="$(mktemp -d "${TMPDIR:-/tmp}/exdp-app.XXXXXX")"
STAGE="$WORK/stage"
APP="$STAGE/External Display.app"
DMG="$ROOT/dist/ExternalDisplay-$VERSION.dmg"

echo "Building External Display $VERSION ($BUILD_NUMBER) for Apple Silicon and Intel…"
(cd "$ROOT/mac-sender" && xcodebuild -scheme ExternalDisplay -configuration Release \
    -destination 'generic/platform=macOS' -derivedDataPath "$WORK/dd" \
    ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO build) > "$WORK/build.log" 2>&1 || {
    grep -E "error:" "$WORK/build.log" | head -20
    echo "Build failed — full log: $WORK/build.log"
    exit 1
}
BIN="$WORK/dd/Build/Products/Release/ExternalDisplay"
[ -f "$BIN" ] || { echo "No app binary at $BIN"; exit 1; }
ARCHS=$(lipo -archs "$BIN")

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/ExternalDisplay"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>            <string>com.externaldisplay.sender</string>
    <key>CFBundleName</key>                  <string>External Display</string>
    <key>CFBundleDisplayName</key>           <string>External Display</string>
    <key>CFBundleExecutable</key>            <string>ExternalDisplay</string>
    <key>CFBundlePackageType</key>           <string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>CFBundleShortVersionString</key>    <string>$VERSION</string>
    <key>CFBundleVersion</key>               <string>$BUILD_NUMBER</string>
    <key>LSMinimumSystemVersion</key>        <string>13.0</string>
    <key>LSApplicationCategoryType</key>     <string>public.app-category.utilities</string>
    <key>NSHighResolutionCapable</key>       <true/>
    <!-- macOS asks before an app may use the local network (pairing, video) -->
    <key>NSLocalNetworkUsageDescription</key>
    <string>External Display sends this Mac's virtual display to your Windows PC over the local network.</string>
    <key>NSBonjourServices</key>             <array><string>_externaldisplay._tcp</string></array>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" > /dev/null
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"

cp "$ROOT/docs/INSTALL.md" "$STAGE/Read me first.txt"
ln -s /Applications "$STAGE/Applications"
mkdir -p "$ROOT/dist"
hdiutil create -volname "External Display $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" > /dev/null
rm -rf "$WORK"
echo "Done: $DMG ($(du -h "$DMG" | cut -f1), for $ARCHS)"
