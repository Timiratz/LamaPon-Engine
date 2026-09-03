# CarGame Portable Web target

This regression target compiles the original CarGame C++ scripts directly to
WebAssembly. The general Exporter can now infer the same target directly from
CarGame's normal `.lamapon/project.json`; this file remains as a stable engine
regression fixture. Neither path copies nor rewrites the game logic.

Expected sibling layout:

```text
GitHub/
  CarGame/
  LamaPon-web-windows/
```

Export from the engine worktree:

```sh
python3 tools/export_web.py prototypes/cargame-portable/project.json
```

The ordinary-project auto-detection path is:

```sh
python3 tools/export_web.py ../CarGame/.lamapon/project.json \
  --output prototypes/cargame-portable/.lamapon/direct-web \
  --build-dir prototypes/cargame-portable/.lamapon/direct-build
```

The explicit output/build paths above keep every generated file outside the
CarGame repository. Without them, only CarGame's generated `.lamapon/web*`
directories are used; source assets and Project settings are still unchanged.

The distributable entrypoint is:

```text
prototypes/cargame-portable/.lamapon/web/LamaPonWebGL-CarGame.html
```

It is a self-contained HTML package with JavaScript, WebAssembly, Scene data,
textures, and audio embedded by Emscripten. Add `?autopilot=1` to run the
original deterministic CarGame test-driving path.

Controls are read from CarGame's `.lamapon/project.json` during export. The
current build uses WASD/arrow keys to drive, R to restart, and C to switch the
camera; standard gamepad bindings are packaged at the same time. Touch keeps
the left-side steering pad and right-side throttle/brake areas, with the
top-right corner switching the camera.
