# コード一覧（C++ リファレンス）

LamaPonのC++スクリプトで使える機能を、**宣言・引数・戻り値・解説・サンプル**の形式でまとめています。
目的に合う項目を探し、サンプルをコピーして利用してください。

[← ドキュメント一覧へ戻る](index.md)

スクリプトの作り方そのものは[C++スクリプティング](scripting.md)を参照してください。

## サンプルの読み方

このページのサンプルには2種類あります。

**① クラスまるごと**（`class ... { };`まで書いてあるもの）そのままファイルへ貼れば動きます。

**② 一部だけ**（`void Update(...)`や`private:`から始まるもの）**クラスの中身**です。
下の枠の該当する場所へ貼ってください。

```cpp
#include "LamaPon/LamaPon.h"

class MyScript final : public LamaPon::Script
{
public:
    // ← サンプルの「void Start()」「void Update()」などはここへ

private:
    // ← サンプルの「private:」の下にある変数はここへ
};

LAMAPON_SCRIPT(MyScript);
```

`LamaPon::`は名前空間です。
エンジンの機能には全部これが付きます（`LamaPon::Logger`、`LamaPon::RigidbodyComponent`など）。

## よく出てくる型

| 型 | 意味 | 書き方の例 |
|---|---|---|
| `float` | 小数 | `1.5f`（末尾のfを忘れずに） |
| `DirectX::XMFLOAT3` | 3つの小数の組（位置・回転・大きさ・色RGB） | `{ 1.0f, 2.0f, 3.0f }` |
| `DirectX::XMFLOAT2` | 2つの小数の組（2Dの位置やサイズ） | `{ 64.0f, 64.0f }` |
| `DirectX::XMFLOAT4` | 4つの小数の組（色RGBAなど、0〜1） | `{ 1.0f, 0.0f, 0.0f, 1.0f }`＝赤 |
| `std::filesystem::path` | ファイルの場所（assetsからの相対パス） | `"textures/player.png"` |
| `std::string` | 文字列 | `"こんにちは"` |
| `std::string_view` | 文字列（読むだけ。`"..."`をそのまま渡せます） | `"Player"` |
| `bool` | true / false | `true` |
| `std::uint64_t` | 大きい整数（IDやハンドル） | — |
| `T*`（ポインター） | 「無いかもしれない」もの。**必ず`nullptr`か確認** | `if (p != nullptr)` |
| `T&`（参照） | 「必ずある」もの。確認不要 | — |

**角度はラジアン**です。
度で書きたいときは`DirectX::XMConvertToRadians(90.0f)`で変換します。

## 用語

| 用語 | 意味 |
|---|---|
| GameObject | シーンに置く「物」。それ自体は空っぽの入れ物 |
| コンポーネント | GameObjectに付ける「機能」（見た目・当たり判定・音など） |
| Script | あなたが書くC++のコンポーネント |
| Transform | 位置・回転・大きさ。全GameObjectが必ず持つ |
| Prefab | GameObjectを設定ごと保存したファイル。何個でも配置できる |
| シーン | GameObjectの集まり（ステージ1つ分など） |
| deltaTime | 前のフレームからの経過秒数。掛け算すると速度が一定になる |

## 目次

| 分類 | 内容 |
|---|---|
| [サンプルの読み方](#サンプルの読み方) | コードをどこへ貼るか |
| [よく出てくる型](#よく出てくる型) | XMFLOAT3などの意味 |
| [用語](#用語) | GameObject、コンポーネントとは |
| [はじめての1本](#はじめての1本全体像) | 動くスクリプトの全体像 |
| [使用必須](#使用必須) | スクリプトの骨格、ライフサイクル |
| [GameObject操作](#gameobject操作) | 作成・削除・検索・親子・Tag |
| [Transform](#transform) | 位置・回転・拡縮 |
| [コンポーネント操作](#コンポーネント操作) | 取得・追加 |
| [入力](#入力) | キーボード・マウス・ゲームパッド |
| [時間](#時間) | deltaTime、ポーズ、スロー再生 |
| [コルーチンとタイマー](#コルーチンとタイマー) | 待機処理、遅延実行 |
| [イベント](#イベント) | スクリプト間の通知 |
| [シーン](#シーン) | 切り替え、値の引き継ぎ |
| [物理（クエリ）](#物理クエリ) | Raycast、範囲判定 |
| [描画](#描画) | Mesh/Sprite/Model/Text/パーティクル |
| [物理（コンポーネント）](#物理コンポーネント) | Rigidbody、Collider、CharacterController |
| [オーディオ](#オーディオ) | 効果音・BGM |
| [アニメーション](#アニメーション) | クリップ再生、Trigger |
| [UI](#ui) | ボタン、スライダー、入力欄 |
| [ナビゲーション](#ナビゲーション) | NavMeshによる経路移動 |
| [セーブとログ](#セーブとログ) | 進行状況の保存、デバッグ出力 |
| [逆引き](#逆引きやりたいこと別) | やりたいことから探す |
| [よくあるコンパイルエラー](#よくあるコンパイルエラー) | エラー文から原因を引く |

---

---

## はじめての1本（全体像）

細かい話に入る前に、**動く1本を丸ごと**見ておくと分かりやすいです。
矢印キーで動いて、Spaceでジャンプして、敵に当たると止まるスクリプトです。

```cpp
#include "LamaPon/LamaPon.h"

class SimplePlayer final : public LamaPon::Script
{
public:
    // 最初に1回だけ呼ばれます
    void Start() override
    {
        // 自分に付いているRigidbodyを覚えておきます。
        // 毎回GetComponentすると遅いので、ここで1回だけ。
        m_body = GetComponent<LamaPon::RigidbodyComponent>();

        LamaPon::Logger::Instance().Info("プレイヤー開始");
    }

    // 毎フレーム呼ばれます（deltaTime = 前フレームからの秒数）
    void Update(const float deltaTime) override
    {
        // 入力を読む（-1.0〜1.0）
        auto& input = Graphics().Input();
        const float x = input.Value("MoveHorizontal");

        // 左右へ移動。deltaTimeを掛けると、
        // どんなフレームレートでも同じ速さになります
        GetTransform().position.x += x * 5.0f * deltaTime;

        // Spaceを「押した瞬間」だけジャンプ
        if (input.WasPressed("Jump") && m_body != nullptr)
        {
            m_body->AddForce(
                { 0.0f, 6.0f, 0.0f },
                LamaPon::ForceMode::Impulse);
        }
    }

    // 何かにぶつかった瞬間に呼ばれます
    void OnCollisionEnter(
        const LamaPon::CollisionEvent& event) override
    {
        // ぶつかった相手のタグを見て判断します
        if (event.other.CompareTag("Enemy"))
        {
            m_alive = false;
            LamaPon::Logger::Instance().Info("やられた！");
        }
    }

private:
    // クラスの中で覚えておきたい値は、ここに置きます
    LamaPon::RigidbodyComponent* m_body{};
    bool m_alive{ true };
};

// この1行でエンジンへ登録されます。忘れると一覧に出てきません
LAMAPON_SCRIPT(SimplePlayer);
```

このスクリプトを使うには、GameObjectへ**このScript**と**Rigidbody**と**Collider**を付けます。
以降のページで、ここに出てきた一つひとつを詳しく説明します。

---

## 使用必須

### スクリプトの骨格

**宣言**

```cpp
class 名前 final : public LamaPon::Script { ... };
LAMAPON_SCRIPT(名前);
```

**概略**

C++スクリプトを作る際の最小構成です。

**引数**

| 記述 | 説明 |
|---|---|
| `名前` | 好きなクラス名（`PlayerController`など）。**ファイル名と一致**させます |
| `final` | 継承されないクラスの印。付けておくと安全で、少し速くなります |
| `LAMAPON_SCRIPT(名前)` | エンジンへ登録するマクロ。**書き忘れるとInspectorに出てきません** |

**解説**

`LamaPon::Script`を継承し、必要なメソッドだけを`override`します。
最後の`LAMAPON_SCRIPT`でエンジンへ登録され、Inspectorの「コンポーネントを追加」→「Game Module (C++)」に表示されるようになります。
ファイル名とクラス名は一致させてください。

**サンプル**

```cpp
#include "LamaPon/LamaPon.h"

class MyScript final : public LamaPon::Script
{
public:
    void Start() override
    {
        // 最初のフレームの前に1回だけ呼ばれます
    }

    void Update(const float deltaTime) override
    {
        // 毎フレーム呼ばれます
    }
};

LAMAPON_SCRIPT(MyScript);
```

---

### ライフサイクル（上書きできるメソッド）

**宣言**

```cpp
void Awake();
void Start();
void OnEnable();
void OnDisable();
void OnDestroy();
void Update(float deltaTime);
void LateUpdate(float deltaTime);
void FixedUpdate(float fixedDeltaTime);
void OnCollisionEnter(const LamaPon::CollisionEvent& event);
void OnCollisionStay(const LamaPon::CollisionEvent& event);
void OnCollisionExit(const LamaPon::CollisionEvent& event);
void OnTriggerEnter(const LamaPon::CollisionEvent& event);
void OnTriggerStay(const LamaPon::CollisionEvent& event);
void OnTriggerExit(const LamaPon::CollisionEvent& event);
```

**概略**

エンジンが自動的に呼び出すメソッドです。
使うものだけ書けば十分です。

**引数**

| 引数 | 説明 |
|---|---|
| `deltaTime` | 前のフレームからの経過秒数。`Time::SetTimeScale`の影響を受けます |
| `fixedDeltaTime` | 固定更新の間隔。常に1/60秒（0.0166…）です |
| `event` | 衝突相手のGameObjectや接触点の情報が入っています |

**解説**

呼ばれる順番は`Awake`→`OnEnable`→`Start`→（毎フレーム`FixedUpdate`→`Update`→`LateUpdate`）→`OnDisable`→`OnDestroy`です。

物理に力を加える処理は`FixedUpdate`へ書くと、フレームレートが変わっても挙動が同じになります。
カメラの追従など「他が動いた後に処理したい」ものは`LateUpdate`が向いています。

Colliderの「トリガー」がONの相手との接触は`OnTrigger〜`、押し合う衝突は`OnCollision〜`へ届きます。

**サンプル**

```cpp
#include "LamaPon/LamaPon.h"

class Player final : public LamaPon::Script
{
public:
    void Start() override
    {
        m_body = GetComponent<LamaPon::RigidbodyComponent>();
    }

    void FixedUpdate(const float fixedDeltaTime) override
    {
        // 継続的な力は固定更新で加えます
        if (m_body != nullptr)
        {
            m_body->AddForce(
                { 0.0f, 0.0f, 5.0f },
                LamaPon::ForceMode::Acceleration);
        }
    }

    void OnCollisionEnter(
        const LamaPon::CollisionEvent& event) override
    {
        LamaPon::Logger::Instance().Info("何かにぶつかりました");
    }

private:
    LamaPon::RigidbodyComponent* m_body{};
};

LAMAPON_SCRIPT(Player);
```

---

## GameObject操作

### CreateGameObject / Instantiate / Destroy

**宣言**

```cpp
GameObject& CreateGameObject(std::string name);
GameObject& Instantiate(const std::filesystem::path& prefabPath,
                        GameObject* parent = nullptr);
bool Destroy(GameObject& gameObject);
```

**概略**

GameObjectを作る・Prefabから配置する・削除します。

**引数**

| 引数 | 説明 |
|---|---|
| `name` | 新しいGameObjectの名前。Hierarchyに表示されます |
| `prefabPath` | `"prefabs/enemy.prefab.json"`のようなassetsからの相対パス |
| `parent` | 親にするGameObject。`nullptr`を渡すとシーンルートに置かれます |

**戻り値**

| 関数 | 戻り値 |
|---|---|
| `CreateGameObject` / `Instantiate` | 作られたGameObjectへの参照。そのまま`AddComponent`を続けられます |
| `Destroy` | 削除できたらtrue |

**サンプル**

```cpp
void Start() override
{
    // 空のGameObjectを作る
    auto& marker = CreateGameObject("目印");
    marker.GetTransform().position = { 0.0f, 3.0f, 0.0f };

    // Prefabから弾を出す
    auto& bullet = Instantiate("prefabs/bullet.prefab.json");
    bullet.GetTransform().position = GetTransform().position;
}

void OnTriggerEnter(
    const LamaPon::CollisionEvent& event) override
{
    // ぶつかった相手を消す
    Destroy(event.other);
}
```

---

### LoadDataAsset

**宣言**

```cpp
std::shared_ptr<const LamaPon::DataAsset> LoadDataAsset(
    const std::filesystem::path& path) const;
```

**概略**

データアセット（`*.asset.json`）を読み込みます。UnityのScriptableObject
に相当する、GameObjectへぶら下がらないデータの入れ物です。
型は`LAMAPON_DATA_ASSET`で宣言し、値はエディターのインスペクターで
編集します。詳しくは[データアセット](data-assets.md)を参照してください。

**引数**

| 引数 | 説明 |
|---|---|
| `path` | `"data/ゴブリン.asset.json"`のようなassetsからの相対パス |

**戻り値**

読み込んだデータアセット。**nullになることはありません**。ファイルが
無い・壊れている場合は空のデータアセットが返り、`GetInt`などは既定値を
返します（Consoleへ警告が出ます）。同じパスは2回目以降キャッシュから
返るため、`Update`から呼んでもファイルは読み直されません。

**主な取り出し関数**

| 関数 | 戻り値 |
|---|---|
| `GetBool(key, 既定値)` / `GetInt` / `GetFloat` | `bool` / `int` / `float` |
| `GetText(key, 既定値)` | `std::string` |
| `GetVector2/3/4(key, 既定値)` / `GetColor` | `XMFLOAT2/3/4` |
| `GetAssetPath(key)` | `std::filesystem::path`（他アセットへの参照） |
| `Count(key)` / `Item(key, index)` | 並び（list）の要素数と要素 |
| `GetIntAt` / `GetFloatAt` / `GetTextAt` / `GetBoolAt` | 数値・文字列だけの並びの要素 |
| `Has(key)` / `TypeName()` / `Name()` | 有無・型名・ファイル名 |

**サンプル**

```cpp
void Start() override
{
    const auto enemy = LoadDataAsset("data/ゴブリン.asset.json");

    m_hitPoints = enemy->GetInt("hitPoints");
    m_speed = enemy->GetFloat("moveSpeed");

    // 参照しているPrefabをそのまま出す
    auto& body = Instantiate(enemy->GetAssetPath("prefab"));
    body.SetName(enemy->GetText("displayName"));

    // 並び（list）を回す
    for (std::size_t i = 0; i < enemy->Count("dropRates"); ++i)
    {
        m_dropRates.push_back(enemy->GetFloatAt("dropRates", i));
    }
}
```

---

### Find / FindWithTag / FindObjectsWithTag

**宣言**

```cpp
GameObject* Find(std::string_view name) const noexcept;
GameObject* FindWithTag(std::string_view tag) const noexcept;
std::vector<GameObject*> FindObjectsWithTag(std::string_view tag) const;
```

**概略**

シーン全体からGameObjectを探します。

**引数**

| 引数 | 説明 |
|---|---|
| `name` | Hierarchyに表示されている名前。**完全一致**です |
| `tag` | プロジェクト設定で登録したタグ名（`"Player"`、`"Enemy"`など） |

**戻り値**

| 関数 | 戻り値 |
|---|---|
| `Find` / `FindWithTag` | 見つかったGameObjectへのポインター。無ければ`nullptr`。**使う前に必ずnullチェック**を |
| `FindObjectsWithTag` | 見つかった全GameObjectの`vector`。無ければ空（`nullptr`にはなりません） |

**解説**

どれもシーン全体を1つずつ調べるため、**毎フレーム呼ぶと重くなります**。
`Start`で1回だけ探して、結果をメンバー変数に持つのが基本です。

タグはプロジェクト設定で登録しておくとInspectorのドロップダウンに出ます。

**サンプル**

```cpp
void Start() override
{
    // 結果を覚えておく（毎フレーム探さない）
    m_player = FindWithTag("Player");
    m_enemies = FindObjectsWithTag("Enemy");
}

void Update(float) override
{
    if (m_player == nullptr)
    {
        return;
    }
    const auto target = m_player->GetTransform().position;
    // ...
}

private:
    LamaPon::GameObject* m_player{};
    std::vector<LamaPon::GameObject*> m_enemies;
```

---

### GameObjectの主なメソッド

**宣言**

```cpp
const std::string& Name() const noexcept;
void SetName(std::string name);
const std::string& Tag() const noexcept;
void SetTag(std::string tag);
bool CompareTag(std::string_view tag) const noexcept;

bool IsEnabled() const noexcept;
void SetEnabled(bool enabled);
bool IsActiveInHierarchy() const noexcept;

void SetParent(GameObject* parent);
GameObject* Parent() const noexcept;
const std::vector<GameObject*>& Children() const noexcept;
GameObject* FindChild(std::string_view path) const noexcept;

void TranslateWorld(const DirectX::XMFLOAT3& displacement) noexcept;
void RotateWorld(const DirectX::XMFLOAT3& radians) noexcept;
DirectX::XMMATRIX WorldMatrix() const noexcept;
```

**引数と戻り値**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `Name` / `SetName` | `name` : 名前 | Hierarchyに出る名前です |
| `Tag` / `SetTag` | `tag` : タグ名 | 種類分けの札。プロジェクト設定で登録した名前を使います |
| `CompareTag` | `tag` : 比べたいタグ名 | 一致すればtrue。`Tag() == "Enemy"`より安全で速いです |
| `IsEnabled` / `SetEnabled` | `enabled` : true/false | falseにすると自分と**子孫すべて**が止まります |
| `IsActiveInHierarchy` | なし | 自分と先祖が全部有効ならtrue。**実際に動いているか**の判定はこちら |
| `SetParent` | `parent` : 親にするGameObject（`nullptr`でルートへ） | 親子関係を変えます。**ワールド位置は保たれません**（`position`が親からの相対に変わるため） |
| `Parent` | なし | 親。ルート直下なら`nullptr` |
| `Children` | なし | 直接の子の一覧（孫は含みません） |
| `FindChild` | `path` : `"腕/手/武器"`のような名前のパス | 子を名前でたどります。無ければ`nullptr` |
| `TranslateWorld` | `displacement` : 動かす量 | 親の回転に影響されず、ワールド軸で動かします |
| `RotateWorld` | `radians` : 回転ベクトル（ラジアン） | 長さを角度、向きをワールド軸としてクォータニオンで合成します。度数法は`DirectX::XMConvertToRadians`で変換 |
| `WorldMatrix` | なし | 親の変換まで含んだ最終的な行列 |

**解説**

`SetEnabled(false)`にすると、そのGameObjectと**子孫すべて**が更新・描画・物理から外れます。
実際に動いているかは`IsActiveInHierarchy()`（自分と先祖が全部有効ならtrue）で判定します。

`FindChild`は子を名前でたどります。
`"腕/手/武器"`のようにスラッシュで孫以降も指定できます（非アクティブな子も見つかります）。

⚠️ `SetParent`で**自分自身や自分の子孫を親に指定すると例外**が投げられます（親子関係が輪になってしまうため）。
動的に付け替えるときは注意してください。

**サンプル**

```cpp
void Start() override
{
    // 名前とタグ
    Owner().SetName("勇者");
    Owner().SetTag("Player");

    // 深い階層の子を取る
    if (auto* weapon = Owner().FindChild("腕/手/武器"))
    {
        weapon->SetEnabled(false);   // 最初は武器を隠す
    }
}

void OnCollisionEnter(
    const LamaPon::CollisionEvent& event) override
{
    // タグで相手を判別する
    if (event.other.CompareTag("Enemy"))
    {
        LamaPon::Logger::Instance().Info("敵に当たった！");
    }
}
```

---

## Transform

### Transform構造体

**宣言**

```cpp
struct Transform final
{
    DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
    // 回転の正本。単位クォータニオン（x, y, z, w）
    DirectX::XMFLOAT4 rotationQuaternion{
        0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

    // オイラー角（ラジアン。x=ピッチ, y=ヨー, z=ロール）
    DirectX::XMFLOAT3 EulerAngles() const noexcept;
    void SetEulerAngles(const DirectX::XMFLOAT3&) noexcept;
    void SetEulerAngles(
        float pitch, float yaw, float roll) noexcept;

    // 回転を後から合成する
    void Rotate(
        const DirectX::XMFLOAT3& axis,
        float radians) noexcept;
    void RotateEuler(
        const DirectX::XMFLOAT3& radians) noexcept;
};

Transform& GetTransform() noexcept;   // Scriptのショートカット
```

**概略**

位置・回転・拡縮です。
位置と拡縮は直接書き換えます。
**回転はクォータニオンが正本**で、オイラー角は関数を通して読み書きします。

**メンバー**

| メンバー | 説明 |
|---|---|
| `position` | 位置。Xが右、Yが上、Zが奥（左手系）です |
| `rotationQuaternion` | 回転の正本。単位クォータニオン。**通常は直接触りません** |
| `scale` | 各軸の拡大率。`{1,1,1}`が元の大きさ、`{2,1,1}`で横だけ2倍 |
| `EulerAngles()` | 現在の回転をオイラー角（**ラジアン**）で取得 |
| `SetEulerAngles(...)` | オイラー角で回転を設定 |
| `Rotate(axis, radians)` | 指定軸まわりに回転を**足す** |
| `RotateEuler({x,y,z})` | オイラー角ぶんの回転を**足す** |

**解説**

角度は**ラジアン**です。
度で指定したいときは`DirectX::XMConvertToRadians(90.0f)`で変換します。

**なぜクォータニオンなのか**: オイラー角だとピッチ±90度でヨーとロールが縮退して独立に回せなくなります（ジンバルロック）。
また補間が最短経路を通らず、途中で軸が振れます。
クォータニオンにはどちらの問題もありません。

**「回転を足す」ときは `Rotate` か `RotateEuler` を使ってください。**`EulerAngles()` は読み取り専用なので、`EulerAngles().y += ...` のような書き方はできません（コンパイルエラーになります）。
オイラー角を取得して足して設定し直すやり方も、順序依存の誤差が溜まるため勧めません。

```cpp
// NG（コンパイルできない）
transform.EulerAngles().y += delta;

// OK（回転として合成される）
transform.Rotate({ 0.0f, 1.0f, 0.0f }, delta);
```

親がいる場合、この値は**親から見た相対位置**になります。
ワールド座標で動かしたいときは`Owner().TranslateWorld()`を使います。

**サンプル**

```cpp
void Update(const float deltaTime) override
{
    auto& transform = GetTransform();

    // 上へ移動（毎秒2単位）
    transform.position.y += 2.0f * deltaTime;

    // Y軸まわりに毎秒90度回転
    transform.Rotate(
        { 0.0f, 1.0f, 0.0f },
        DirectX::XMConvertToRadians(90.0f) * deltaTime);

    // 大きさを1.5倍に
    transform.scale = { 1.5f, 1.5f, 1.5f };

    // ワールド基準で動かす（親の回転に影響されない）
    Owner().TranslateWorld({ 0.0f, 0.0f, deltaTime });
}
```

---

## コンポーネント操作

### GetComponent / AddComponent

**宣言**

```cpp
template<typename T> T* GetComponent() noexcept;
template<typename T> T* GetComponentInChildren(bool includeInactive = false) noexcept;
template<typename T> T* GetComponentInParent(bool includeInactive = false) noexcept;
template<typename T, typename... Args> T& AddComponent(Args&&... args);
```

**概略**

コンポーネントを取得・追加します。

**引数**

| 引数 | 説明 |
|---|---|
| `T`（テンプレート引数） | 探すコンポーネントの型（`LamaPon::RigidbodyComponent`など） |
| `includeInactive` | `GetComponentInChildren`系のみ。trueで非アクティブな子も探します（既定はfalse） |
| `args...` | `AddComponent`のみ。そのコンポーネントのコンストラクタへそのまま渡されます |

**戻り値**

| 関数 | 戻り値 |
|---|---|
| `GetComponent`系 | 見つかればポインター、無ければ`nullptr`。**nullチェックが必要**です |
| `AddComponent` | 追加したコンポーネントへの**参照**。`nullptr`にはならないのでそのまま使えます |

**解説**

`GetComponentInChildren`は**自分自身も含めて**子を深さ優先で探します。
自分自身も検索対象です。
既定では非アクティブな階層を飛ばすので、隠しているオブジェクトも対象にしたい場合は`true`を渡します。

複数まとめて取る`GetComponentsInChildren`は`Owner()`経由で呼びます。

**サンプル**

```cpp
void Start() override
{
    // 取得（無いかもしれないので必ずnullptr確認）
    if (auto* body = GetComponent<LamaPon::RigidbodyComponent>())
    {
        body->SetMass(2.0f);
    }

    // 追加（引数はコンポーネントのコンストラクタへ渡る）
    auto& audio = AddComponent<LamaPon::AudioSourceComponent>(
        std::filesystem::path{ "audio/hit.wav" },
        0.8f);

    // 子から探す（自分自身も対象）
    auto* effect = GetComponentInChildren<
        LamaPon::ParticleSystemComponent>();

    // 子を全部まとめて取る場合はOwner()から
    auto lights = Owner().GetComponentsInChildren<
        LamaPon::PointLightComponent>();
}
```

---

### GetScript / GetScriptInChildren

**宣言**

```cpp
template<typename T> T* GetScript() const noexcept;
template<typename T> T* GetScriptInChildren(bool includeInactive = false) const noexcept;
```

**概略**

自分で書いたスクリプトを、その型や**自作インターフェース**で取得します。
`GetComponent`はコンポーネント型しか受け取れないため、スクリプト同士を疎結合につなぐときはこちらを使います。

**引数**

| 引数 | 説明 |
|---|---|
| `T`（テンプレート引数） | 探すスクリプトの型、またはそれが実装するインターフェース |
| `includeInactive` | `GetScriptInChildren`のみ。trueで非アクティブな子も探します（既定はfalse） |

**戻り値**

見つかれば`T*`、無ければ`nullptr`。
**nullチェックが必要**です。

**解説**

インターフェースはエンジンへ登録しない普通のC++の型で構いません。
実装側は`LamaPon::Script`と一緒に継承し、順番はどちらが先でも動きます。
`GetScriptInChildren`は`GetComponentInChildren`と同じく自分自身も対象にします。
同じ関数は`GameObject`にもあるので、他のオブジェクトに対しても呼べます。

インターフェースを追加・変更したらGame Moduleのビルドが必要です（`assets`以下の`.cpp`／`.h`を保存すると自動でビルドされます）。

**サンプル**

```cpp
// assets/scripts/IDamageable.h
struct IDamageable
{
    virtual ~IDamageable() = default;
    virtual void ApplyDamage(int amount) = 0;
};

// 実装側（LamaPon::Scriptは先頭でなくてよい）
class Enemy final
    : public IDamageable
    , public LamaPon::Script
{
public:
    void ApplyDamage(int amount) override { m_health -= amount; }

private:
    int m_health{ 100 };
};

LAMAPON_SCRIPT(Enemy);

// 呼ぶ側はEnemyを知らなくてよい
void Bullet::OnCollisionEnter(const LamaPon::CollisionEvent& event)
{
    if (auto* target = event.other.GetScript<IDamageable>())
    {
        target->ApplyDamage(25);
    }
}
```

---

## 入力

### Value / IsDown / WasPressed / WasReleased

**宣言**

```cpp
float Value(std::string_view action) const noexcept;
bool IsDown(std::string_view action, float threshold = 0.5f) const noexcept;
bool WasPressed(std::string_view action, float threshold = 0.5f) const noexcept;
bool WasReleased(std::string_view action, float threshold = 0.5f) const noexcept;
```

**概略**

プロジェクト設定で定義したInput Actionの状態を調べます。
`Graphics().Input()`から呼びます。

**引数**

| 引数 | 説明 |
|---|---|
| `action` | Action名。**未登録の名前を渡しても落ちず、常に0／falseが返ります**（typoに注意） |
| `threshold` | 押したと見なす値のしきい値。スティックの遊びを無視したいときに上げます |

**戻り値**

| 関数 | 戻り値 |
|---|---|
| `Value` | `-1.0`〜`1.0`。キーボードは0か±1、スティックは中間値も返ります |
| `IsPressed` / `WasPressed` / `WasReleased` | 条件を満たせばtrue |

**解説**

既定のActionは`MoveHorizontal` / `MoveVertical` / `LookHorizontal` /`LookVertical` / `Jump` / `Submit` / `Cancel` / `Fire` / `AltFire`です。

**サンプル**

```cpp
void Update(const float deltaTime) override
{
    auto& input = Graphics().Input();

    // 移動（-1〜1、キーとスティックの両対応）
    const float x = input.Value("MoveHorizontal");
    const float z = input.Value("MoveVertical");
    GetTransform().position.x += x * 5.0f * deltaTime;
    GetTransform().position.z += z * 5.0f * deltaTime;

    // 押した瞬間だけ
    if (input.WasPressed("Jump"))
    {
        LamaPon::Logger::Instance().Info("ジャンプ");
    }

    // 押している間ずっと
    if (input.IsDown("Fire"))
    {
        // 連射
    }
}
```

---

### Pointer（マウス）

**宣言**

```cpp
const InputPointerState& Pointer() const noexcept;

struct InputPointerState final
{
    DirectX::XMFLOAT2 position{};
    bool valid{};
    bool down{}, pressed{}, released{};   // 左ボタン相当
    DirectX::XMFLOAT2 delta{};
    float wheel{}, wheelHorizontal{};
    const InputPointerButtonState& Button(PointerButton button) const noexcept;
};

struct InputPointerButtonState final
{
    bool down{}, pressed{}, released{};
};

enum class PointerButton : std::uint8_t
{ Left, Right, Middle, Extra1, Extra2, Count };
```

**概略**

マウスカーソルの位置・ボタン・ホイールです。

**メンバー**

| メンバー | 説明 |
|---|---|
| `position` | カーソルの位置（ピクセル）。画面左上が`{0,0}` |
| `valid` | カーソルがウィンドウ内にあればtrue。**最初にこれを確認**します |
| `down` | 左ボタンを**押している間**ずっとtrue |
| `pressed` | 左ボタンを**押した瞬間**のフレームだけtrue |
| `released` | 左ボタンを**離した瞬間**のフレームだけtrue |
| `delta` | 前のフレームからの移動量。カメラ回転に使います |
| `wheel` | ホイールの回転量。手前に回すとマイナス |
| `wheelHorizontal` | 横スクロールの量（対応マウスのみ） |
| `Button(button)` | 左以外のボタンの状態。`PointerButton::Right`などを渡します |

`PointerButton`は`Left` / `Right` / `Middle` / `Extra1` / `Extra2`です。

**サンプル**

```cpp
void Update(float) override
{
    const auto& pointer = Graphics().Input().Pointer();
    if (!pointer.valid)
    {
        return;   // 画面外
    }

    // 左クリックした瞬間
    if (pointer.pressed)
    {
        LamaPon::Logger::Instance().Info(
            "クリック位置: "
            + std::to_string(pointer.position.x));
    }

    // 右クリック
    if (pointer.Button(LamaPon::PointerButton::Right).pressed)
    {
        // メニューを開くなど
    }

    // ホイールでズーム
    if (pointer.wheel != 0.0f)
    {
        GetTransform().position.z += pointer.wheel * 0.01f;
    }
}
```

---

## 時間

### Time名前空間

**宣言**

```cpp
float LamaPon::Time::DeltaTime() noexcept;
float LamaPon::Time::UnscaledDeltaTime() noexcept;
double LamaPon::Time::TimeSinceStartup() noexcept;
double LamaPon::Time::UnscaledTimeSinceStartup() noexcept;
std::uint64_t LamaPon::Time::FrameCount() noexcept;
float LamaPon::Time::TimeScale() noexcept;
void LamaPon::Time::SetTimeScale(float scale) noexcept;
bool LamaPon::Time::IsPaused() noexcept;
constexpr float LamaPon::Time::FixedDeltaTime() noexcept;   // 1/60
```

**概略**

経過時間の取得と、ポーズ・スロー再生の制御です。

**関数**

| 関数 | 引数 | 戻り値・説明 |
|---|---|---|
| `DeltaTime` | なし | 前フレームからの経過秒数。**`TimeScale`が掛かった後**の値です |
| `UnscaledDeltaTime` | なし | 経過秒数（`TimeScale`の影響なし）。ポーズ中も進みます |
| `TimeSinceStartup` | なし | 起動からの累計秒数（`TimeScale`適用後） |
| `UnscaledTimeSinceStartup` | なし | 起動からの実時間の秒数 |
| `FrameCount` | なし | 起動からのフレーム数 |
| `TimeScale` | なし | 現在の時間倍率。1.0が通常 |
| `SetTimeScale` | `scale` : 倍率 | 0で停止、0.5でスロー、2.0で倍速。**0〜100に丸められます** |
| `IsPaused` | なし | `TimeScale`が0ならtrue |
| `FixedDeltaTime` | なし | 固定更新の間隔。常に1/60秒 |

**解説**

`SetTimeScale(0.0f)`で**ゲームだけを止められます**（0〜100にクランプ）。
UIやポーズメニューを動かし続けたい場合は`UnscaledDeltaTime()`を使います。

`Update`の引数`deltaTime`は`Time::DeltaTime()`と同じ値です。

**サンプル**

```cpp
void Update(float) override
{
    // Pキーでポーズ切り替え
    if (Graphics().Input().KeyboardState().P)
    {
        LamaPon::Time::SetTimeScale(
            LamaPon::Time::IsPaused() ? 1.0f : 0.0f);
    }
}

// スローモーション演出（3秒かけて戻す）
LamaPon::Coroutine SlowMotion()
{
    LamaPon::Time::SetTimeScale(0.25f);
    co_await LamaPon::WaitForSeconds{ 1.0f };
    LamaPon::Time::SetTimeScale(1.0f);
}
```

---

## コルーチンとタイマー

### StartCoroutine

**宣言**

```cpp
std::uint64_t StartCoroutine(Coroutine coroutine);
void StopCoroutine(std::uint64_t handle);
void StopAllCoroutines();

// co_awaitできる待機（この4つだけ）
struct WaitForSeconds final { float seconds{}; };
struct WaitForNextFrame final { };
struct WaitUntil final { std::function<bool()> condition; };
struct WaitWhile final { std::function<bool()> condition; };
```

**概略**

「1秒待ってから次の処理」のような時間のかかる流れを、**1つの関数として順番に書けます**。

**引数**

| 引数 | 説明 |
|---|---|
| `coroutine` | `LamaPon::Coroutine`を返すメンバー関数の呼び出し。`StartCoroutine(Intro())`のように書きます |
| `handle` | `StartCoroutine`が返したハンドル。止めたいコルーチンを指定します |

**待機の種類（`co_await`に渡せるのはこの4つだけ）**

| 型 | メンバー | 説明 |
|---|---|---|
| `WaitForSeconds` | `seconds` : 待つ秒数 | 指定秒だけ待ちます。`co_await LamaPon::WaitForSeconds{ 2.0f };` |
| `WaitForNextFrame` | なし | 1フレームだけ待ちます |
| `WaitUntil` | `condition` : 条件 | 条件が**trueになるまで**待ちます |
| `WaitWhile` | `condition` : 条件 | 条件が**falseになるまで**待ちます |

**戻り値**

コルーチンのハンドル。
`StopCoroutine`で止めるときに使います。
最初の`co_await`まで到達せずに終わった場合は0です。

**解説**

戻り値の型を`LamaPon::Coroutine`にした**メンバー関数**を作り、`StartCoroutine(関数名())`で開始します。
待機は`WaitForSeconds`、`WaitForNextFrame`、`WaitUntil`、`WaitWhile`の4種類だけです。

待ち時間はゲーム時間なので、`SetTimeScale(0)`のポーズ中は進みません。
スクリプトが破棄されると自動的に全部止まります。

**サンプル**

```cpp
class Boss final : public LamaPon::Script
{
public:
    void Start() override
    {
        StartCoroutine(AttackLoop());
    }

private:
    LamaPon::Coroutine AttackLoop()
    {
        while (true)
        {
            // 溜め
            SetColor({ 1.0f, 0.5f, 0.0f, 1.0f });
            co_await LamaPon::WaitForSeconds{ 1.5f };

            // 攻撃
            Fire();
            co_await LamaPon::WaitForSeconds{ 0.2f };

            // プレイヤーが範囲内に来るまで待つ
            co_await LamaPon::WaitUntil{
                [this] { return IsPlayerNear(); } };
        }
    }
};
```

---

### Invoke / InvokeRepeating

**宣言**

```cpp
std::uint64_t Invoke(float delaySeconds, std::function<void()> callback);
std::uint64_t InvokeRepeating(float delaySeconds,
                              float intervalSeconds,
                              std::function<void()> callback);
void CancelInvoke(std::uint64_t handle);
void CancelAllInvokes();
```

**概略**

指定秒後に1回、または一定間隔で繰り返し処理を呼びます。

**引数**

| 引数 | 説明 |
|---|---|
| `delaySeconds` | 最初に呼ぶまでの待ち時間（秒）。負の値を渡すと0として扱われます |
| `intervalSeconds` | 2回目以降の間隔（秒）。0を渡しても最小0.001秒になります（無限ループ防止） |
| `callback` | 時間が来たら呼ばれる処理。**引数の最後**に書きます。ラムダが便利です |
| `handle` | `CancelInvoke`で止めたいタイマーのハンドル |

**戻り値**

タイマーのハンドル。
`CancelInvoke`で止めるときに使います。
止める予定がなければ無視してかまいません。

**サンプル**

```cpp
void Start() override
{
    // 3秒後に自分を消す（弾やエフェクト向け）
    Invoke(3.0f, [this] { Destroy(Owner()); });

    // 0.5秒ごとに敵を出す
    m_spawner = InvokeRepeating(1.0f, 0.5f,
        [this] { SpawnEnemy(); });
}

void StopWave()
{
    CancelInvoke(m_spawner);   // 湧きを止める
}

private:
    std::uint64_t m_spawner{};
```

---

## イベント

### On / Emit

**宣言**

```cpp
std::uint64_t On(std::string_view eventName,
                 std::function<void(const EventArgs&)> handler);
std::uint64_t On(std::string_view eventName,
                 std::function<void()> handler);
void Off(std::uint64_t handle);
void Emit(std::string_view eventName);
void Emit(std::string_view eventName, EventArgs eventArgs);

struct EventArgs final
{
    GameObject* sender{};
    float number{};
    std::string text;
};
```

**概略**

スクリプト同士が**お互いを知らないまま**通知をやり取りできます（敵が死んだ→スコア加算、など）。

**引数**

| 引数 | 説明 |
|---|---|
| `eventName` | イベントの名前。発行側と購読側で**同じ文字列**にします（typoに注意） |
| `handler` | 通知が来たときに呼ばれる処理。引数ありと引数なし、どちらの形でも書けます |
| `handle` | `On`が返したハンドル。`Off`で購読をやめるときに使います |
| `eventArgs` | 一緒に送る値。省略できます |

**EventArgsのメンバー**

| メンバー | 説明 |
|---|---|
| `sender` | 発行したGameObject。`Emit`では**自動で自分**が入ります |
| `number` | 数値を1つ送れます（スコア、ダメージ量など） |
| `text` | 文字列を1つ送れます |

**戻り値**

`On`は購読ハンドルを返します。
`Off`で手動解除するとき以外は無視してかまいません。

**解説**

`On`で購読、`Emit`で発行します。
購読はスクリプトが壊れるときに自動で解除されるので、`Off`を呼び忘れても大丈夫です。

UI Buttonの「クリック時のイベント」に名前を入れておくと、ボタン側にコードを書かずに`On`で受け取れます。

**サンプル**

```cpp
// 受け取る側（スコア表示）
void Start() override
{
    On("EnemyDied", [this](const LamaPon::EventArgs& args)
    {
        m_score += static_cast<int>(args.number);
        UpdateLabel();
    });

    On("GameOver", [this] { ShowResult(); });
}

// 発行する側（敵）
void Die()
{
    LamaPon::EventArgs args;
    args.number = 100.0f;      // スコア
    args.text = "スライム";
    Emit("EnemyDied", args);
    Destroy(Owner());
}
```

---

## シーン

### シーンの切り替え

**宣言**

```cpp
[[nodiscard]] bool RequestLoad(std::filesystem::path scenePath);
[[nodiscard]] bool RequestLoadAsync(std::filesystem::path scenePath);
[[nodiscard]] bool RequestReload();
[[nodiscard]] bool RequestReloadAsync();
void CancelPending() noexcept;
bool IsLoading() const noexcept;
float LoadProgress() const noexcept;
const std::string& LastError() const noexcept;
```

**概略**

別のシーンへ移動します。
`GetScene().Scenes()`から呼びます。

**引数**

| 引数 | 説明 |
|---|---|
| `scenePath` | `"scenes/stage-02.scene.json"`のようなassetsからの相対パス。フルパスではありません |

**戻り値**

要求を受け付ければtrue。
**falseのときは`LastError()`に理由が入ります**（読み込み中に重ねて要求した、パスが空、など）。
戻り値は必ず確認してください（`[[nodiscard]]`です）。

**解説**

切り替えは**次のフレームの先頭**で実行されます（更新中にオブジェクトを壊さないため）。
大きなシーンは`Async`版を使うと、読み込み中も現在のシーンが動き続け、標準のローディング画面が出ます。

**サンプル**

```cpp
void GoToNextStage()
{
    auto& scenes = GetScene().Scenes();
    if (!scenes.RequestLoadAsync("scenes/stage-02.scene.json"))
    {
        LamaPon::Logger::Instance().Error(
            "シーンを読み込めません: " + scenes.LastError());
    }
}

void Update(float) override
{
    // 読み込みの進捗を見る
    const auto& scenes = GetScene().Scenes();
    if (scenes.IsLoading())
    {
        const float progress = scenes.LoadProgress();  // 0〜1
    }
}
```

---

### レンダーテクスチャ（ミニマップ・防犯カメラ）

**宣言**

```cpp
// CameraComponent
void SetTargetTexture(std::string name);        // 空で通常の画面描画
void SetTargetTextureSize(std::uint32_t width, std::uint32_t height);
void SetTargetClearColor(const DirectX::XMFLOAT4& color);

// SpriteRendererComponent / UIImageComponent
void SetRenderTexture(std::string name);
```

**概略**

Cameraの映像をテクスチャへ描き、SpriteやUIへ貼ります。

**引数**

| 引数 | 説明 |
|---|---|
| `name` | 描画先の名前。Camera側と表示側で同じ文字列にすると繋がります |
| `width` / `height` | テクスチャの解像度（既定512x512）。小さいほど軽いです |
| `color` | 毎フレームの塗りつぶし色。アルファを下げると半透明になります |

**解説**

2D／UIはテクスチャへ描かれません（表示中のSprite自身が写り込むのを防ぐため）。
Bloom・トーンマッピング・FXAAは品質設定に従って適用されます。
Scene切り替えでテクスチャは破棄され、必要な分だけ作り直されます。

**サンプル**

```cpp
auto& minimap = GetScene().CreateGameObject("Minimap Camera");
minimap.GetTransform().position = { 0.0f, 40.0f, 0.0f };
minimap.GetTransform().SetEulerAngles(
    { 1.5f, 0.0f, 0.0f });
auto& camera = minimap.AddComponent<LamaPon::CameraComponent>();
camera.SetTargetTexture("minimap");
camera.SetTargetTextureSize(256, 256);

auto& hud = GetScene().CreateGameObject("Minimap HUD");
auto& sprite = hud.AddComponent<LamaPon::SpriteRendererComponent>(
    DirectX::XMFLOAT2{ 200.0f, 200.0f });
sprite.SetRenderTexture("minimap");
```

---

### シーンの追加読み込み（Additive）

**宣言**

```cpp
// SceneManager（GetScene().Scenes()）
[[nodiscard]] bool RequestLoadAdditive(std::filesystem::path scenePath);
[[nodiscard]] bool RequestLoadAdditiveAsync(std::filesystem::path scenePath);
[[nodiscard]] bool RequestUnload(std::filesystem::path scenePath);

// Scene（GetScene()）。こちらは即時に反映されます
SceneHandle MergeFromFile(const std::filesystem::path& path);
bool UnloadScene(SceneHandle handle);
bool UnloadScene(const std::filesystem::path& path);
void UnloadAllAdditiveScenes();
const std::vector<LoadedSceneInfo>& AdditiveScenes() const noexcept;
```

**概略**

今のシーンを消さずに、別のシーンを重ねて読み込みます。
常駐UI、ポーズ画面、ステージの分割読み込みに使います。

**引数**

| 引数 | 説明 |
|---|---|
| `scenePath` / `path` | `"scenes/hud.scene.json"`のようなassetsからの相対パス |
| `handle` | `MergeFromFile`が返した番号。その追加シーンだけを破棄するときに渡します |

**戻り値**

`Request系`は要求を受け付ければtrue（falseの理由は`LastError()`）。
`MergeFromFile`は追加シーンの`SceneHandle`。
`UnloadScene`は破棄できればtrue。

**解説**

追加したGameObjectのIDは、既存のシーンと衝突しないよう振り直されます。
環境設定（空・霧・Bloom）とMain Cameraは主シーンのものが残り、シーンを保存しても追加分は書き出されません。
`RequestLoad`で切り替えると追加分もまとめて破棄されます。
どのシーン由来かは`GameObject::SourceScene()`で確認できます（0が主シーン）。

**サンプル**

```cpp
void ShowPauseMenu()
{
    auto& scenes = GetScene().Scenes();
    if (!scenes.RequestLoadAdditive("scenes/pause.scene.json"))
    {
        LamaPon::Logger::Instance().Error(
            "ポーズ画面を開けません: " + scenes.LastError());
    }
}

void HidePauseMenu()
{
    static_cast<void>(
        GetScene().Scenes().RequestUnload("scenes/pause.scene.json"));
}
```

---

### シーンをまたいで値を残す

**宣言**

```cpp
RuntimeGameState& State() noexcept;   // GetScene().Scenes().State()

void SetInteger(std::string key, std::int64_t value);
void SetNumber(std::string key, double value);
void SetBoolean(std::string key, bool value);
void SetString(std::string key, std::string value);
std::int64_t Integer(std::string_view key, std::int64_t fallback = 0) const noexcept;
double Number(std::string_view key, double fallback = 0.0) const noexcept;
bool Boolean(std::string_view key, bool fallback = false) const noexcept;
std::string String(std::string_view key, std::string fallback = {}) const;
```

**概略**

シーンを切り替えても消えない値です（スコアや所持アイテムなど）。

**引数**

| 引数 | 説明 |
|---|---|
| `key` | 値につける名前。`"score"`のように自由に決めます |
| `value` | 保存する値。整数・小数・真偽値・文字列の4種類が使えます |
| `fallback` | **キーが無いとき、または型が違うときに返る値**。省略時は0／false／空文字列 |

**戻り値**

取り出し側（`Integer`など）は保存されている値を返します。
キーが無ければ`fallback`が返るので、`Has〜`のような存在確認は不要です。

**解説**

GameObjectはシーン切り替えで消えるので、**数値やフラグはここに置きます**。
アプリを終了すると消えるので、次回起動時にも残したいものはPlayerPrefs／SaveDataを使います。

GameObjectごと残したい場合は`GetScene().DontDestroyOnLoad(オブジェクト)`です。

**サンプル**

```cpp
// ステージクリア時
void OnClear()
{
    auto& state = GetScene().Scenes().State();
    state.SetInteger("score",
        state.Integer("score", 0) + 500);
    state.SetBoolean("hasKey", true);
}

// 次のシーンで読む
void Start() override
{
    const auto score =
        GetScene().Scenes().State().Integer("score", 0);
}
```

---

## 物理（クエリ）

### Raycast（光線を飛ばして当たりを調べる）

**宣言**

```cpp
[[nodiscard]] bool Raycast(const Ray& ray, float maximumDistance,
                           PhysicsHit& hit,
                           const PhysicsQueryFilter& filter = {}) const;
[[nodiscard]] std::vector<PhysicsHit> RaycastAll(const Ray& ray,
                           float maximumDistance,
                           const PhysicsQueryFilter& filter = {}) const;
[[nodiscard]] bool SphereCast(const Ray& ray, float radius,
                           float maximumDistance, PhysicsHit& hit,
                           const PhysicsQueryFilter& filter = {}) const;
[[nodiscard]] std::vector<PhysicsOverlapHit> OverlapSphere(
                           const DirectX::XMFLOAT3& center, float radius,
                           const PhysicsQueryFilter& filter = {}) const;

struct Ray final
{
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction;
};

struct PhysicsHit final
{
    GameObject* gameObject{};
    DirectX::XMFLOAT3 point{};
    DirectX::XMFLOAT3 normal{};
    float distance{};
    // 以下、当たったコライダー種別に応じて片方が入る
    BoxCollider3DComponent* collider{};
    CapsuleCollider3DComponent* capsuleCollider{};
    SphereCollider3DComponent* sphereCollider{};
    ConvexHullCollider3DComponent* hullCollider{};
    MeshCollider3DComponent* meshCollider{};
};

struct PhysicsQueryFilter final
{
    std::uint32_t layerMask{ 0xffffffffu };
    bool includeTriggers{};
    std::uint64_t ignoredGameObjectId{};
};
```

**概略**

指定した向きへ光線を飛ばし、最初に当たったものを調べます（銃の弾道、足元の地面チェック、視線判定など）。

**引数**

| 引数 | 説明 |
|---|---|
| `ray` | 光線。`{ 始点, 方向 }`で作ります。**方向は正規化してください** |
| `hit` | 当たった情報を受け取る変数。当たったときだけ書き込まれます |
| `filter` | 無視したいGameObjectやレイヤーの指定。省略できます |

**戻り値**

当たればtrueで、`hit`に位置・法線・距離・相手のIDが入ります。
外れた場合はfalseで、`hit`の中身は変わりません。

**解説**

`Ray`は`{ 始点, 方向 }`で作ります（方向は正規化してください）。
`filter.ignoredGameObjectId`に自分のIDを入れると、自分に当たるのを防げます。

**サンプル**

```cpp
// 足元に地面があるか調べる
bool IsGrounded()
{
    LamaPon::Ray ray{
        GetTransform().position,
        { 0.0f, -1.0f, 0.0f }
    };
    LamaPon::PhysicsQueryFilter filter;
    filter.ignoredGameObjectId = Owner().Id();   // 自分は無視

    LamaPon::PhysicsHit hit;
    return GetScene().Raycast(ray, 1.1f, hit, filter);
}

// 前方を撃つ
void Shoot()
{
    const auto position = GetTransform().position;
    LamaPon::Ray ray{ position, { 0.0f, 0.0f, 1.0f } };

    LamaPon::PhysicsHit hit;
    if (GetScene().Raycast(ray, 100.0f, hit)
        && hit.gameObject != nullptr)
    {
        LamaPon::Logger::Instance().Info(
            "命中: " + hit.gameObject->Name());
        Destroy(*hit.gameObject);
    }
}

// 爆発の範囲判定
void Explode()
{
    const auto hits = GetScene().OverlapSphere(
        GetTransform().position,
        5.0f);
    for (const auto& hit : hits)
    {
        if (hit.gameObject != nullptr)
        {
            // ダメージ処理
        }
    }
}
```

---

## 描画

### MeshRendererComponent

**宣言**

```cpp
explicit MeshRendererComponent(
    PrimitiveShape shape = PrimitiveShape::Cube,
    DirectX::XMFLOAT4 color = { 0.16f, 0.65f, 0.95f, 1.0f },
    std::filesystem::path albedoTexture = {},
    std::filesystem::path normalTexture = {},
    float roughness = 0.5f,
    float normalStrength = 1.0f,
    std::filesystem::path materialAsset = {});
```

**概略**

立方体・球などの基本形状を描画します。

**引数**

| 引数 | 説明 |
|---|---|
| `shape` | 形。`PrimitiveShape::Cube` / `Sphere` / `Cylinder` / `Plane` から選びます |
| `color` | ベースカラー（RGBA、各0〜1）。テクスチャを指定した場合は掛け算されます |
| `albedoTexture` | 色を決める画像のassets相対パス。空なら`color`だけで塗ります |
| `normalTexture` | 凹凸を表現する法線マップのパス。空なら平坦になります |
| `roughness` | 表面の粗さ（0〜1）。0で光沢のある鏡面、1でざらざらのマット |
| `normalStrength` | 法線マップの効き具合。1.0が標準、0で無効と同じ |
| `materialAsset` | `.material.json`のパス。指定すると上の設定より優先され、共有Materialになります |

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `SetColor` | `color` : 色（RGBA、各0〜1） | ベースカラーを変えます。`{1,0,0,1}`で赤 |
| `SetRoughness` | `value` : 0〜1 | 表面の粗さ。0でつやつや（鏡面）、1でざらざら |
| `SetMetallic` | `value` : 0〜1 | 金属らしさ。1で金属、0で非金属 |
| `SetAlbedoTexturePath` | `path` : assets相対パス | 貼る画像を変えます |
| `SetNormalTexturePath` | `path` : assets相対パス | 凹凸を表す法線マップを設定します |
| `SetShaderPath` | `path` : `.hlsl`のパス | カスタムShaderに切り替えます |
| `SetCustomParameter` | `index` : 0〜7<br>`value` : 4つ組の値（`XMFLOAT4`） | カスタムShaderへ値を渡します。範囲外の`index`は無視されます |
| `SetMaterialAssetPath` | `path` : `.material.json` | 共有Materialを割り当てます |
| `SetWorldOverlay` | `enabled` : true/false | trueで深度を無視し、壁の向こうでも見えるようになります |
| `SetProceduralMesh` | `vertices`, `indices`, `recalculateNormals` | Scriptで作った三角形メッシュへ切り替えます。最大65536頂点 |
| `ClearProceduralMesh` | なし | 実行時メッシュを破棄し、コンストラクターで選んだ基本形状へ戻します |

**サンプル**

```cpp
void Start() override
{
    // 赤い球を持つGameObjectにする
    auto& mesh = AddComponent<LamaPon::MeshRendererComponent>(
        LamaPon::MeshRendererComponent::PrimitiveShape::Sphere,
        DirectX::XMFLOAT4{ 1.0f, 0.2f, 0.2f, 1.0f });
    mesh.SetRoughness(0.2f);   // つやつやに
    mesh.SetMetallic(1.0f);    // 金属っぽく
}
```

道路・地形などをScriptで生成する場合は、位置・法線・UVを持つ
`ProceduralMeshVertex`と、時計回りの三角形indexを渡します。頂点データは
Scene JSONへ保存されないため、`Start()`などで毎回生成してください。

```cpp
auto& road = AddComponent<LamaPon::MeshRendererComponent>(
    LamaPon::PrimitiveShape::Plane);
std::vector<LamaPon::ProceduralMeshVertex> vertices{
    { { -2, 0, 0 }, {}, { 0, 0 } },
    { {  2, 0, 0 }, {}, { 1, 0 } },
    { { -2, 0, 8 }, {}, { 0, 1 } },
    { {  2, 0, 8 }, {}, { 1, 1 } },
};
road.SetProceduralMesh(
    std::move(vertices),
    { 0, 2, 1, 1, 2, 3 },
    true); // 面から滑らかな法線を再計算
```

---

### SpriteRendererComponent

**宣言**

```cpp
explicit SpriteRendererComponent(
    DirectX::XMFLOAT2 size = { 128.0f, 128.0f },
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
    std::filesystem::path texturePath = {});
```

**概略**

2Dの画像（スプライト）を描画します。

**引数**

| 引数 | 説明 |
|---|---|
| `size` | 表示する大きさ（幅・高さ、ピクセル）。画像の実サイズとは無関係に指定できます |
| `color` | 画像に掛ける色（RGBA）。白`{1,1,1,1}`で元の色のまま、アルファを下げると半透明 |
| `texturePath` | 表示する画像のassets相対パス。空だと`color`の単色四角になります |

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `SetSize` | `size` : 幅と高さ（ピクセル） | 表示する大きさを変えます |
| `SetColor` | `color` : 色（RGBA） | 画像に色を掛けます。アルファを下げると半透明に |
| `SetTexturePath` | `path` : assets相対パス | 表示する画像を変えます |
| `SetSortOrder` | `order` : 整数 | 描画順。**大きいほど手前**に描かれます |
| `SetPivot` | `pivot` : 割合（0〜1） | 基準点。`{0,0}`で左上（既定）、`{0.5,0.5}`で中心。**位置が指す点と回転の中心**が変わります。UI Rect Transformがある場合は使われません |
| `SetSourceRect` | `rect` : `{x, y, 幅, 高さ}`（各0〜1） | 画像の一部だけを表示します |
| `SetShaderPath` | `path` : `.hlsl`のパス | カスタムShaderに切り替えます |
| `SetCustomParameter` | `index` : 0〜7<br>`value` : 4つ組の値（`XMFLOAT4`） | カスタムShaderへ値を渡します |

**解説**

`SetSourceRect`はテクスチャの一部だけを表示する指定です（値は0〜1の正規化座標）。
スプライトシートから1コマだけ出すときに使います。
コマ送りアニメーションは`SpriteAnimatorComponent`が自動でこれを更新します。

**サンプル**

```cpp
void Start() override
{
    auto& sprite = AddComponent<LamaPon::SpriteRendererComponent>(
        DirectX::XMFLOAT2{ 64.0f, 64.0f },
        DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
        std::filesystem::path{ "textures/player.png" });
    sprite.SetSortOrder(10);

    // 中心を基準にする（位置が指す点＝回転の中心＝絵の中心）
    sprite.SetPivot({ 0.5f, 0.5f });

    // 4x4シートの左上1コマだけ表示する
    sprite.SetSourceRect({ 0.0f, 0.0f, 0.25f, 0.25f });
}
```

---

### ModelRendererComponent

**宣言（よく使う引数のみ）**

```cpp
explicit ModelRendererComponent(
    std::filesystem::path modelPath = {},
    bool wireframe = false,
    bool materialOverrideEnabled = false,
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
    /* ...省略... */);
```

**概略**

glTF/GLB/FBX/CMOなどの3Dモデルを描画します。
スケルタルアニメーションにも対応します。

**引数**

| 引数 | 説明 |
|---|---|
| `modelPath` | モデルファイルのassets相対パス（`.glb`、`.fbx`など） |
| `wireframe` | trueで線だけの表示。形の確認に使えます |
| `materialOverrideEnabled` | trueにすると、モデルに埋め込まれた色を無視して`color`を使います |
| `color` | 上書きが有効なときの色（RGBA） |

残りの引数（アニメーション設定など）はInspectorから設定するのが簡単です。

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `SetModelPath` | `path` : モデルのパス | 表示するモデルを差し替えます |
| `PlayAnimation` | なし | アニメーションを再生します |
| `PauseAnimation` | なし | その場で一時停止します |
| `StopAnimation` | なし | 停止して先頭へ戻します |
| `SetAnimationIndex` | `index` : 0から始まる番号 | 再生するクリップを番号で選びます |
| `SetAnimationSpeed` | `speed` : 倍率 | 1.0が等速、2.0で倍速、0.5で半速 |
| `SetAnimationLoop` | `loop` : true/false | trueで繰り返します |
| `SetAnimationTime` | `seconds` : 秒 | 再生位置を指定します |
| `SetAnimationTrigger` | `trigger` : Trigger名 | Animator Controllerの遷移を発火します |
| `SetAnimationFloat` | `parameter` : 名前<br>`value` : 値 | Blend Treeのパラメーターを設定します |
| `PollAnimationEvent` | `event` : 受け取る変数 | イベントを1件取り出します。**あればtrue** |
| `SetMaterialOverrideEnabled` | `enabled` : true/false | trueでモデル内蔵の色を無視し、自前の設定を使います |
| `SetColor` | `color` : 色（RGBA） | 上書きが有効なときの色 |
| `SetWireframe` | `enabled` : true/false | trueで線だけの表示になります |

**サンプル**

```cpp
void Start() override
{
    m_model = GetComponent<LamaPon::ModelRendererComponent>();
    if (m_model != nullptr)
    {
        m_model->SetAnimationSpeed(1.5f);
        m_model->PlayAnimation();
    }
}

void Update(float) override
{
    // Animator Controllerへ「走れ」と伝える
    if (Graphics().Input().WasPressed("Jump") && m_model != nullptr)
    {
        m_model->SetAnimationTrigger("Run");
    }

    // アニメーションイベント（足音など）を受け取る
    LamaPon::AnimationEventNotification event;
    while (m_model != nullptr
        && m_model->PollAnimationEvent(event))
    {
        LamaPon::Logger::Instance().Info(
            "アニメーションイベント: " + event.name);
    }
}

private:
    LamaPon::ModelRendererComponent* m_model{};
```

---

### TextRendererComponent

**宣言**

```cpp
explicit TextRendererComponent(
    std::string text = "日本語テキスト",
    std::string fontFamily = "Yu Gothic UI",
    float fontSize = 32.0f,
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
    DirectX::XMFLOAT2 layoutSize = { 0.0f, 0.0f },
    bool wordWrap = false,
    TextHorizontalAlignment horizontalAlignment = TextHorizontalAlignment::Left,
    TextVerticalAlignment verticalAlignment = TextVerticalAlignment::Top);
```

**概略**

ゲーム画面へ日本語テキストを描画します（スコア表示など）。

**引数**

| 引数 | 説明 |
|---|---|
| `text` | 表示する文字列（UTF-8）。日本語をそのまま書けます |
| `fontFamily` | フォント名。Windowsに入っている名前を指定します（例: `"Yu Gothic UI"`、`"MS Gothic"`） |
| `fontSize` | 文字の大きさ（ピクセル） |
| `color` | 文字の色（RGBA） |
| `layoutSize` | 文字を収める枠の大きさ。`{0,0}`なら文字数に合わせて自動 |
| `wordWrap` | trueで`layoutSize`の幅を超えたら折り返します（`layoutSize`の指定が必要） |
| `horizontalAlignment` | 横位置。`Left` / `Center` / `Right` |
| `verticalAlignment` | 縦位置。`Top` / `Center` / `Bottom` |

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `SetText` | `text` : 表示する文字列（UTF-8） | 文字を変えます。日本語もそのまま使えます |
| `SetFontSize` | `size` : 文字の大きさ | 単位はピクセルです |
| `SetColor` | `color` : 色（RGBA） | 文字の色を変えます |
| `SetSortOrder` | `order` : 整数 | 描画順。大きいほど手前 |

**サンプル**

```cpp
void Start() override
{
    m_score = &AddComponent<LamaPon::TextRendererComponent>(
        "スコア: 0",
        "Yu Gothic UI",
        28.0f);
}

void AddScore(const int amount)
{
    m_points += amount;
    m_score->SetText("スコア: " + std::to_string(m_points));
}

private:
    LamaPon::TextRendererComponent* m_score{};
    int m_points{};
```

---

### ParticleSystemComponent

**宣言（よく使う引数のみ）**

```cpp
explicit ParticleSystemComponent(
    std::uint32_t maxParticles = 512,
    float emissionRate = 36.0f,
    DirectX::XMFLOAT2 lifetime = { 0.8f, 1.8f },
    DirectX::XMFLOAT2 speed = { 1.0f, 3.0f },
    DirectX::XMFLOAT2 startSize = { 0.08f, 0.22f },
    DirectX::XMFLOAT4 startColor = { 0.20f, 0.72f, 1.0f, 1.0f },
    DirectX::XMFLOAT4 endColor = { 0.04f, 0.18f, 0.55f, 0.0f },
    ParticleEmitterShape shape = ParticleEmitterShape::Cone,
    std::filesystem::path texturePath = {});
```

**概略**

爆発・炎・煙などのエフェクトを出します。

**引数**

| 引数 | 説明 |
|---|---|
| `maxParticles` | 同時に存在できる粒の上限。多いほど重くなります |
| `emissionRate` | 1秒あたりに出す粒の数 |
| `lifetime` | 粒が消えるまでの秒数。`{最小, 最大}`の**範囲**で、この間からランダムに決まります |
| `speed` | 飛び出す速さの範囲`{最小, 最大}` |
| `startSize` | 出た瞬間の大きさの範囲`{最小, 最大}` |
| `startColor` | 出た瞬間の色（RGBA） |
| `endColor` | 消える直前の色。アルファを0にすると自然にフェードアウトします |
| `texturePath` | 粒に使う画像。空なら単色の四角になります |

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `Play` | なし | 放出を開始します |
| `Stop` | `clearParticles` : true/false | 放出を止めます。trueで出ている粒子も消します |
| `Restart` | なし | 最初から出し直します |
| `Emit` | `count` : 個数 | 指定個数を一気に放出します（爆発などの単発演出） |
| `SetGravity` | `gravity` : 重力の向きと強さ | `{0,-9.8f,0}`で下へ落ちます |
| `SetAdditive` | `enabled` : true/false | trueで加算合成。炎や光の表現向け |
| `SetLooping` | `loop` : true/false | trueで繰り返し放出します |

**サンプル**

```cpp
// 敵を倒したときに爆発させる
void Explode()
{
    if (auto* effect = GetComponentInChildren<
        LamaPon::ParticleSystemComponent>())
    {
        effect->Emit(60);   // 60個まとめて放出
    }
}
```

---

### SpriteParticles2DComponent

**宣言**

```cpp
explicit SpriteParticles2DComponent(
    std::uint32_t maxParticles = 128,
    DirectX::XMFLOAT2 lifetime = { 0.25f, 0.65f },
    DirectX::XMFLOAT2 speed = { 24.0f, 72.0f },
    DirectX::XMFLOAT2 startSize = { 4.0f, 12.0f },
    DirectX::XMFLOAT4 startColor = { 1.0f, 0.85f, 0.35f, 1.0f },
    DirectX::XMFLOAT4 endColor = { 1.0f, 0.25f, 0.05f, 0.0f });
```

**概略**

2Dの粒を**その場で一度に撒く**エフェクトです。
コインを取った、敵を壊した、着地した、のような一瞬の手応えを足すためのもので、画像もShaderも要りません。
3Dの`ParticleSystemComponent`（連続放出）とは別物です。

**引数**

| 引数 | 説明 |
|---|---|
| `maxParticles` | 同時に存在できる粒の上限（1〜4096） |
| `lifetime` | 粒が消えるまでの秒数。`{最小, 最大}`の**範囲**で、この間からランダムに決まります |
| `speed` | 飛び出す速さの範囲`{最小, 最大}`（ピクセル/秒）。全方向へ均等に散ります |
| `startSize` | 出た瞬間の一辺の長さの範囲`{最小, 最大}`（ピクセル） |
| `startColor` | 出た瞬間の色（RGBA） |
| `endColor` | 消える直前の色。アルファを0にすると自然にフェードアウトします |

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `Emit` | `count` : 個数 | **今のGameObjectの位置**から全方向へその数だけ撒きます。**これを呼ばない限り1粒も出ません** |
| `Clear` | なし | 出ている粒を全部消します |
| `ActiveParticleCount` | なし | いま出ている粒の数 |
| `SetMaxParticles` | `count` : 1〜4096 | 同時に存在できる上限 |
| `SetLifetime` | `range` : `{最小, 最大}`（秒） | 粒が消えるまでの時間 |
| `SetStartSpeed` | `range` : `{最小, 最大}`（ピクセル/秒） | 飛び出す速さ |
| `SetStartSize` | `range` : `{最小, 最大}`（ピクセル） | 出た瞬間の大きさ |
| `SetSizeGrowth` | `growth` : ピクセル/秒 | 1秒あたりの大きさの増減。既定`-4`（縮みながら消える）。プラスにすると膨らみます |
| `SetGravity` | `gravity` : `{x, y}`（ピクセル/秒²） | **Yは下向き**。既定`{0, 180}`で下へ落ちます |
| `SetDrag` | `drag` : 0〜100 | 空気抵抗。大きいほど早く減速します（0で減速なし） |
| `SetStartColor` | `color` : 色（RGBA） | 出た瞬間の色 |
| `SetEndColor` | `color` : 色（RGBA） | 消える直前の色 |
| `SetTexturePath` | `path` : assets相対パス | 粒に使う画像。空なら白い四角。`builtin/circle`／`builtin/triangle`／`builtin/ring`も指定できます |
| `SetSortOrder` | `order` : 整数 | 描画順。**大きいほど手前**（Sprite Rendererと同じ並びです） |

**解説**

- **自動では出ません。`Emit`を呼んだときだけ**撒かれます。煙のように出し
  続けたい場合は、Scriptで一定間隔に`Emit`を呼んでください。
- **撒いた粒はその場に残ります。** GameObjectを動かしても付いてきません
  （動きながら撒けば、そのまま尾を引く演出になります）。
- 上限に達している間は`Emit`しても増えません（古い粒を押し出しません）。
- 粒は`一辺 × 一辺`の正方形として描かれます。画像を指定しても縦横比は
  保たれません。
- **UI Rect Transformを持つGameObjectでは描画されません。** ワールド空間の
  2D専用です（座標の決まりは[UIと2D機能](ui-2d.md)を参照）。
- エディターのInspectorに「Burst 8」「Burst 32」「Clear」があるので、
  **再生しなくても**散り方を確かめられます。

**サンプル**

```cpp
void Start() override
{
    auto& burst = AddComponent<LamaPon::SpriteParticles2DComponent>();
    burst.SetStartColor({ 1.0f, 0.90f, 0.40f, 1.0f });  // 黄色から
    burst.SetEndColor({ 1.0f, 0.20f, 0.00f, 0.0f });    // 赤へ薄れて消える
    burst.SetSortOrder(100);                            // 絵の手前に描く
}

// コインを取った瞬間に24粒はじけさせる
void OnCoinTaken()
{
    if (auto* burst =
        GetComponent<LamaPon::SpriteParticles2DComponent>())
    {
        burst->Emit(24);
    }
}
```

---

## 物理（コンポーネント）

### RigidbodyComponent

**宣言（よく使う引数のみ）**

```cpp
explicit RigidbodyComponent(
    DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f },
    bool useGravity = true,
    bool isKinematic = false,
    CollisionDetectionMode collisionDetection = CollisionDetectionMode::Discrete,
    float mass = 1.0f,
    /* ...省略... */);
```

**概略**

重力や力で動く物理オブジェクトにします。

**引数**

| 引数 | 説明 |
|---|---|
| `velocity` | 初速（毎秒の移動量）。`{0,0,0}`ならその場から始まります |
| `useGravity` | trueで重力を受けて落ちます |
| `isKinematic` | trueにすると物理で動かなくなり、Transformを書き換えて動かす専用になります（動く床など） |
| `collisionDetection` | `Discrete`（軽い）か`Continuous`（速い物のすり抜け防止） |
| `mass` | 質量。大きいほど押されにくくなります |

残りの引数（空気抵抗、回転の固定など）はInspectorから設定できます。

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `AddForce` | `force` : 力の向きと強さ<br>`mode` : 力の加え方（下表） | 力を加えます。`{0,6,0}`で上向き |
| `AddTorque` | `torque` : 回転の軸と強さ<br>`mode` : 力の加え方 | 回転させる力を加えます |
| `AddForceAtPosition` | `force` : 力<br>`worldPosition` : 力を加える位置<br>`mode` : 力の加え方 | 重心からずれた位置に力を加えます（回転が生じます） |
| `SetVelocity` | `velocity` : 速度 | 速度を直接指定します（毎秒の移動量） |
| `SetMass` | `mass` : 質量 | 重さ。大きいほど動かしにくくなります |
| `SetUseGravity` | `enabled` : true/false | falseで重力を受けなくなります（浮遊物） |
| `SetKinematic` | `enabled` : true/false | trueで物理の影響を受けず、Transformで動かす専用になります |
| `Sleep` / `WakeUp` | なし | 物理計算を一時停止／再開します（通常は自動） |

**解説**

`ForceMode`は4種類です。

| モード | 意味 | 使いどころ |
|---|---|---|
| `Force` | 継続的な力（質量の影響あり） | 推進力、風 |
| `Acceleration` | 継続的な加速（質量を無視） | 重力のような効果 |
| `Impulse` | 瞬間的な力（質量の影響あり） | ジャンプ、爆風 |
| `VelocityChange` | 瞬間的な速度変更（質量を無視） | 即座に速度を与える |

継続的な力は`FixedUpdate`で、瞬間的な力は`Update`で加えるのが基本です。

**サンプル**

```cpp
void Update(float) override
{
    auto* body = GetComponent<LamaPon::RigidbodyComponent>();
    if (body == nullptr)
    {
        return;
    }

    // ジャンプ（瞬間的な力）
    if (Graphics().Input().WasPressed("Jump"))
    {
        body->AddForce(
            { 0.0f, 6.0f, 0.0f },
            LamaPon::ForceMode::Impulse);
    }
}

void FixedUpdate(float) override
{
    // 前進（継続的な力）
    if (auto* body = GetComponent<LamaPon::RigidbodyComponent>())
    {
        const float forward =
            Graphics().Input().Value("MoveVertical");
        body->AddForce(
            { 0.0f, 0.0f, forward * 8.0f },
            LamaPon::ForceMode::Acceleration);
    }
}
```

---

### CharacterControllerComponent

**宣言**

```cpp
CharacterControllerComponent(
    float radius = 0.4f,
    float height = 1.8f,
    float moveSpeed = 4.0f,
    float gravity = 20.0f,
    float jumpSpeed = 7.0f,
    float stepOffset = 0.3f,
    float skinWidth = 0.03f,
    std::uint32_t layer = 2,
    std::uint32_t collisionMask = 0xffffffffu);
```

**概略**

人型キャラクターの移動用です。
Rigidbodyと違い、物理に振り回されずに思いどおり動かせます（重力・接地判定・段差越え・壁ずりは自動）。

**引数**

| 引数 | 説明 |
|---|---|
| `radius` | カプセルの半径。細い通路を通れるかに影響します |
| `height` | カプセルの高さ。人型なら1.6〜1.8くらい |
| `moveSpeed` | 1秒あたりの移動量（自動入力のとき） |
| `gravity` | 落下の加速度。大きいほどキビキビ落ちます（現実は9.8ですが、ゲームでは20前後が気持ちよい） |
| `jumpSpeed` | ジャンプの初速。大きいほど高く跳びます |
| `stepOffset` | この高さまでの段差は、ジャンプせずに乗り越えます |
| `skinWidth` | 壁にめり込まないための余白。小さすぎるとすり抜けの原因になります |
| `layer` | 0〜31のレイヤー番号 |
| `collisionMask` | 衝突する相手のレイヤーのビットマスク |

**主なメソッド**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `Move` | `displacement` : このフレームで動かす量 | 移動を指示します。速度ではなく**移動量**なので`deltaTime`を掛けます |
| `Jump` | なし | ジャンプします。接地していないときは無視されます |
| `SetMoveSpeed` | `speed` : 1秒あたりの移動量 | 自動入力で動くときの速さ |
| `SetUseInput` | `enabled` : true/false | 既定はtrue（自動で入力を読む）。falseで`Move`／`Jump`の手動操作に切り替わります |
| `IsGrounded` | なし | 地面に接していればtrue |

**解説**

既定では`MoveHorizontal`／`MoveVertical`／`Jump`のInput Actionを自分で読んで動きます。
スクリプトから制御したい場合は`SetUseInput(false)`にして`Move()`と`Jump()`を呼びます。

**サンプル**

```cpp
void Update(const float deltaTime) override
{
    auto* controller = GetComponent<
        LamaPon::CharacterControllerComponent>();
    if (controller == nullptr)
    {
        return;
    }

    // 接地しているときだけジャンプできるようにする
    if (controller->IsGrounded()
        && Graphics().Input().WasPressed("Jump"))
    {
        controller->Jump();
    }
}
```

---

### コライダー（当たり判定）

**宣言**

```cpp
// よく使う3種
explicit BoxCollider3DComponent(
    DirectX::XMFLOAT3 size = { 1.0f, 1.0f, 1.0f },
    DirectX::XMFLOAT3 offset = {},
    bool isTrigger = false,
    std::uint32_t layer = 0,
    std::uint32_t collisionMask = 0xffffffffu,
    PhysicsMaterial material = {});

explicit SphereCollider3DComponent(float radius = 0.5f, /* 以下同様 */);
explicit CapsuleCollider3DComponent(float radius = 0.5f, float height = 2.0f, /* 以下同様 */);
```

**概略**

物体の当たり判定です。
ほかに`BoxCollider2D` / `CircleCollider2D` /`ConvexHullCollider3D` / `MeshCollider3D`（モデルの三角形）があります。

**引数**

| 引数 | 説明 |
|---|---|
| `isTrigger` | trueにすると押し戻しをせず、`OnTrigger〜`だけが呼ばれます。アイテム取得判定やゴール判定に使います |
| `layer` | 0〜31のレイヤー番号。「敵」「弾」などのグループ分けに使います |
| `collisionMask` | 衝突を許可するレイヤーのビットマスク。**双方が相手を許可しているときだけ**判定されます |

**サンプル**

```cpp
void Start() override
{
    // 押し戻しのある当たり判定
    AddComponent<LamaPon::BoxCollider3DComponent>(
        DirectX::XMFLOAT3{ 1.0f, 2.0f, 1.0f });
}

// 通り抜けるアイテム（Trigger）の受け取り
void OnTriggerEnter(
    const LamaPon::CollisionEvent& event) override
{
    LamaPon::Logger::Instance().Info("アイテムに触れました");
}
```

---

## オーディオ

### AudioSourceComponent

**宣言**

```cpp
explicit AudioSourceComponent(
    std::filesystem::path audioPath = {},
    float volume = 1.0f,
    float pitch = 0.0f,
    float pan = 0.0f,
    bool loop = false,
    bool playOnStart = false,
    bool spatial = false,
    float minimumDistance = 1.0f,
    float maximumDistance = 20.0f);
```

**概略**

効果音やBGMを再生します。

**引数**

| 引数 | 説明 |
|---|---|
| `audioPath` | 音声ファイルのassets相対パス（`.wav`） |
| `volume` | 音量（0〜1）。0で無音 |
| `pitch` | 音の高さ。**0が元の高さ**で、1.0で1オクターブ上、-1.0で1オクターブ下 |
| `pan` | 左右の定位。-1.0で左、0で中央、1.0で右 |
| `loop` | trueで繰り返し再生（BGM向け） |
| `playOnStart` | trueならシーン開始と同時に鳴り始めます |
| `spatial` | trueで3Dサウンド。GameObjectの位置とカメラの距離で音量・定位が変わります |
| `minimumDistance` | 3Dサウンドで、この距離までは減衰しません |
| `maximumDistance` | 3Dサウンドで、この距離を超えると聞こえなくなります |

**主なメソッド**

| メソッド | 引数 | 説明 |
|---|---|---|
| `Play` | なし | 最初から再生します（再生中なら鳴り直します） |
| `PlayOneShot` | なし | **重ねて**1回鳴らします。連射音や足音向け |
| `Pause` / `Resume` | なし | 一時停止／再開します |
| `Stop` | なし | 停止して先頭に戻します |
| `SetVolume` | `volume` : 0〜1 | 音量。0で無音、1で最大 |
| `SetLoop` | `loop` : true/false | trueで繰り返し再生（BGM向け） |
| `SetStreaming` | `streaming` : true/false | trueで逐次デコード。長いBGMのメモリ節約に。ただし**3D定位と`PlayOneShot`は使えなくなります** |
| `SetBus` | `bus` : `AudioBus::Effects` / `Music` | 音量をまとめて調整するグループを選びます |

**解説**

`pitch`は0が原音で、±で高さが変わります。
`spatial`をtrueにすると3D定位（距離減衰と左右パン）が有効になり、`AudioListenerComponent`を付けたカメラとの位置関係で聞こえ方が変わります。

**サンプル**

```cpp
void Start() override
{
    m_shot = &AddComponent<LamaPon::AudioSourceComponent>(
        std::filesystem::path{ "audio/shot.wav" },
        0.6f);
}

void Fire()
{
    m_shot->PlayOneShot();   // 連射しても重なって鳴る
}

private:
    LamaPon::AudioSourceComponent* m_shot{};
```

---

## アニメーション

### TransformAnimatorComponent

**宣言**

```cpp
explicit TransformAnimatorComponent(
    std::filesystem::path clipPath = {},
    float speed = 1.0f,
    bool loop = true,
    bool playOnStart = true,
    std::filesystem::path controllerPath = {});
```

**概略**

`.animation.json`のキーフレームでGameObjectを動かします。

**引数**

| 引数 | 説明 |
|---|---|
| `clipPath` | `.animation.json`のassets相対パス |
| `speed` | 再生速度の倍率。1.0が等速、マイナスで逆再生 |
| `loop` | trueで繰り返します |
| `playOnStart` | trueならシーン開始と同時に再生します |
| `controllerPath` | Animator Controller（`.animator.json`）のパス。指定すると`SetTrigger`で状態遷移できます |

**主なメソッド**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `Play` | なし | 再生します |
| `Pause` | なし | その場で止めます（再生位置は保持） |
| `Stop` | なし | 停止して先頭へ戻します |
| `SetSpeed` | `speed` : 倍率 | 1.0が等速。マイナスで逆再生 |
| `SetTime` | `time` : 秒 | 再生位置を直接指定します |
| `SetTrigger` | `trigger` : Trigger名 | Animator Controllerの状態遷移を発火します |
| `IsPlaying` | なし | 再生中ならtrue |

**サンプル**

```cpp
void Update(float) override
{
    if (auto* animator = GetComponent<
        LamaPon::TransformAnimatorComponent>())
    {
        if (!animator->IsPlaying())
        {
            animator->Play();   // 止まっていたら再生し直す
        }
    }
}
```

---

### SpriteAnimatorComponent

**宣言**

```cpp
explicit SpriteAnimatorComponent(int columns = 1, int rows = 1);
```

**概略**

1枚のスプライトシートをコマ送りします（2Dキャラのアニメ）。

**引数**

| 引数 | 説明 |
|---|---|
| `columns` | シートの横のコマ数 |
| `rows` | シートの縦のコマ数 |

画像は`SpriteRendererComponent`側に設定します。
このコンポーネントは「その画像を何コマに分けるか」だけを決めます。

**主なメソッド**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `Play` | `clipName` : クリップ名 | そのクリップを再生。**名前が見つからなければfalse**（typoに気づけます） |
| `Stop` | なし | 再生を止めます |
| `AddClip` | `clip` : クリップ定義（下記） | 使えるアニメを登録します |
| `SetSpeed` | `speed` : 倍率 | 1.0が等速。2.0で倍速 |
| `IsPlaying` | なし | 再生中ならtrue |
| `ActiveClipName` | なし | 再生中のクリップ名。停止中は空文字列 |

**解説**

`SpriteAnimationClip`は`name` / `startFrame` / `frameCount` /`framesPerSecond` / `loop`を持つ構造体です。
コマ番号は左上から右へ、次の行へと数えます。

**サンプル**

```cpp
void Start() override
{
    auto& animator =
        AddComponent<LamaPon::SpriteAnimatorComponent>(4, 2);

    animator.AddClip({ "idle", 0, 4, 6.0f, true });
    animator.AddClip({ "run", 4, 4, 12.0f, true });
    animator.Play("idle");
}

void Update(float) override
{
    auto* animator = GetComponent<
        LamaPon::SpriteAnimatorComponent>();
    const bool moving =
        std::abs(Graphics().Input().Value("MoveHorizontal"))
            > 0.1f;
    if (animator != nullptr)
    {
        animator->Play(moving ? "run" : "idle");
    }
}
```

---

## UI

### UIButtonComponent

**宣言**

```cpp
explicit UIButtonComponent(
    std::string label = "ボタン",
    DirectX::XMFLOAT2 fallbackSize = { 220.0f, 56.0f },
    std::filesystem::path texturePath = {});
```

**概略**

クリックできるボタンです。
`UICanvas`の子で、`UIRectTransform`と一緒に使います。

**引数**

| 引数 | 説明 |
|---|---|
| `label` | ボタンに表示する文字列（UTF-8） |
| `fallbackSize` | `UIRectTransform`でサイズを決めていないときに使う大きさ |
| `texturePath` | ボタンの背景画像。空なら標準の見た目になります |

**主なメソッド**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `WasClicked` | なし | 押された瞬間のフレームでtrue。状態は変わりません |
| `ConsumeClick` | なし | 押下を**消費して**trueを返します。二重処理を防げます |
| `SetLabel` | `label` : 表示文字列 | ボタンの文字を変えます |
| `SetInteractable` | `enabled` : true/false | falseで押せなくなります（灰色表示） |
| `SetClickEventName` | `name` : イベント名 | クリック時にそのイベントを発行します |
| `SetTargetScene` | `path` : シーンのパス | 押すとそのシーンへ移動します |

**解説**

クリックの受け取り方は2通りです。
`ConsumeClick()`でポーリングするか、`SetClickEventName("StartGame")`としておいて別のスクリプトで`On("StartGame", ...)`で受け取るか。
後者はボタン側にコードが不要です。

**サンプル**

```cpp
void Update(float) override
{
    if (auto* button = GetComponent<LamaPon::UIButtonComponent>();
        button != nullptr && button->ConsumeClick())
    {
        LamaPon::Logger::Instance().Info("押されました");
    }
}
```

---

### UISliderComponent / UIToggleComponent / UIInputFieldComponent

**宣言**

```cpp
explicit UISliderComponent(
    float minimumValue = 0.0f,
    float maximumValue = 1.0f,
    float value = 0.5f) noexcept;

explicit UIToggleComponent(
    std::string label = "トグル",
    bool isOn = false);

explicit UIInputFieldComponent(
    std::string text = {},
    std::string placeholder = "テキストを入力...");
```

**概略**

ボタン以外の基本UIです。
いずれも`UICanvas`の子で、`UIRectTransform`と一緒に使います。

**引数**

| コンストラクタ | 引数 | 説明 |
|---|---|---|
| `UISliderComponent` | `minimumValue` : 最小値<br>`maximumValue` : 最大値<br>`value` : 初期値 | 音量スライダーなら`(0.0f, 1.0f, 0.8f)`のように |
| `UIToggleComponent` | `label` : 横に出す文字列<br>`isOn` : 初期のオン／オフ | 「フルスクリーン」などの設定項目に |
| `UIInputFieldComponent` | `text` : 初期の入力内容<br>`placeholder` : 空のときに薄く出す案内文 | プレイヤー名の入力などに |

**主なメソッド**

**UISliderComponent（つまみで数値を選ぶ）**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `SetRange` | `minimumValue` : 最小値<br>`maximumValue` : 最大値 | 選べる範囲を決めます。音量なら`(0.0f, 1.0f)` |
| `SetValue` | `value` : 値 | つまみの位置を設定します。範囲外は自動で丸められます |
| `SetNormalizedValue` | `normalized` : 0〜1 | 範囲に関係なく「何割の位置か」で設定します |
| `SetWholeNumbers` | `wholeNumbers` : true/false | trueで整数のみ選べるようになります（難易度レベルなど） |
| `SetInteractable` | `interactable` : true/false | falseで操作できなくなります |
| `Value` | なし | 現在の値 |
| `ConsumeValueChanged` | なし | 変わっていればtrueを返し、**フラグを下ろします**。毎フレーム反映せずに済みます |

**UIToggleComponent（オン／オフの切り替え）**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `SetIsOn` | `isOn` : true/false | チェック状態を設定します |
| `SetLabel` | `label` : 文字列 | 横に出す説明文を変えます |
| `IsOn` | なし | オンならtrue |
| `ConsumeValueChanged` | なし | 切り替わっていればtrueを返し、フラグを下ろします |

**UIInputFieldComponent（文字を入力する欄）**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `SetText` | `text` : 文字列（UTF-8） | 入力欄の中身を設定します |
| `SetPlaceholder` | `placeholder` : 文字列 | 空のときに薄く表示する案内文 |
| `SetMaxLength` | `maxLength` : 最大文字数 | これ以上は入力できなくなります |
| `Text` | なし | 現在入力されている文字列 |
| `ConsumeSubmit` | なし | **Enterで確定された**フレームでtrue。名前入力の決定などに |

**サンプル**

```cpp
void Update(float) override
{
    // 音量スライダーの値をマスター音量へ反映する
    if (auto* slider = GetComponent<LamaPon::UISliderComponent>();
        slider != nullptr && slider->ConsumeValueChanged())
    {
        LamaPon::Logger::Instance().Info(
            "音量: " + std::to_string(slider->Value()));
    }
}
```

---

## ナビゲーション

### NavMeshAgentComponent

**宣言**

```cpp
explicit NavMeshAgentComponent(
    float speed = 3.0f,
    float stoppingDistance = 0.1f,
    bool rotateToPath = true);
```

**概略**

Bake済みのNavMesh上を、障害物を避けながら移動します。

**引数**

| 引数 | 説明 |
|---|---|
| `speed` | 1秒あたりの移動量 |
| `stoppingDistance` | 目的地にこの距離まで近づいたら到着とみなして止まります |
| `rotateToPath` | trueで進行方向を向きます。falseなら向きは変わりません |

**主なメソッド**

| メソッド | 引数 | 戻り値・説明 |
|---|---|---|
| `SetDestination` | `destination` : 目的地のワールド座標 | シーン内のNavMeshを自動で探して経路を計算。**成功でtrue** |
| `SetDestination` | `destination` : 目的地<br>`navMesh` : 使うNavMesh | NavMeshが複数あるとき、どれを使うか明示します |
| `Stop` | なし | 移動をやめ、経路を捨てます |
| `SetSpeed` | `speed` : 1秒あたりの移動量 | 移動の速さ |
| `HasPath` | なし | 経路を持っていればtrue（移動中の判定に） |
| `HasArrived` | なし | 目的地の`stoppingDistance`以内に着いたらtrue |

**戻り値**

`SetDestination`は経路が見つかればtrue、見つからなければfalseです。

**サンプル**

```cpp
void Start() override
{
    // 0.3秒ごとに追いかけ先を更新（毎フレームは重いので間引く）
    InvokeRepeating(0.0f, 0.3f, [this] { Chase(); });
}

void Chase()
{
    auto* player = FindWithTag("Player");
    auto* agent = GetComponent<LamaPon::NavMeshAgentComponent>();
    if (player != nullptr && agent != nullptr)
    {
        agent->SetDestination(
            player->GetTransform().position);
    }
}
```

---

## セーブとログ

### Logger（デバッグ出力）

**宣言**

```cpp
LamaPon::Logger& LamaPon::Logger::Instance() noexcept;

void Info(std::string message, std::uint64_t gameObjectId = 0);
void Warning(std::string message, std::uint64_t gameObjectId = 0);
void Error(std::string message, std::uint64_t gameObjectId = 0);
```

**概略**

エディターのConsoleと`LamaPon.log`へメッセージを出します。

**引数**

| 引数 | 説明 |
|---|---|
| `message` | 出力する文字列（UTF-8）。日本語もそのまま使えます |
| `gameObjectId` | 関連するGameObjectのID。`Owner().Id()`を渡すと、Consoleからその行をクリックして対象を選択できます |

**解説**

エクスポートしたゲームでは**F1キー**のデバッグオーバーレイに直近の警告・エラーが表示されます。
「動かない」ときの原因追跡に使えます。

**サンプル**

```cpp
void Start() override
{
    LamaPon::Logger::Instance().Info("開始しました");

    if (GetComponent<LamaPon::RigidbodyComponent>() == nullptr)
    {
        // どのオブジェクトの話かを紐づける
        LamaPon::Logger::Instance().Warning(
            "Rigidbodyがありません",
            Owner().Id());
    }
}
```

---

### 進行状況の保存

**宣言**

```cpp
// Scriptから直接（ハイスコアなどの小さな値はこれで足ります）
void SaveInteger(std::string_view key, std::int64_t value) const;
std::int64_t LoadInteger(std::string_view key, std::int64_t defaultValue = 0) const;
void SaveNumber(std::string_view key, double value) const;
double LoadNumber(std::string_view key, double defaultValue = 0.0) const;
void SaveText(std::string_view key, std::string value) const;
std::string LoadText(std::string_view key, std::string defaultValue = {}) const;
bool HasSaved(std::string_view key) const;
void DeleteSaved(std::string_view key) const;

// Application経由（構造化データはこちら）
PlayerPrefs& Preferences() const;
SaveDataStore& Saves() const;

// PlayerPrefs
void SetInteger(std::string key, std::int64_t value);
std::int64_t GetInteger(std::string_view key, std::int64_t defaultValue = 0) const;
void SetString(std::string key, std::string value);
std::string GetString(std::string_view key, std::string defaultValue = {}) const;
void Save();

// SaveDataStore
void SaveJson(std::string_view slot, std::string_view json);
std::optional<std::string> LoadJson(std::string_view slot) const;
bool HasSlot(std::string_view slot) const;
std::vector<std::string> ListSlots() const;
bool DeleteSlot(std::string_view slot);
```

**概略**

アプリを終了しても残るデータです。
設定値は`PlayerPrefs`、ゲーム進行のような構造化データは`SaveDataStore`（JSONスロット）を使います。

**引数**

| 引数 | 説明 |
|---|---|
| `key` | 設定値につける名前（`"highScore"`など） |
| `value` | 保存する値。整数・小数・真偽値・文字列が使えます |
| `defaultValue` | キーが無いとき、または型が違うときに返る値 |
| `slot` | セーブスロットの名前（`"slot1"`など）。ファイル名になるので記号は避けます |
| `json` | 保存するJSON文字列。中身の構造はゲーム側の自由です |

**戻り値**

| 関数 | 戻り値 |
|---|---|
| `GetInteger` / `GetString`など | 保存されている値。無ければ`defaultValue` |
| `LoadJson` | `std::optional<std::string>`。**スロットが無ければ空**なので`if`で確認します |
| `HasSlot` | 存在すればtrue |
| `ListSlots` | 保存済みスロット名の一覧 |
| `DeleteSlot` | 削除できたらtrue |

**解説**

**Scriptからは`SaveInteger`／`LoadInteger`などを直接呼べます。**ハイスコアや音量のような小さな値はこれで足ります。

```cpp
void Start() override
{
    m_best = static_cast<int>(LoadInteger("bestScore", 0));
}

void OnGameOver()
{
    if (m_score > m_best)
    {
        m_best = m_score;
        SaveInteger("bestScore", m_best);   // ここでファイルへ書きます
    }
}
```

⚠️ **書くたびにファイルへ保存します。毎フレーム呼ばないでください**（ゲームオーバー時など、区切りで呼ぶ想定です）。
また、Applicationを立てないツール（`LamaPonCli`の`render`など）では**保存は黙って無視され、読み出しは既定値**になります。
落ちはしません。

セーブスロットのような構造化データを扱う`SaveDataStore`と、`PlayerPrefs`そのものは、いまも`Application`の持ち物なのでScriptからは触れません。
シーンをまたぐだけの一時的な値は`GetScene().Scenes().State()`（[シーンをまたいで値を残す](#シーンをまたいで値を残す)）が向いています。
エディターの「セーブデータ」タブからは編集・確認ができます。

保存先は`%LOCALAPPDATA%/LamaPon/<ゲーム名>/`です。

---

## 逆引き（やりたいこと別）

| やりたいこと | 書き方 |
|---|---|
| 毎フレーム動かす | `Update(float deltaTime)`で`GetTransform().position`を変える |
| キー入力で動かす | `Graphics().Input().Value("MoveHorizontal")` |
| 押した瞬間だけ反応 | `Graphics().Input().WasPressed("Jump")` |
| ジャンプさせる | Rigidbodyへ`AddForce({0,6,0}, ForceMode::Impulse)` |
| キャラを歩かせる | `CharacterControllerComponent`（重力・段差は自動） |
| 敵を追いかけさせる | `NavMeshAgentComponent::SetDestination()` |
| 当たり判定を取る | `OnCollisionEnter` / `OnTriggerEnter`を上書き |
| 相手を判別する | `event.other.CompareTag("Enemy")` |
| 弾を出す | `Instantiate("prefabs/bullet.prefab.json")` |
| 敵やカードの性能表を持つ | データアセット。`LoadDataAsset("data/ゴブリン.asset.json")` |
| 3秒後に消す | `Invoke(3.0f, [this]{ Destroy(Owner()); })` |
| 一定間隔で処理 | `InvokeRepeating(0.0f, 0.5f, [this]{ ... })` |
| 「◯秒待ってから」の流れ | コルーチン＋`co_await WaitForSeconds{1.0f}` |
| 効果音を鳴らす | `AudioSourceComponent::PlayOneShot()` |
| スコア表示 | `TextRendererComponent::SetText()` |
| ボタンの反応 | `UIButtonComponent::ConsumeClick()` |
| 別スクリプトへ通知 | `Emit("EnemyDied")` と `On("EnemyDied", ...)` |
| シーンを変える | `GetScene().Scenes().RequestLoadAsync("scenes/next.scene.json")` |
| ポーズ画面を重ねる | `GetScene().Scenes().RequestLoadAdditive("scenes/pause.scene.json")` |
| 重ねた画面を閉じる | `GetScene().Scenes().RequestUnload("scenes/pause.scene.json")` |
| シーンをまたいでスコアを残す | `GetScene().Scenes().State().SetInteger("score", 100)` |
| ハイスコアを次の起動まで残す | `SaveInteger("bestScore", 1200)` / `LoadInteger("bestScore", 0)` |
| 文字を薄くする・フェードさせる | `TextRenderer`の`SetColor`でアルファを下げる（テクスチャは作り直されません） |
| スプライトを中心で回す | `sprite.SetPivot({ 0.5f, 0.5f })` |
| ポーズする | `LamaPon::Time::SetTimeScale(0.0f)` |
| 地面に接地しているか | 下方向へ`GetScene().Raycast(...)` |
| 道路・地形を実行時生成 | `MeshRendererComponent::SetProceduralMesh(...)` |
| 爆発の巻き込み判定 | `GetScene().OverlapSphere(中心, 半径)` |
| デバッグ出力 | `LamaPon::Logger::Instance().Info("...")` |

---

## よくあるコンパイルエラー

| エラーの内容 | 原因と直し方 |
|---|---|
| `'LamaPon': 識別子が見つかりません` | 先頭の`#include "LamaPon/LamaPon.h"`が抜けています |
| `未解決の外部シンボル` / コンポーネント一覧に出てこない | 最後の`LAMAPON_SCRIPT(クラス名);`が抜けています |
| `'override': メンバー関数がオーバーライドしていません` | 関数名か引数が違います（`Update(float)`など、宣言どおりに） |
| `戻り値を無視しています`（警告C4834） | `[[nodiscard]]`が付いた関数です。`if (...)`で結果を受けてください |
| `'->': ポインターではないものに使用` | `.`（ドット）と`->`（アロー）の取り違えです。ポインターは`->`、参照は`.` |
| `1.5` で型が合わない | 小数は`1.5f`と書きます（末尾のf） |
| `angle`が変な値になる | 角度はラジアンです。`XMConvertToRadians(90.0f)`を使います |
| 実行時に落ちる（アクセス違反） | `GetComponent`などの結果を`nullptr`チェックせずに使っています |
| `error C2513: 型は、識別子の名前を指定せずに宣言されています`<br>（`const bool far = ...;` などで出る） | **`far`／`near`／`pascal`はWindowsのマクロ**（16bit時代の名残で「空」に定義されている）なので、変数名に使えません。`distant`／`nearPlane`のように改名してください。エンジン側で`#undef`はしていません——Windowsのヘッダー自身が`struct midihdr_tag far *lpNext;`（mmeapi.h）のように小文字の`far`を使っており、消すとSDKが壊れるためです |

## つまずいたら

- **`GetComponent`が`nullptr`を返す** — そのコンポーネントが本当に
  アタッチされているか、Inspectorで確認してください。
  取得結果は必ず`nullptr`チェックしてから使います。
- **Actionの値が常に0** — プロジェクト設定の入力アクションに、その名前が
  登録されているか確認してください。
  未登録の名前は0を返します。
- **`Find`が重い** — シーン全体を走査するため、`Start`で1回だけ探して
  結果を持っておきます。
- **コンパイルエラー「co_awaitできない」** — 待てるのは
  `WaitForSeconds` / `WaitForNextFrame` / `WaitUntil` / `WaitWhile`の4種類だけです。
- **戻り値を無視すると警告が出る** — `RequestLoadAsync`、`Raycast`、
  `ConsumeClick`などは`[[nodiscard]]`です。
  結果を必ず受け取ってください。
- **物理の動きがフレームレートで変わる** — 継続的な力は`FixedUpdate`へ
  移してください。

---
