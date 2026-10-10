"""Stage ASCII embedding names and restore UTF-8 virtual asset paths before main."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys


def prepare(source: Path, output: Path) -> None:
    source = source.resolve(strict=True)
    if output.is_symlink():
        raise ValueError("Embedded asset output must not be a symbolic link")
    output = output.resolve()
    if not source.is_dir() or output == source or output in source.parents or source in output.parents:
        raise ValueError("Embedded asset output must be separate from the source assets")
    owner = output / "owner.json"
    if owner.is_symlink():
        raise ValueError("Embedded asset owner must not be a symbolic link")
    expected = {"format": "lamapon.embedded-assets", "version": 1, "source": str(source)}
    if output.exists() and any(output.iterdir()) and (
            not owner.is_file() or json.loads(owner.read_text(encoding="utf-8")) != expected):
        raise ValueError("Embedded asset output must be empty or owned by this asset source")
    files = []
    for path in source.rglob("*"):
        if path.is_symlink():
            raise ValueError("Embedded assets must not contain symbolic links: " + str(path))
        if path.is_file():
            files.append(path)
    output.mkdir(parents=True, exist_ok=True)
    owner.write_text(json.dumps(expected, ensure_ascii=True) + "\n", encoding="utf-8")
    payload = output / "payload"
    if payload.is_symlink():
        raise ValueError("Embedded payload must not be a symbolic link")
    payload.mkdir(exist_ok=True)
    mappings, selected = [], set()
    for path in sorted(files):
        relative = path.relative_to(source).as_posix()
        name = hashlib.sha256(relative.encode("utf-8")).hexdigest()
        destination = payload / name
        if destination.is_symlink():
            raise ValueError("Embedded payload must not contain symbolic links")
        shutil.copy2(path, destination)
        selected.add(name)
        mappings.append(["/__lamapon_embedded__/" + name, "/assets/" + relative])
    for stale in payload.iterdir():
        if stale.is_symlink() or not stale.is_file():
            raise ValueError("Embedded payload contains an unexpected entry")
        if stale.name not in selected:
            stale.unlink()
    script = """var lamaponPreviousRuntimeInitialized = Module['onRuntimeInitialized'];
Module['onRuntimeInitialized'] = function() {
  var entries = %s;
  for (var i = 0; i < entries.length; ++i) {
    var destination = entries[i][1];
    FS.mkdirTree(destination.slice(0, destination.lastIndexOf('/')));
    FS.rename(entries[i][0], destination);
  }
  if (lamaponPreviousRuntimeInitialized) lamaponPreviousRuntimeInitialized.apply(Module, arguments);
};
""" % json.dumps(mappings, ensure_ascii=True)
    aliases = output / "aliases.js"
    if aliases.is_symlink():
        raise ValueError("Embedded aliases must not be a symbolic link")
    if not aliases.is_file() or aliases.read_text(encoding="ascii") != script:
        aliases.write_text(script, encoding="ascii")


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.source, args.output)
        return 0
    except (OSError, ValueError) as error:
        parser.exit(1, "Web asset embedding failed: " + str(error) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
