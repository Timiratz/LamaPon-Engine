# Easing & Tween

動きに緩急をつけるための公式パッケージです。**イージング関数**（数式だけの
ヘッダー）と、**Tweenコンポーネント**（GameObjectを時間をかけて動かす）が
入っています。

エンジン本体には手を入れません。導入するとGame Moduleが自動ビルドされ、
「コンポーネントを追加」から使えるようになります。

## Tweenコンポーネント

GameObjectへ**「Tween（移動・回転・拡縮）」**を追加し、Inspectorで設定します。

| 設定 | 説明 |
|---|---|
| 動きの緩急 | `OutCubic`、`OutBack`、`OutBounce`など（下の一覧の名前を入力） |
| 所要時間（秒） | 目標へ到達するまでの時間 |
| 開始までの待ち（秒） | 遅れて動き始めます |
| 繰り返す / 往復する | 繰り返しON＋往復ONで行ったり来たりします |
| 開始時に再生 | オフにすると待機します |
| 位置を動かす / 移動量 XYZ | 開始位置からの相対移動 |
| 回転させる / 回転量 XYZ（度） | 開始回転からの相対回転 |
| 拡縮させる / 拡縮の倍率 | 1.5なら1.5倍まで拡大 |

開始時のTransformを基準にした**相対**の動きなので、置いた場所を変えても
そのまま動きます。

## 自分のScriptから使う

イージングだけ使いたい場合は`Easing.h`をincludeします。

```cpp
#include "LamaPon/LamaPon.h"
#include "packages/easing-tween/Easing.h"

class Popup final : public LamaPon::Script
{
public:
    void Update(const float deltaTime) override
    {
        m_elapsed += deltaTime;
        const float t = m_elapsed / 0.4f;   // 0.4秒で出現
        const float k = LamaPonEasing::Ease(
            LamaPonEasing::Type::OutBack,   // 少し行きすぎて戻る
            t);
        GetTransform().scale = { k, k, k };
    }

private:
    float m_elapsed{};
};

LAMAPON_SCRIPT(Popup);
```

`Ease(種別, 進み具合)`は進み具合を0〜1に丸めてから計算するので、
1を超えても安全です。

## イージングの一覧

`Linear`のほか、次の系統に`In`／`Out`／`InOut`があります。

| 系統 | 印象 |
|---|---|
| `Quad` / `Cubic` / `Quart` | 素直な加減速（数字が大きいほど急） |
| `Sine` | もっとも柔らかい加減速 |
| `Expo` | 一気に加速・減速する |
| `Circ` | 円弧のような重い加減速 |
| `Back` | 目標を少し行きすぎて戻る（ポップな出現） |
| `Elastic` | ばねのように何度も揺れる |
| `Bounce` | ボールが弾むように跳ねる |

名前の読み方は、**In＝始めがゆっくり**、**Out＝終わりがゆっくり**、
**InOut＝両端がゆっくり**です。UIの出現やキャラクターの着地には
`OutCubic`や`OutBack`、跳ね返りには`OutBounce`が使いやすいです。

## ライセンス

LamaPon本体と同じMITライセンスです。
