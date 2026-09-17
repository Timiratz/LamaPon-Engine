# Changelog

## Unreleased

### パッケージのネイティブライブラリ対応

- `package.json`へ`native`（`includeDirectories` / `libraries` / `runtimeFiles` / `defines`）を書けるようにし、外部SDKを含むパッケージを配れるようにした。Game Moduleのビルド、編集中のプレイ、ゲームの書き出しへ自動で反映される。
- 指定できるのはこの4項目だけで、コンパイル／リンクオプションは渡せない。パッケージフォルダーの外を指すパス、拡張子違い、未知のキー、不正なマクロ名はインストール時に拒否する。
- エンジン自身のDLLと同じ名前、および複数パッケージで重複するDLL名は同梱できない。
- SDKの配置忘れは、リンカーのエラーではなく「どのパッケージの何が無いか」を名指しする案内で止める。
- 「パッケージを作成...」で`package.json`を作り直しても、手で書いた`native`は残る。

### Discord Rich Presence

- プレイ中の状況をDiscordへ表示する汎用APIを追加。`DiscordActivity`の`details`、`state`、大小の画像、開始／終了タイムスタンプをゲーム制作者が自由に設定でき、ジャンルを前提にしない。
- Rich PresenceはDiscordアカウント連携・クラウドセーブから完全に独立。ログインなしでも使え、ログインしていてもRich Presenceだけを切れる。
- Discord固有APIを`DiscordPresenceBackend`アダプターへ閉じ込め、ゲーム側コードからDiscord SDKの型を使わせない。LamaPonはSDKを同梱しないため、アダプター未登録・Discord未起動でも警告ログだけを出してゲームは通常どおり動く。
- プロジェクト設定「オンライン」にRich Presence設定（Application ID、既定の大画像キーとテキスト）とエディターからの動作確認を追加。`project.json`の`online.discordPresence`へ公開情報だけを保存し、`discordPresence`が無い古いプロジェクトは無効として読み込む。
- C++ ScriptへDiscord Rich Presence APIを追加したため、Game Module APIを23へ更新。
- 拡張機能へ公式パッケージ`discord-presence-sdk`を追加。Discord公式のSocial SDKでRich Presenceを実際に表示するアダプターで、Game Module読み込み時に自動登録するためゲーム側のコード変更は要らない。ライセンス上SDK本体は同梱できないため、利用者がDiscordから入手して`sdk/`へ置く。
- 配布パッケージのソースを`packages/src/`へ置き、`build_package.py`でZipと一覧を作れるようにした。改行をLFへ固定し、どのOSで作っても利用者が受け取る中身が変わらないようにした。

### オンラインアカウントとクラウドセーブ

- Windows x64ゲームに、自前バックエンドを経由するDiscordログイン、保護したセッション復元、ログアウトを追加。
- PlayerPrefsとJSONセーブスロットを内部プレイヤーIDごとに分離し、オフライン対応の永続ジャーナル、クラウド同期、ETag競合解決、強制終了後の復旧操作を追加。
- プロジェクトのオンライン設定、エディターのアカウント／同期／復旧UI、C++ Script API、通信契約と安全な運用ガイドを追加。Game Module APIを22へ更新。

### ファイル名の統一

- サンプルのアニメーションとPrefabを英語ファイル名へ変更し、`.meta`のGUIDを維持して参照を更新。
- 新規Scene・Material・データ・フォルダーの既定名を英語に統一。日本語のGameObjectからPrefabを作る場合も英語の保存名を提案する。
- 開発規約に英語／ローマ字のファイル命名を明記。日本語の表示・コメントとUTF-8パスの対応は維持する。

### 物理と描画の時刻同期

- Windows Runtimeに`Scene::PhysicsTiming()`を追加。固定更新で進むゴーストやリプレイを、LateUpdateでRigidbodyと同じ描画時刻へ合わせられる。
- 物理時計を専用クラスへ分離し、追いつき上限で捨てた時間をCLIの実行状態へ出力。一時停止時の補間位置を保持し、固定更新中に刻み幅が混在する問題を修正。
- 更新順序のドキュメントを修正。FPS変動・停止再開・時間制限・Scene初期化・設定変更の回帰テストを追加。
- Game Module APIを16へ更新。SDK反映時に既存のゲーム用DLLの再ビルドが必要。

### エクスポート画面

- Windows（EXE）とWeb（HTML）の出力形式を選択可能に。Webは互換性検査とビルドを別プロセスで実行し、進行状況・失敗理由・ログ・完成した出力先を画面から確認できる。
- Webビルド環境はPC内に保存し、完成したHTMLパッケージだけを出力先へ反映する。出力ダイアログとWebジョブの状態をEditorLayerから分離。

### 品質・保守性の改善

- 配布ゲームの起動判定、Script終了時の解放、Scene設定のリセット、EventBusの例外復帰を修正。
- BGMパネル、UIコンポーネントのInspector、描画用空間索引、実行時サービス、CLIのScene／Prefabコマンドを分離。
- 責務分割に伴う公開クラスのレイアウト変更でGame Module APIを14から15へ更新し、
  物理と描画の時刻同期で現在の16へ更新。
- MSVC／Ninjaのヘッダー依存検出を修正。ローカライズされた出力と既存ビルドの依存情報再取得に対応。
- Web入力の登録解除・例外処理・ログを改善し、SDK構成、ライセンス同梱、Windows／Webの回帰検査を整備。

### LamaPon 0.1.0 開発版

- Windows・C++20・DirectX 11によるゲームエンジン。
- LamaPon Hub、Editor、Runtime、Game、CLIの開発環境。
- Scene・Prefab・C++ Script・リアクティブAPI・2D／3D描画・物理・音声。
- 学習用プロジェクト、サンプルゲーム、日本語ドキュメント。
- Windows向けゲームパッケージとWebGLエクスポート。
- MITライセンス、コントリビューションガイド、第三者ライセンス表記。
