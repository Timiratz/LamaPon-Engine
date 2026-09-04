# エクスポート形式の選択（2026-09-04）

対象: `58f4a83d1573`以降のエディター改修。

## 変更内容

- 「ゲームをエクスポート」でWindows（EXE）とWeb（HTML）を選択できる。初回の既定値はWindows。
- 既定の出力先を`dist/LamaPonGame`と`dist/LamaPonWeb`に分ける。手入力・参照で選んだ出力先は切り替え時にも保持する。
- Windowsのパッケージ出力とZIP作成は従来の`ExportGamePackage`を使う。
- Webは既存の互換性検査・Emscriptenビルドを別プロセスで実行する。ダイアログを閉じて編集を続けられ、完了・エラー・ログへの導線を表示する。
- Emscripten SDKとPythonの場所をPC内の`%LOCALAPPDATA%/LamaPon/web-export-tools.json`へ保存する。SDK同梱Pythonも自動検出する。
- 完成したWebパッケージだけを出力先へ入れ替える。失敗時は既存の出力を保持し、原本・SDK・Git管理情報・無関係なファイルのあるフォルダーを出力先として拒否する。

## 責務とプロセスの寿命

`GameExportDialog`が形式・入力・診断を所有し、`WebExportJob`が子プロセスと結果を所有する。EditorLayerはパスと設定、およびUIスレッド上のシーン保存・通知・フォルダー選択だけを渡す。

ビルドはシェルを介さずPythonを起動する。継承するハンドルを標準入出力に限定し、Windows Job Objectでコンパイラの子孫も管理する。エディター終了時は所有するビルドを終了する。中断時の作業用ディレクトリが残る場合はあるが、完成前の出力を成功として扱わない。

公開Runtimeのクラス構造は変更しておらず、Game Module APIは15を維持する。

## 検証・通常環境への反映

- Debug／ReleaseのCTest各50件成功。Web出力のPython検査34件に加え、Editor用の出力保護・SDK検出・公開失敗時の復元6件を実行。
- 独立したImGuiコンテキストで両形式を描画。実子プロセスで空白・日本語・`&`を含むパス、成功後の失敗通知、非同期の結果取得を検査。
- Emscripten 6.0.9で単一HTMLを生成し、ChromeでRuntimeの実行状態到達を確認。
- `%LOCALAPPDATA%/Programs/LamaPon`へReleaseを反映。インストールされたWebツールとソースだけでも、同じプロセス経路からHTMLを生成してChromeで起動した。SDK同梱Pythonの自動検出も使用。
- インストール済みエディターでEXE／HTML両画面のスクリーンショットを生成し、表示を確認。
- CarGameのWindows用DLLを再ビルド。車種選択・レース遷移9項目、加速・旋回7項目が成功。両セッションでDLLロード成功、実行ログのエラー・警告なし。セーブデータは退避コピーとSHA-256が一致。

旧インストール・CarGame DLL・セーブデータの退避先は`tmp/quality-review/web-dialog-deployment-20260904-195420`。ビルド・テスト・出力・ブラウザーのログは同フォルダー階層の`web-dialog-*`。これらの生成物とPC固有の設定はGitへ含めない。

## CarGameのWeb対応に残る制約

選択肢の追加だけでCarGame全体がWeb対応になったわけではない。現在の互換性検査では、PointLight／SpotLight、SceneLoadState、DirectX依存などの未対応API、音声・モデル変換ツールの不足などで出力を拒否する。バックアップ内のヘッダーまで走査する既存検査の過剰な指摘もあり、その整理と実際の移植を区別する必要がある。

今回検証したHTMLは小さなテスト用プロジェクトのもの。CarGameのブラウザー版は未完成で、ゲームコード・アセットや既存のWeb Runtimeを無条件に書き換える対応は行っていない。
