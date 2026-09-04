# LamaPon 本体の改修記録（2026-09-04）

対象: `3ee44b874d57`に対する作業ツリーの変更。
[品質監査](quality-2026-09-04.md)の不具合・配布・コメント・検証基盤を改修した。
神クラスの解消は継続中。BGM編集に続き、UI Inspector、描画用空間索引、実行時サービス、CLIの文書操作を独立させた。ゲーム固有のステージ／道路／車両編集は本体の改修対象から外した。

## 監査項目への対応

| 項目 | 状態 | 変更・確認 |
|---|---|---|
| Q1 配布ゲームの起動失敗 | 修正 | アーカイブ鍵を埋め込む前後でRuntimeのビルド時刻を保持。古いモジュールを拒否する検査は維持。実際に書き出したゲームを別プロセスで起動するテストを追加。 |
| Q2 終了処理の例外による解放漏れ | 修正 | ScriptBridgeがunique_ptrで終了時の所有権を持ち、OnDestroy例外時もデストラクタと購読解除を実行。NativeScriptComponentの終了・有効状態変更時も診断を記録。 |
| Q3 WebInputのイベント寿命 | 修正 | 成功した登録だけを解除し、部分失敗時もロールバック。所有者・イベント・関数で解除対象を限定し、他機能の購読を保持。同時初期化と再試行を検査。DOM要素はWebInputより長く保持する契約を明記。 |
| Q4 シーン切り替えの設定残留 | 修正 | TAA・SSR・ボリュメトリックライト、関連するフレーム情報、空間索引・プローブ参照をClearでリセット。環境を省略したシーンの通常ロードも回帰検査。 |
| Q5 EventBusの例外後状態 | 修正 | 正常終了と例外終了の両方で発行深度と無効購読を整理。入れ子の発行・Clear・例外後の再購読を検査。 |
| Q6 責務の集中 | 一部対応 | BgmLoopPanel、9種のUI Inspector、RenderSpatialIndex、RuntimeServices、CLIのScene／Prefab操作・コンポーネント定義・ファイル操作を分離。独立生成と既存動作の回帰テストを追加。境界と残りは下記。 |
| Q7 WebテストのWindows失敗とCI未登録 | 修正 | 偽変換器を実Pythonプロセスとして起動し、パス区切りを統一。Python 3.11以上をCTestに登録。Windows CIをDebug/Releaseに分け、Web CIでWasm生成とブラウザー実行を検査。 |
| Q8 ライセンス本文の欠落 | 修正 | 9件の許諾本文と通知をRuntime隣・インストールSDK・Windowsゲームへ同梱。Webは必要な本文をHTMLへ埋め込み、単体共有でも保持。原本をCMakeから複製する。 |
| Q9 配布SDKのWeb構成不足 | 修正 | Python書き出しツール、Web用CMake、Web/Portableソース、cgltfヘッダーと標準HTMLをインストール対象に追加。隔離した配布SDKから全Portableモジュールをコンパイル。 |
| Q10 APIコメントと実装の不一致 | 修正 | 深度プリパスの2つの戻り値、SSAO/SSR条件、購読解除の所有者、例外時の処理を説明。ヘッダー非同梱を暗号上の保護とする誤解も除去。 |
| Q11 モジュール再リンク・診断文字列 | 修正 | サンプルGame ModuleにRuntime DLLのリンク依存を追加。Load/Reloadの実行後にLastErrorを取得する順序へ変更。 |
| Q12 Portableログの無処理 | 修正 | Info/Warning/Errorを対応するブラウザーconsoleへ出力。UTF-8、文字列の長さ、ログレベルを実ブラウザーで検査。 |
| Q13 自前コードの警告 | 修正 | 未使用変数、ローカル変数の隠蔽、意図しないnodiscard破棄を解消。外部ライブラリの警告を一括抑制する変更はしていない。 |
| Q14 MSVC／Ninjaのヘッダー依存欠落 | 修正 | showIncludesの接頭辞を実コンパイラから未変換で取得。検出方式をコンパイルコマンドへ含め、修正前のオブジェクトも一度再コンパイルして依存を採取。エンジン本体・ゲーム用DLL・配布SDKへ適用。 |

ブラウザー検査で追加確認した点も修正した。Webビルドの既定設定で無効だったC++例外のcatchを有効化し、Script::Start内で例外から復帰して実行状態に到達することを検査した。また、Portableシーンでparentを省略したルートオブジェクトがJSONのassertで停止しないようにした。

## 検証

- MSVC x64のDebug/ReleaseでCTest各48件成功。初回の43件に、UI Inspector・RuntimeServices・RenderSpatialIndexの独立検査、CLIの5項目のプロセステスト、MSVCの増分ビルド検査を追加。PythonのWeb書き出し検査34件も含む。
- Windows配布ゲームは、DLLあり・なしの両方をWARPで別プロセス起動。DLLと暗号化シーンのロード、Script更新までを`--validate-startup`で検査。通常の描画は既存のレンダー回帰テストで検査。
- BGMパネルをEditorLayerから独立して生成し、カタログとWAV波形の描画、不正なカタログの診断を検査。
- Emscripten 6.0.9とChromeで入力の登録解除・部分失敗・再試行・他機能の購読保持、日本語ログを検査。実際のWeb用CMakeから全Portableモジュールを生成し、Scriptの例外復帰とゲーム起動を検査。
- 配布SDKを別フォルダーにインストールし、そのSDK内のツールとソースだけからWeb HTMLを生成。ライセンス・Web用ファイルの同梱を確認。
- 今回の最終差分でも隔離SDKを更新し、SDK内のCMake・公開ヘッダー・ライブラリを使って外部プロジェクトのゲーム用DLLを生成・ロード。API 15とヘッダー依存検出モジュールの同梱を確認（`refactor-sdk-module-test.log`）。その後、下記の手順で通常のインストール先にも反映した。

ローカル検証ログと生成物はGit管理外の`tmp/quality-review`、`out/build/quality-fixed-*`、`out/build/quality-web-*`に保存。今回のネイティブ検証は`refactor-release-tests.log`と`refactor-debug-tests.log`。Webのブラウザー検査は前回改修時の結果で、今回のネイティブ責務分割ではWebソースを変更していない。CIワークフロー自体のGitHub上での実行は、この作業では行っていない。

## 通常環境へのビルド反映

2026-09-04、Releaseを再構成・ビルドし、CTest 48件の成功後に`%LOCALAPPDATA%/Programs/LamaPon`へインストールした。Runtime DLL／ライブラリ、Editor、Hub、Game、CLIの6ファイルがビルド成果物とSHA-256で一致し、SDKのAPI 15と9件のライセンス本文も確認した。

更新前のインストール全体、CarGameのゲーム用DLL一式、セーブデータを`tmp/quality-review/deployment-20260904-151343`へ退避した。反映時に起動中のLamaPonアプリはなく、アプリの強制終了は行っていない。

インストール済みCLIでCarGameのRelease DLLを再生成し、`moduleUpdated: true`と`runtimeCompatible: true`を確認した。同じCLIで既存の`photorun-select-vehicle.json`（車種選択からレースへ、9項目）と`no-rotation-displacement.json`（加速・旋回・ドリフト、7項目）が成功。両セッションでDLLロード成功、実行ログのエラー・警告なし、正常停止を確認した。選択画面のスクリーンショットも確認した。

CarGameのソース・アセット内容は今回編集していない。実際のセーブデータ2ファイルは検査後もバックアップとSHA-256が一致した。ゲーム側の既存コードにはビルド警告6件（`GhostSystem.cpp`の`sscanf` 4件、`CarSystem.cpp`の未使用引数1件、`TrackSystem.cpp`の数値変換1件）が残る。長時間プレイや全コースの検証は、この反映作業には含めていない。

反映ログは`deploy-configure.log`、`deploy-build.log`、`deploy-tests.log`、`deploy-install.log`、`deploy-cargame-build.json`、`deploy-cargame-select.json`、`deploy-cargame-driving.json`。退避先と検証結果の詳細は同じフォルダーの`deployment.json`と`deployment-result.json`に保存した。

## 今回分離した責務

| 担当 | 所有する状態・契約 | 検証 |
|---|---|---|
| UIComponentInspectors | Canvas、RectTransform、Button、Image、Toggle、Slider、InputField、LayoutGroup、ScrollViewを個別の描画関数へ分離。画面サイズ、選択アセット、履歴保存、通知、Render Texture選択のみを借用。状態やコールバックをフレーム間で保持しない。 | Scene・GraphicsDevice・EditorLayerなしで9種を描画。画像の変更とUndo確定を分け、未対応の型は変更せず委譲。 |
| RenderSpatialIndex | 描画対象の境界、順序、BVHノードを所有。Sceneは候補を収集し、通常カメラ・影それぞれの判定を渡す。再構築に必要な容量を先に確保し、確保失敗時は旧索引の内容を保持。同規模の更新では容量を再利用。 | 線形走査と比較し、境界・順序変更、再利用、全削除、Clear後の再生成を検査。既存描画回帰も実行。 |
| RuntimeServices | アセット・音声・入力の初期化と破棄順を所有。GraphicsDeviceは従来APIの窓口として保持。GPUなしのファイル読み込み、未初期化アクセスの例外、Shutdown後の再生成を明文化。 | GraphicsDeviceなしでファイルを読み、通常サービスの生成・終了・再開・重複初期化の拒否を検査。 |
| CLIコマンド群 | Scene／Prefabの検査・パッチ・アサーションをSceneCommandsへ、変更不能の型定義をComponentSchemasへ、共通JSON保存・パス処理を各担当へ分離。Mainが引数解析と例外をJSONへ変換する。 | 分離前後で5項目のプロセステストを実行。17種のスキーマJSONは分離前と完全一致。プレビュー・保存・失敗時の元ファイル保持・日本語パスも確認。 |

行数は規模の参考値。Inspectorの翻訳単位は13,986→12,946行、SceneVisibilityは1,082→880行、CLI Mainは9,697→6,923行。移動先に状態や操作の境界を定義しており、行数削減だけを完了基準にはしていない。

SceneとGraphicsDeviceの構造変更に合わせ、Game Module APIを14→15へ更新。ゲーム側の公開メソッドの呼び方は維持するが、既存のゲーム用DLLは再ビルドが必要。

## 残る設計作業

Inspectorの環境設定・描画／物理コンポーネント・Script編集、Sceneの描画・ベイク・物理・シリアライズ、CLIの常駐ランタイム・描画・ビルド・書き出しは引き続き分離が必要。RuntimeServicesの寿命は互換窓口のGraphicsDeviceに結び付いており、Applicationでの構成と各利用者への注入までを完了したわけではない。ゲーム固有の編集機能はこの残作業に含めない。

次の分離では、独立した状態所有者と狭い操作インターフェースを先に定義し、既存の公開APIを維持する。Game Moduleから見えるクラスのレイアウトを変える場合はAPIバージョンと再ビルド経路も変更する。

全API・全コメントの個別精査、実機での長時間プレイ、全ブラウザー・GPUでの互換性検証は未実施。今回の成功結果を、欠陥がないことや公開準備がすべて完了したことの保証にはしない。

MSVCの日本語showIncludes接頭辞の問題はQ14で対応済み。依存情報が欠落した既存ビルドからの移行、日本語・空白を含むヘッダーの変更後の再コンパイル、無変更時のオブジェクト再利用を実コンパイルで検査した。通常のインストール先は上記のとおり更新済み。この反映作業ではGitHubへのpushやリリース公開は行っていない。
