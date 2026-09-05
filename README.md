# LamaPon

LamaPonは、C++でゲームを作りながら、ゲームエンジンの仕組みを学べるWindows向けゲームエンジンです。
次の体験を一つの開発環境にまとめています。

- **C++のコードとゲームループを読む**
- **SceneとComponentを編集してゲームを作る**
- **描画、物理、入力、音声などの仕組みを確かめる**
- **検証・ビルド・デバッグの手順を繰り返せる形で学ぶ**

LamaPon Hubの「3D学習」「2D学習」には、最初から遊べるScene、コメント付きのC++コード、
段階別の課題、個人の進捗を記録する仕組みが含まれます。

**はじめての方は[学習ロードマップ](docs/learning-path.md)と[入門チュートリアル](docs/getting-started.md)からどうぞ。**

C++20、DirectX 11、DirectXTK11、`GameObject + Component`、ランタイムとエディターの分離を
技術的な土台としています。同じJSON形式のSceneを、エディター、単体ゲーム、CLIテストで読み込めます。

## 入手・ビルド

ソースコード：[LamaPon-Engine](https://github.com/Timiratz/LamaPon-Engine)

LamaPonのソースコードはMITライセンスで提供します。リポジトリが非公開の場合は、閲覧と取得にアクセス権が必要です。

Windows 10/11、Visual Studioの「C++によるデスクトップ開発」、CMake 3.25以上、Ninjaを用意し、x64 Native Tools Command Promptから実行します。

```bat
git clone https://github.com/Timiratz/LamaPon-Engine.git
cd LamaPon-Engine
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
out\build\windows-release\LamaPonHub.exe
```

ビルド済みパッケージは、リリース作成後に[Releases](https://github.com/Timiratz/LamaPon-Engine/releases)へ掲載します。
ZIPを展開して`LamaPonHub.exe`を起動すると、プロジェクト作成、Scene編集、ゲームのエクスポートを利用できます。
C++ ScriptのビルドにはVisual StudioのC++開発環境が必要です。

- `LamaPon-<version>-windows-x64.zip` — エンジン一式
- `LamaPon-symbols-windows-x64.zip` — クラッシュ解析用 PDB

## ドキュメント

使い方は[docs/のドキュメント一覧](docs/index.md)から機能別に確認できます。

- [入門チュートリアル](docs/getting-started.md) — 最初のゲームを30〜60分で
- [学習ロードマップ](docs/learning-path.md) — C++とゲーム制作を学ぶ順序
- [エディター](docs/editor.md) / [C++スクリプティング](docs/scripting.md)
- [グラフィックス](docs/graphics.md) / [物理](docs/physics.md) / [UIと2D](docs/ui-2d.md) / [Animation](docs/animation.md)
- [SceneとPrefab](docs/scenes.md) / [オーディオ](docs/audio.md) / [入力](docs/input.md) / [NavMesh](docs/navigation.md)
- [プロジェクト管理とビルド](docs/project.md)
- [WebGLエクスポート](docs/web-export.md) — 通常のLamaPonプロジェクトをC++／Wasm／WebGLの単一HTMLへ変換
- [コマンドライン（LamaPonCli）](docs/cli.md) — エディターなしで撮影とJSONレポート

## 主な機能

- **開発環境** — LamaPon Hub、ドッキング対応Editor、エディターなしのGame Runtime、CLI
- **ゲーム構成** — `Scene`、`GameObject`、`Component`、親子Transform、Prefab、Undo／Redo
- **C++スクリプト** — Game Module、Hot Reload、ライフサイクル、タイマー、コルーチン、イベント
- **3D描画** — Direct3D 11、PBRマテリアル、各種ライトと影、glTF／GLB／FBX、スキニング、LOD、ポストエフェクト
- **2DとUI** — Sprite、Tilemap、Particle、Canvas、各種UIウィジェット、日本語テキストとIME入力
- **物理と移動** — 2D／3D Collider、Rigidbody、CCD、Joint、Raycast、Character Controller、NavMesh
- **アニメーションと音声** — Animation Clip、Animator Controller、WAV／OGG、3D音声、ストリーミング、ミキサー
- **アセット管理** — GUID付き`.meta`、参照を保つ改名・移動、Material、データアセット、非同期読み込み
- **配布と検証** — Windowsゲーム、ポータブルWebGL、CTest、描画回帰、CLIによる検証と自動プレイテスト
- **診断** — Console、CPU／GPUプロファイラー、クラッシュダンプ、ゲーム内デバッグオーバーレイ

詳しい対応範囲と使い方は[ドキュメント一覧](docs/index.md)から各機能のページを参照してください。

## ライセンス

LamaPon本体はMITライセンスです。
DirectXTK、Dear ImGui、nlohmann/jsonもMITライセンスです。
XAudio2 RedistributableにはMicrosoftのライセンスが適用されます。
詳細は`THIRD_PARTY_NOTICES.md`を参照してください。
