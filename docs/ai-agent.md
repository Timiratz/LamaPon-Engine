# ゲームを作る（操作ガイド）

LamaPonの制作工程ごとの手順をまとめています。
シーンの編集、ビルド、実行、画面の確認までを順番に進められます。
各機能の詳しい説明はそれぞれのページ（[CLI](cli.md)・[物理](physics.md)など）にあります。

[← ドキュメント一覧へ戻る](index.md)

## 工程別の使い方

| 工程 | 手段 | 確認方法 |
|---|---|---|
| プロジェクト作成 | `LamaPonCli new` | JSONの `ok` |
| シーン編集 | `assets/scenes/*.scene.json` を直接編集 | `LamaPonCli render` |
| C++ Script | `assets/*.cpp` を書く → `LamaPonCli build` | `buildErrors`（空なら成功） |
| Sceneを読む | `LamaPonCli inspect` | GameObject／Component／AssetのJSON |
| Sceneを検証 | `LamaPonCli validate` | `problems`が空になるまで修正 |
| Sceneを変更 | `LamaPonCli patch` | `--dry-run` → 検証 → 書き込み |
| Sceneをテスト | `LamaPonCli test` | JSONアサーションが全て成功 |
| Projectを読む | `LamaPonCli project inspect` | 設定・起動Scene・Asset集計 |
| Assetを調べる | `LamaPonCli asset list/inspect` | GUID・Importer・依存関係 |
| Scriptを読む | `LamaPonCli script list/inspect` | ソース・行数・GUID |
| Assetを取り込む | `LamaPonCli asset import` | 取り込み結果・連番名・失敗一覧 |
| Scriptを作る | `LamaPonCli script create` | 登録済みC++ Script雛形 |
| Prefabを検証・変更 | `LamaPonCli prefab inspect/validate/patch` | Root・Nested Prefab・Component・Asset |
| Componentを理解する | `LamaPonCli component list/schema` | 型・初期値・編集項目 |
| 見た目の確認 | `LamaPonCli render --out shot.png` | PNG画像＋`problems`／`shaderFallbackDraws` |
| 物理の確認 | `render --simulate <秒>` | 前後のPNG比較 |
| 書き出し | `LamaPonCli export --zip` | `gameModuleIncluded`・exeの起動 |
| 長い処理を裏で実行 | `LamaPonCli job start/status` | `job.status`・`job.result` |
| エディター不要のゲーム実行 | `LamaPonCli runtime start/status` | `runtime`の状態JSON・ログ・PNG |
| 自動プレイテスト・リプレイ | `LamaPonCli runtime test/replay` | フレーム単位の操作・アサーション・再現 |
| Runtime異常復旧 | `LamaPonCli runtime recover` | 旧セッションを引き継いだ再起動 |
| エディターUIの確認 | `LamaPonEditor --screenshot` | PNG画像 |
| エディターの操作 | `LamaPonEditor --remote` | `dump`＋スクリーンショット |
| 実行中ゲームの観測・操作 | `LamaPonEditor --remote` | `runtime`＋入力／step |

**原則: すべてのコマンドはJSONを返し、exitコード 0=成功。**
`problems` や `buildErrors` が空になるまで修正します。

## レシピ1: ゲームを1本作る（エディター不要）

```bat
LamaPonCli new --dir C:\work\MyGame --name MyGame
:: assets/scenes/Main.scene.json を編集（形式は既存シーンを参照）
:: assets/Player.cpp などを書く
LamaPonCli inspect --project C:\work\MyGame --scene scenes/Main.scene.json
LamaPonCli validate --project C:\work\MyGame --scene scenes/Main.scene.json
LamaPonCli patch --project C:\work\MyGame --scene scenes/Main.scene.json --operations patch.json --dry-run
LamaPonCli patch --project C:\work\MyGame --scene scenes/Main.scene.json --operations patch.json
LamaPonCli test --project C:\work\MyGame --scene scenes/Main.scene.json --spec scene.tests.json
LamaPonCli build --project C:\work\MyGame
LamaPonCli render --project C:\work\MyGame --out check.png
LamaPonCli export --project C:\work\MyGame --zip
```

- `render` のJSONで `uniqueColors: 1`（単色で描画されている可能性）、
  `shaderFallbackDraws > 0`（Shader破損）、`problems`（原因の特定）を確認
- **`magentaPixels` で壊れ判定をしないこと。** この値は色からの推測で、
  ピンクや水色を使った正常な絵でも増えます。
  事実は`shaderFallbackDraws`（エンジンが数えた代役の使用回数）です
- シーンJSONの形式は `new` が生成する `Main.scene.json` が最小の見本
- **C++ Scriptで画面を組み立てるゲームも`render`で確認できます。** CLIは
  `.lamapon/bin/LamaPonGameModule.dll` を読み込み、Scriptがあるシーンでは`Start()`のために最低1フレーム進めます。
  `gameModule.loaded` が false のときは `gameModule.reason` に理由が出ます（多いのは「先に`build`」と「エンジンを建て直したのにモジュールが古い」）
- **操作した結果も撮れます。** `--input "Jump@0.5:0.2"` と書くと、進めている
  途中で入力Actionを押せます（`Action@秒[:押している秒数]`をカンマ区切り）。
  ジャンプや攻撃の確認は`--simulate`だけでは撮れないので、こちらを使います
- **`patch`でScriptを付ける名前は`Game.<クラス名>`です。**
  `LAMAPON_SCRIPT(Player);`なら`"script": "Game.Player"`。
  `Game.`を忘れても`patch`は成功し、`render`の`problems`に`Game Moduleに型が登録されていません` と出るまで気付けません

ビルドや書き出しを作業の流れから切り離す場合は、ジョブAPIを使います。

```bat
LamaPonCli job start build --project C:\work\MyGame
LamaPonCli job status --project C:\work\MyGame --id <jobId>
```

`job start`はすぐに戻るため、返された`jobId`を保存し、`job.status`が`succeeded`または`failed`になるまで`status`を確認します。
完了結果は`job.result`、実行中のログ末尾は`job.logTail`に入ります。
処理を止めるときは次を使います。

```bat
LamaPonCli job cancel --project C:\work\MyGame --id <jobId>
```

## レシピ2: エディターUIを目視確認する

```bat
LamaPonEditor --project <dir> --screenshot ui.png --report ui.json --show project-settings:physics:bottom
```

- `--show` の対象と `:bottom` は [CLIページ](cli.md) を参照
- 撮影後に自動終了する。`ui.json` の `ok` を確認

## レシピ3: エディターをリアルタイム操作する

```bat
LamaPonEditor --project <dir> --remote C:\work\remote
```

> **このモードは実際のマウスカーソルを動かしません。** 注入した座標をエディター
> 側で保持し、毎フレームImGuiへ入れ直しています。実行中もそのPCで他の作業が
> できます。ただし自動操作中のエディターウィンドウを手動で操作すると、入力が
> 競合して意図しない位置へ送られる場合があります。

操作は `C:\work\remote\command.json`（連番seq）で送り、結果は`state.json` で受け取る。
**基本ループ**:

1. `{"type":"dump"}` — 画面のウィジェット一覧をテキストで読む
2. `{"type":"click-label","label":"..."}` — ラベルで操作する
3. `{"type":"screenshot"}` — 画像で結果を確かめる

```json
{ "seq": 1, "commands": [ { "type": "dump" } ] }
{ "seq": 2, "commands": [ { "type": "click-label", "label": "ファイル", "window": "##LamaPonToolbar" } ] }
{ "seq": 3, "commands": [ { "type": "set-value", "label": "##ObjectName", "value": "Player" } ] }
{ "seq": 4, "commands": [ { "type": "screenshot" } ] }
```

### 操作例

| やりたいこと | 手順 |
|---|---|
| メニューを開く | `click-label`（window=`##LamaPonToolbar`）→ `dump` で項目一覧 → `click-label` |
| GameObjectを作る | GameObjectメニュー → 「空のルートを作成」等 |
| 名前を変える | 対象を選択 → `set-value` label=`##ObjectName` |
| 設定値を変える | プロジェクト設定を開く → `set-value`（数値・文字列・bool対応） |
| スライダーを動かす | `drag`（座標はdumpの矩形から） |
| Transformの位置など | `dump` に `"all": true` を付けるとラベル無しの軸フィールドも矩形付きで出る → `set-value` を `x`/`y` 座標指定で |

### 注意点

- **クリックの効果は次のフレーム**に出る。操作と確認の
  スクリーンショットは**別のseq**に分ける
- `set-value`・`drag` は複数フレームかけて実行される。`state.json`
  に結果が出るまで**待ってから**次を送る
- 同じseqは再実行されない。**必ず連番を進める**
- エディターが応答しない場合は、デスクトップ全体を撮って
  **何のダイアログが出ているか**を確認する。未想定のダイアログが
  操作を妨げていないか調べる

## レシピ4: 実行中ゲームを観測・操作する

`--remote`はエディターUIだけでなく、再生中のゲームにも命令を送れます。
ゲーム側の状態を直接JSONで読めるので、画像だけでは分からない「入力を受けたか」「どのObjectが動いたか」「物理接触が増えたか」をフレーム単位で確認できます。

```json
{ "seq": 10, "commands": [ { "type": "play" } ] }
{ "seq": 11, "commands": [ { "type": "input", "action": "Jump", "value": 1, "frames": 1 } ] }
{ "seq": 12, "commands": [ { "type": "pause" }, { "type": "runtime" } ] }
{ "seq": 13, "commands": [ { "type": "step" }, { "type": "runtime" } ] }
```

`state.json`の`runtime`には、GameObject／Transform／Component、`SceneManager::State()`の`gameState`、入力Action、時間、物理・描画統計、CPUプロファイラ直近フレーム、Warning/Errorログが入ります。
`input`はAction名か`KeyboardSpace`等のControl名を指定でき、`frames`の後に自動的に離されます。
`pause`中は`step`だけがゲーム更新を1フレーム通します。

## レシピ5: エディター不要の常駐Runtimeを使う

ゲームを専用ワーカープロセスで起動すると、エディターを占有せずにゲームの状態をJSONで読み、入力や時間を制御できます。

```bat
LamaPonCli runtime start --project C:\work\MyGame --warp --fps 60
LamaPonCli runtime status --project C:\work\MyGame --id <sessionId>
LamaPonCli runtime send --project C:\work\MyGame --id <sessionId> --command pause
LamaPonCli runtime send --project C:\work\MyGame --id <sessionId> --command step
LamaPonCli runtime send --project C:\work\MyGame --id <sessionId> --command "{op:input,action:Jump,value:1,frames:1}"
LamaPonCli runtime send --project C:\work\MyGame --id <sessionId> --command "{op:screenshot,path:shots/frame.png}"
LamaPonCli runtime stop --project C:\work\MyGame --id <sessionId>
```

`start`のJSONに含まれる`sessionId`を保存し、各操作の後に`status`を呼びます。
`status.session.runtime`にはObject・Transform・Component・ゲーム状態・入力・物理・可視性・Profiler・ログが入り、`lastCommandOk`が操作の成否を示します。
複雑なJSONは`--command-file`で渡せます。
セッションは`stop`で終了し、異常終了時は`status`が`failed`とエラー本文を返すため、ログを読んで修正→再実行できます。

### 再現可能なプレイテスト

ゲームの状態を変更・観測し、期待値を確認するテストをJSONで保存します。
`deterministic: true`と`fixedDeltaTime`を併用し、`record`で操作列を保存すると、同じシナリオを後から`runtime replay`で再実行できます。
長いロジック検証は`paceFrames: false`と`renderEveryNFrames`を指定すると、
ゲーム内時間を変えずに実時間待機と不要な描画を減らせます。画像が必要な
stepの`screenshot`／`observe`は自動的に描画を強制します。

```bat
LamaPonCli runtime test --project C:\work\MyGame --spec playtest.json
LamaPonCli runtime replay --project C:\work\MyGame --file final-replay.json
LamaPonCli runtime recover --project C:\work\MyGame --id <oldSessionId>
```

テストの各stepは、コマンド、待機フレーム数、アサーションを持ちます。
Runtimeで使える操作は`query`、`set-transform`、`set-state`、`reload`、`reload-module`、`screenshot`です。
アサーションが失敗した場合は`ok: false`と失敗箇所がJSONで返るため、SceneやScriptを修正して同じテストを再実行できます。

## トラブルシューティング早見表

| 症状 | 見る場所 |
|---|---|
| 画面が一色だけ | `render` の `uniqueColors`、メインカメラの有無（`logs`） |
| どこかが紫 | まず `shaderFallbackDraws`。0なら**壊れていません**（絵がそういう色なだけ）。1以上なら `problems` の `shader-compile-error`（ファイル・行・エラー本文入り） |
| ビルドが失敗 | `build` の `buildErrors`（error行だけ抽出済み） |
| DLLが更新されない | `moduleUpdated: false` — up-to-dateか、失敗して古いDLLが残っているか |
| すり抜ける・当たらない | コライダーのマスクと[衝突マトリクス](physics.md)。**テンプレートの床はレイヤー1** |
| exportにScriptが入らない | `gameModuleIncluded: false` — 先に `build` を実行 |
