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

「クリック時のScene操作」でSceneを移動すると、既定の遷移（`SetDefaultTransition`、最初はすぐ切り替え）を使います。
遷移の途中は覆われて見えないボタンや表示し終える前の新Sceneのボタンを押せないため、連打しても二重に移動しません。
画面を覆う演出はエンジンに含まれません。自分で作る方法は[SceneとPrefab](scenes.md#シーン遷移)を参照してください。

用意された演出を使う場合は、Scene Transition Showcaseパッケージを導入し、
Sceneの「シーン遷移コントローラー」へプリセットと移動先Sceneを指定します。
Buttonの「クリック時のイベント」とコントローラーの「再生イベント名」を揃え、
Button側の移動先Scene・再読み込みは空にします。

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

## 絵を曲げるためのメッシュ分割

Sprite Rendererの「メッシュ分割（列×行）」を2×2以上にすると、画像を格子状の三角形メッシュとして描きます。
分割しただけでは見た目は変わりませんが、格子の頂点を動かすと、髪の毛先や服の裾など**絵の一部だけを曲げる**ことができます。

- 分割数は1〜64です。曲げたい方向に細かく分けるほど滑らかに曲がります（例: 縦長の髪なら1×6）。
- 頂点はスクリプトの`SetMeshDeformation`で動かします（`MeshRestPositions()`で変形前の位置を取れます）。
- メッシュで描くと、親の左右反転（`scale.x = -1`）や斜めの歪みも絵に反映されます。
  通常の矩形は回転と拡縮だけを反映するため、反転は180度回転として描かれます。
- UI Rect Transformを持つSpriteと、Web書き出しでは常に通常の矩形で描きます。

```cpp
auto* sprite = GetComponent<LamaPon::SpriteRendererComponent>();
sprite->SetMeshGrid(1, 4);                      // 縦に4分割
auto positions = sprite->MeshRestPositions();   // 上の行から順に並ぶ
positions.back().x += 20.0f;                    // 右下の頂点だけ右へ
positions[positions.size() - 2].x += 20.0f;     // 左下の頂点も右へ
sprite->SetMeshDeformation(positions);          // 裾が右へ流れる
```

## ボーンで絵を曲げる（Sprite Skin 2D）

`Sprite Skin 2D`は、ボーンにしたGameObjectの動きに合わせてSpriteのメッシュを曲げます。
長い髪・しっぽ・マント・リボンのように、**1枚の絵のまま途中から曲がる**パーツに使います。

1. 髪のSpriteの基準点（Pivot）を付け根に合わせ、Sprite Rendererの「メッシュ分割」を曲げる方向に細かくします
   （縦長の髪なら1×6など）。
2. 同じGameObjectへ`Sprite Skin 2D`を追加し、ボーン数を決めて「ボーンの鎖を作成」を押します。
   付け根から反対側の端までボーン（子のGameObject）が並び、その姿勢を基準にバインドされます。
   「揺れ物も付ける」をオンにすると、根元以外のボーンに`Sway 2D`が付き、頭を動かすと髪がしなって揺れます。
3. ボーンを回すと絵が曲がります。アニメーションでボーンを回したり、スクリプトで動かしたりできます。

- ボーンは任意のGameObjectを「ボーンを追加」で選ぶこともできます。並べ替えた後は「現在の姿勢でバインド」で基準を取り直します。
- 各頂点は近いボーンほど強く（最大4本）動きます。「重みの減衰」を大きくすると近いボーンだけが効き、関節がくっきり曲がります。
- メッシュ分割を変えたときは「現在の姿勢でバインド」をやり直してください（それまでは曲がりません）。
- ボーンを削除すると、その部分はSpriteと一緒に動くだけになります。
- 髪のSpriteを複製すると、子のボーンも複製され、複製側のボーンへ自動で付け替えられます。
- Web書き出しにはまだ対応していません（シーンに含まれていると書き出しを中止します）。

```cpp
auto* skin = hair->GetComponent<LamaPon::SpriteSkin2DComponent>();
auto bones = skin->CreateBoneChain(3, true);   // 3本のボーン＋揺れ物
bones[2]->GetTransform().SetEulerAngles(
    0.0f, 0.0f, DirectX::XMConvertToRadians(30.0f));  // 毛先だけ曲げる
```

## パラメータで部品を連動させる（Rig 2D／Keyform 2D）

Live2Dのパラメータのように、「角度X」「目の開き」「口の開き」といった**1つの値で複数の部品をまとめて動かす**仕組みです。

1. キャラクターの一番上のGameObjectへ`Rig 2D`を追加し、「パラメータを追加」で値を作ります
   （例: `AngleX` 範囲-30〜30・既定値0、`EyeOpen` 範囲0〜1・既定値1）。
2. 動かしたい部品（目・鼻・口・前髪など）へ`Keyform 2D`を追加します。
   パラメータが既定値の状態で「現在の姿勢を基準姿勢として記録」を押します。
3. Rig 2Dの値を動かしてから部品を移動・回転・拡縮し、Keyform 2Dで「現在の値で姿勢をキーに記録」を押します。
   例: `AngleX`を30にして鼻を右へずらして記録、-30にして左へずらして記録。
4. Rig 2Dの値を動かすと、キーの間が補間されて部品が動きます（編集中でもプレビューできます）。

| キーで変えられるもの | 合成のしかた |
|---|---|
| 位置・回転 | パラメータごとの差を足し合わせます（角度Xと角度Yを同時に動かせます） |
| 拡縮・不透明度 | パラメータごとの倍率を掛け合わせます |
| メッシュの頂点 | パラメータごとの移動量を足し合わせます。「メッシュを記録」で、ボーンなどで曲げた形を頂点の移動として保存できます |

- スクリプトからは`SetParameter("AngleX", 15.0f)`で値を変えます。マウスの方向に顔を向ける、会話中に口を動かす、などに使います。
- `Sprite Skin 2D`のボーンへ`Keyform 2D`を付けると、パラメータでボーンを回してメッシュを曲げられます
  （例: `AngleX`で前髪のボーンを傾け、顔の向きに合わせて前髪をしならせる）。
- パラメータの「自動の揺れ幅」を入れると、呼吸のように値が揺れ続けます（例: `Breath` 0〜1、揺れ幅0.5、0.25Hz）。
- 部品の姿勢は基準姿勢からの差として毎フレーム書き込むため、同じ部品に`Transform Animator`を併用しないでください。
  `Sway 2D`は併用でき、キーの回転の上に揺れが足されます。
- 「メッシュを記録」でボーンの形を保存した後は、ボーンを元の姿勢に戻してください（そのままだと二重に曲がります）。
- Web書き出しにはまだ対応していません（シーンに含まれていると書き出しを中止します）。

```cpp
auto* rig = GetComponent<LamaPon::Rig2DComponent>();
const auto& pointer = Graphics().Input().Pointer();
// 画面の中央からのずれで顔の向きを決める
rig->SetParameter("AngleX", (pointer.position.x - 640.0f) / 640.0f * 30.0f);
```

## 髪・服・飾りの揺れ（Sway 2D）

頭・体・腕・髪などをパーツごとのSpriteに分け、親子でつないだ2Dキャラクターでは、
髪や服のパーツへ`Sway 2D`を追加すると、親の移動や回転に遅れて揺れるようになります。

1. 揺らすパーツのSprite Rendererで、**基準点（Pivot）を付け根**に合わせます
   （前髪なら`{0.5, 0}`＝上辺の中央など）。この点が回転の中心になります。
2. パーツを頭や体の子にして、付け根の位置へ置きます。
3. `Sway 2D`を追加し、「Sprite Rendererの大きさから先端を設定」を押します。
   先端は回転中心から垂れ下がる先までのローカル座標です（Yは下向き）。
4. Playで親を動かして、ばね・減衰・最大角度を調整します。

| 項目 | 意味 | 既定値 |
|---|---|---|
| 先端 | 回転中心から揺れの先端までのローカル座標（拡縮前のピクセル） | `{0, 100}` |
| ばね | 静止姿勢へ戻る強さ。大きいほど硬く、速く戻ります | 60 |
| 減衰 | 揺れの収まりやすさ。小さいほど長く揺れ続けます | 8 |
| 慣性 | 親の移動に取り残される割合。0で移動には反応せず、回転にだけ反応します | 1 |
| 重力 | 先端に掛かる加速度（ピクセル/秒²、Yは下向き）。`{0, 980}`などで垂れ下がります | `{0, 0}` |
| 最大角度 | 静止姿勢から振れる角度の上限（度） | 45 |
| 風の揺れ幅／周波数／位相 | 何もしていなくても揺れ続ける周期的な揺れ。位相をずらすと房ごとにばらつきます | 0度／0.5Hz／0度 |

- **しなる揺れ:** 長い髪やマントは数節に分け、各節を親子でつないで全部に`Sway 2D`を付けます。
  親の節の揺れを先に計算するため、GameObjectの並び順に関係なく根元から先へ揺れが伝わります。
- **アニメーションとの併用:** `Transform Animator`などが書いた回転を静止姿勢として、その上に揺れを足します。
  毎フレーム少しずつ回転を足していくスクリプトを同じパーツに使う場合は、揺れを付けた子を別に作ってください。
- **左右反転:** 親の`scale.x`を`-1`にして向きを変えても、揺れの向きは正しく反転します。
- **瞬間移動:** 先端までの長さの10倍を超えて1フレームで移動すると、揺れを作り直します。
  スクリプトから明示的にやり直す場合は`ResetSimulation()`を呼びます。
- 時間が止まっている間（経過時間0）は揺れの姿勢を保ちます。無効にすると足した回転を外します。
- Web書き出しにはまだ対応していません（シーンに含まれていると書き出しを中止します）。

```cpp
LamaPon::Sway2DSettings settings{};
settings.tipOffset = { 0.0f, 120.0f };  // 付け根から下へ120ピクセル
settings.gravity = { 0.0f, 980.0f };     // 下へ垂れる
settings.windAmplitudeDegrees = 3.0f;    // そよ風
hair.AddComponent<LamaPon::Sway2DComponent>(settings);
```

## 目の瞬き（Blink 2D）

目のSpriteへ`Blink 2D`を追加すると、ランダムな間隔で「開→半目→閉→半目→開」とコマを切り替えて瞬きします。

1. 目の絵を1枚のスプライトシートに並べます。例: 3列1行で、左から「開いた目」「半目」「閉じた目」。
   閉じ始めのコマから閉じた目までは**連続した番号**に並べます（開くときはその逆順で戻ります）。
2. 左右の目を空のGameObject（例: `Eyes`）の子にまとめ、`Eyes`へ`Blink 2D`を追加します。
   自身と子孫のSprite Rendererがすべて同じコマになるので、両目が必ず同時に瞬きます。
   片目だけなら、その目に直接追加してもかまいません。
3. 「シート分割（列×行）」「開いた目のコマ」「閉じ始めのコマ」「閉じるまでのコマ数」をシートに合わせます。
   設定するとすぐに開いた目のコマが表示されます。

| 項目 | 意味 | 既定値 |
|---|---|---|
| 途中のコマ秒数 | 半目など途中のコマを1枚表示する秒数 | 0.04秒 |
| 閉じている秒数 | 閉じた目を表示する秒数 | 0.06秒 |
| 間隔 | 次の瞬きまでの秒数。最短〜最長の間でランダムに決まります | 2〜6秒 |
| 二度瞬きの確率 | 瞬きの直後（0.1〜0.25秒後）にもう一度瞬く確率 | 0.15 |
| 自動で瞬く | オフにすると、スクリプトの`Blink()`でだけ瞬きます | オン |
| 子のSpriteも切り替える | オフにすると、自身のSprite Rendererだけを切り替えます | オン |

- 別の`Blink 2D`を持つ子孫は、その`Blink 2D`に任せて上書きしません（片目ずつ別の瞬きにする場合など）。
- 寝ている・笑っている表情などで目を閉じたままにするには`SetHoldClosed(true)`を呼びます。
  `false`に戻すと、閉じている秒数の後に開きます。
- 対象のSprite Rendererの画像領域（Source Rect）を毎フレーム書き換えるため、
  同じSpriteに`Sprite Animator`を併用しないでください。
- 瞬きの時刻はGameObjectごとにずれるので、複数のキャラクターが一斉に瞬くことはありません。
- Web書き出しにはまだ対応していません（シーンに含まれていると書き出しを中止します）。

```cpp
auto* blink = eyes->GetComponent<LamaPon::Blink2DComponent>();
blink->SetHoldClosed(true);   // 目を閉じたままにする
blink->SetHoldClosed(false);  // 少しして開く
blink->Blink();               // 今すぐ1回瞬く
```

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

TextRendererはDirectWriteでWindowsフォントを透過テクスチャへ変換し、API非依存のSprite passでGame Viewへ描画します。

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
