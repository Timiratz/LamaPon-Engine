# データアセット（ScriptableObject相当）

GameObjectへぶら下がらない、単体で存在するデータの入れ物です。
カードの性能、敵のパラメーター、ステージの設定のように「シーンとは
別に持っておきたい数値」をエディターで編集し、ゲームから読みます。

[← ドキュメント一覧へ戻る](index.md)

## どういうときに使うか

| やりたいこと | 向いている置き場所 |
|---|---|
| 敵1体ごとの体力・速度・見た目 | **データアセット** |
| カード100枚の性能表 | **データアセット**（1枚1ファイル、または1ファイルへlistで） |
| そのGameObject専用の設定（弾の速さなど） | C++ Scriptの公開プロパティ |
| シーンに置いた物の位置や親子関係 | Scene / Prefab |
| プレイヤーの進行状況・設定 | PlayerPrefs / SaveData |

Unityを触ったことがあれば、**ScriptableObjectと同じ役割**です。
「データを持つだけの型を宣言し、その型のアセットを何個でも作る」形は
そのまま使えます。

## 手順

### 1. 型を宣言する（C++）

C++ Scriptのどこかで`LAMAPON_DATA_ASSET`を1回書きます。
Scriptの公開プロパティ（`LAMAPON_SCRIPT_WITH_SCHEMA`）と同じ書式の
スキーマで、Inspectorへ出す入力欄を決めます。

```cpp
#include "LamaPon/LamaPon.h"

namespace
{
    constexpr char EnemyDataSchema[] = R"({
        "fields": [
            { "name": "displayName", "displayName": "表示名",
              "type": "string", "default": "スライム" },
            { "name": "hitPoints", "displayName": "体力",
              "type": "int", "default": 10, "min": 1, "max": 9999 },
            { "name": "moveSpeed", "displayName": "移動速度",
              "type": "float", "default": 2.0,
              "min": 0.0, "max": 20.0, "step": 0.1 },
            { "name": "prefab", "displayName": "見た目のPrefab",
              "type": "asset", "assetType": "prefab" }
        ]
    })";
}

LAMAPON_DATA_ASSET("Game.EnemyData", "敵データ", EnemyDataSchema)
```

保存すると自動でGame Moduleがビルドされ、型が使えるようになります。

### 2. アセットを作る（エディター）

1. アセットウィンドウでフォルダーを右クリックし、
   「**新規データアセット**」を選びます。
2. **型**を選び、`ゴブリン.asset.json`のような名前を入力します
   （拡張子は`.asset.json`）。
3. 作られたファイルを選ぶと、インスペクターに宣言した入力欄が並びます。
   値を編集して「**保存**」を押します。

同じ型のアセットは何個でも作れます。敵ごと・カードごとに1ファイル
作るのが分かりやすい形です。

### 3. ゲームから読む（C++）

```cpp
class EnemySpawner final : public LamaPon::Script
{
public:
    void Start() override
    {
        const auto enemy = LoadDataAsset("data/ゴブリン.asset.json");

        const std::string name = enemy->GetText("displayName");
        const int hitPoints = enemy->GetInt("hitPoints");
        const float speed = enemy->GetFloat("moveSpeed");

        auto& spawned = Instantiate(enemy->GetAssetPath("prefab"));
        spawned.SetName(name);
    }
};
```

- **戻り値がnullになることはありません。** ファイルが無い・壊れている
  ときは空のデータアセットが返り、`GetInt`などは既定値を返します
  （ゲームは止まりません。読めなかったことはConsoleへ警告が出ます）。
- 同じパスは2回目以降キャッシュから返るので、`Update`から呼んでも
  ファイルを読み直しません。
- **`std::ifstream`で直接開かないでください。** 書き出したゲームでは
  アセットが1本の暗号化アーカイブへ入るため、エディターでだけ動いて
  書き出すと動かない実装になります。`LoadDataAsset`はどちらでも
  同じように読めます。

## 値の取り出し

| 関数 | 戻り値 | 対応する型 |
|---|---|---|
| `GetBool(key, 既定値)` | `bool` | `bool` |
| `GetInt(key, 既定値)` | `int` | `int` |
| `GetFloat(key, 既定値)` | `float` | `float` |
| `GetText(key, 既定値)` | `std::string` | `string` |
| `GetVector2/3/4(key, 既定値)` | `XMFLOAT2/3/4` | `vec2` / `vec3` / `vec4` |
| `GetColor(key, 既定値)` | `XMFLOAT4` | `color3` / `color4` |
| `GetAssetPath(key)` | `std::filesystem::path` | `asset` |
| `Has(key)` | `bool` | すべて |
| `TypeName()` / `Name()` | `std::string` | — |

キーが無いときは既定値が返ります。**型が違っても落ちません**（既定値に
なります）。

## 並び（list）

1つのアセットへ可変長の並びを持てます。要素は数値・文字列のほか、
`object`（フィールドの集まり）も入れられます。

```json
{
    "name": "waves",
    "displayName": "出現ウェーブ",
    "type": "list",
    "item": {
        "type": "object",
        "fields": [
            { "name": "count", "displayName": "数",
              "type": "int", "default": 3 },
            { "name": "delay", "displayName": "間隔（秒）",
              "type": "float", "default": 1.5 }
        ]
    }
}
```

インスペクターには「＋ 追加」と各行の「－」が出ます。読むときは:

```cpp
for (std::size_t index = 0; index < stage->Count("waves"); ++index)
{
    const auto& wave = stage->Item("waves", index);
    SpawnWave(wave.GetInt("count"), wave.GetFloat("delay"));
}
```

数値や文字列だけを並べた場合は`GetFloatAt` / `GetIntAt` /
`GetTextAt` / `GetBoolAt`で直接引けます。

```cpp
const float rate = enemy->GetFloatAt("dropRates", 0);
```

## 他のアセットを参照する（asset）

`"type": "asset"`の欄は、インスペクターで一覧から選ぶか、アセット
ウィンドウからドラッグ＆ドロップして指定します。値はassetsフォルダー
からの相対パスで保存されます。

```json
{ "name": "icon", "displayName": "アイコン",
  "type": "asset", "assetType": "texture" }
```

`assetType`で候補を絞れます。

| `assetType` | 候補 |
|---|---|
| `texture` | 画像（png / jpg / dds など） |
| `prefab` | `*.prefab.json` |
| `material` | `*.material.json` |
| `scene` | `*.scene.json` |
| `audio` | wav / ogg |
| `animation` / `animator` | `*.animation.json` / `*.animator.json` |
| `model` | fbx / gltf / glb など |
| `shader` | `*.hlsl` |
| `data` | データアセット。`"dataType": "Game.CardData"`でさらに型を絞れます |
| 省略 / `any` | すべて |

**アセットの名前を変えても参照は切れません。** エディターが移動・
リネーム時にJSONの中のパスを書き換えます（`.meta`と一緒にコミット
してください）。

この`asset`欄は**C++ Scriptの公開プロパティでも使えます**。
Script側からデータアセットを差したいときは次のように書きます。

```json
{ "name": "enemy", "displayName": "敵データ",
  "type": "asset", "assetType": "data",
  "dataType": "Game.EnemyData" }
```

```cpp
void LoadProperties(const std::string_view propertiesJson) override
{
    const auto properties =
        nlohmann::json::parse(propertiesJson, nullptr, false);
    m_enemyPath = properties.value("enemy", std::string{});
}
```

## ファイルの形

`*.asset.json`は素直なJSONです。手で編集しても、Gitで差分を見ても
読めます。

```json
{
  "format": "LamaPonDataAsset",
  "version": 1,
  "type": "Game.EnemyData",
  "values": {
    "displayName": "ゴブリン",
    "hitPoints": 24,
    "moveSpeed": 3.5,
    "prefab": "prefabs/goblin.prefab.json"
  }
}
```

インスペクターの「詳細設定 (JSON)」から直接編集することもできます。

## 困ったときは

| 症状 | 確認すること |
|---|---|
| 「新規データアセット」に型が出ない | `LAMAPON_DATA_ASSET`を書いた`.cpp`を保存し、Game Moduleのビルドが成功しているか（Consoleにエラーが出ていないか） |
| 「型『…』の宣言が見つかりません」 | 型名のつづりが宣言と一致しているか。Game Moduleがまだビルドされていないだけのこともあります |
| 値が既定値のまま | キー名がスキーマの`name`と一致しているか。`Has(key)`で確認できます |
| 書き出したゲームで読めない | `std::ifstream`で直接開いていないか。`LoadDataAsset`を使ってください |

## 関連

- [C++スクリプティング](scripting.md) — Scriptの公開プロパティ（同じスキーマ書式）
- [SceneとPrefab、セーブデータ](scenes.md) — 進行状況の保存はこちら
- [コード一覧](code-reference.md) — 使える関数の一覧
