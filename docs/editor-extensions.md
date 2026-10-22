# エディター拡張

LamaPon Editorの追加パネルと拡張メニューは、
`EditorExtensionRegistry`へ機能単位で登録できます。
登録されたパネルは描画ループ、「ウィンドウ」メニュー、表示状態の保存、
「標準レイアウトに戻す」へ自動的に参加します。

## 拡張機能を登録する

`EditorExtensionDefinition`には一意なID、表示名、パネル、必要に応じた
ライフサイクル処理を指定します。

```cpp
EditorExtensionDefinition extension;
extension.id = "sample.level-tools";
extension.displayName = "レベル制作ツール";
extension.panels.push_back({
    "sample.level-validator",
    "レベル検査",
    false, // 既定では閉じる
    true,  // 「ウィンドウ」メニューへ表示
    [](bool& open)
    {
        if (ImGui::Begin("レベル検査", &open))
        {
            ImGui::TextUnformatted("検査結果をここへ表示します");
        }
        ImGui::End();
    }
});

std::string error;
if (!editorLayer.ExtensionRegistry().Register(
        std::move(extension), &error))
{
    // IDの重複や必須項目不足を報告します。
}
```

`EditorPanelDefinition`の6番目の`windowMenuGroup`へ名前を入れると、「ウィンドウ」メニューの
その名前のサブメニューへまとめて表示します。LamaPon標準の解析パネルは`"解析"`を使っているため、
独自の計測・デバッグ用パネルも同じ名前にすると並べて置けます。

```cpp
extension.panels.push_back({
    "sample.navmesh-inspector",
    "NavMesh検査",
    false,
    true,
    [](bool& open) { /* ... */ },
    "解析" // 「ウィンドウ」→「解析」へ表示
});
```

パネルIDはすべての拡張機能を通して一意にしてください。
登録時に重複や空のID、描画コールバック不足を検出し、失敗した登録は
途中のパネルを残しません。

## ライフサイクルとメニュー

- `onAttach`: 登録完了時の初期化
- `onUpdate`: エディターの各フレームで行うバックグラウンド結果の回収など
- `drawMenu`: 「拡張機能」メニュー内の項目描画
- `onShutdown`: 登録解除またはエディター終了前の後始末

通常の`drawMenu`は拡張機能名のサブメニューへまとめられます。
`menuInline`はLamaPon標準機能の互換配置用なので、追加機能では既定値の
`false`を推奨します。

`Unregister(extensionId)`は、その拡張機能が所有する全パネルをまとめて外し、
`onShutdown`を一度呼び出します。外部DLLの管理器もこの境界を使い、callbackを
登録所から外してからDLLを解放します。

組み込み機能向けのC++登録APIに加えて、外部DLL向けの固定C ABIを使えます。

## Engineパッケージから外部DLLを読み込む

`target`が`Engine`のパッケージは、`editorExtension`へDLLの相対パスとABI版を
記述すると、Editor起動時にそのDLLからパネルを登録できます。反映には再起動が
必要です。DLLは`assets/packages/<名前>/`の内側からだけ読み込まれます。

```json
{
  "name": "sample-editor-tools",
  "target": "Engine",
  "activation": "Restart",
  "editorExtension": {
    "abiVersion": 1,
    "runtimeLibrary": "editor/SampleEditorTools.dll"
  }
}
```

拡張DLLは`LamaPonGetEditorExtensionV1`をexportし、静的な
`LamaPonEditorExtensionV1`記述を返します。C++標準ライブラリやImGuiの型を
DLL境界へ渡さず、ABIヘッダーの関数表でパネル描画、更新、初期化、終了を行います。
API定義は配布SDKの`include/LamaPon/Editor/EditorExtensionApi.h`にあります。
パネルUIで現在使える部品はテキスト、ボタン、チェックボックス、浮動小数入力と
スライダー、区切り線です。

最小のDLLは次の形です。記述と文字列はDLLがロードされている間保持し、callbackから
C++例外をDLL境界の外へ送出しないでください。

```cpp
namespace
{
    void LAMAPON_EDITOR_EXTENSION_CALL DrawPanel(
        void*,
        void*,
        const LamaPonEditorUiApiV1* ui,
        int32_t*)
    {
        ui->text(ui->userData, "Live2D拡張の設定");
    }

    int32_t LAMAPON_EDITOR_EXTENSION_CALL Initialize(
        const LamaPonEditorExtensionHostApiV1* host,
        void** userData)
    {
        *userData = nullptr;
        host->log(host->userData, 0, "Live2D tools ready");
        return 1;
    }

    void LAMAPON_EDITOR_EXTENSION_CALL Shutdown(void*) {}

    const LamaPonEditorPanelV1 g_panels[]{
        {
            sizeof(LamaPonEditorPanelV1),
            "sample.live2d.panel",
            "Live2D設定",
            nullptr,
            LAMAPON_EDITOR_PANEL_SHOW_IN_WINDOW_MENU,
            nullptr,
            DrawPanel
        }
    };

    const LamaPonEditorExtensionV1 g_extension{
        sizeof(LamaPonEditorExtensionV1),
        LAMAPON_EDITOR_EXTENSION_ABI_VERSION,
        "sample.live2d.tools",
        "Live2D Tools",
        g_panels,
        1,
        Initialize,
        nullptr,
        Shutdown
    };
}

extern "C" LAMAPON_EDITOR_EXTENSION_EXPORT
const LamaPonEditorExtensionV1* LAMAPON_EDITOR_EXTENSION_CALL
LamaPonGetEditorExtensionV1()
{
    return &g_extension;
}
```

ABI版、最低エンジン版、再起動指定、DLLの存在とパッケージ内パスを読み込み前に
確認します。問題のある拡張はログへ理由を記録してスキップし、他の拡張とEditorの
起動は続けます。拡張DLLはEditorプロセス内で動作するため、信頼できる配布元の
パッケージだけをインストールしてください。

最小DLLのビルド可能なサンプルは
[`examples/editor-extension-sample`](../examples/editor-extension-sample/README.md)にあります。
インストール済みSDKにはサンプル一式と`include/LamaPon/Editor/EditorExtensionApi.h`が入ります。

## ゲーム実行時の描画拡張

C++ Game Moduleの`Script`には`OnRender2D(const SpriteDrawContext&)`があります。
Game Moduleが登録する任意のScriptから`SpriteDrawContext::DrawMesh`を呼ぶと、
既存の2D描画経路にメッシュを送れます。描画コールバックはSceneの2D描画中、
メインスレッドから呼び出され、受け取った描画窓口はその呼び出し中だけ有効です。

このGame Module APIはエンジンと同じWindows/MSVC/C++ ABIを使います。
独立したC ABIでEditorを拡張する`editorExtension`とは境界が異なります。
C++スクリプトと外部SDKは[Projectパッケージ](packages.md)へ追加できます。
Live2Dの利用例は[`live2d-cubism`](../packages/src/live2d-cubism/README.md)を参照してください。
