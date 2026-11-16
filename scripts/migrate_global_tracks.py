#!/usr/bin/env python3
"""Rename saved Project Tracks to Global Tracks, recursively in a data/Presets folder.

Preview by default; --apply writes changes and keeps a .before-global-track backup
beside each changed JSON. Custom names and captured parameter values are preserved.
"""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import tempfile


def rename_field(obj, old, new):
    if old not in obj:
        return False
    if new in obj and obj[new] != obj[old]:
        raise ValueError(f"Conflicting {old} and {new} fields")
    obj[new] = obj.pop(old)
    return True


def default_name(name, old, new):
    if isinstance(name, str) and re.fullmatch(re.escape(old) + r"(?: [2-9][0-9]*| 1[0-9]+)?", name):
        return new + name[len(old):]
    return name


def migrate(value):
    """Modify timeline track objects in place; return whether anything changed."""
    changed = False
    if isinstance(value, list):
        for item in value:
            changed = migrate(item) or changed
    elif isinstance(value, dict):
        clips = value.get("clips")
        if isinstance(clips, list) and ("isProjectTrack" in value or value.get("isGlobalTrack") is True):
            changed = rename_field(value, "isProjectTrack", "isGlobalTrack") or changed
            if value.get("isGlobalTrack") is True:
                name = default_name(value.get("name"), "Project Track", "Global Track")
                if name != value.get("name"):
                    value["name"] = name
                    changed = True
                for clip in clips:
                    if not isinstance(clip, dict):
                        continue
                    changed = rename_field(clip, "projectKeyframes", "globalKeyframes") or changed
                    name = default_name(clip.get("name"), "Project", "Global")
                    if name != clip.get("name"):
                        clip["name"] = name
                        changed = True
        for key, item in value.items():
            # Keyframes contain captured user data, not nested timelines.
            if key not in ("globalKeyframes", "projectKeyframes"):
                changed = migrate(item) or changed
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--apply", action="store_true", help="Write the migration (default: preview only)")
    args = parser.parse_args()
    root = args.folder.expanduser().resolve()
    if not root.is_dir():
        parser.error(f"Not a folder: {root}")
    pending = []
    try:
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() != ".json" or not path.is_file() or path.is_symlink():
                continue
            original = path.read_bytes()
            if b'"isProjectTrack"' not in original and b'"projectKeyframes"' not in original:
                continue
            data = json.loads(original)
            if not migrate(data):
                continue
            backup = path.with_name(path.name + ".before-global-track")
            if args.apply and backup.exists():
                raise ValueError(f"Backup already exists: {backup}")
            text = original.decode("utf-8")
            indent_match = re.search(r'\n([ \t]+)"', text)
            indent = indent_match.group(1) if indent_match else "    "
            updated = json.dumps(data, ensure_ascii=False, indent=indent, allow_nan=False)
            if text.endswith("\n"):
                updated += "\n"
            pending.append((path, backup, original, updated))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Migration stopped before writing: {error}\n")

    for path, backup, original, updated in pending:
        print(f"{'Updating' if args.apply else 'Would update'} {path.relative_to(root)}")
        if not args.apply:
            continue
        if path.read_bytes() != original:
            parser.exit(1, f"File changed since scanning: {path}\n")
        with backup.open("xb") as saved:
            saved.write(original)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as output:
                temporary = Path(output.name)
                output.write(updated)
            temporary.chmod(stat.S_IMODE(path.stat().st_mode))
            os.replace(temporary, path)
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
    print(f"{len(pending)} JSON file(s) {'updated' if args.apply else 'would change'}.")


if __name__ == "__main__":
    main()
