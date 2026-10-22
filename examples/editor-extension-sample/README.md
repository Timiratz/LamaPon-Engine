# LamaPon Editor Extension SDK Sample

This sample builds a Windows/MSVC DLL against the stable C ABI in `EditorExtensionApi.h`.
The extension adds one panel and only calls UI functions provided by LamaPon.

## Build

Use the repository `src` folder or an installed SDK `include` folder as the header root.

```powershell
cmake -S examples/editor-extension-sample -B out/editor-extension-sample `
  -DLAMAPON_EDITOR_EXTENSION_INCLUDE_DIR="C:/path/to/LamaPon/src"
cmake --build out/editor-extension-sample --config Release
```

The DLL is written to `out/editor-extension-sample/package/runtime/`.
To make an installable package, copy this sample directory under
`packages/src/sample-editor-tools`, copy the DLL into its `runtime` folder, and rename
`package.json.example` to `package.json`. Then run:

```powershell
python packages/src/build_package.py sample-editor-tools --update-index
```

`package.json` sets `target` to `Engine` and `activation` to `Restart`; this extension
is therefore loaded when LamaPon Editor starts. The host validates the ABI version and
DLL path before loading it.
