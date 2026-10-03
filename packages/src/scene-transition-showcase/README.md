# Scene Transition Showcase 0.3.0

画面を覆ってからSceneを切り替える、おしゃれなシーン遷移のサンプルです。
16種類のプリセット（データアセット）をInspectorで編集でき、複数のSceneで共有できます。
UnityのScriptableObjectとMonoBehaviourを組み合わせる使い方に相当します。
Unityの標準機能を移植したものではありません。

エンジン本体は「覆い終えてから切り替える」流れと覆い具合（0～1）だけを持ち、
画面を覆う絵は描きません。このパッケージは、画面全体へ引き伸ばした1枚の
Spriteと自前のシェーダー（`shaders/LamaPonSceneTransition.hlsl`）で覆いを描きます。
自分で遷移を作るときの手本としても読めます。

## 導入と再生

導入後に `scenes/Showcase.scene.json` を開いてPlayすると、
配置済みのボタンから演出を再生できます。「Transition Controller」を選び、
プリセットを差し替えて試せます。自分のSceneへ追加する手順は次のとおりです。

1. 「パッケージマネージャー」でScene Transition Showcaseを追加します。
   ダウンロードしたZIPは「拡張機能」→「Zipから読み込む...」でも導入できます。
2. Game Moduleのビルド完了後、Sceneに空のGameObjectを作ります。
3. 「コンポーネントを追加」から「シーン遷移コントローラー」を追加します。
4. 「遷移プリセット」へ `presets/IrisGold.asset.json` などを指定します。
   「移動先Scene」が空なら、Sceneを切り替えずに演出だけ再生します。
5. 試すときは「再生開始時に一度実行」をオンにしてPlayします。
   通常の既定値はオフです。起動Sceneの読み込みが完了してから実行します。

ボタンから再生する場合は、コントローラーの「再生イベント名」と、
UI Buttonの「クリック時のイベント」を同じ名前にします。
既定名は `SceneTransition.Play` です。移動先はコントローラーに指定し、
**ボタン側の移動先Scene・再読み込みを設定しないでください。**
同じイベント名を使うコントローラーはSceneに1つ配置します。
複数必要なら、それぞれ別のイベント名にします。

## しくみ

1. コントローラーがプリセットを読み、時間（覆う・保持・開く）をエンジンへ渡して
   `Scenes().RequestLoadAsync(移動先, 遷移)` か `Scenes().PlayTransition(遷移)` を呼びます。
2. 同時に `SceneTransition.Overlay` という名前のGameObjectを用意します
   （UI Rect Transformで画面全体へ広げたSprite Rendererと、
   「シーン遷移の覆い」Script）。シーンを切り替えても残るように
   `DontDestroyOnLoad` を指定し、次の遷移でも使い回します。
3. 「シーン遷移の覆い」Scriptが毎フレーム `Scenes().TransitionCoverage()` を読み、
   シェーダーのCustomParametersへ渡します。遷移が終わると自動で隠れます。

「シーン遷移の覆い」はコントローラーが自動で追加します。自分で追加する必要はありません。
UI Buttonの移動先Sceneなど、このパッケージ以外が始めた遷移には覆いを描きません。

## 演出を編集する

アセットウィンドウで `presets/*.asset.json` を選び、Inspectorで編集して保存します。
演出・向き・動き方・シェーダーの模様はドロップダウンです。
色、覆う／保持／開く時間、中心、分割数、時間差、入力・BGMも指定できます。
シェーダーの模様は、演出を「シェーダー」にしたときだけ使われます。
「なし（すぐ切り替え）」は覆いを描かず、時間も使わずに切り替えます。

自分専用の設定は「新規データアセット」から「シーン遷移プリセット」を選んで作ります。
共有するプリセットを編集すると、それを参照するすべてのSceneへ反映されます。
覆っている間に読み込みが続いたときの読み込み画面は、プロジェクト設定の
「ゲーム」→「読み込み画面」、または自分のScriptの `Scenes().LoadingScreen()` で編集できます。

「独自シェーダー」へ `.hlsl` を指定すると、パッケージのシェーダーの代わりに使います。
`shaders/LamaPonSceneTransition.hlsl` を複製して書き換えるのが簡単です。
受け取るCustomParametersの並びはファイル先頭のコメントにあります。

## コードをコピーして使う

パッケージを使わず、`SceneTransitionController.cpp`、`SceneTransitionOverlay.cpp`、
`SceneTransitionOverlay.h`、`SceneTransitionAssets.h`、`SceneTransitionSchema.h` を
同じフォルダーへコピーし、`presets`、`rules`、`shaders` を
`assets/packages/scene-transition-showcase/` へ配置しても使えます。
Scriptの既定のパスはこの配置を前提にしています。
登録を重複させないため、パッケージ導入と手動コピーはどちらか一方を使います。

自分のScriptからプリセットを再生する最小例:

```cpp
#include "SceneTransitionOverlay.h"

std::string error;
if (!LamaPonSceneShowcase::PlayPreset(GetScene(),
        "packages/scene-transition-showcase/presets/IrisGold.asset.json",
        "scenes/Stage02.scene.json", error))
{
    LamaPon::Logger::Instance().Warning(error);
}
```

アセットは `LoadDataAsset` 経由で読みます。書き出したゲームの暗号化アーカイブにも
対応するため、ファイルを直接開くコードは不要です。
無効なプリセットは警告を出して再生せず、コントローラー無効中・読み込み中・
演出中のイベントは無視します。破棄時にはイベント購読を自動解除します。

## プリセット一覧

Fade、DiagonalWipe、IrisGold、DiamondIris、Blinds、Tiles、DiamondTilesGold、
DotsForward、Shutter、RuleImage、DissolveGold、Clock、Spiral、Ripple、Hexagons、Heart。
`previews/transitions.png` は演出の参考画像です。

## 0.2.0からの変更

エンジンが遷移の覆いを描かなくなったため、覆いはこのパッケージが描きます。
プリセットの形式は同じなので、自作のプリセットもそのまま使えます。
`ReadPreset` は時間（`SceneTransitionSettings`）と見た目（`Look`）を別々に返します。
自分のScriptで0.2.0の `ReadPreset(preset, transition)` を使っていた場合は、
上の `PlayPreset` へ置き換えてください（時間だけを渡すと覆いが描かれません）。
0.2.0の「独自シェーダー」はエンジンのCustomParametersの並びを前提にしていたため、
パッケージのシェーダーの並びに合わせて書き直す必要があります。

ソース、プリセット、ルール画像はMITライセンスです。
