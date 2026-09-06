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
`onShutdown`を一度呼び出します。これにより、将来DLLやパッケージから機能を
読み込む場合にも登録と解除の境界を保てます。

現段階で用意しているのはC++登録APIです。外部DLLの探索・読み込みとABIの
固定はまだ行っていません。
