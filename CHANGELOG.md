# Changelog

## Unreleased

### 描画API設定

- Project SettingsのGraphicsへ`Auto` / `DirectX 11` / `DirectX 12 Experimental`の選択を追加。既定は従来どおりDirectX 11で、変更は次回起動時に反映する。
- DirectX 12は未実装のため、安全にDirectX 11へフォールバックする。将来のD3D11 / D3D12バックエンド分離に備えて起動時の選択経路を追加。
- Device / Context / SwapChainとバックバッファ資源の所有を`D3D11Backend`へ分離し、`GraphicsDevice`をBackend選択の窓口に変更。API固有型を含まない共通Backend契約と、選択結果・フォールバック理由を追加。
- 最終合成時のバックバッファbindを共通Backend契約へ移し、RendererがD3D11のバックバッファRTVを直接参照しない経路へ変更。
- Dear ImGuiの描画API固有処理を`EditorGuiRenderer`へ分離し、実効APIに応じてD3D11実装を生成するEditor側の選択経路を追加。
- Editor UIのテクスチャ参照生成を`EditorGuiRenderer`へ集約し、Assetと表示用RenderTargetのD3D11 SRV変換をD3D11実装内へ分離。
- Editorのオフスクリーン描画先操作を`GraphicsDevice`の共通入口へ集約し、ViewportとプレビューからD3D11 Device / Contextの受け渡しを削減。
- オフスクリーン描画先の作成・bind・clear・表示用確定を共通Backend契約へ移し、`RenderTarget`のD3D11直呼びAPIをBackend内部へ限定。
- 深度プリパスのbindとSSR用深度コピーを共通Backend契約へ移し、呼び出し側からD3D11 Contextの受け渡しを削減。
- SSRとTAAのカラー履歴コピーを共通Backend契約へ移し、シーンとポスト処理からD3D11 Contextの受け渡しを削減。
- 自動露出の非同期輝度readbackと次回用転送を共通Backend契約へ移し、ポスト処理からD3D11 Contextの受け渡しを削減。
- 方向・スポット・ポイント影の資源作成と描画先操作を共通Backend契約へ移し、シーンからD3D11 Contextの受け渡しを削減。
- 車両パラメータープレビューのモデル送信を`EditorModelPreviewRenderer`へ分離し、DirectXTK11のContext / CommonStates / BasicEffect操作をD3D11実装内へ隔離。
- `GraphicsSettings`と`GraphicsDevice`のABI変更、および共通Backend契約の拡張に伴い、Game Module APIを24へ更新。

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
  物理と描画の時刻同期で16、描画API設定で17、Backend分離で18、
  バックバッファ契約の追加で19、オフスクリーン描画先契約の追加で20、
  オフスクリーン深度操作の追加で21、カラー履歴操作の追加で22、
  自動露出の輝度readback契約の追加で23、影マップ操作の追加で
  現在の24へ更新。
- MSVC／Ninjaのヘッダー依存検出を修正。ローカライズされた出力と既存ビルドの依存情報再取得に対応。
- Web入力の登録解除・例外処理・ログを改善し、SDK構成、ライセンス同梱、Windows／Webの回帰検査を整備。

### LamaPon 0.1.0 開発版

- Windows・C++20・DirectX 11によるゲームエンジン。
- LamaPon Hub、Editor、Runtime、Game、CLIの開発環境。
- Scene・Prefab・C++ Script・リアクティブAPI・2D／3D描画・物理・音声。
- 学習用プロジェクト、サンプルゲーム、日本語ドキュメント。
- Windows向けゲームパッケージとWebGLエクスポート。
- MITライセンス、コントリビューションガイド、第三者ライセンス表記。
