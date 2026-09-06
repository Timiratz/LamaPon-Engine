# 入力（Input Action Mapping）

キーボード、マウス、ゲームパッドの入力とAction Mappingを説明します。

[← ドキュメント一覧へ戻る](index.md)

## 1分でためす

WASDで動くキャラクターは、次のスクリプトをGameObjectへ付けるだけです（既定Actionの`MoveHorizontal`／`MoveVertical`を使うので設定は不要です）。

```cpp
#include "LamaPon/LamaPon.h"

class SimpleMover final : public LamaPon::Script
{
public:
    void Update(float deltaTime) override
    {
        auto& input = Graphics().Input();

        // -1～1の値。WASDとゲームパッド左Stickの両方に反応します
        const float x = input.Value("MoveHorizontal");
        const float z = input.Value("MoveVertical");

        GetTransform().position.x += x * 5.0f * deltaTime;
        GetTransform().position.z += z * 5.0f * deltaTime;

        if (input.WasPressed("Jump"))
        {
            // Space／ゲームパッドAを押した最初のフレームだけ通ります
        }
    }
};

LAMAPON_SCRIPT(SimpleMover);
```

押しっぱなし・押した瞬間・離した瞬間は使い分けられます。

| API | 返るタイミング | 用途の例 |
|---|---|---|
| `Value("名前")` | 毎フレーム`-1～1`の値 | 移動、視点操作 |
| `IsDown("名前")` | 押している間ずっと`true` | ダッシュ、チャージ |
| `WasPressed("名前")` | 押した瞬間の1フレーム | ジャンプ、決定 |
| `WasReleased("名前")` | 離した瞬間の1フレーム | チャージ解放 |

## Actionの設定

「ファイル」→「プロジェクト設定とビルド...」の入力アクションから、名前付きActionへキーボードとゲームパッドを割り当てられます。
各Bindingの倍率を`-1`にすると、Aキーの左移動のような負方向の入力になります。
同じActionへ複数のBindingを設定すると、値を合成して`-1～1`へクランプします。

既定では次のActionがあります。

- `MoveHorizontal`: A／D、ゲームパッド左Stick X
- `MoveVertical`: S／W、ゲームパッド左Stick Y
- `LookHorizontal`／`LookVertical`: ゲームパッド右Stick
- `Jump`: Space、ゲームパッドA
- `Submit`／`Cancel`
- `Fire`／`AltFire`: マウス左／右ボタン、ゲームパッドトリガー

## マウスと生のキーボード

マウスカーソルの位置・ボタン・ホイールは`Graphics().Input().Pointer()`から取得できます。
Actionを介さない生のキーボード状態も`Application::KeyboardState()`で従来どおり利用できます。
サンプルシーンの立方体には`InputMover`があり、Play中にWASDまたは左Stickで移動できます。

## よくあるつまずき

- **自分で決めたAction名に反応しない** — `Value("Dash")`のように書く前に、
  プロジェクト設定の入力アクションへ`Dash`を登録してください。
  未登録の名前は常に0を返します。
- **ジャンプが連打される** — `IsDown`は押している間ずっとtrueです。
  1回だけ反応させたい操作は`WasPressed`を使います。
- **左に動かない** — Aキー側のBinding倍率を`-1`にします（既定Actionは設定済み）。
- **ゲームパッドが反応しない** — Windowsに認識されているか確認してください。
  接続し直した場合もPlayし直す必要はありません。
- **エディターで入力が効かない** — Play中の入力はGame Viewへ送られます。
  Game Viewタブが表示されているか確認してください。
  なお**名前や数値を入力している最中（テキスト欄にカーソルがある間）は、キーボードのActionは意図的に止まります**。
  オブジェクト名に「w」と打つたびに主人公が動いては困るためです。
  欄の外をクリックすれば戻ります（マウスとゲームパッドは止まりません）。
