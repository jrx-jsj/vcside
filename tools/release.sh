#!/bin/bash
# Publishes a version of VCSIDE: builds it for macOS, Windows and Linux, tags it, makes the
# GitHub release with the packages and tells the website (vcside-web) about it.
#
#   tools/release.sh 0.2.0            # publish
#   tools/release.sh 0.2.0 --dry-run  # build, package and write the notes only
#
# Run it on a Mac (the macOS build needs one) with Docker running (Linux and Windows builds)
# and gh signed in. What is new goes in docs/releases/<version>.md: if it is missing, the
# script writes a draft from the commits since the last release and stops so you can edit it.
# docs/releases/README.md explains the whole process.
set -euo pipefail

VERSION=${1:-}
DRY_RUN=0
[ "${2:-}" = "--dry-run" ] && DRY_RUN=1
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: tools/release.sh X.Y.Z [--dry-run]" >&2
  exit 2
fi
TAG="v$VERSION"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BUILD_DIR="$ROOT/build-release"
DIST="$BUILD_DIR/dist"
NOTES="docs/releases/$VERSION.md"
WEB=${VCSIDE_WEB:-"$ROOT/../vcside-web"}

say() { printf '\n== %s\n' "$*"; }
die() { echo "release: $*" >&2; exit 1; }

# --- checks ---------------------------------------------------------------------------------
[ "$(uname)" = Darwin ] || die "run it on macOS: the macOS build needs a Mac"
command -v docker >/dev/null && docker info >/dev/null 2>&1 || die "Docker is not running"
if [ $DRY_RUN = 0 ]; then
  command -v gh >/dev/null && gh auth status >/dev/null 2>&1 || die "gh is not installed or not signed in"
  [ "$(git branch --show-current)" = main ] || die "not on main"
  [ -z "$(git status --porcelain)" ] || die "there are uncommitted changes"
  git fetch -q origin
  [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || die "main is not the same as origin/main"
  ! git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || die "$TAG exists already"
fi

PREVIOUS_TAG=$(git describe --tags --abbrev=0 --match 'v*' 2>/dev/null || true)
if [ ! -f "$NOTES" ]; then
  mkdir -p docs/releases
  {
    echo "## What is new"
    echo
    echo "<!-- A few lines for people who use VCSIDE: what they can do now that they could not before."
    echo "     The commits since ${PREVIOUS_TAG:-the start}, as a starting point (delete this comment): -->"
    git log --no-merges --format='- %s' ${PREVIOUS_TAG:+"$PREVIOUS_TAG"..HEAD}
  } > "$NOTES"
  die "wrote a draft of $NOTES from the commits: edit it, commit it and run this again"
fi
grep -q '<!--' "$NOTES" && die "$NOTES still has the draft's comment: finish it first"

# --- version --------------------------------------------------------------------------------
CURRENT=$(sed -n 's/^project(vcside VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
if [ "$CURRENT" != "$VERSION" ]; then
  say "version $CURRENT -> $VERSION"
  sed -i '' "s/^project(vcside VERSION $CURRENT/project(vcside VERSION $VERSION/" CMakeLists.txt
fi

# --- builds ---------------------------------------------------------------------------------
rm -rf "$DIST"
mkdir -p "$DIST"

say "macOS (Apple silicon + Intel, macOS 11 or later, SDL3 built in)"
cmake -S . -B "$BUILD_DIR/macos" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVCSIDE_VENDORED_SDL=ON \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$BUILD_DIR/macos"
"$BUILD_DIR/macos/vcside_tests" | tail -1
APP_BUNDLE="$BUILD_DIR/macos/VCSIDE.app"
if otool -L "$APP_BUNDLE/Contents/MacOS/VCSIDE" | grep -q -e /opt/homebrew -e /usr/local; then
  die "the macOS build links a library outside the system"
fi
codesign --force --deep --sign - "$APP_BUNDLE"  # ad hoc: not a Developer ID, but a valid bundle
MAC_ZIP="VCSIDE-$VERSION-macos.zip"
ditto -c -k --sequesterRsrc --keepParent "$APP_BUNDLE" "$DIST/$MAC_ZIP"
# The disk image: the app and a link to Applications, to drag it there.
MAC_DMG="VCSIDE-$VERSION-macos.dmg"
DMG_STAGE="$BUILD_DIR/dmg"
rm -rf "$DMG_STAGE"
mkdir -p "$DMG_STAGE"
ditto "$APP_BUNDLE" "$DMG_STAGE/VCSIDE.app"
ln -s /Applications "$DMG_STAGE/Applications"
hdiutil create -quiet -volname "VCSIDE $VERSION" -srcfolder "$DMG_STAGE" -fs HFS+ -format UDZO "$DIST/$MAC_DMG"

# Linux for one architecture: Docker platform (amd64, arm64) and the name in the package.
# On Apple silicon arm64 runs natively and amd64 is emulated (slower).
linux_package() {
  local platform=$1 arch=$2
  say "Linux $arch (Docker, SDL3 built in)"
  docker build -q --platform "linux/$platform" -t "vcside-linux-$platform" -f tools/docker/linux.Dockerfile tools/docker >/dev/null
  docker run --rm --platform "linux/$platform" -v "$ROOT":/src -w /src "vcside-linux-$platform" \
    sh tools/docker/build-linux.sh "build-release/linux-$platform" -DVCSIDE_VENDORED_SDL=ON
  local name="vcside-$VERSION-linux-$arch"
  mkdir -p "$BUILD_DIR/pkg/$name"
  cp "$BUILD_DIR/linux-$platform/vcside" "$BUILD_DIR/pkg/$name/"
  cp -R "$BUILD_DIR/linux-$platform/data" "$BUILD_DIR/pkg/$name/"
  cp LICENSE "$BUILD_DIR/pkg/$name/"
  tar -C "$BUILD_DIR/pkg" -czf "$DIST/$name.tar.gz" "$name"
}
linux_package amd64 x86_64
linux_package arm64 arm64
LINUX_X64="vcside-$VERSION-linux-x86_64.tar.gz"
LINUX_ARM64="vcside-$VERSION-linux-arm64.tar.gz"

say "Windows x86-64 (Docker, MinGW-w64)"
docker build -q -t vcside-windows -f tools/docker/windows.Dockerfile tools/docker >/dev/null
docker run --rm -v "$ROOT":/src -w /src vcside-windows sh tools/docker/build-windows.sh build-release/windows
WINDOWS_NAME="vcside-$VERSION-windows-x86_64"
mkdir -p "$BUILD_DIR/pkg/$WINDOWS_NAME"
cp "$BUILD_DIR/windows/vcside.exe" "$BUILD_DIR/pkg/$WINDOWS_NAME/"
cp -R "$BUILD_DIR/windows/data" "$BUILD_DIR/pkg/$WINDOWS_NAME/"
cp LICENSE "$BUILD_DIR/pkg/$WINDOWS_NAME/LICENSE.txt"
(cd "$BUILD_DIR/pkg" && zip -qr "$DIST/$WINDOWS_NAME.zip" "$WINDOWS_NAME")
WINDOWS_SETUP="$WINDOWS_NAME-setup.exe"
docker run --rm -v "$ROOT":/src -w /src vcside-windows makensis -V2 -DVERSION="$VERSION" \
  -DSOURCE="/src/build-release/pkg/$WINDOWS_NAME" -DOUTFILE="/src/build-release/dist/$WINDOWS_SETUP" \
  tools/windows/vcside.nsi

(cd "$DIST" && shasum -a 256 "$MAC_DMG" "$MAC_ZIP" "$WINDOWS_SETUP" "$WINDOWS_NAME.zip" "$LINUX_X64" "$LINUX_ARM64" > SHA256SUMS)

# --- notes ----------------------------------------------------------------------------------
REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || echo jrx-jsj/vcside)
{
  cat "$NOTES"
  echo
  sed -e "s/{{VERSION}}/$VERSION/g" docs/releases/install.md
  echo
  echo '```'
  cat "$DIST/SHA256SUMS"
  echo '```'
  if [ -n "$PREVIOUS_TAG" ]; then echo; echo "All the changes: https://github.com/$REPO/compare/$PREVIOUS_TAG...$TAG"; fi
} > "$DIST/notes.md"

say "packages"
ls -l "$DIST"
if [ $DRY_RUN = 1 ]; then
  if [ "$CURRENT" != "$VERSION" ]; then git checkout -q CMakeLists.txt; fi
  echo "dry run: nothing committed, tagged or published. Notes: $DIST/notes.md"
  exit 0
fi

# --- tag and release ------------------------------------------------------------------------
say "commit, tag $TAG and push"
git add CMakeLists.txt "$NOTES"
git diff --cached --quiet || git commit -q -m "VCSIDE $VERSION"
git tag -a "$TAG" -m "VCSIDE $VERSION"
git push -q origin main "$TAG"

say "GitHub release"
gh release create "$TAG" --title "VCSIDE $VERSION" --notes-file "$DIST/notes.md" \
  "$DIST/$MAC_DMG" "$DIST/$MAC_ZIP" "$DIST/$WINDOWS_SETUP" "$DIST/$WINDOWS_NAME.zip" \
  "$DIST/$LINUX_X64" "$DIST/$LINUX_ARM64" "$DIST/SHA256SUMS"

# --- website --------------------------------------------------------------------------------
say "website"
if [ ! -d "$WEB/.git" ]; then
  echo "no website at $WEB (set VCSIDE_WEB): skipped"
elif [ "$(git -C "$WEB" branch --show-current)" != main ] || [ -n "$(git -C "$WEB" status --porcelain)" ]; then
  echo "the website is not on a clean main: skipped. Run this there by hand:"
  echo "  python3 $ROOT/tools/release_web.py $VERSION $DIST $REPO $WEB"
else
  python3 tools/release_web.py "$VERSION" "$DIST" "$REPO" "$WEB"
  python3 "$WEB/scripts/build.py"
  git -C "$WEB" add -A
  git -C "$WEB" commit -q -m "Download: VCSIDE $VERSION"
  git -C "$WEB" push -q
  echo "website updated (deploy it to make it public)"
fi
say "done: https://github.com/$REPO/releases/tag/$TAG"
