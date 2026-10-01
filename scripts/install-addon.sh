#!/usr/bin/env bash
# Installs the built add-on to ~/Library/ClaudeConnector/AC<version>/ and registers it in
# Archicad's Add-On Manager list. Archicad must be closed (it rewrites its prefs on quit);
# restart it afterwards (scripts/archicad.sh start).
source "$(dirname "$0")/lib.sh"

SRC="$BUILD_DIR/$BUNDLE_NAME"
[[ -d "$SRC" ]] || { echo "Bundle not built: $SRC (run scripts/build-addon.sh)"; exit 1; }
if [[ -n "$(archicad_pids)" ]]; then
  echo "Archicad is running - stop it first (scripts/archicad.sh stop)"; exit 1
fi

DEST_DIR="${CLAUDE_CONNECTOR_INSTALL_DIR:-$HOME/Library/ClaudeConnector/AC$ARCHICAD_VERSION}"
mkdir -p "$DEST_DIR"
rm -rf "$DEST_DIR/$BUNDLE_NAME"
cp -R "$SRC" "$DEST_DIR/"
xattr -c "$DEST_DIR/$BUNDLE_NAME" 2>/dev/null || true
echo "Installed: $DEST_DIR/$BUNDLE_NAME"

python3 "$REPO_ROOT/scripts/addon_manager.py" register "$DEST_DIR/$BUNDLE_NAME"

# A borrowed Tapir MDID (addon/mdid.local.cmake mentions Tapir) cannot coexist with a loaded Tapir.
if grep -qi "tapir" "$ADDON_DIR/mdid.local.cmake" 2>/dev/null; then
  python3 "$REPO_ROOT/scripts/addon_manager.py" disable-tapir
fi
