# Changelog

## Unreleased

### 品質・保守性の改善

- 配布ゲームの起動判定、Script終了時の解放、Scene設定のリセット、EventBusの例外復帰を修正。
- BGMパネル、UIコンポーネントのInspector、描画用空間索引、実行時サービス、CLIのScene／Prefabコマンドを分離。
- Game Module APIを15へ更新。公開クラスのレイアウト変更に伴い、既存のゲーム用DLLは再ビルドが必要。
- MSVC／Ninjaのヘッダー依存検出を修正。ローカライズされた出力と既存ビルドの依存情報再取得に対応。
- Web入力の登録解除・例外処理・ログを改善し、SDK構成、ライセンス同梱、Windows／Webの回帰検査を整備。

### LamaPon 0.1.0 開発版

- Windows・C++20・DirectX 11 によるゲームエンジン。
- LamaPon Hub、Editor、Runtime、Game、CLI の開発環境。
- Scene・Prefab・C++ Script・リアクティブ API・2D/3D 描画・物理・音声。
- 学習用プロジェクト、サンプルゲーム、日本語ドキュメント。
- Windows 向けゲームパッケージと WebGL エクスポート。
- MIT ライセンス、コントリビューションガイド、第三者ライセンス表記。
