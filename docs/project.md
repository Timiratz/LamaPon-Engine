# プロジェクト管理とビルド

LamaPon Hub、プロジェクト設定、ゲームのエクスポート、ディレクトリ構成、
ビルド方法を説明します。

[← ドキュメント一覧へ戻る](index.md)

## LamaPon Hubとプロジェクト

`LamaPonHub.exe`はプロジェクトの作成・選択・起動を行う入口です。

- プロジェクト名と保存場所を指定して新規作成
- 3D、2D、3D学習、2D学習テンプレート
- 既存のLamaPonプロジェクトを追加
- 最近使ったプロジェクトを一覧表示
- 一覧からの除外（プロジェクトファイル自体は削除しません）
- 作成または選択したプロジェクトを`LamaPonEditor.exe`で起動
- プロジェクト単位のC++ Game Moduleビルド、Hot Reload、ゲーム書き出し

すべての新規プロジェクトに`assets/scenes/Main.scene.json`、`.lamapon/project.json`、描画に必要な組み込みShader、プロジェクト用`.gitignore`とREADMEが生成されます。

「3D学習」「2D学習」には、さらに遊べるScene、`LEARNING.md`、`learning/journey.json`、`learning/design-note.md`、コメント付き`assets/scripts/LearningPlayer.cpp`が入ります。[学習ロードマップ](learning-path.md)も参照してください。

3D／2Dは最小構成です。`scripts`や`textures`などは必要になったときにAsset Browserから作成してください。
履歴は`%LOCALAPPDATA%/LamaPon/hub.json`へ保存されます。

Hubは起動時にバックグラウンドでリポジトリ（`LamaPon-Engine`）の最新版を確認し、新しいエンジンが公開されていればタイトル下へ通知バナーを表示します（「ダウンロードページを開く」「このバージョンをスキップ」）。プライベートまたはリリースがない間は、認証なしでは取得できないため通知しません。
オフライン時や配布リポジトリが未作成の間は何も表示されません。

### プロジェクトを他の人と共有する（Git）

プロジェクトはそのままGitリポジトリにできます。
新規作成時に`.gitignore`とREADMEが生成されるので、フォルダーのルートで`git init`して全部コミットすれば共有できます。

```bash
cd <プロジェクトフォルダー>
git init
git add .
git commit -m "初回"
git remote add origin https://github.com/<自分>/<リポジトリ>.git
git push -u origin main
```

**共有するもの**

- `assets/` 一式（シーン、プレハブ、スクリプト、画像、音声、Shader）
- **`.meta`ファイルも必ず一緒に**コミットします。アセットのGUIDが
  入っていて、シーンやマテリアルからの参照はこのGUIDで結ばれています。
  `.meta`を共有しないと相手のPCで新しいGUIDが振られ、参照が全部切れます
- `.lamapon/project.json`（プロジェクト設定と起動シーン）
- `LEARNING.md`と`learning/`（学習プロジェクトの共有教材と企画メモ）

**共有しないもの**（生成された`.gitignore`が除外します）

- `.lamapon/editor-settings.json`、`imgui-layout.ini` — エディターの
  ウィンドウ配置。
  PCごとに違うので、共有すると相手のレイアウトを壊します
- `.lamapon/bin/`、`.lamapon/build/`、`build/`、`dist/` — ビルド生成物
- `.lamapon/LamaPonEditor.log`、`game-module-build.log`、`profile.json` —
  ログと計測値
- `.lamapon/package-backups/` — パッケージ更新前の退避（本体の複製）
- `.lamapon/Crashes/` — クラッシュダンプ
- `.lamapon/jobs/`、`.lamapon/runtime/` — CLIの非同期処理・実行セッション
- `.lamapon/learning-progress.json` — 個人の完了状態とCLIで設定した方向。教材自体は共有します
- `captures/`、`tests/output/` — 自動撮影とテストの生成結果
- `__pycache__/`、`.pytest_cache/`、`*.py[cod]` — Python補助ツールのキャッシュ
- `*.bak` — プロジェクト移行が組み込みアセットを更新するときの退避

**受け取った側の手順**

1. リポジトリをクローンする
2. LamaPon Hubで「既存のLamaPonプロジェクトを追加」からそのフォルダーを選ぶ
3. 開く。C++スクリプトがあれば初回起動時に自動でビルドされます
  （Consoleにビルド完了が出るまで少し待ってください）

エディターは開く前にバージョンを確かめ、必要なら確認ダイアログを出します（[詳細](#新しいエンジンで既存プロジェクトを開く)）。

### エディターが落ちたとき（セーフモード）

C++スクリプトの不具合でエディターごと落ちてしまうことがあります。
その場合、次にプロジェクトを開くと選択ダイアログが表示されます。

- **通常どおり開く** — もう一度試します
- **セーフモードで開く** — **C++スクリプトを読み込まずに**起動します。
  シーンの編集や保存はできるので、原因のスクリプトを直したり、問題のコンポーネントを外したりできます
- **開かない** — 何もせず終了します

セーフモード中は上部に警告が出て、隣の「通常モードで開き直す」でいつでも戻れます。
LamaPon Hubの「セーフモードで開く」ボタン、または`--safe`を付けた起動でも同じ状態になります。

```powershell
.\LamaPonEditor.exe --safe --project "C:\Projects\MyGame"
```

落ちた原因は`.lamapon/Crashes`に診断テキストとミニダンプとして保存されています。

### 新しいエンジンで既存プロジェクトを開く

`.lamapon/project.json`の`engineVersion`と、いま動いているエディターのバージョンを比べて、開く前に次のどれかになります。

| プロジェクト | 動き |
|---|---|
| 同じバージョン | そのまま開きます |
| **古い**／記録なし | 「更新して開きますか？」と確認します。「いいえ」なら開かずに終了 |
| **新しい** | **開きません。** そのバージョン以降のエディターで開くよう案内します |

**新しいバージョンで作られたプロジェクトを古いエディターで開くことはできません。** 開いて保存すると、新しい版が足した設定を落としたり、組み込みシェーダーを巻き戻したりして、**元のエディターでも壊れた状態**になるためです。
この場合はエンジンを更新してください。

更新に同意すると、何を更新したか（`.bak`へ退避したファイル名も含めて）ダイアログで報告します。

バージョンが読めない`project.json`（手で書き換えた場合など）は「古い」扱いになります。
ここで開けなくすると、直す手段ごと失われるためです。

プロジェクトは作成時にエンジンのシェーダー（`assets/shaders/`）をコピーして持ちます。
エンジンを新しくすると内部のデータ配置が変わることがあるため、**エディターはプロジェクトを開くときに組み込みシェーダーを現在のエンジンのものへ自動で更新します**（欠けていれば復元し、更新したことはConsoleへ表示します）。
この同期は中身を比べて行うので、バージョンが同じでも差があれば揃います。

プロジェクト側のシェーダーが現在のエンジンのものと違っていた場合は、上書き前に`<名前>.bak`として残します（編集していた内容を失わないため）。
改造内容を活かしたいときは、`.bak`と新しいファイルを見比べて必要な変更を入れ直してください。
適用済みのエンジンバージョンは`.lamapon/project.json`の`engineVersion`に記録されます。

`.bak`は**Asset Browserには表示されません**（アセットではないため、`.meta`も作らず、ゲームの書き出しにも含まれません）。
ファイル自体は`assets/`内にそのまま残るので、エクスプローラーやコードエディターから開けます。
退避が起きたときはConsoleに「元の内容は .bak へ保存しています」と警告が出るので、そこでファイル名を確認できます。
不要ならエクスプローラーで削除して構いません。

Hubを使わず、エディターを直接起動することもできます。

```powershell
.\build\LamaPonEditor.exe --project "C:\Projects\MyGame"
```

引数を省略した場合は、LamaPonリポジトリ自体を既定プロジェクトとして開きます。
既存プロジェクトには`assets`フォルダーと`.lamapon/project.json`が必要です。

GPUが無い・正しく動かない環境（仮想マシン、リモートデスクトップなど）では、`--warp`を付けるとGPUを使わずCPUラスタライザ（WARP）で起動できます。
描画は遅くなりますが、エディター操作や動作確認には十分です（エクスポートしたゲームのexeでも同じフラグが使えます）。

```powershell
.\build\LamaPonEditor.exe --warp --project "C:\Projects\MyGame"
```

## プロジェクト設定

エディターの「ファイル」→「プロジェクト設定...」から次を編集できます。

- ゲーム名（エクスポートしたexe名とウィンドウタイトルになります）
- ゲームアイコン（assets内の.png/.jpg/.ico。Export時にexeへ埋め込み）
- 初期ウィンドウ解像度
- 起動シーン
- ビューポート設定（フライ操作／オービット操作プリセット、回転・パン・ズーム感度、Y軸反転）
- グラフィック品質プリセット
- 描画方式（Forward+／Forward。[詳細](graphics.md#描画方式rendering-path)）
- 描画スケール、Shadow解像度／Cascade上限
- Bloom、Screen Space Lens Flare、FXAA、Fog、VSync、FPS上限
- テクスチャのランタイムBC圧縮（色はBC1/BC3、法線はBC5）
- Point／Spot Lightの描画上限
- GameObjectタグの一覧（InspectorのTag欄の候補になります）
- 入力Actionとキーボード／ゲームパッドBinding
- 外部スクリプトエディター（`.cpp`／`.hlsl`を開くときに使うエディター）
- Inspectorの小数点桁数

タグを登録すると、InspectorのTag欄がドロップダウン選択になり、Scene／Prefab読み込み時に未登録タグの使用をConsoleへ警告します。
一覧が空の間は従来通り検査なしで動作します。

「スクリプト」カテゴリーでは、Asset Browserで`.cpp`や`.hlsl`をダブルクリックしたときに開くエディターを選べます。
「新規C++ Script」で作成した直後に開くのも同じエディターです。
このPCのVisual Studio Code（Insidersを含む）と、vswhere経由で見つかるVisual Studio（Community／Professional／Enterprise、プレリリース版を含む）を自動検出して一覧へ並べます。
見つからない場合や別のエディターを使いたい場合は「参照...」から実行ファイルを直接指定できます。
「システムの既定（ファイルの関連付け）」を選ぶと従来通りWindowsの関連付けで開きます。
指定した実行ファイルが存在しないまま保存するとエラーになります。

「ビューポート設定」カテゴリーでは、Scene Viewのカメラ操作をプロジェクトごとに切り替えられます。
「LamaPon（従来）」は右ドラッグで視点回転、右ドラッグ中のWASDで移動する既存の操作です。
「オービット操作」ではAlt+左ドラッグで回転、中ドラッグでパン、Alt+右ドラッグでズーム、右ドラッグでフライ操作を行えます。
回転・パン・ズームの感度とY軸反転も設定できます。
この設定は`.lamapon/project.json`へ保存され、ゲームの書き出しには含まれません。

設定は`.lamapon/project.json`へ保存されます。
起動シーンは直接入力、シーン一覧、または「現在のシーン」ボタンから選択できます。
Exportすると配布用`LamaPonGame.json`へ変換され、ゲームはウィンドウ作成前にゲーム名と解像度を、シーン読込前に起動シーンを反映します。

「スクリプト」カテゴリーの「Inspectorの小数点桁数」は、InspectorがTransformの位置・回転・拡縮を何桁まで表示するかです。
既定は1桁（`0.0`表示）で、位置や角度をざっと確認するのに読みやすい桁数にしてあります。
細かく詰めたいときだけ0〜6の範囲で増やしてください。
丸めるのは表示だけなので、入力した値やシーンに保存される値は桁数に関係なくそのまま保持されます。
この設定はエディターの見た目だけに効くため、配布用`LamaPonGame.json`には含まれません。

スクリプトエディターは`scriptEditorPath`としてこのPC上の絶対パスで保存され、ゲームアイコンと同じく配布用`LamaPonGame.json`には含まれません。
`.lamapon/project.json`はGit管理対象なので、別のPCでは同じパスにエディターが無いことがあります。
その場合はプロジェクト設定から選び直してください。

プリセットの既定値は次の通りです。

| 品質 | 描画スケール | Shadow | Cascade | Bloom／Lens Flare／FXAA／Fog | テクスチャ圧縮 |
|---|---:|---:|---:|---|---|
| Low | 0.65 | 無効 | 1 | 無効 | 有効 |
| Medium | 0.85 | 1024 | 2 | 有効 | 有効 |
| High | 1.00 | 2048 | 3 | 有効 | 無効 |
| Ultra | 1.00 | 4096 | 4 | 有効 | 無効 |

Screen Space Lens FlareはHigh／Ultraで品質設定側が有効です。
実際に表示するには、シーン環境の「Screen Space Lens Flare」も有効にしてください。

### 描画スケールとギザギザ（アンチエイリアス）

描画スケールは**0.50〜2.00**で設定できます。
1.00より小さいと軽くなり、**1.00より大きいと高解像度で描いてから画面サイズへ縮小します**（スーパーサンプリング／SSAA）。
高解像度から縮小することで、輪郭のギザギザ、細い線、テクスチャの
ちらつきを抑えます。

- **2.00**では、縮小時に2×2ピクセルを平均します。
  ピクセル数が4倍になるため、描画負荷も増えます
- 1.50のような中間の値でも効果はありますが、縮小の計算がぴったり
  合わないため2.00ほど綺麗にはなりません
- プリセットはどれも1.00です。SSAAを使うには描画スケールを手で上げて
  ください（自動でCustomになります）

FXAA（プロジェクト設定の「FXAA」）は画面を後から見てエッジをぼかす軽い手法です。
ほぼタダで効きますが、細い線が消えたり文字がぼやけたりします。
描画スケールを上げられるなら、そちらの方が綺麗です。
両方同時に有効にもできます。

テクスチャ圧縮を有効にすると、PNG/JPG等の読み込み時にBCフォーマットへランタイム圧縮し、VRAM使用量をおよそ1/8〜1/4へ削減します。
切り替えは次に読み込まれるテクスチャから反映されます（DDSは元の形式のまま）。

**マップの種類ごとに圧縮の仕方が変わります。** 色をそのまま持つマップと、数値として読むマップでは、同じ潰し方をすると壊れるためです。

| マテリアルの枠 | フォーマット | 理由 |
|---|---|---|
| ベースカラー、Emissive | BC1（不透明）／BC3（アルファ付き） | 見たままの色。透過の有無で使い分けます |
| 法線マップ | BC5 | RGの2チャンネルを高精度で持ちます。BC1（RGB565）へ入れると量子化が向きの誤差になり、陰影に帯が出ます。Zはシェーダーが`sqrt(1 - x² - y²)`で復元します |
| 粗さ、金属度、遮蔽 | BC1 | 明暗だけの情報でアルファに意味が無いため |

**モデル（glTF／FBX）の中のテクスチャもここに含まれます。** 埋め込み画像も、モデルが参照する外部画像も、単体テクスチャと同じ経路（デコード→ミップ生成→BC圧縮→キャッシュ）を通ります。

> **自作Shaderの注意:** `NormalTexture`（t1）はBC5で読み込まれることがあり、その場合**Bチャンネルは0が返ります**。`.xyz`をそのまま法線として使わず、`.xy`から `z = sqrt(saturate(1 - dot(xy, xy)))` で復元してください。組み込みのLit Shaderはそうしています。

デコード・ミップ生成・圧縮の結果は`%LOCALAPPDATA%\LamaPon\texture-cache`へ自動保存され、同じテクスチャの2回目以降の読み込みはファイルを読むだけになります（シェーダーキャッシュと同じ仕組み）。
キャッシュの鍵はファイルの**中身**から作るので、テクスチャを差し替えれば自動で作り直されます。
フォルダーごと消しても、次の読み込みで作り直されるだけで害はありません。

個別項目を変更するとCustomになります。
設定は保存直後にScene View／Game Viewへ反映され、Export後のゲームでも`LamaPonGame.json`から同じ値が読み込まれます。
C++から実行中に切り替えることもできます。

```cpp
application.Graphics().ApplyQualityPreset(
    LamaPon::GraphicsQualityPreset::Medium);

auto settings = application.Graphics().Settings();
settings.vSyncEnabled = false;
settings.targetFrameRate = 144; // 0は無制限
settings.renderScale = 0.75f;
settings.preset = LamaPon::GraphicsQualityPreset::Custom;
application.Graphics().SetGraphicsSettings(settings);
```

### FPS上限とリフレッシュレート

**モニターのリフレッシュレートを超えたいときは、VSyncを切ってください。**VSyncが有効なままだと、FPS上限に360を選んでも60Hzのモニターでは60から動きません。
同じ設定・同じビルドでも「360出る人」と「60しか出ない人」がいる場合、まず両者のモニターのリフレッシュレートを比べてください。

VSyncを切ると、スワップチェーンのティアリング許可（`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`）を使ってリフレッシュレートを超えて提示します。
対応していない環境（古いWindows、リモートデスクトップ、一部の仮想GPU）では上限がリフレッシュレートのままになり、起動ログと「パフォーマンス」タブにその旨が出ます。

リフレッシュレートを超えて描くと、画面の途中で絵が切り替わる**ティアリング**が見えます。
それが困る場合はVSyncを有効に戻してください。

「パフォーマンス」タブでは、FPS、Frame ms、CPU＋Present時間、直近120フレームのグラフ、固定物理Step数・補間率・補間中Rigidbody数、Collider／Broad Phase／Contact数、描画カリング数を確認できます。
タブを閉じた場合は「ウィンドウ」→「パフォーマンス」から再表示できます。

## ゲームをエクスポート

エディターの「ファイル」→「ゲームをエクスポート...」から出力先を指定できます。
現在のシーンが保存され、プロジェクト設定で指定した起動シーンとともに次の配布物が生成されます。

```text
LamaPonGame/
├─ <ゲーム名>.exe
├─ LamaPonRuntime.dll
├─ LamaPonGameModule.dll
├─ xaudio2_9redist.dll
├─ vcruntime140.dll などVC++ランタイム
├─ LamaPonGame.json
├─ assets.tpak
└─ shader-cache/
```

`assets.tpak`（アセット一式）と`shader-cache`（事前コンパイル済みシェーダー）は
**書き出しごとに作る鍵で暗号化**され、その鍵は同梱の`LamaPonRuntime.dll`へ
焼き込まれます。改ざんも検知します。何が守れて何が守れないのかは
[書き出したゲームの保護](export-protection.md)を読んでください。

実行ファイルはプロジェクト設定のゲーム名になります（`\ / : * ? " < > |`などファイル名に使えない文字は`_`へ置換）。
ゲームアイコンを設定していればexeへ埋め込まれ、Explorerのファイルアイコンとウィンドウのタイトルバーアイコンが自分のゲームのものになります。

ダイアログの「配布用ZIPも作成」にチェックを入れると、出力フォルダーの隣にそのまま配れる`<フォルダー名>.zip`も作成されます。
WebDAVやnetwork driveでは配布フォルダーをローカル一時領域へ複製してからZIP化し、
空でないことと転送後のサイズを検証してから既存ZIPを置き換えます。

`LamaPonGame.json`には起動シーンが保存されるため、Sandboxの固定シーンに依存せず、現在編集中のシーンからゲームを開始できます。
再Exportではステージングへ全ファイルを作成してから既存パッケージを置き換えるため、コピー途中の不完全な配布物を残しません。ZIPも同じく、完成確認後にだけ置き換えます。

C++ Scriptがある場合は、Game Moduleが未作成、Scriptより古い、または現在の
`LamaPonRuntime.dll`より古い状態ではエクスポートを中止します。先にGame Moduleを
ビルドしてください。これにより、Native Scriptが動かず背景だけ表示される配布物を
成功扱いしません。万一互換性のないDLLを手作業で差し替えた場合も、ゲーム起動時に
理由をダイアログ表示して終了します。

CMakeからは次のコマンドで`build-release/export/LamaPonGame`へ出力できます。

```powershell
cmake --build build-release --target LamaPonGamePackage
```

配布版エンジンからエクスポートするとVC++ランタイムDLLが同梱されるため、渡した相手のPCに追加のインストールは不要です（ソースからビルドしたエディターで、エディターの隣にランタイムDLLが無い場合は従来どおりMicrosoft Visual C++ Redistributable 2022以降が必要です）。

エクスポートしたゲームでは**F1キー**でデバッグオーバーレイを表示できます。
FPS・フレーム時間、GameObject数と描画カリング統計、Collider数、直近の警告/エラーログが画面左上に出るため、「書き出したら動かない」ときの原因調査に使えます（もう一度F1で閉じます）。

## ディレクトリ

```text
LamaPon-Engine/
├─ .lamapon/               プロジェクト／エディター設定
├─ assets/                 シーン、Prefab、各種ゲームアセット
├─ cmake/                  依存関係の検出
├─ samples/Game/           エディターなしゲームサンプル
├─ samples/GameModule/     ゲーム固有C++ DLLサンプル
├─ samples/Sandbox/        エディター付きサンプル
├─ src/LamaPon/
│  ├─ Animation/           Animation Clipとキーフレーム補間
│  ├─ Assets/              アセット読み込みとキャッシュ
│  ├─ Audio/               DirectXTK AudioEngineとWAV／OGGキャッシュ
│  ├─ Components/          Camera、Mesh、Spriteなど
│  ├─ Core/                ウィンドウとゲームループ
│  ├─ Editor/              Dear ImGuiエディター
│  ├─ Graphics/            Direct3D 11管理
│  ├─ Input/               Action Mappingと入力状態
│  ├─ Scene/               GameObject、階層、JSON
│  └─ Scripting/           Game Module ABIとHot Reload
├─ tests/                  シーン往復テスト
└─ third_party/            DirectXTK、Dear ImGui、nlohmann/json
```

## ビルド

Visual StudioのDeveloper Command Prompt、または`vcvars64.bat`を実行したターミナルでビルドします。

CMake Presetを使う場合:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
.\build\LamaPonHub.exe
.\build\LamaPonEditor.exe --project "."
.\build\LamaPonGame.exe
```

Releaseビルド:

```powershell
cmake -S . -B build-release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

テスト:

```powershell
ctest --test-dir build --output-on-failure
```

配布用ZIP:

```powershell
cmake --preset windows-release
cmake --build --preset windows-package
```

ZIPは`out/build/windows-release`以下へ生成されます。
ZIPにはゲーム制作へ必要なものだけが入り、サンプルゲームやテストは含まれません（PDBはリリース時に別の`LamaPon-symbols`ZIPとして配布されます）。
ルートの`build-info.json`にはエンジン版とGitビルド識別子が含まれます。
クラッシュ診断にも同じ識別子が記録され、実行ファイル横、またはプロジェクトの`.lamapon/Crashes`へ保存されます。

Visual Studioでは「ローカル フォルダーを開く」で、このフォルダーをCMakeプロジェクトとして扱えます。

## リリース（配布パッケージの公開）

開発版は `0.1.0` から開始します。バージョンは `MAJOR.MINOR.PATCH` 形式です。
`v0.1.0` のようなタグをプッシュすると、GitHub Actions がビルド・全テスト・パッケージングを行い、[LamaPon-Engine の Releases](https://github.com/Timiratz/LamaPon-Engine/releases) に配布物を添付します。

リポジトリがプライベートの間、ソースとリリースの閲覧にはアクセス権が必要です。
リリース作成には標準の `GITHUB_TOKEN` を使います。別リポジトリへの転送や追加の配布用トークンは不要です。

- `LamaPon-<version>-windows-x64.zip` — エンジン一式
- `LamaPon-symbols-windows-x64.zip` — クラッシュ解析用 PDB

手順:

1. `CHANGELOG.md` の `Unreleased` 節を新しいバージョン節（例: `## 0.1.0`）へ確定する
2. `CMakeLists.txt` の `project(LamaPon VERSION 0.1.0 ...)` とサンプルのエンジン版を更新する
3. ローカルでビルド・全テストを確認してコミットする
4. バージョンタグを作成してプッシュする

```bash
git tag v0.1.0
git push origin v0.1.0
```

リリースノートは `CHANGELOG.md` の該当バージョン節から生成されます。
