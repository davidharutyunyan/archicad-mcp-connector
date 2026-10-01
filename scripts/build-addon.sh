#!/usr/bin/env bash
# Configures (first time) and builds the Claude Connector add-on bundle.
source "$(dirname "$0")/lib.sh"

DEVKIT="${AC_API_DEVKIT_DIR:-$REPO_ROOT/.devkit/devkit$ARCHICAD_VERSION}"
[[ -f "$DEVKIT/Support/Inc/ACAPinc.h" ]] || "$REPO_ROOT/scripts/fetch-devkit.sh"

# Always (re)configure: cheap, and picks up new source files and addon/mdid.local.cmake changes.
cmake -S "$ADDON_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DAC_API_DEVKIT_DIR="$DEVKIT" >/dev/null
ln -sf "$BUILD_DIR/compile_commands.json" "$ADDON_DIR/compile_commands.json" 2>/dev/null || true
cmake --build "$BUILD_DIR" "$@"
echo "Built: $BUILD_DIR/$BUNDLE_NAME"
