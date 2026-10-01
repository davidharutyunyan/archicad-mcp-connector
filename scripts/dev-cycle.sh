#!/usr/bin/env bash
# Build -> install -> restart Archicad -> verify the add-on answers.
source "$(dirname "$0")/lib.sh"
"$REPO_ROOT/scripts/build-addon.sh"
"$REPO_ROOT/scripts/archicad.sh" stop
"$REPO_ROOT/scripts/install-addon.sh"
"$REPO_ROOT/scripts/archicad.sh" start "${1:-}"
ac_addon GetAddOnInfo; echo
