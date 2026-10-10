"""Run the real native smoke game twice using an explicitly chosen empty save directory.

This runner does not create a test directory or delete files. The game writes two
save files inside --data-dir; authorize that location before invoking the runner.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path, required=True)
    arguments = parser.parse_args()
    game = arguments.game.resolve(strict=True)
    directory = arguments.data_dir.resolve(strict=True)
    if not game.is_file() or not directory.is_dir() or any(directory.iterdir()):
        parser.error("--game must be a file and --data-dir must be an existing empty directory")
    for expected in (1, 2):
        result = subprocess.run(
            [str(game), "--probe", "--data-dir", str(directory)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=45,
        )
        if result.returncode:
            raise RuntimeError("Native smoke failed:\n" + result.stdout + result.stderr)
        log = result.stdout + result.stderr
        if "LamaPon native frame limit reached" not in log or "LamaPon native graphics resources restored" not in log \
                or "LamaPon native gamepad UI navigation probe passed" not in log:
            raise RuntimeError("Native smoke exited before completing its frame and context recovery probes:\n" + log)
        values = json.loads((directory / "values.json").read_text(encoding="utf-8"))
        if values.get("native-probe-starts") != str(expected):
            raise RuntimeError("Save counter did not persist across process restarts")
        if values.get("native-probe-text") != "保存テスト / Linux Android":
            raise RuntimeError("UTF-8 save value changed")
    print("Native scene, SDL gamepad UI navigation/disconnection, framebuffer, context replacement, lifecycle and save restart probes passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
