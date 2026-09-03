# SceneとPrefab、セーブデータ

Scene Managerの切り替えと非同期読み込み、Prefab、PlayerPrefs、
セーブデータの扱い方を説明します。

[← ドキュメント一覧へ戻る](index.md)

## Scene Manager

Sceneの更新中にGameObjectを直接破棄しないよう、Scene切り替えは要求した次フレームの先頭で実行されます。
読み込みに失敗した場合は現在のSceneを維持し、`LastError()`から理由を取得できます。
現在のSceneパス、予約中のパス、読み込み成功回数も確認できます。

```cpp
auto& scenes = scene.Scenes();
scenes.RequestLoad("scenes/stage-01.scene.json");

// 現在のSceneをファイルから戻す
scenes.RequestReload();
```

時間のかかるSceneは非同期読み込みを使用できます。
ファイルI/O、JSON検証、Scene／Prefabの旧version移行、`assetManifest`に列挙された依存アセットの先読みはワーカースレッドで行い、GameObject生成だけをメインスレッドで安全に実行します。
テクスチャ（PNG/JPG/DDS等）はWICデコード、ミップ生成、GPUテクスチャ作成までワーカースレッド側で完了させるため（D3D11デバイスのリソース生成はフリースレッド）、Scene有効化時のスパイクが小さくなります。
読み込み中も現在のSceneは更新・描画されます。

```cpp
auto& scenes = scene.Scenes();
scenes.RequestLoadAsync(
    "scenes/stage-02.scene.json");

if (scenes.IsLoading())
{
    const float progress =
        scenes.LoadProgress(); // 0.0～1.0
    const auto state =
        scenes.LoadState();
}
```

エクスポートしたゲームでは標準Loading画面が自動表示され、日本語メッセージ、進捗バー、パーセントを描画します。
色とメッセージは変更でき、独自UIを使用する場合は`enabled`を無効にできます。

```cpp
auto& loading = scenes.LoadingScreen();
loading.message = "海底都市へ移動中...";
loading.barFillColor =
    { 0.1f, 0.7f, 0.9f, 1.0f };
loading.showPercentage = true;

scenes.SetMinimumLoadingScreenDuration(0.2f);
scenes.CancelPending(); // 非同期要求をキャンセル
```

`SceneLoadState`ではQueued、Reading、Parsing、Preloading、ReadyToActivate、Activating、Succeeded、Failed、Cancelledを確認できます。
UI ButtonによるScene移動とゲームの起動Sceneも非同期読み込みを使用します。
直近の先読み結果は`PrefetchedAssetCount()`と`PrefetchedAssetBytes()`で確認できます。
先読みできなかった任意参照はScene読込を即失敗させず、`PrefetchFailureCount()`へ記録して通常の有効化処理へ委ねます。

大きいテクスチャ（転送データ8MiB以上）はGPUへの転送も1フレームにまとめず、毎フレームの予算内で粗いミップから段階的にアップロードされます。
読み込み直後は少しぼやけた状態から数フレームで鮮明になり、巨大なテクスチャを読んでもフレームが止まりません。

## 追加読み込み（Additive）

今のSceneを消さずに、別のSceneを重ねて読み込めます。
常駐UI、ポーズ画面、ステージを複数ファイルに分割した構成に使います。

```cpp
auto& scenes = scene.Scenes();

// 今のSceneへ足す（次フレームの先頭で実行）
scenes.RequestLoadAdditive("scenes/hud.scene.json");

// 非同期版（先読みとLoading画面の仕組みを共有）
scenes.RequestLoadAdditiveAsync("scenes/stage-01-props.scene.json");

// 足した分だけ破棄する
scenes.RequestUnload("scenes/hud.scene.json");
```

即時に処理したい場合は`Scene`側のAPIを直接呼べます。
返り値の`SceneHandle`は、そのまま`UnloadScene`へ渡せます。
**Scriptの`Update`など更新中からは呼ばないでください**（GameObjectの配列を更新中に変更するため）。
Scriptからは必ず`Request系`を使います。

```cpp
const LamaPon::SceneHandle hud =
    scene.MergeFromFile("scenes/hud.scene.json");

for (const auto& loaded : scene.AdditiveScenes())
{
    // loaded.handle / loaded.path / loaded.name / loaded.rootCount
}

scene.UnloadScene(hud);
scene.UnloadAllAdditiveScenes();
```

エディターでは、Asset Browserでシーンを右クリックして「シーンを追加読み込み（Additive）」を選ぶと足せます。
Hierarchyには`[追加] シーン名`のグループが出て、右クリックの「この追加シーンを破棄」でそのシーンのGameObjectだけを消せます。
UI Buttonの「追加読み込み（Additive）で開く」をオンにすると、C++を書かずにポーズ画面を重ねられます。

追加読み込みの決まりごと:

- **GameObjectのIDは振り直されます** — 既存Sceneと衝突しないようにするためで、
  ファイル内の親子関係はそのまま保たれます。
- **環境設定は主Sceneのものが残ります** — 追加Scene側の空・霧・Bloom・
  カラーグレーディング・物理・カリング設定は無視します。
- **Main Cameraは主Sceneが優先されます** — 主SceneにMain Cameraが無いときだけ、
  追加Scene側のカメラを採用します。
- **保存には混ざりません** — シーンを保存しても、追加読み込みしたGameObjectは
  書き出されません（元のファイルのままです）。
- **切り替え読み込みで一緒に消えます** — `RequestLoad`での切り替えや、
  エディターの停止（編集状態の復元）で追加Sceneは破棄されます。
- **実行中に作ったGameObjectは主Sceneの所属になります** — 追加Sceneの
  GameObjectの子として作った場合は、その階層ごと一緒に破棄されます。
- 同じパスの二重読み込みは拒否され、理由が`LastError()`に入ります。

`DontDestroyOnLoad`との使い分けは、**Sceneを跨いで残したい単体のGameObject**なら`DontDestroyOnLoad`、**まとめて足したり外したりしたい一式**なら追加読み込みです。

ルートGameObjectは、Inspectorの「シーン切替後も維持」を有効にするか、C++から`DontDestroyOnLoad`を呼ぶと、子階層とコンポーネントの実体を保ったまま次のSceneへ移動します。

```cpp
auto& session = scene.CreateGameObject("GameSession");
scene.DontDestroyOnLoad(session, "MainGameSession");

// 再び通常のScene所属へ戻す
scene.DestroyOnLoad(session);
```

永続キーはScene内で一意です。
移動先Sceneに同じ永続キーの初期配置オブジェクトがあった場合は、実行中のインスタンスを残して初期配置側を除去するため、Sceneを往復してもGameSessionやBGM管理オブジェクトが重複しません。
永続化できるのはルートGameObjectで、その子階層は自動的に一緒に維持されます。
Hierarchyでは`[維持]`と表示されます。

Scene間だけで共有する軽量な値は`RuntimeGameState`へ保持できます。

```cpp
auto& state = scene.Scenes().State();
state.SetInteger("score", 1200);
state.SetNumber("health", 85.5);
state.SetBoolean("hasKey", true);
state.SetString("checkpoint", "港");

const auto score = state.Integer("score", 0);
```

`RuntimeGameState`は整数、小数、真偽値、UTF-8文字列に対応し、Scene切替や再読み込みでは消えません。
アプリケーション終了後も残す値にはPlayerPrefsまたはSaveDataを使用します。

UI ButtonのInspectorでは「現在のSceneを再読み込み」、またはAsset Browserから移動先Sceneをドラッグして設定できます。
Sceneファイルの名前変更・移動・削除時にはButtonの参照も追従または安全に解除されます。

サンプルの右下ButtonをPlay中に押すと`ui-destination.scene.json`へ移動し、遷移先の「元のSceneへ戻る」ButtonでSandboxへ戻れます。
エディターでStopすると、Scene遷移後でもPlay開始前の編集状態へ復元されます。

## Prefab

**HierarchyのGameObjectをAsset Browserへドラッグ＆ドロップすると、その場でPrefabになります**（落としたフォルダーへ`<名前>.prefab.json`が作られ、同名があれば連番が付きます）。
Hierarchyの右クリック「Prefabとして保存...」や、「ファイル」→「選択をPrefabとして保存...」でも保存できます。
いずれも選択したGameObjectと子階層が対象です。
PrefabにはローカルID、親子関係、Transform、対応する全コンポーネントが含まれます。

Asset BrowserでPrefabを右クリックして「Prefabを配置」を選ぶか、ダブルクリックすると、現在選択中のGameObjectの子として新しい階層を配置します。
GameObjectが未選択の場合はシーンルートへ配置します。
配置後はHierarchyに`[Prefab]`と表示され、元の`.prefab.json`へのリンクはシーン保存、コピー、複製、Undo／Redoでも維持されます。

`assets/prefabs/操作できる立方体.prefab.json`には、`MeshRenderer`、`Rotator`、`InputMover`とNested Prefabの`青いアクセント.prefab.json`を含むサンプルがあります。
Prefabインスタンスまたはその子を選択すると、Inspectorの「Prefab」に元アセットとOverride状態が表示されます。
`Apply`は現在の階層全体を元Prefabへ反映し、`Revert`はインスタンスの変更を破棄して元Prefabから再構築します。
壊れた、または存在しないPrefabに対するRevertは失敗し、現在のインスタンスを保持します。

Inspectorの「元Prefabを選択」でAsset Browserの該当アセットへ移動でき、「インスタンスルートを選択」で子から親のインスタンスへ戻れます。
逆にAsset BrowserでPrefabを右クリックして「シーン内のインスタンスを選択」を選ぶと、そのPrefabから配置したインスタンスをまとめて選択できます（どこで使っているかの確認や、まとめて移動・削除に使えます）。

「Override詳細」には、名前、Transform、Component設定などの差分パスと元／現在値が一覧表示されます。
値の差分は「この項目をApply」「この項目をRevert」で1項目だけ反映または破棄できます。
GameObject／Componentの追加・削除は構造差分として表示され、階層全体のApply／Revertを使用します。

Prefabインスタンス内の子階層を別Prefabとして保存するとNested Prefabになります。
親PrefabをApplyしても子Prefabへのリンクは保持され、親Prefabを配置し直した後も子を選択して個別にOverride、Apply、Revertできます。

## PlayerPrefsとセーブデータ

ユーザーごとの設定は`Application::Preferences()`から保存できます。

```cpp
auto& prefs = application.Preferences();
prefs.SetInteger("highScore", 4200);
prefs.SetNumber("masterVolume", 0.75);
prefs.SetBoolean("subtitles", true);
prefs.SetString("playerName", "トライデント");
prefs.Save();

const auto score = prefs.GetInteger("highScore", 0);
```

整数、小数、真偽値、UTF-8文字列に対応し、型が異なる場合やキーが存在しない場合は指定した既定値を返します。
未保存の変更はアプリケーション終了時にも自動保存されます。

ゲーム進行など構造化されたデータは`Application::Saves()`へJSONとして保存します。

```cpp
application.Saves().SaveJson(
    "slot1",
    R"({"level":3,"position":[1,2,3]})");

if (const auto json =
        application.Saves().LoadJson("slot1"))
{
    // ゲーム側のJSON構造へ復元
}
```

`HasSlot`、`ListSlots`、`DeleteSlot`も利用できます。
保存先は`%LOCALAPPDATA%/LamaPon/<ゲーム名>/`で、PlayerPrefsと`Saves`フォルダーに分離されます。
保存は`.tmp`へ書き出してから置き換えるため、書き込み途中のデータを完成済みファイルとして残しません。

Scene、Prefab、Project設定、PlayerPrefs、SaveDataは読み込み前に共通のversion移行処理を通ります。
現在は旧version 0をversion 1へ変換し、将来の形式変更も段階的な移行処理を追加できる構成です。
未対応の新しいversionは安全のため拒否します。

エディター下部の「セーブデータ」タブでは、PlayerPrefsの追加・更新・削除・保存、JSONスロットの編集・保存・読込・削除、保存フォルダーを開く操作ができます。

```cpp
LamaPon::Logger::Instance().Info("ステージを開始しました");
LamaPon::Logger::Instance().Warning("敵の生成位置が見つかりません");
LamaPon::Logger::Instance().Error(
    "Playerの初期化に失敗しました",
    player.Id());
```

`RayIntersectsBounds`と`TransformBounds`は、エディター選択以外の射撃判定や簡易Raycastにも再利用できます。

Scene ViewではColliderが次の色で表示されます。

- 緑: 通常のCollider
- 黄: Trigger

Cameraコンポーネントの視錐台もScene Viewへ表示されます。

- 黄: Main Camera
- 紫: その他のCamera

各Colliderには0〜31のレイヤーと32ビットの衝突マスクを設定できます。
双方のマスクが相手のレイヤーを許可している場合だけ衝突判定が実行されます。

## よくあるつまずき

- **RequestLoadしたのにすぐ切り替わらない** — Scene切り替えは要求した
  **次のフレーム先頭**で実行される仕様です（更新中のGameObject破棄を防ぐため）。
- **読み込みに失敗して何も起きない** — 失敗時は現在のSceneが維持されます。
  `Scenes().LastError()`に理由が入っているので、Consoleやログで確認します。
  パスは`assets`からの相対パス（例: `scenes/stage-01.scene.json`）です。
- **Sceneを切り替えたらスコアが消えた** — GameObjectはScene切り替えで
  破棄されます。
  数値やフラグは`Scenes().State()`（RuntimeGameState）へ、BGM管理などのGameObjectごと残したいものは`DontDestroyOnLoad`を使います。
- **PlayerPrefsとSaveDataどちらを使う？** — 音量設定やハイスコアのような
  単純な値はPlayerPrefs、ゲーム進行のような構造化データはSaveDataのJSONスロットが向いています。
  どちらもアプリ終了後も残ります。
- **Prefabを編集したのに他の配置に反映されない** — インスタンスの変更は
  そのままではインスタンス限りです。
  Inspectorの「Prefab」欄から`Apply`で元Prefabへ反映すると、全配置へ行き渡ります。
