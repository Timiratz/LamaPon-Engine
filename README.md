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

> [!IMPORTANT]
> コミュニティ向けの変更は、必ず`community/main`から新しい作業ブランチを作成し、Pull Requestのマージ先にも`community/main`を指定してください。`community/main`への直接pushは禁止です。Pull Requestを作成した後は、レビューで明示的な許可が下りるまでマージしないでください。`main`と`student/main`は、作業ブランチの起点、編集、Pull Request、マージ、削除の対象にしないでください。詳しくは[コントリビューションガイド](CONTRIBUTING.md#必須のブランチ運用ルール)を確認してください。

C++20、DirectX 11、DirectXTK11、`GameObject + Component`、ランタイムとエディターの分離を
技術的な土台としています。同じJSON形式のSceneを、エディター、単体ゲーム、CLIテストで読み込めます。

## 開発環境の構築

LamaPonのソースコードはMITライセンスで公開しています。ビルド済みの開発環境はリポジトリへ含めず、Visual Studioとビルドツールは各開発者のPCへ用意する構成です。

対応環境はWindows 10/11（x64）です。Visual Studio InstallerからVisual Studio 2022以降の「C++によるデスクトップ開発」を導入し、次を利用できる状態にしてください。

- MSVC x64 C++ビルドツール
- Windows 10またはWindows 11 SDK
- CMake 3.25以上
- Ninja
- Git

Visual Studio付属のx64 Native Tools Command Promptを開き、ソースコードを取得します。

```bat
git clone https://github.com/Timiratz/LamaPon-Engine.git
cd LamaPon-Engine
```

## LamaPon Hubのビルドと起動

リポジトリ直下で、Release版のHubをビルドして起動します。

```bat
cmake --preset windows-release
cmake --build --preset windows-release --target LamaPonHub
out\build\windows-release\LamaPonHub.exe
```

デバッグビルドを使う場合は、`windows-release`を`windows-debug`へ置き換えます。

```bat
cmake --preset windows-debug
cmake --build --preset windows-debug --target LamaPonHub
out\build\windows-debug\LamaPonHub.exe
```

起動後は「新しいプロジェクトを作成」または「既存のLamaPonプロジェクトを追加」を選びます。新規プロジェクトの保存場所には、このエンジンリポジトリの外側を指定してください。詳しい操作は[入門チュートリアル](docs/getting-started.md)を参照してください。

ビルド成果物は`out/`へ作成され、Gitの追跡対象にはなりません。Visual Studioのユーザー設定、ログ、キャッシュ、生成したプロジェクトもコミットしないでください。

ビルドとテストをまとめて確認する場合は、次を実行します。

```bat
cmake --build --preset windows-release
ctest --preset windows-release
```

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
