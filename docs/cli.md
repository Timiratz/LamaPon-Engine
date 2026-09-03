# コマンドラインツール（LamaPonCli）

エディターを開かずにプロジェクトを操作するコマンドラインツールです。
シーンの構造、ログ、生成画像を使って、変更結果を確認できます。

[← ドキュメント一覧へ戻る](index.md)

## 1分でためす

プロジェクトの作成から書き出しまで、エディターを開かずに一通りできます。

```bat
LamaPonCli.exe new --dir "C:\path\to\MyProject" --name MyGame
LamaPonCli.exe learn status --project "C:\path\to\MyProject"
LamaPonCli.exe learn doctor --project "C:\path\to\MyProject"
LamaPonCli.exe render --project "C:\path\to\MyProject" --out shot.png
LamaPonCli.exe inspect --project "C:\path\to\MyProject" --scene scenes/main.scene.json
LamaPonCli.exe validate --project "C:\path\to\MyProject" --scene scenes/main.scene.json
LamaPonCli.exe project inspect --project "C:\path\to\MyProject"
LamaPonCli.exe asset list --project "C:\path\to\MyProject" --importer CppScript
LamaPonCli.exe script list --project "C:\path\to\MyProject"
LamaPonCli.exe script create --project "C:\path\to\MyProject" --path scripts/Player.cpp --class PlayerController
LamaPonCli.exe component list
LamaPonCli.exe component list --category UI
LamaPonCli.exe component schema --type UIButton
LamaPonCli.exe prefab validate --project "C:\path\to\MyProject" --path prefabs/player.prefab.json
LamaPonCli.exe patch --project "C:\path\to\MyProject" --scene scenes/main.scene.json --operations patch.json --dry-run
LamaPonCli.exe test --project "C:\path\to\MyProject" --scene scenes/main.scene.json --spec scene.tests.json
LamaPonCli.exe build --project "C:\path\to\MyProject"
LamaPonCli.exe export --project "C:\path\to\MyProject" --zip
```

- `new` は３D学習を既定で作ります（`--template 3d`／`2d`／`learning-3d`／`learning-2d`）
- `learn` は学習進捗、役職、教材診断をJSONで扱います
- `render` は `shot.png` にスクリーンショットを書き出します
- `build` はエディターの「保存→自動ビルド」と同じ方法でC++ Game Moduleを建てます
- `export` はエディターの「エクスポート」と同じ配布用パッケージを作ります
- どのコマンドも標準出力にJSONのレポートが1つ出ます（下記）
- 終了コードは 0=成功 / 1=失敗 です

共有フォルダーやWebDAV上のプロジェクトを`build`すると、CMakeの中間生成物は
自動的に`%LOCALAPPDATA%\LamaPon\BuildCache`へ置かれます。リンクに
成功したDLLと最終ログだけをプロジェクトへ戻すため、共有先へ細かなファイルを
大量に書き込みません。JSONの`usesLocalBuildCache`と`buildDirectory`で
実際の保存先を確認できます。

`new`はLamaPonのソースツリー内を通常の保存先として受け付けません。
ゲームはエンジンリポジトリと同じ階層など、独立したフォルダーへ作成します。
エンジン自身のサンプルを意図的に生成する場合だけ`--allow-inside-engine`を
指定できます。

Hubの「最近使ったプロジェクト」はCLIからも管理できます。

```bat
LamaPonCli.exe project list
LamaPonCli.exe project add --project "C:\path\to\MyProject"
LamaPonCli.exe project remove --project "C:\path\to\MyProject"
LamaPonCli.exe project move --project "C:\path\to\MyProject" --to "D:\Games\MyProject"
```

`project move`は移動先へ一時コピーし、項目数・総バイト数・Project構造を
検証してから元フォルダーを削除します。検証に失敗した場合は元を残します。

## 学習進捗と環境診断

学習テンプレートの進捗と教材をCLIから操作できます。

```bat
LamaPonCli.exe learn status --project "C:\path\to\MyProject"
LamaPonCli.exe learn complete --project "C:\path\to\MyProject"
LamaPonCli.exe learn complete --project "C:\path\to\MyProject" --step understand-loop
LamaPonCli.exe learn role --project "C:\path\to\MyProject" --role engineer
LamaPonCli.exe learn doctor --project "C:\path\to\MyProject"
```

- `status`は全ステップ、完了率、残り目安時間、次の具体的な操作を返します。
- `complete`は`--step`を省略すると次のステップを1つ完了します。同じIDを再度完了しても重複しません。
- `role`は`undecided`、`engineer`、`planner`、`designer`のいずれかです。
- `doctor`は教材JSON、ガイド、参照ファイルを検証します。C++ DLLが未ビルド／古い場合は必須失敗にはせず、次の`build`コマンドを案内します。

既存の空テンプレートや自作プロジェクトへ、Sceneを上書きせず教材を追加できます。

```bat
LamaPonCli.exe learn init --project "C:\path\to\ExistingGame"
```

`LEARNING.md`、`learning/`、練習用`LearningPlayer.cpp`が作られます。既に同名の教材がある場合は上書きしません。個人の完了状態だけを最初からやり直す場合は次を使います。

```bat
LamaPonCli.exe learn reset --project "C:\path\to\MyProject"
```

共有する`learning/journey.json`は変更せず、Git除外された`.lamapon/learning-progress.json`だけを削除します。

長い処理を作業の流れから切り離したいときは、`job`を使えます。
`build`／`render`／`export`／`inspect`／`validate`／`patch`／`test`を専用ワーカープロセスで実行し、結果を`<project>/.lamapon/jobs/<jobId>/`へ保存します。

```bat
LamaPonCli.exe job start build --project "C:\path\to\MyProject"
LamaPonCli.exe job status --project "C:\path\to\MyProject" --id job-1234-5678-1
LamaPonCli.exe job cancel --project "C:\path\to\MyProject" --id job-1234-5678-1
LamaPonCli.exe job list --project "C:\path\to\MyProject"
```

`job start`はすぐに`jobId`を返します。
`job status`の`job.status`は`queued`／`running`／`succeeded`／`failed`／`cancelled`のいずれかです。
完了時の元コマンドのJSONは`job.result`に入り、実行中の標準エラー末尾は`job.logTail`、完全なログは`job.progressLogPath`で確認できます。
`job cancel`はワーカーと、そのワーカーが起動したコマンドプロセス群をまとめて停止します。

## エディター不要の常駐Runtimeセッション

ゲームを別プロセスで常駐実行し、状態を読みながら一時停止・1フレーム送り・入力・時間倍率・スクリーンショットを送れます。
セッションのファイルは`<project>/.lamapon/runtime/<sessionId>/`に保存されます。

```bat
LamaPonCli.exe runtime start --project "C:\path\to\MyProject" --warp --fps 60
LamaPonCli.exe runtime status --project "C:\path\to\MyProject" --id <sessionId>
LamaPonCli.exe runtime send --project "C:\path\to\MyProject" --id <sessionId> --command pause
LamaPonCli.exe runtime send --project "C:\path\to\MyProject" --id <sessionId> --command step
LamaPonCli.exe runtime send --project "C:\path\to\MyProject" --id <sessionId> --command "{op:input,action:Jump,value:1,frames:1}"
LamaPonCli.exe runtime send --project "C:\path\to\MyProject" --id <sessionId> --command "{op:timescale,value:0.5}"
LamaPonCli.exe runtime send --project "C:\path\to\MyProject" --id <sessionId> --command "{op:screenshot,path:shots/frame.png}"
LamaPonCli.exe runtime stop --project "C:\path\to\MyProject" --id <sessionId>
```

`runtime start`はすぐに`sessionId`を返します。
`runtime status`の`session.status`は`queued`／`running`／`paused`／`stopped`／`failed`です。
状態JSONの`runtime`にはGameObject・Transform・Component・ゲーム状態・入力・物理・可視性・Profiler・ログが入り、`lastCommandOk`と`lastCommandError`で直前の操作結果を確認できます。
決定論モードでも`runtime.time.deltaTime`は固定されたゲーム内時間、
`runtime.frame.fps`／`frameTimeMilliseconds`は実際の描画間隔です。重い処理を
固定60 FPSと誤表示しません。命令は1件ずつ処理されるため、手動の`send`で
前の命令が未完了なら上書きせず明示的なエラーを返します。

`runtime.screen`には実行中の画面サイズ（`width`／`height`）が入ります。
**2Dは1ワールド単位＝1画素**なので、位置のアサーションはこれを見て
書いてください。解像度を決め打ちすると、ゲームが正しくてもテストだけが
落ちます。

PowerShellで引用符が取り除かれる環境でも、`pause`／`resume`／`step`／`observe`／`stop`や`{op:pause}`形式をそのまま使えます。
複雑な値や完全なJSONを送りたい場合は、JSONファイルを作り、`--command-file <file>`を使ってください。

## Runtime自動プレイテストとリプレイ

`runtime test`はゲームをワーカープロセスで起動し、フレームを進めながら操作とJSONアサーションを実行します。
`deterministic`／`fixedDeltaTime`を指定すると、同じ入力を同じフレームに適用できます。
`record`を指定すると、実行した操作をリプレイファイルへ保存できます。
ロジック中心の長いテストでは`paceFrames: false`で実時間待機を外し、
`renderEveryNFrames`で描画頻度を下げられます。これは`deterministic: true`の
ときだけ有効です。`screenshot`と`observe`は間引き中でも必ず1回描画し、
保存・観測が完了してから次のstepへ進みます。

```json
{
  "scene": "scenes/Main.scene.json",
  "fps": 20,
  "deterministic": true,
  "fixedDeltaTime": 0.05,
  "paceFrames": false,
  "renderEveryNFrames": 10,
  "warp": true,
  "record": "final-replay.json",
  "timeoutMs": 8000,
  "steps": [
    { "command": "pause", "waitFrames": 1 },
    { "command": { "op": "set-transform", "name": "Player", "position": [1, 2, 0] }, "waitFrames": 1 },
    { "command": { "op": "query", "name": "Player" }, "waitFrames": 1,
      "assert": [{ "path": "lastQuery.position[0]", "equals": 1 }] }
  ],
  "assert": [
    { "path": "runtime.objectCount", "gte": 1 }
  ]
}
```

```bat
LamaPonCli.exe runtime test --project "C:\path\to\MyProject" --spec playtest.json
LamaPonCli.exe runtime replay --project "C:\path\to\MyProject" --file final-replay.json
LamaPonCli.exe runtime recover --project "C:\path\to\MyProject" --id <oldSessionId>
```

テストの`steps`では`pause`／`resume`／`step`／`input`／`timescale`／`screenshot`に加えて、`query`、`set-transform`、`set-state`、`reload`、`reload-module`を使えます。
アサーションは`exists`、`equals`、`notEquals`、`gt`、`gte`、`lt`、`lte`、`contains`に対応し、`objects[ name=Player ]`のような配列選択もできます。
`runtime.gameState.player.hp`のように**キー自体がドットを含む**場合も、分割探索で見つからなければ残りのパス全体を1個のキーとして引き直すため、そのまま書けます。
ワーカーが異常終了した場合は状態が`failed`になり、ログとエラー本文を確認してから`recover`でセッションを再起動できます。

## Sceneのinspect / validate

Sceneの構造を直接把握できるよう、描画なしで確認できます。

```bat
LamaPonCli.exe inspect --project "C:\path\to\MyProject" --scene scenes/main.scene.json
LamaPonCli.exe validate --project "C:\path\to\MyProject" --scene scenes/main.scene.json
```

`inspect`はScene JSON本体、GameObject、Transform、Component、アセット参照を返します。
`validate`は親子関係、重複ID、Main Camera、Component形式、存在しないアセット参照を`problems`へ構造化して返します。
問題が無ければ終了コードは0、問題があれば1です。

## Project / Asset / Scriptのinspect

さらに、エディターを開かずにプロジェクト設定とAsset Databaseを読めます。
`asset list`と`script list`は読み取り専用で、`.meta`を新規作成しません。

```bat
LamaPonCli.exe project inspect --project "C:\path\to\MyProject"
LamaPonCli.exe asset list --project "C:\path\to\MyProject"
LamaPonCli.exe asset list --project "C:\path\to\MyProject" --importer CppScript
LamaPonCli.exe asset inspect --project "C:\path\to\MyProject" --path scenes/main.scene.json
LamaPonCli.exe asset inspect --project "C:\path\to\MyProject" --guid <guid>
LamaPonCli.exe script list --project "C:\path\to\MyProject"
LamaPonCli.exe script inspect --project "C:\path\to\MyProject" --path scripts/Player.cpp
```

`asset list`の各項目にはGUID、Importer、依存Asset、被依存Asset、`.meta`の内容が入り、「このAssetを変更すると何が影響を受けるか」を確認できます。
`project inspect`はProject SettingsのJSONと起動Scene、Asset種別ごとの件数を返します。
`script inspect`はソース、バイト数、行数、GUIDを返します。

**`script list`と`script inspect`は`scriptTypes`も返します。** これは
シーンJSONの`NativeScript`へ書く登録名です。`LAMAPON_SCRIPT(Player);`と
書いたソースなら`{ "class": "Player", "id": "Game.Player" }`が入るので、
`patch`の`add-component`へは`id`をそのまま渡せます。クラス名だけを
書くと`build`も`patch`も成功したまま何も動かないため、名前は
推測せずここから取ってください（ソースを読むだけなので`build`前でも
答えられます）。

Assetの取り込みとScript雛形の生成もCLIから実行できます。

```bat
LamaPonCli.exe asset import --project "C:\path\to\MyProject" --source "C:\assets\enemy.fbx" --target models
LamaPonCli.exe script create --project "C:\path\to\MyProject" --path scripts/Player.cpp --class PlayerController
```

`asset import`は既存ファイルを上書きせず、必要なら自動で連番名を付けます。
`script create`は`LamaPon::Script`継承、`Start`、`Update`、`LAMAPON_SCRIPT`登録まで含む最小雛形を作ります。
既存Scriptを上書きする場合だけ`--force`が必要です。

## Component schema（UI / Physics / Animation）

組み込みComponentを安全に編集できるよう、型・初期値・編集可能なフィールドをJSONで取得できます。

```bat
LamaPonCli.exe component list
LamaPonCli.exe component list --category UI
LamaPonCli.exe component schema --type UIButton
LamaPonCli.exe component schema --type Rigidbody
```

`patch`と`prefab patch`の`add-component`／`set-component`は、schemaがあるComponentのフィールド型とenum値を検証します。
未知のComponent型や未知のフィールドは、プロジェクト固有の拡張を壊さないよう許可されます。

## Prefabのinspect / validate / patch

PrefabもSceneと同じJSON操作の対象です。
`validate`はRoot、親子関係、Nested Prefab、Component形式、Asset参照を確認します。

```bat
LamaPonCli.exe prefab inspect --project "C:\path\to\MyProject" --path prefabs/player.prefab.json
LamaPonCli.exe prefab validate --project "C:\path\to\MyProject" --path prefabs/player.prefab.json
LamaPonCli.exe prefab patch --project "C:\path\to\MyProject" --path prefabs/player.prefab.json --operations prefab.patch.json --dry-run
```

`prefab patch`ではSceneの`rename`、`set`、`reparent`、`add-object`、`remove-object`、Component操作をそのまま利用できます。
書き込み時はPrefabの隣にバックアップを残します。

## Sceneのpatch

Sceneを直接変更するための意味単位のJSON操作です。
`--dry-run`ではファイルを変更せず、変更後の検証結果だけを返します。
書き込み時は元ファイルの隣に`.bak-<番号>`バックアップを作成します。

```json
{
  "operations": [
    { "op": "rename", "target": { "id": 3 }, "name": "Player" },
    { "op": "set", "target": { "name": "Player" }, "path": "transform.position", "value": [0, 1, 0] },
    { "op": "add-component", "target": { "name": "Player" }, "type": "Rigidbody" },
    { "op": "add-object", "name": "SpawnPoint", "parent": { "id": 1 } }
  ]
}
```

```bat
LamaPonCli.exe patch --project "C:\path\to\MyProject" --scene scenes/main.scene.json --operations patch.json --dry-run
LamaPonCli.exe patch --project "C:\path\to\MyProject" --scene scenes/main.scene.json --operations patch.json
```

利用できる操作は `set`、`set-scene`、`rename`、`reparent`、`add-object`、`remove-object`、`add-component`、`remove-component`、`set-component`です。
対象は `{ "id": 3 }` または一意な `{ "name": "Player" }`で指定できます。
`--out`を指定すると、プロジェクト内の別ファイルへ出力できます。

**C++ Scriptを付けるときは、名前に`Game.`を付けます。**`assets/scripts/Player.cpp`に`LAMAPON_SCRIPT(Player);`と書いた場合、シーンJSONへ書く名前は**`Game.Player`**です（`LAMAPON_SCRIPT`が`"Game." + クラス名`で登録するため）。

```json
{ "op": "add-component", "target": { "name": "Player" }, "type": "NativeScript",
  "data": { "script": "Game.Player", "properties": {} } }
```

`Game.`を付け忘れても`patch`は成功します。
**気付けるのは`render`の`problems`**で、`Game Moduleに型が登録されていません: Player`と出ます（画面には何も出ません）。
付ける前に`build`を済ませておいてください。

## Sceneのtest

`test`は、Sceneを起動せずに機械可読なアサーションを実行します。
`patch`の後に必ず実行するチェックとして利用できます。

```json
{
  "tests": [
    { "name": "scene-is-valid", "kind": "scene-valid" },
    { "name": "player-exists", "kind": "object-exists", "target": { "name": "Player" } },
    { "name": "player-has-body", "kind": "component-exists", "target": { "name": "Player" }, "type": "Rigidbody" },
    { "name": "spawn-count", "kind": "object-count", "min": 3 },
    { "name": "player-position", "kind": "value-equals", "target": { "name": "Player" }, "path": "transform.position", "value": [0, 1, 0] }
  ]
}
```

利用できる種類は `scene-valid`、`object-exists`、`component-exists`、`object-count`、`asset-exists`、`value-equals`です。
全アサーション成功時は終了コード0、失敗時は1です。
`--report`を指定すると同じレポートをプロジェクト内にも保存します。

## 出力の約束（機械可読の契約）

- **標準出力にはJSONオブジェクトを1つだけ**書きます。進行状況などの
  人向けの文は標準エラーに出ます。
  パイプやリダイレクトで安全に拾えます
- 失敗してもJSONは出ます（`ok: false` と `error` に理由）
- キーは**足すことはあっても消さない**方針です

```json
{
  "ok": true,
  "command": "render",
  "project": "C:/path/to/MyProject",
  "scene": "scenes/main.scene.json",
  "frames": 4,
  "simulatedSeconds": 0.0,
  "image": {
    "path": "C:/path/to/shot.png",
    "width": 1280,
    "height": 720,
    "meanColor": [42.5, 51.0, 63.2],
    "uniqueColors": 5183,
    "magentaPixels": 0
  },
  "shaderFallbackDraws": 0,
  "errorCount": 0,
  "warningCount": 1,
  "logs": [
    { "level": "Warning", "message": "..." }
  ]
}
```

| キー | 意味 |
|---|---|
| `image.meanColor` | RGBそれぞれの平均（0〜255）。真っ黒／真っ白の検出に |
| `image.uniqueColors` | 異なる色の数。**1なら一色だけ＝ほぼ確実に何も描画されていない** |
| `image.magentaPixels` | 壊れたShaderの代役（マゼンタ）とみなした画素数。**色から推測しているだけ**なので、本当にピンクや水色を使う絵でも増えます（実測: 正常なシーンで画面の7.6%）。壊れているかの判定には使わないでください |
| `shaderFallbackDraws` | 代役シェーダーが実際に使われた回数。エンジン自身が数えた**事実**で、推測ではありません。**0なら代役は一度も使われていない**と言い切れます。撮った1フレーム分だけを数えます |
| `problems` | 壊れているものの構造化一覧（下記）。**この配列が空になるまで確認・修正します** |
| `logs` | エンジンの警告とエラー（infoは含めない）。`--d3ddebug` 時はD3D11デバッグレイヤーの警告もここに入る |

### problemsの中身

`magentaPixels` が「どこかが壊れた」までなのに対し、`problems` は**どのオブジェクトの・何が・どう壊れたか**まで特定します。

```json
"problems": [
  {
    "object": "Cube",
    "component": "MeshRenderer",
    "kind": "shader-compile-error",
    "shader": "shaders/broken.hlsl",
    "detail": "broken.hlsl(4,27): error X3004: undeclared identifier ..."
  }
]
```

| `kind` | 意味 | 追加キー |
|---|---|---|
| `shader-compile-error` | カスタムShaderのコンパイル失敗（MeshRenderer / ModelRenderer / SpriteRenderer / ParticleSystem） | `shader` |
| `script-error` | C++ Scriptの解決失敗 | `script` |
| `collider-error` | Mesh Colliderの構築失敗 | `model` |

Shaderのコンパイルは最初の描画時に走るため、**描かれなかったもの**（無効化されている、カメラに映っていない等）のエラーは載りません。

## renderのオプション

| オプション | 意味 |
|---|---|
| `--project <dir>` | プロジェクトフォルダー（必須。`.lamapon/project.json` があること） |
| `--scene <path>` | 撮るシーン。省略時はプロジェクトの起動シーン。`assets/` からの相対・絶対どちらでも可 |
| `--out <file.png>` | 出力画像（既定: `render.png`） |
| `--width <n>` `--height <n>` | 解像度の上書き（既定: プロジェクト設定） |
| `--frames <n>` | 撮影前に描くフレーム数（既定: 4）。TAAやSSRのような「前のフレームを材料にする」効果を効かせるためのウォームアップで、撮るのは最後の1枚 |
| `--simulate <秒>` | ゲーム時間を進めてから撮る（既定: 0）。物理で物を落ち着かせたいときなどに。1/60秒刻みで進める。**C++ Scriptがあるシーンでは、省略しても`Start()`のために1フレームだけ進めます**（Scriptが画面を作るゲームが真っ黒に写らないように） |
| `--input <イベント>` | 進めている間に入力Actionを押す。`Action[=向き]@秒[:押している秒数]`をカンマ区切りで指定（押している秒数の既定は0.1秒）。例: `--input "Jump@0.5:0.2,Fire@1.0"`。`--simulate`を省略した場合は「押し終わり＋0.25秒」まで自動で進め、明示した場合はその秒数を尊重します（押した直後の1コマを撮れます） |

**軸のActionを逆向きへ倒すには`=-1`を付けます。** `MoveHorizontal`は
AとDが1つのActionにまとまっているため、既定（正方向）だけでは
**右にしか動かせません**。`--input "MoveHorizontal=-1@2.0:1.0"`と書くと
負の倍率を持つBinding（A）を押します。JSONの`inputEvents`には
どちらを押したかが`value`として載ります。
| `--warp` | GPUを使わずCPU（WARP）で描く |
| `--d3ddebug` | D3D11デバッグレイヤーを有効にする。不正な描画状態が `logs` に載る |

## newのオプション

LamaPon Hubの「新規作成」と同じ処理です。
既定では遊べるScene、学習教材、コメント付きC++を含む３D学習ができ、Hubの「最近使ったプロジェクト」にも載ります。

| オプション | 意味 |
|---|---|
| `--dir <dir>` | 作成先フォルダー（必須。空であること） |
| `--name <name>` | ゲーム名（既定: フォルダー名） |
| `--template <t>` | `3d`、`2d`、`learning-3d`、`learning-2d`（既定: `learning-3d`）。 |

JSONの `startupScene` に起動シーンのパスが入るので、続けて `render` へ渡せます。

## buildのオプション

エディターの「保存→自動ビルド」と同じコマンド（CMake＋NMake、`assets/` 配下の `.cpp` を全部拾う）を同期で実行します。
成果物は `<プロジェクト>/.lamapon/bin/LamaPonGameModule.dll` です。

| オプション | 意味 |
|---|---|
| `--project <dir>` | プロジェクトフォルダー（必須） |
| `--config <c>` | `Release` または `Debug`（既定: このツール自身と同じ構成） |

- JSONの `buildErrors` に **コンパイラ／CMakeのerror診断行だけ**が抜き出されます
  （`error C2065` や`CMake Error`など）。`winerror.h`のようにファイル名へ
  `error`を含むだけの行や`0 Error(s)`は含みません。
  ログ全文を漁らずにこの配列だけ読めばよく、失敗時は `logTail`（ログ末尾）も付きます
- 終了コードが0でも**DLLが実在しなければ失敗扱い**にします
- `runtimeCompatible` はGame Moduleが現在の`LamaPonRuntime.dll`以降に
  再リンク済みかを示します。`false`なら終了コード0でもbuildは失敗扱いです
- `moduleUpdated` は今回のビルドでDLLが実際に書き換わったか。
  **失敗時に前回の古いDLLが残っている**場合は `moduleExists: true` でも`moduleUpdated: false` になります（変更なしのup-to-dateビルドでもfalseですが、その場合は前回の成果物のままで正しい）
- Visual StudioのC++ツールセットが必要です（エディターの
  自動ビルドと同じ前提）

## exportのオプション

エディターの「エクスポート」と同じ梱包処理です。
exe・`LamaPonRuntime.dll`・`assets.tpak`・設定を出力フォルダーへまとめます。

| オプション | 意味 |
|---|---|
| `--project <dir>` | プロジェクトフォルダー（必須） |
| `--out <dir>` | 出力先（既定: `<プロジェクト>/export`）。既存の中身はパッケージ完成後に置き換える |
| `--zip` | 配布用ZIPも作る（出力フォルダーの隣） |

- Game Module（C++ Script）は `.lamapon/bin/LamaPonGameModule.dll` から
  同梱します。C++ソースがあるのにDLLが無い、ソースより古い、または現在の
  Runtimeより古い場合は、背景だけの壊れた配布を防ぐためエクスポートが失敗します。
  C++ソースが無いプロジェクトだけは、従来どおりGame Moduleなしで書き出せます
- エディターと違い、**開いているシーンの保存は行いません**。
  ディスクにある内容がそのまま入ります
- WebDAV／network driveへの`--zip`はローカル一時領域で作成し、非空と
  転送サイズを検証してから既存ZIPを置き換えます

## エディターUIのスクリーンショット（LamaPonEditor側）

CLIとは別に、**エディター自体**にもスクリーンショットモードがあります。
実物のエディターを数フレーム起動し、指定のUIを開いた状態でPNGへ撮って自動終了します。
エディターのUI変更（Inspector・設定画面）が本当に画面へ出ているかを、ゲーム制作者自身が目視確認するための入口です。

```bat
LamaPonEditor.exe --project "C:\path\to\MyProject" --screenshot ui.png --report ui.json --show project-settings:physics
```

| オプション | 意味 |
|---|---|
| `--screenshot <png>` | これを渡すとスクリーンショットモードになる（撮影後に自動終了） |
| `--report <json>` | 実行結果のJSON。エディターはコンソールを持たないのでファイルへ書く |
| `--show <対象>` | 撮る前に開いておくUI（下記）。省略時は既定レイアウトのまま |
| `--shot-frames <n>` | 撮影するフレーム番号（既定: 12。UIが落ち着くまで数フレーム待つ） |

`--show` に指定できるもの:

| 値 | 開くUI |
|---|---|
| `project-settings:<区分>` | プロジェクト設定ダイアログ。区分は `game` / `graphics` / `viewport` / `physics` / `tags` / `input` / `scripts`（日本語名も可） |
| `inspector:<GameObject名>` | そのGameObjectを選択した状態（Inspectorに中身が出る） |

末尾に `:bottom` を付けると、対象を**末尾までスクロールした状態**で撮ります（例: `project-settings:physics:bottom` で衝突マトリクスが、`inspector:Player:bottom` で下の方のコンポーネントが写ります）。

### リモート操作モード（--remote）

1枚撮って終わりではなく、**エディターを開いたまま外から操作**できます。
「撮る → 画像で座標を読む → クリックを送る → また撮る」のループで、実物のエディターをリアルタイムに操作するための仕組みです。

```bat
LamaPonEditor.exe --project "C:\path\to\MyProject" --remote C:\work\remote
```

> **このモードは本物のマウスカーソルを動かしません。** 注入した座標を
> エディター側で保持し、毎フレームImGuiへ入れ直しています（ImGuiの
> Win32バックエンドが毎フレーム報告する実カーソル位置を、後ろから
> 上書きする形）。操作中もそのPCで他の作業ができます。
> ただし**エディターのウィンドウ内はAIが握っている**ので、人がその
> ウィンドウをクリックしても意図した場所には当たりません。終わらせる
> ときは `{"type":"quit"}` を送ってください。

指定フォルダーの `command.json` を毎フレーム監視します。

```json
{ "seq": 1, "commands": [
  { "type": "click", "x": 640, "y": 360, "button": 0 },
  { "type": "move", "x": 100, "y": 50 },
  { "type": "wheel", "x": 640, "y": 360, "deltaY": -5 },
  { "type": "text", "value": "Hello" },
  { "type": "key", "value": "enter" },
  { "type": "screenshot" },
  { "type": "quit" }
] }
```

- `seq` は連番。同じ値は再実行されません（ファイルを書き換えて
  seqを進めるたびに1回実行）
- 座標はウィンドウのクライアント座標（＝スクリーンショットのピクセル）
- `screenshot` は `screenshot-<seq>.png` へ書きます
- 実行結果は `state.json`（`{"seq":1,"ok":true}`）で確認できます
- **クリックの効果は次のフレームに出る**ので、クリックと確認の
  スクリーンショットは別のseqに分けてください
- `key` は enter / tab / escape / backspace / delete / space /
  up / down / left / right

座標を使わない操作もできます。

```json
{ "seq": 2, "commands": [ { "type": "dump" } ] }
{ "seq": 3, "commands": [ { "type": "click-label", "label": "プロジェクト設定...", "window": "ファイル" } ] }
```

- `dump` は**いま画面に見えているウィジェットの一覧**
  （ウィンドウ名・ラベル・矩形・状態フラグ）を `state.json` の`items` へ返します。
  まずdumpして、そこにあるラベルへ`click-label` するのが最も確実です（レイアウト変更に強い）
- `click-label` は完全一致を優先し、無ければ部分一致。`window` で
  ウィンドウ名を絞れます（省略可）

値の設定とドラッグもできます。

```json
{ "seq": 4, "commands": [ { "type": "set-value", "label": "速さのしきい値", "value": "0.5" } ] }
{ "seq": 5, "commands": [ { "type": "set-value", "label": "超えたら頭打ちにする", "value": true } ] }
{ "seq": 6, "commands": [ { "type": "drag", "x": 500, "y": 300, "toX": 620, "toY": 300, "frames": 12 } ] }
```

- `set-value` は文字列・数値ならCtrl+クリック→全選択→入力→Enterを
  自動で行います（Drag/Slider/InputTextで使えます）。
  真偽値ならチェックボックスの現在値と違うときだけクリックします
- `set-value` と `drag` は**複数フレームかけて実行**されます。
  `state.json` に結果が出るまで待ってから次のコマンドを送ってください（完了まで新しいコマンドは受け付けません）

### 実行中ゲームの観測・操作

同じリモート操作口から、エディターの再生中ゲームも機械的に操作できます。
これで「入力を送る → 1フレーム進める → 実行結果を読む」のループを作れます。

```json
{ "seq": 10, "commands": [ { "type": "play" } ] }
{ "seq": 11, "commands": [ { "type": "input", "action": "Jump", "value": 1, "frames": 1 } ] }
{ "seq": 12, "commands": [ { "type": "pause" }, { "type": "runtime" } ] }
{ "seq": 13, "commands": [ { "type": "step" }, { "type": "runtime" } ] }
{ "seq": 14, "commands": [ { "type": "resume" } ] }
```

| コマンド | 役割 |
|---|---|
| `play` / `pause` / `resume` | 再生・一時停止・再開 |
| `step` | 一時停止中にゲーム更新を1フレームだけ進める |
| `runtime`（`observe`も同じ） | `state.json`へ実行状態を出す |
| `input` | `action`または`control`へ値を送り、`frames`フレーム保持する |
| `timescale` | `{"type":"timescale","value":0.25}` のように時間倍率を変更する |

`runtime`の結果は`state.runtime`に入り、GameObjectのID・名前・親・有効状態・Transform・Component一覧、`SceneManager::State()`の`gameState`、入力Action、時間、物理／可視性統計、CPUプロファイラ直近フレーム、Warning/Errorログを含みます。
`input`の`action`はプロジェクト設定のAction名、`control`は`KeyboardSpace`などの`InputControlName`です。
入力は指定フレームの後に自動的に離されます。

- 無人実行を想定して、クラッシュ復旧のダイアログは出ません
- 対象が見つからない場合も撮影は行い、警告をログに残します

## ゲーム制作で使う

シーンを編集したら、次の1コマンドで「本当に直ったか」を確かめられます。

```bat
LamaPonCli.exe render --project . --scene scenes/main.scene.json --out check.png
```

`new` → シーンJSONを編集 → `render` で確認 → `export`、と全工程がエディター無しで回ります。
まずは小さなシーンを作り、結果を確かめながら進めてください。

- `render`が出力した画像を見て、意図した見た目になっているか確認する
- 画像を見なくても、`meanColor` / `uniqueColors` /
  `shaderFallbackDraws` / `problems` / `logs` から「一色しか出ていない」「Shaderが壊れた」「アセットが見つからない」を判定できます

## よくあるつまずき

- **`ok: false` で「not a LamaPon project」** — `--project` は
  `.lamapon/project.json` がある**プロジェクトの根元**を指します。
  `assets/` やシーンファイルを直接指してはいけません
- **画像が一色だけ（`uniqueColors: 1`）** — シーンにメインカメラが
  無いか、カメラが何も映していません。
  `logs` に警告が出ます
- **C++ Scriptが動いていない** — renderはGame Moduleを読み込みません。
  シーンに置いてあるものをそのまま描きます（`--simulate` も物理とアニメーションだけ進めます）。
  Scriptが生成するものを撮りたい場合は、今のところ実行中のゲームからは撮れません
- **スクリーンショットが実行画面と微妙に違う** — 垂直同期は切って
  撮ります。
  それ以外の品質設定はプロジェクト設定のままです
