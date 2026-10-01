#!/usr/bin/env bash
# Downloads the official Graphisoft Archicad API DevKit (macOS) into .devkit/.
source "$(dirname "$0")/lib.sh"

case "$ARCHICAD_VERSION" in
  26) TAG="26.3000" ;;
  25) TAG="25.3002" ;;
  27) TAG="27.3001" ;;
  *) echo "No DevKit tag configured for Archicad $ARCHICAD_VERSION (set DEVKIT_TAG)"; exit 1 ;;
esac
TAG="${DEVKIT_TAG:-$TAG}"
DEST="$REPO_ROOT/.devkit"
ZIP="API.Development.Kit.MAC.$TAG.zip"
mkdir -p "$DEST"
if [[ ! -f "$DEST/$ZIP" ]]; then
  echo "Downloading $ZIP from github.com/GRAPHISOFT/archicad-api-devkit ..."
  curl -L --fail -o "$DEST/$ZIP" "https://github.com/GRAPHISOFT/archicad-api-devkit/releases/download/$TAG/$ZIP"
fi
rm -rf "$DEST/devkit$ARCHICAD_VERSION"
unzip -q "$DEST/$ZIP" -d "$DEST/devkit$ARCHICAD_VERSION"
rm -rf "$DEST/devkit$ARCHICAD_VERSION/__MACOSX"
echo "DevKit ready: $DEST/devkit$ARCHICAD_VERSION"
