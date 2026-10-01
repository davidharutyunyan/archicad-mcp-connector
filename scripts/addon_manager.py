#!/usr/bin/env python3
"""Manage Archicad's Add-On Manager list (the same list the Add-On Manager dialog edits).

Archicad keeps extra add-on locations in its preferences plist under
"Add-On Manager" > "Include" as {"#1.": "lan.null2:///path/X.bundle", "Include Number": 1}.
Archicad must NOT be running while this list is edited (it rewrites prefs when quitting).

Usage:
  addon_manager.py status
  addon_manager.py register <bundle>      add a bundle to the list (idempotent)
  addon_manager.py unregister <bundle>
  addon_manager.py disable-tapir            remove Tapir entries (remembered for enable-tapir)
  addon_manager.py enable-tapir             restore previously removed Tapir entries

Every modification backs up the prefs file to ~/Library/ClaudeConnector/prefs-backups/.
"""

import datetime
import glob
import json
import os
import plistlib
import shutil
import subprocess
import sys

VERSION = os.environ.get("ARCHICAD_VERSION", "26")
STATE_DIR = os.path.expanduser("~/Library/ClaudeConnector")
DISABLED_FILE = os.path.join(STATE_DIR, "disabled-includes.json")
PREFIX = "lan.null2://"


def prefs_path() -> str:
    candidates = glob.glob(os.path.expanduser(f"~/Library/Preferences/com.graphisoft.AC {VERSION}.*.plist"))
    if not candidates:
        sys.exit(f"Archicad {VERSION} preferences not found in ~/Library/Preferences (start Archicad once first).")
    return sorted(candidates)[0]


def archicad_running() -> bool:
    r = subprocess.run(["pgrep", "-f", f"Archicad {VERSION}.app/Contents/MacOS/Archicad( |$)"], capture_output=True)
    return r.returncode == 0


def load(path: str) -> dict:
    with open(path, "rb") as f:
        return plistlib.load(f)


def save(path: str, data: dict) -> None:
    if archicad_running():
        sys.exit("Archicad is running - quit it first (scripts/archicad.sh stop), it overwrites its preferences on exit.")
    os.makedirs(os.path.join(STATE_DIR, "prefs-backups"), exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    shutil.copy2(path, os.path.join(STATE_DIR, "prefs-backups", f"{os.path.basename(path)}.{stamp}"))
    with open(path, "wb") as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    # cfprefsd caches preferences; restart it so Archicad reads the file we wrote.
    subprocess.run(["killall", "cfprefsd"], capture_output=True)


def includes(data: dict) -> list[str]:
    inc = data.get("Add-On Manager", {}).get("Include", {})
    n = int(inc.get("Include Number", 0))
    return [inc[f"#{i}."] for i in range(1, n + 1) if f"#{i}." in inc]


def set_includes(data: dict, entries: list[str]) -> None:
    manager = data.setdefault("Add-On Manager", {})
    inc = {f"#{i}.": e for i, e in enumerate(entries, start=1)}
    inc["Include Number"] = len(entries)
    manager["Include"] = inc


def to_entry(bundle: str) -> str:
    return PREFIX + os.path.abspath(os.path.expanduser(bundle))


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    path = prefs_path()
    data = load(path)
    entries = includes(data)

    if cmd == "status":
        print(f"Preferences: {path}")
        print("Included add-ons:")
        for e in entries:
            print("  " + e[len(PREFIX):] if e.startswith(PREFIX) else "  " + e)
        if os.path.exists(DISABLED_FILE):
            print("Disabled (restorable):", ", ".join(json.load(open(DISABLED_FILE))))
        return

    if cmd in ("register", "unregister"):
        if len(sys.argv) < 3:
            sys.exit(f"usage: addon_manager.py {cmd} <bundle>")
        entry = to_entry(sys.argv[2])
        if cmd == "register":
            if entry in entries:
                print("Already registered:", entry[len(PREFIX):])
                return
            entries.append(entry)
        else:
            if entry not in entries:
                print("Not registered:", entry[len(PREFIX):])
                return
            entries.remove(entry)
        set_includes(data, entries)
        save(path, data)
        print(f"{cmd}ed:", entry[len(PREFIX):])
        return

    if cmd == "disable-tapir":
        tapir = [e for e in entries if "tapir" in e.lower()]
        if not tapir:
            print("No Tapir add-on is registered.")
            return
        os.makedirs(STATE_DIR, exist_ok=True)
        previous = json.load(open(DISABLED_FILE)) if os.path.exists(DISABLED_FILE) else []
        json.dump(sorted(set(previous + tapir)), open(DISABLED_FILE, "w"), indent=2)
        set_includes(data, [e for e in entries if e not in tapir])
        save(path, data)
        print("Disabled:", ", ".join(t[len(PREFIX):] for t in tapir))
        return

    if cmd == "enable-tapir":
        if not os.path.exists(DISABLED_FILE):
            print("Nothing to restore.")
            return
        restore = json.load(open(DISABLED_FILE))
        set_includes(data, entries + [e for e in restore if e not in entries])
        save(path, data)
        os.remove(DISABLED_FILE)
        print("Restored:", ", ".join(r[len(PREFIX):] for r in restore))
        return

    sys.exit(__doc__)


if __name__ == "__main__":
    main()
