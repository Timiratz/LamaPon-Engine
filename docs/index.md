# LamaPon ドキュメント

LamaPonのプロジェクト作成、C++スクリプト、エディター操作、ビルド、エクスポートを機能別に案内します。
はじめて使う場合は、まず[学習ロードマップ](learning-path.md)を読み、
[入門チュートリアル](getting-started.md)を実際に遊びながら進めてください。
各ページには短い手順と、問題が起きたときの確認項目を用意しています。

## はじめに

| ページ | 内容 |
|---|---|
| [学習ロードマップ](learning-path.md) | ゲームループ、C++、ゲーム制作、検証とデバッグを学ぶ手順 |
| [入門チュートリアル](getting-started.md) | プロジェクト作成からエクスポートまでを30〜60分で体験 |
| [プロジェクト管理とビルド](project.md) | LamaPon Hub、プロジェクト設定、エクスポート、ビルド方法 |
| [WebGLエクスポート](web-export.md) | C++ゲームを単一HTMLへ変換する対応機能、自動代替、拒否条件 |
| [書き出したゲームの保護](export-protection.md) | 配布物の暗号化、書き出しごとに生成する配布物固有鍵、改ざん検知と、その限界 |

## エディターとスクリプト

| ページ | 内容 |
|---|---|
| [エディター](editor.md) | 画面構成、Asset Browser、Scene View / Game View、Console |
| [C++スクリプティング](scripting.md) | C++ Scriptの作成、ホットリロード、Script API |
| [コマンドライン（LamaPonCli）](cli.md) | エディターなしで作成・撮影・ビルド・書き出し。JSONレポート付き |
| [ゲームを作る](ai-agent.md) | CLI・エディター・リモート操作を使った制作手順 |
| [コード一覧](code-reference.md) | 使える機能を宣言・引数・サンプル付きで一覧 |
| [データアセット](data-assets.md) | ScriptableObject相当。GameObjectに依存しないデータをエディターで編集して読む |
| [パッケージ](packages.md) | 公式パッケージのワンクリック導入・更新・削除 |

## 機能別ガイド

| ページ | 内容 |
|---|---|
| [グラフィックス](graphics.md) | 3Dモデル、ライティング、マテリアル、Skybox、LOD |
| [カスタムShader](shaders.md) | HLSLでマテリアル・2D・ポストエフェクトを拡張。宣言でInspectorに名前付きUI、テクスチャ追加、半透明・加算の指定 |
| [物理と衝突判定](physics.md) | Rigidbody、Collider、Raycast、物理マテリアル |
| [UIと2D機能](ui-2d.md) | UI Canvas、ウィジェット、2Dエフェクト、Tilemap、日本語テキスト |
| [Animation](animation.md) | スケルタルアニメーション、Animator Controller |
| [SceneとPrefab、セーブデータ](scenes.md) | Scene切り替え、非同期読み込み、Prefab、セーブ |
| [オーディオ](audio.md) | 効果音・BGM、3Dサウンド、ストリーミング、バス |
| [入力](input.md) | キーボード・マウス・ゲームパッド、Action Mapping |
| [Navigation Mesh](navigation.md) | NavMeshのベイクとAI経路移動 |

機能の一覧は[README](../README.md)の「実装済み」を参照してください。
