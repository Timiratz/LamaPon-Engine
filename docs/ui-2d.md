# UIと2D機能

UI Canvas、Button、各ウィジェット、2D Tilemap、ゲーム内日本語テキストの使い方を説明します。

[← ドキュメント一覧へ戻る](index.md)

## 2Dの座標系

2Dは3Dと**別の座標系**で描かれます。
ここを知らずに書くと位置が総崩れになるので、先に4つだけ覚えてください。

| 決まり | 意味 |
|---|---|
| **1ワールド単位＝1ピクセル** | `position`や`Size`の数値はそのまま画面のピクセル数です |
| **原点は画面の左上、Yは下向き** | `y`を増やすと**下**へ動きます（3Dの上向きYとは逆） |
| **`position`はスプライトの左上角** | 既定では中心ではありません。**基準点（Pivot）**を変えると、位置が指す点と回転の中心を移せます（下記） |
| **カメラは2Dに影響しません** | `Sprite Renderer`と`Text Renderer`はカメラの位置・画角・投影を一切見ません。カメラを動かしても2Dは動きません |

```cpp
// 画面の左上から (100, 60) の位置に、64x64の赤い四角を出す
auto& sprite = object.AddComponent<LamaPon::SpriteRendererComponent>(
    DirectX::XMFLOAT2{ 64.0f, 64.0f },
    DirectX::XMFLOAT4{ 1.0f, 0.3f, 0.3f, 1.0f });
object.GetTransform().position = { 100.0f, 60.0f, 0.0f };
```

画面の大きさは実行時に`Graphics().UIWidth()` / `UIHeight()`で取れます（エディターではGame Viewの大きさ、書き出したゲームではウィンドウの大きさです）。
**解像度が変わっても同じ遊びにしたい場合**は、基準の高さ（720など）を決めてゲーム側はその座標で計算し、描画のときだけ`実際の高さ / 720` を掛けるのが簡単です。

- **`Transform`の`scale`も効きます。** `Size`（ピクセル）× `scale`が
  最終的な大きさです
- **`position.z`は描画順に使われません。** 前後関係は
  `SetSortOrder`（大きいほど手前）で決めます
- **UI Rect Transformを付けたGameObjectだけは例外**で、`position`ではなく
  Anchor／Pivot／Anchored Positionから画面上の位置が決まります（解像度に追従させたいHUDはこちらを使います）

### 基準点（Pivot）— 中心で置く・中心で回す

`Sprite Renderer`の**基準点**は「`position`がスプライトのどこを指すか」と「回転の中心」を同時に決めます。
0〜1の割合で、`{0,0}`が左上（既定）、`{0.5,0.5}`が中心、`{1,1}`が右下です。

```cpp
sprite.SetPivot({ 0.5f, 0.5f });          // 中心を基準にする
object.GetTransform().position = { 640.0f, 360.0f, 0.0f };  // 画面中央へ
object.GetTransform().SetEulerAngles(
    0.0f, 0.0f, DirectX::XMConvertToRadians(45.0f));        // その場で45度
```

**既定が左上のままなのは、既存のシーンを動かさないため**です。
回転や「中心を指定して置く」を使うときは`{0.5,0.5}`にしてください。
左上のまま回すと、回転のたびに絵の位置がずれます（角を軸に回るため）。

45度回した正方形はひし形になるので、画像なしでも山やトゲのような図形が作れます。
基準点を中心にしておけば、置きたい座標をそのまま書けます。

- **UI Rect Transformを持つGameObjectでは基準点は使いません。** 位置は
  Rect Transform側のAnchorとPivotが決め、回転は矩形の中心になります（Inspectorにもその旨が出ます）。

## 1分でためす（押すと反応するボタン）

1. GameObjectへ「**UI Canvas**」を追加します（画面全体の基準になります）。
2. その**子**GameObjectへ「**UI Rect Transform**」と「**UI Button**」を追加し、
  ラベルに「スタート」などを入力します。
3. ButtonのInspectorの「**クリック時のイベント**」へ`StartGame`と入力します。
4. 反応させたいGameObjectのスクリプトで、イベント名を購読します。

```cpp
#include "LamaPon/LamaPon.h"

class TitleScreen final : public LamaPon::Script
{
public:
    void Start() override
    {
        // ボタン側にコードは不要。イベント名だけでつながります
        On("StartGame", [this]
        {
            GetScene().Scenes().RequestLoadAsync(
                "scenes/stage-01.scene.json");
        });
    }
};

LAMAPON_SCRIPT(TitleScreen);
```

購読はスクリプトの破棄時に自動解除されます。
ボタンを介さずスクリプト同士で通知したい場合も同じ仕組みで`Emit("イベント名")`が使えます。

## UI CanvasとButton

`UI Canvas`は基準解像度と「幅／高さのどちらへ合わせるか」を持ち、子GameObjectの`UI Rect Transform`を実際のGame View解像度へ変換します。
Rect TransformではAnchor Min／Max、Pivot、Anchored Position、Size Deltaを編集でき、中央、四隅、全画面Stretchのプリセットも選択できます。

UI Rect Transformを持つGameObjectへSpriteRendererまたはTextRendererを追加すると、通常のTransform座標ではなく計算済みUI領域へ配置されます。
`UI Button`は背景色または任意の画像、日本語ラベル、通常／Hover／Pressed／Disabled色を持ちます。
エディターのGame View内マウス座標と、エクスポートしたゲームのクライアント座標の両方で動作します。

C++からクリックを処理する場合:

```cpp
if (auto* button = object.GetComponent<LamaPon::UIButtonComponent>();
    button != nullptr && button->ConsumeClick())
{
    // シーン切り替えやゲーム開始処理
}
```

サンプルSceneの右下には解像度変更へ追従する`クリックできます`ボタンを配置しています。
PlayしてGameタブを開くと、マウス操作に応じた色の変化を確認できます。

ButtonのInspectorには「クリック時のイベント」欄があり、イベント名（例:`StartGame`）を設定すると、クリック時にSceneのイベントバスへ発行されます。
C++ Scriptは`On("StartGame", [this]{ ... });`と書くだけで反応でき、ボタン側にコードは不要です（詳細は[C++スクリプティング](scripting.md)のイベント節へ）。

## スプライトアニメーション（Sprite Animator）

Sprite Rendererと同じGameObjectへ`Sprite Animator`を追加すると、1枚のスプライトシートをコマ送りするフリップブックアニメーションを再生できます。

1. Sprite Rendererへ歩行アニメ等を並べたスプライトシート画像を割り当てます。
2. Sprite Animatorの「シート分割（列×行）」でシートのコマ数を指定します
  （コマ番号は左上から右へ、次の行へと数えます）。
3. 「クリップを追加」で`walk`のようなアニメーションを定義し、
  開始コマ・コマ数・コマ/秒・ループを設定します。
4. 既定クリップは再生開始時に自動で流れます（「自動再生」で無効化できます）。

スクリプトからの切り替えは`Play`を使います。

```cpp
auto* animator = GetComponent<LamaPon::SpriteAnimatorComponent>();
animator->Play("jump");            // ループしないクリップは最終コマで停止
if (!animator->IsPlaying())
{
    animator->Play("idle");
}
```

スプライトの見た目そのものをHLSLで加工したい場合（フラッシュ、ディゾルブ等）は[カスタムShaderガイド](shaders.md)の2D節を参照してください。

内部的にはSprite Rendererの`SetSourceRect`（テクスチャの正規化部分矩形）を毎フレーム更新しています。
`SetSourceRect`は単体でも使えるため、アトラスから1コマだけを表示する静的スプライトにも利用できます。
シート分割・クリップ・既定クリップはシーンJSONへ保存され、複製やPrefabにも引き継がれます。

## はじけるエフェクト（2D Sprite Particles）

コインを取った、敵を壊した、着地した——そういう**一瞬の手応え**を足すための粒です。
**その場で一度に撒く**だけの単純なもので、画像もShaderも要りません（既定は白い四角）。

**エディターで試す**

1. GameObjectへ「**2D Sprite Particles**」を追加します
  （Add Component → Rendering）。
2. Inspectorの「**Burst 8**」「**Burst 32**」を押すと、その場で撒かれます。
  **再生しなくても**散り方を確かめられます（「Clear」で消えます）。
3. Start Color / End Color、Gravity、Size Growthを触って好みの散り方にします。

**スクリプトから出す**

```cpp
// 当たった瞬間に24粒はじけさせる
if (auto* burst = GetComponent<LamaPon::SpriteParticles2DComponent>())
{
    burst->Emit(24);
}
```

**知っておくと迷わない4つ**

| 決まり | 意味 |
|---|---|
| **`Emit`を呼んだときだけ出ます** | 自動では1粒も出ません。煙のように出し続けたいときは一定間隔で`Emit`を呼びます（自動放出は3Dの`Particle System`側の機能です） |
| **撒いた粒はその場に残ります** | GameObjectを動かしても付いてきません。動きながら撒けば、そのまま尾を引く演出になります |
| **Yは下向き** | Gravityの既定`{0, 180}`は**下**へ落ちます（このページ冒頭の「2Dの座標系」のとおりです） |
| **UI Rect Transformがあると描画されません** | ワールド空間の2D専用です。HUDの位置で出したいときは、Rect Transformを付けないGameObjectを別に置いてください |

粒の画像はInspectorの「Built-in Texture」から`builtin/circle`・`builtin/triangle`・`builtin/ring`を選べます（指定しなければ白い四角です）。
設定はシーンJSONへ保存され、複製やPrefabにも引き継がれます。

各メソッドの一覧は[コード一覧のSpriteParticles2DComponent](code-reference.md#spriteparticles2dcomponent)を参照してください。

## 2Dライティング（Light2D）

`Light2D`コンポーネントを追加したGameObjectは、Transformの位置を中心に半径・色・強度を持つ光源になります。
ワールド空間の`Sprite Renderer`と`Tilemap`を加算式に照らします。

```cpp
auto& torchLight = torch.AddComponent<LamaPon::Light2DComponent>();
torchLight.SetColor({ 1.0f, 0.7f, 0.3f });
torchLight.SetIntensity(1.5f);
torchLight.SetRadius(200.0f);
```

- **暗くはなりません。** 光源から遠いスプライトは通常どおりの明るさの
  ままで、近いスプライトへ色と明るさが加算されます。
  画面全体を暗くする「昼夜の陰影」のような用途ではなく、ランタン・ネオン・魔法陣のような「光る」演出向けです。
- 対象は独自Shaderを持たない`Sprite Renderer`と`Tilemap`です。
  Particle Systemと独自HLSLを設定済みのSpriteは対象外です（独自Shader側で同様の効果が欲しい場合は[カスタムShaderガイド](shaders.md)を参照してください）。
- **UIは既定では照らしません。** UIは読めることが最優先で、勝手に色が
  乗ると困る場面のほうが多いためです。
  `SetAffectsUI(true)`（Inspectorの「UIも照らす」）を入れたLight2Dが1つでもあると、UIはその灯りだけで照らされます。
  ランタンでメニューを照らすような演出用です。
- **画面で16灯まで**が同時に効きます。灯りはスプライト単位ではなく画面
  単位で評価するので、画面いっぱいに広がるTilemapでも、地図の端と端でそれぞれ近くの灯りに照らされます。
- Radius・色・位置（画面ピクセル基準）はこのエンジンのワールド座標系
  そのまま（1ワールド単位＝1ピクセル）を使うため、Sprite RendererのSizeと同じ感覚で数値を決められます。
- Source Rect（アトラス部分表示）やTilemapのアトラスとも併用できます
  （UVは頂点から受け取るため、コマの切り出しがそのまま効きます）。

## Sprite Mask（表示範囲の切り抜き）

`Sprite Mask`コンポーネントを追加したGameObjectは、Transformの位置を中心に矩形または円の範囲を持つマスクになります。
`Sprite Renderer`の「Sprite Mask」欄で`マスクの内側だけ表示`／`マスクの外側だけ表示`を選ぶと、そのSpriteが最も近いマスクでクリップされます。
フォグ・オブ・ウォーの視界、懐中電灯の明かり、ウィンドウ状の演出に使えます。

```cpp
auto& mask = maskObject.AddComponent<LamaPon::SpriteMaskComponent>();
mask.SetShape(LamaPon::SpriteMaskShape::Circle);
mask.SetSize({ 300.0f, 300.0f }); // 円は幅の値を直径として使います

auto& fogSprite = fog.AddComponent<LamaPon::SpriteRendererComponent>();
fogSprite.SetMaskInteraction(
    LamaPon::SpriteMaskInteraction::VisibleOutsideMask);
```

- クリップは境界がくっきりした二値判定です（半透明フェードなし）。
- マスク形状は矩形・円のみです。任意画像の透明部分を型として使う
  画像の透明部分を使うアルファマスクには対応していません。
- 対象は独自Shaderを持たない`Sprite Renderer`のみで、1つのSpriteが
  従うのは最も近いマスク1つだけです。
  Light2Dと同時に必要なSpriteではSprite Maskが優先されます（バッチ切替で使えるカスタムShaderは1つのため、同時使用はできません）。
- Light2Dと同じくSource Rectの矩形情報は復元していないため、
  アトラス使用時は全体表示になります。

## 2D TilemapとTile Palette

GameObjectの「コンポーネントを追加」から`Tilemap`を追加できます。
Tilemapは1枚のタイルシートを列数・行数で分割し、各グリッドセルにはタイル番号だけを保持します。
負座標を含む任意の位置へ配置でき、セルサイズ、Atlas分割、全体色とアルファを設定できます。
配置セルとタイルシート参照はScene／PrefabのJSONへ保存されます。
Inspectorの「描画順」（Sprite RendererのSortOrderと同じ尺度、数値が大きいほど手前）を使うと、背景／地形／前景のように複数のTilemapを重ねたときの表示順を制御できます。

「タイルパレット」タブは他のEditorタブと同様に、タブをドラッグしてDock位置の変更、独立ウィンドウ化、サイズ変更ができます。
使い方は次の通りです。

1. HierarchyでTilemapを持つGameObjectを選択
2. Asset Browserからタイルシート画像をパレットへドロップ
3. 画像の列数・行数とゲーム上のセルサイズを設定
4. パレットでタイルを選択し、Sceneタブを左ドラッグして配置
5. 「消去」へ切り替えて左ドラッグするとセルを削除

Sceneタブにマウスがある間は`B`でペイント、`E`で消去へ切り替えられます。
一筆分の連続編集が1回のUndo履歴になるため、`Ctrl+Z`でまとめて取り消せます。
タイルシートのファイル移動・改名・削除確認はAsset Browserの参照管理にも統合されています。

### 当たり判定の自動生成

InspectorのTilemapコンポーネントにある「コライダーを生成」ボタンを押すと、配置済みのセルをすべて衝突対象として、隣接セルをまとめた最小限の`BoxCollider2D`を子GameObjectとして自動生成します（貪欲な矩形マージ。
タイル1枚ごとにColliderを置くより描画・判定コストが小さく済みます）。
生成されたColliderはTilemapの子として`タイルコライダー（自動生成）`という名前で並び、通常のBox Collider 2Dと同じくSceneのJSONへ保存されます。
セルを編集したら再度「コライダーを生成」を押してください（既存の生成済みColliderは置き換えられます）。
現状は配置済みセル全体が衝突対象になるため、当たり判定の要らない背景・装飾タイルは衝突を持たせたいTilemapとは別のTilemap GameObjectへ分けてください。

### 奥行きのあるスクロール（Parallax Layer）

`Parallax Layer`コンポーネントを背景・前景のTilemapやSpriteへ追加すると、参照（既定はMain Camera）の移動量に倍率を掛けた分だけ自身を動かします。

```cpp
auto& parallax =
    background.AddComponent<LamaPon::ParallaxLayerComponent>();
parallax.SetFactor({ 0.3f, 0.3f }); // カメラの30%の速さで動く遠景
```

- 倍率1.0で参照と同じ速さ（奥行きなし）、0.5で半分の速さ（遠い背景）、
  0で画面に固定（空のような最遠景）、1.0より大きいと参照より速く動きます（近景）。
  X／Yを別々に設定できます。
- 参照はInspectorの「参照」欄でGameObjectを直接指定することもできます
  （未設定＝Main Camera追従が既定です）。
- 初回更新時点の位置を原点として記録するため、Playを開始した瞬間から
  参照が動いた分だけ追従します。
  参照を実行中に切り替えると、その時点の位置を新しい原点として記録し直します。

## ゲーム内日本語テキスト

TextRendererはDirectWriteでWindowsフォントを透過テクスチャへ変換し、SpriteBatchでGame Viewへ描画します。

Inspectorでは次を編集できます。

- UTF-8テキスト
- 文字サイズ
- 文字色
- フォントファミリー
- レイアウト幅／高さ（0は自動サイズ）
- 自動折り返し
- 左／中央／右揃え
- 上／中央／下揃え

固定レイアウト枠を指定すると、その範囲内で整列と折り返しが行われます。
日本語文字テクスチャは、テキスト・書式・レイアウト設定ごとにAssetManagerでキャッシュされます。

### 数字が変わり続ける表示のコツ

文字テクスチャは**文字列ごとに1枚**作られます。
「スコア 0」と「スコア 1」は別物なので、スコアやタイマーのように中身が変わる表示は更新のたびに新しいテクスチャができます（DirectWriteでの描画＋GPUテクスチャ生成）。
キャッシュの上限は既定で32MiBです。上限を超えると古い項目から削除されるため、
容量は増え続けません。ただし、毎フレーム内容が変わる表示は毎回作り直します。

描画負荷を抑える方法は2つあります。

- **変わったときだけ`SetText`する。** 同じ値を渡した`SetText`は何もしない
  ので、毎フレーム呼んでも無駄は出ません。
  表示する値を`int`で持ち、直前の値と異なる場合だけ文字列を作ります
- **桁ごとに別のTextRendererにする。** 1桁ずつなら`"0"`〜`"9"`の10枚で
  足りるので、どれだけスコアが伸びてもテクスチャは増えません。
  UI Rect Transformで桁を等間隔に並べ、`SetText`へ1文字ずつ渡します

## よくあるつまずき

- **ボタンが押せない** — ButtonのGameObjectが**UI Canvasの子**で、
  **UI Rect Transform**を持っているかを確認します。
- **UIの位置が解像度でずれる** — Rect TransformのAnchorを画面の四隅や
  中央へ正しく設定します（右下に置くUIは右下Anchorに）。
  Canvasの基準解像度と「幅／高さのどちらへ合わせるか」も確認します。
- **スプライトアニメが動かない** — Sprite Animatorの「シート分割（列×行）」を
  設定したか、`Play("名前")`のクリップ名が定義と一致しているかを確認します。
- **ループしないアニメの終わりを検出したい** — `IsPlaying()`がfalseに
  なったタイミングで次のクリップへ切り替えます（ページ内サンプル参照）。
- **文字が表示されない** — TextRendererのフォントファミリー名がWindowsに
  存在するか、文字色のアルファが0になっていないかを確認します。
- **2D Sprite Particlesの粒が出ない** — `Emit`を呼んでいるかを最初に
  確認します（自動では出ません）。
  呼んでいるのに見えない場合は、そのGameObjectに**UI Rect Transform**が付いていないか、Sort Orderが他のスプライトより手前になっているかを確認します。
- **UIが3Dの後ろに隠れる／色が変わる** — UIと2Dはポストエフェクトの後に
  合成されるため通常は最前面・原色のままです。
  SpriteRendererをUI Rect Transformなしで使うとワールド空間の2Dとして扱われる点に注意してください。
