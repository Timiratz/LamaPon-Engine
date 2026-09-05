# C++スクリプティング（Game Module）

C++ Scriptの作成、ホットリロード、Script API、コンポーネントの例を説明します。

[← ドキュメント一覧へ戻る](index.md)

## ランタイムとエディター

ビルド時に次の独立した成果物が生成されます。

- `LamaPonRuntime.dll`: ゲームループ、シーン、描画、物理、アセット
- `LamaPonEditor.lib`: Dear ImGui／ImGuizmoを使う編集機能
- `LamaPonEditor.exe`: 任意のLamaPonプロジェクトを編集するシーンエディター
- `LamaPonHub.exe`: 新規作成、既存プロジェクト、最近使ったプロジェクトのランチャー
- `LamaPonGame.exe`: Runtimeだけを使うエディターなしゲーム
- `LamaPonGameModule.dll`: ゲーム固有C++ Component

`LamaPonGame.exe`は`LamaPonRuntime.dll`へ動的リンクしますが、Dear ImGui、ImGuizmo、ファイルダイアログなどのエディター機能には依存しません。

## C++ Game Module

ゲーム固有コードは`LamaPonRuntime`本体を変更せず、`LamaPonGameModule.dll`へ分離できます。
DLLは起動時に自動検出され、登録された型はInspectorの「コンポーネントを追加」→「Game Module (C++)」に表示されます。
エディターから次の手順でC++ Scriptを作成できます。

1. アセットウィンドウの任意のフォルダーまたは余白を右クリックします。
2. 「新規C++ Script」を選び、`PlayerController.cpp`のような名前を入力します。
3. 生成されたファイルがコードエディターで開きます。
4. `Update`、60 Hzの`FixedUpdate`、`OnCollisionEnter`などへ処理を書きます。
5. C++ ScriptをヒエラルキーのGameObject、またはInspector下部の
  「コンポーネントを追加」へドロップします。
6. バックグラウンドで自動ビルド・Hot Reloadされ、成功するとそのGameObjectへ
  Scriptが自動でアタッチされます。

**書いたコードを保存すると、少し待ってから自動でビルドされます。** `assets`内の`.cpp`／`.h`が対象で、保存が続いている間は待ち、静かになってから1回だけビルドします。
ビルドが終わるとHot Reloadが差し替えるので、エディターの再起動は不要です。
再生中は自動ビルドしません。
オフにしたい場合は「ファイル」→「プロジェクト設定...」→「スクリプト」の「保存したらGame Moduleを自動ビルド」を外します。

手動で追加する場合は、アセットの右クリックメニューから「Game Moduleをビルド」を選び、Inspectorの「コンポーネントを追加」→「Game Module (C++)」から型を選択できます。

`.cpp`はAsset Browserでダブルクリックしても開けます。
どのエディターで開くかは「ファイル」→「プロジェクト設定...」→「スクリプト」で選択します（Visual Studio CodeとVisual Studioは自動検出、未設定ならWindowsのファイル関連付け。
`.hlsl`も同じ設定です）。
詳しくは[プロジェクト](project.md)の「プロジェクト設定」を参照してください。

使える機能の一覧は[コード一覧](code-reference.md)にまとまっています（宣言・引数・戻り値・サンプル付き）。
困ったときの逆引き表もあります。

生成コードは`LamaPon::Script`を継承し、必要な関数だけを上書きする初心者向けAPIです。

```cpp
#include "LamaPon/LamaPon.h"

class PlayerController final : public LamaPon::Script
{
public:
    void Update(float deltaTime) override
    {
        // 毎フレームの処理
    }
};

LAMAPON_SCRIPT(PlayerController);
```

`Start`、`Update`、`FixedUpdate`、`OnCollisionEnter/Stay/Exit`のうち、使うものだけを記述します。
所有GameObjectは`Owner()`、GraphicsDeviceは`Graphics()`から取得できます。
Create／Destroy、関数ポインター、ホットリロード用の保存、Game Module登録はエンジン側が自動生成します。
プロジェクト専用CMakeは`assets`以下の`.cpp`を自動検出するため、`CMakeLists.txt`や登録一覧の編集は不要です。

Script基底クラスには、`Owner()`や`GetScene()`を書かずに主要な操作を呼べるショートカットがあります。

```cpp
// 自分のGameObjectを操作
GetTransform().position.y += 1.0f;
auto* body = GetComponent<LamaPon::RigidbodyComponent>();
auto& audio = AddComponent<LamaPon::AudioSourceComponent>();

// 自分の階層から探す（自分自身も含みます）
auto* muzzle =
    GetComponentInChildren<LamaPon::ParticleSystemComponent>();
auto* root = GetComponentInParent<LamaPon::RigidbodyComponent>();
auto* weapon = Owner().FindChild("腕/手/武器"); // パス指定

// シーン全体から探す
auto* player = FindWithTag("Player");
auto enemies = FindObjectsWithTag("Enemy");
auto* boss = Find("ボス");
auto* camera =
    GetScene().FindComponentOfType<LamaPon::CameraComponent>();

// 生成と削除
auto& bullet = Instantiate("prefabs/bullet.prefab.json");
Destroy(*enemyObject);
```

`GetComponentInChildren`系は既定で非アクティブ階層をスキップし、引数に`true`を渡すと含めます。

### インターフェースとデザインパターン

`GetComponent<T>()`はコンポーネント型でしか引けませんが、`GetScript<T>()`は**自分で書いたスクリプトの型やインターフェース**で引けます。
`GetComponent<T>()`では取得できないスクリプト型やインターフェースを、`GetScript<T>()`で取得できます。

まずインターフェースを普通のC++として`assets`以下のヘッダに定義します。
エンジン側に登録する必要はありません。

```cpp
// assets/scripts/IDamageable.h
struct IDamageable
{
    virtual ~IDamageable() = default;
    virtual void ApplyDamage(int amount) = 0;
};
```

実装側は`LamaPon::Script`と一緒に継承します。
継承の順番は自由で、`LamaPon::Script`が先頭でなくても構いません。

```cpp
// assets/scripts/Enemy.cpp
class Enemy final
    : public IDamageable
    , public LamaPon::Script
{
public:
    void ApplyDamage(const int amount) override
    {
        m_health -= amount;
        if (m_health <= 0)
        {
            Destroy(Owner());
        }
    }

private:
    int m_health{ 100 };
};

LAMAPON_SCRIPT(Enemy);
```

呼ぶ側はインターフェースだけを知っていれば十分です。
`Enemy`のヘッダをincludeする必要はありません。

```cpp
// 同じGameObject上から
if (auto* target = GetScript<IDamageable>())
{
    target->ApplyDamage(25);
}

// 階層から（自分自身も含みます）
auto* target = GetScriptInChildren<IDamageable>();

// 任意のGameObjectから
auto* boss = Find("ボス");
auto* damageable = boss->GetScript<IDamageable>();
```

見つからなければ`nullptr`を返すので、`if`で受けてください。
`GetScriptInChildren`は既定で非アクティブ階層をスキップし、引数に`true`を渡すと含めます。

これでステートパターンのように、実装を差し替えても呼び出し側を変えなくてよい書き方ができます。

```cpp
// 状態ごとの振る舞いをインターフェースに切り出す
struct IEnemyState
{
    virtual ~IEnemyState() = default;
    virtual void Tick(float deltaTime) = 0;
};

// 巡回・追跡・待機を別スクリプトにして、同じGameObjectへ
// アタッチしたものを差し替えるだけで挙動が変わります。
void EnemyBrain::Update(const float deltaTime)
{
    if (auto* state = GetScript<IEnemyState>())
    {
        state->Tick(deltaTime);
    }
}
```

インターフェースを追加・変更したら、Game Moduleのビルドが必要です（`assets`以下の`.cpp`/`.h`を保存すると自動でビルドされます）。

### タイマーとコルーチン

一定時間後の処理は`Invoke`（1回）／`InvokeRepeating`（繰り返し）で予約できます。
「待って→実行して→また待つ」のような流れは、コルーチンで上から順に書けます。

```cpp
class BossIntro final : public LamaPon::Script
{
public:
    void Start() override
    {
        StartCoroutine(Intro());
    }

    LamaPon::Coroutine Intro()
    {
        SetBossVisible(true);
        co_await LamaPon::WaitForSeconds{ 2.0f };   // 2秒待つ
        PlayRoar();
        co_await LamaPon::WaitUntil{
            [this] { return m_playerInRange; } };   // 条件成立まで待つ
        co_await LamaPon::WaitForNextFrame{};       // 1フレーム待つ
        StartBattle();
    }
};
```

- 待機はゲーム時間（`Time::SetTimeScale`適用後）で進みます。
- `StartCoroutine`はハンドルを返し、`StopCoroutine(handle)`／
  `StopAllCoroutines()`で停止できます。
  Scriptの破棄時には自動で全コルーチンが停止します。
- 待機条件は`WaitForSeconds`／`WaitForNextFrame`／`WaitUntil`／
  `WaitWhile`の4種類です。
- コルーチン内で自分のGameObjectを`Destroy`する場合は、
  それをコルーチンの最後の文にしてください。

### イベント（名前で連携するシグナル）

「敵が倒された」「ゲーム開始」のような出来事を名前で購読・発行できますイベント名だけで連携できるため、受け取る側と発行する側が互いを参照せず、UIとゲームロジックの連携が疎結合になります。

```cpp
// 受け取る側（スコア管理Script）
void Start() override
{
    On("EnemyDied",
        [this](const LamaPon::EventArgs& args)
        {
            m_score += static_cast<int>(args.number);
        });
    On("StartGame", [this] { BeginRound(); });  // 引数なしでもOK
}

// 発行する側（敵Script）
void OnDestroy() override
{
    LamaPon::EventArgs args;
    args.number = 100.0f;  // スコア
    Emit("EnemyDied", args);
}
```

- `EventArgs`は`sender`（発行元GameObject）、`number`、`text`を持つ
  固定の入れ物です。
  `Emit`では`sender`が自動で自分になります。
- 購読はScriptの破棄時に自動解除されます（`Off(handle)`で手動解除も可）。
- イベントバスはSceneの`Events()`が実体で、Scene切り替えでは消えません
  （`DontDestroyOnLoad`の常駐Scriptの購読が維持されます）。
- **UI Buttonの「クリック時のイベント」**にイベント名を設定すると、
  ボタン側はコード不要で、Script側の`On("イベント名", ...)`だけでクリックへ反応できます。

### リアクティブストリーム（型付きの値の流れ）

`EventBus`より細かい値の変化を型安全に扱いたい場合は、
`Subject<T>`と`ReactiveProperty<T>`を使えます。すべて同期実行され、
ゲームのメインスレッドで使うことを想定しています。

```cpp
LamaPon::Subject<int> m_damage;
LamaPon::ReactiveProperty<int> m_health{ 100 };
LamaPon::CompositeSubscription m_subscriptions;

void Start() override
{
    // 偶数のダメージだけを文字列へ変換して受け取ります。
    m_subscriptions.Add(
        m_damage.AsObservable()
            .Where([](int value) { return value % 2 == 0; })
            .Select([](int value)
            {
                return std::to_string(value) + " damage";
            })
            .Subscribe([](const std::string& message)
            {
                // messageをUIなどへ渡す
            }));

    // Observe()は購読直後に現在値（ここでは100）も通知します。
    m_subscriptions.Add(
        m_health.Observe()
            .DistinctUntilChanged()
            .Subscribe([](int health)
            {
                // HP表示を更新する
            }));
}

void ApplyDamage(int amount)
{
    m_damage.OnNext(amount);
    m_health.Set(m_health.Value() - amount);
}
```

- `Subscription`は破棄時に自動解除されます。購読を維持する間は変数や
  `CompositeSubscription`に保持してください。
- `Where`、`Select`、`Take`、`DistinctUntilChanged`を連結できます。
- `Merge`は同じ型の2ストリームを統合し、`CombineLatest`は両方の
  最新値を`std::pair`として通知します。
- `ReactiveProperty<T>::Observe()`は現在値を含めて通知し、`Changes()`は
  以後の変更だけを通知します。同じ値を`Set`しても再通知しません。
- 現段階では`OnError`／`OnCompleted`と、別スレッドへの切り替えは
  提供していません。

既存の名前付きイベントも、そのままストリームへ接続できます。

```cpp
m_subscriptions.Add(
    GetScene().Events().Observe("EnemyDied")
        .Take(3)
        .Subscribe([](const LamaPon::EventArgs& args)
        {
            // 最初の3体だけを処理
        }));
```

ゲーム時間やフレームを起点にする場合は`LamaPon::Reactive`を使います。

```cpp
m_subscriptions.Add(
    LamaPon::Reactive::NextFrame().Subscribe([](std::uint64_t) {}));
m_subscriptions.Add(
    LamaPon::Reactive::EveryFrame().Subscribe([](std::uint64_t frame) {}));
m_subscriptions.Add(
    LamaPon::Reactive::Timer(2.0f).Subscribe([](std::uint64_t) {}));
m_subscriptions.Add(
    LamaPon::Reactive::Interval(0.5f).Subscribe([](std::uint64_t tick) {}));
```

`Timer`と`Interval`の第2引数を`true`にすると、`timeScale`の影響を
受けない時間を使います。通知は`Scene::Update`の先頭で同期実行されます。

ビルド結果はプロジェクトごとの`.lamapon/bin/LamaPonGameModule.dll`へ出力され、中間ファイルは`.lamapon/build/game-module`へ分離されます。
別のプロジェクトを開くとEditorはそのプロジェクトのDLLへ切り替えるため、他ゲームのC++ Componentは混ざりません。
DLLがまだない新規プロジェクトも通常どおり開け、最初のビルドが完了すると自動でHot Reloadされます。
プロジェクトDLLのDebug／Release構成は、起動中のEditorに自動追従します。

上級者向けに`LAMAPON_SCRIPT_WITH_SCHEMA`を使うと、公開値をInspectorへ型付きで表示できます。
現在は`bool`、`int`、`float`、`string`、`vec2`、`vec3`、`vec4`、`color3`、`color4`、`asset`（他のアセットへの参照）、`list`（可変長の並び）に対応し、数値の`min`、`max`、`step`、表示名、ツールチップも指定できます。
`asset`と`list`の書き方は[データアセット](data-assets.md)にまとめてあります（同じスキーマ書式です）。
生のJSON編集欄は「詳細設定 (Properties JSON)」として残るため、スキーマ外の値も編集できます。

```json
{
  "fields": [
    {
      "name": "speed",
      "displayName": "速度",
      "type": "float",
      "default": 5.0,
      "min": 0.0,
      "max": 20.0,
      "step": 0.1,
      "tooltip": "1秒あたりの移動量"
    }
  ]
}
```

サンプルの`samples/GameModule/SampleGameModule.cpp`には、従来どおり`Sample.FloatingAccent`も登録されています。

### データアセット（ScriptableObject相当）

GameObjectへ付けずに持つデータ（敵の性能表、カードの一覧など）は、
`LAMAPON_DATA_ASSET`で型を宣言し、`*.asset.json`として作ります。

```cpp
LAMAPON_DATA_ASSET("Game.EnemyData", "敵データ", EnemyDataSchema)
```

```cpp
const auto enemy = LoadDataAsset("data/goblin.asset.json");
const int hitPoints = enemy->GetInt("hitPoints");
```

作り方と読み方は[データアセット](data-assets.md)を参照してください。

エンジンはDLLを`.lamapon-hot-reload`へシャドウコピーして読み込むため、エディターを終了せずに`LamaPonGameModule`を再ビルドできます。
更新は約0.5秒ごとに検出され、実行中インスタンスをSerializeして破棄した後、新しいDLLで復元します。
Inspectorの「Moduleを再読み込み」から手動実行することもできます。
ゲームを書き出すと、現在のプロジェクトにあるGame Module DLLが配布フォルダーへ`LamaPonGameModule.dll`としてコピーされます。
プロジェクトDLLがない場合は、C++ Componentなしのゲームとして書き出されます。

## コンポーネントの例

```cpp
auto& player = scene.CreateGameObject("Player");
player.GetTransform().position = { 0.0f, 0.0f, 0.0f };
player.AddComponent<LamaPon::MeshRendererComponent>(
    LamaPon::PrimitiveShape::Cube);
```

親子関係:

```cpp
auto& weapon = scene.CreateGameObject("Weapon");
weapon.SetParent(&player);
```

エンジン内蔵コンポーネントは`LamaPon::Component`を継承し、ゲーム固有コンポーネントは上記のC++ Game Moduleへ登録します。
