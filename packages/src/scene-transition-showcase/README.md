# Scene Transition Showcase 0.2.0

演出の設定をデータアセットに保存し、SceneのScriptから参照するサンプルです。
16種類のプリセットはInspectorで編集でき、複数のSceneで共有できます。
UnityのScriptableObjectとMonoBehaviourを組み合わせる使い方に相当します。
Unityの標準機能を移植したものではありません。

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

## 演出を編集する

アセットウィンドウで `presets/*.asset.json` を選び、Inspectorで編集して保存します。
演出・向き・動き方・シェーダーの模様はドロップダウンです。
色、覆う／保持／開く時間、中心、分割数、時間差、入力・BGMも指定できます。
Shaderの項目は演出を「シェーダー」にしたときだけ使われます。

自分専用の設定は「新規データアセット」から「シーン遷移プリセット」を選んで作ります。
共有するプリセットを編集すると、それを参照するすべてのSceneへ反映されます。
読み込み画面の全体設定はプロジェクト設定の「ゲーム」→
「従来の遷移・読み込み設定」、または自分のScriptの `Scenes().LoadingScreen()` で編集できます。

## コードをコピーして使う

パッケージを使わず、`SceneTransitionController.cpp`、
`SceneTransitionAssets.h`、`SceneTransitionSchema.h` を同じフォルダーへコピーし、
`presets`、`rules`、`shaders` を
`assets/packages/scene-transition-showcase/` へ配置しても使えます。
Scriptの既定のパスはこの配置を前提にしています。
登録を重複させないため、パッケージ導入と手動コピーはどちらか一方を使います。

自分のScriptからプリセットを読み、切り替える最小例:

```cpp
#include "SceneTransitionAssets.h"

const auto preset = LoadDataAsset(
    "packages/scene-transition-showcase/presets/IrisGold.asset.json");
LamaPon::SceneTransitionSettings transition;
if (LamaPonSceneShowcase::ReadPreset(*preset, transition))
{
    GetScene().Scenes().RequestLoadAsync(
        "scenes/Stage02.scene.json", transition);
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

## 既存プロジェクト

プロジェクト全体・UI Button専用の従来の遷移設定は読み込みと保存を維持しています。
新規プロジェクトは演出なしで始まり、必要なSceneへこのScriptを追加します。
移行するときは従来の全体設定を「なし」にして、ボタンのScene操作を空にしてから
コントローラーへ移動先・イベント名を指定します。
0.1.0のローカルサンプル `SceneTransition.Showcase` とは別のScript型です。
古いサンプルを置き換える場合は、そのコンポーネントを外して新しいものを追加してください。

ソース、プリセット、ルール画像はMITライセンスです。
