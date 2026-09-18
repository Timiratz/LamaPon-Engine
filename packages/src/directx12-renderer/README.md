# DirectX 12 Renderer

LamaPonのDirectX 12 Experimental描画バックエンドです。

インストール、更新、削除は実行中のDLLを差し替えず、エディターまたは
ゲームの再起動後に反映されます。プロジェクト設定の`Rendering API`を
`DirectX 12 Experimental`へ変更してから再起動してください。

現在の`0.1.0`は物理DLL分離中の移行版です。パッケージDLLがABIと対応APIを
宣言し、描画処理は同じ版の`LamaPonRuntime`に含まれるD3D12実装を利用します。
パッケージとエンジンの版を混在させないでください。
