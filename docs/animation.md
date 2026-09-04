# Animation

Transformアニメーション、スケルタルアニメーション、Animator Controller、
タイムラインの使い方を説明します。

[← ドキュメント一覧へ戻る](index.md)

## 1分でためす（上下にふわふわ動く床）

1. 動かしたいGameObjectへ「**Transformアニメーター**」を追加します。
2. Inspectorの「**新規Animation Clipを作成...**」でClipを作り、
  「**Timelineを開く**」を押します。
3. 時刻0でKeyframeを追加 → 時刻スライダーを1秒へ動かし、GameObjectを
  少し上へ移動してKeyframeを追加 → ループをONにして保存します。
4. 「Play開始時に自動再生」がONのままPlayすると、上下に往復します。

スクリプトから操作する場合:

```cpp
auto* animator =
    GetComponent<LamaPon::TransformAnimatorComponent>();
if (animator != nullptr)
{
    animator->SetTrigger("Run"); // Controllerの遷移Triggerを発火
}
```

2Dスプライトのコマ送りアニメーションはこのページではなく、[UIと2D機能のSprite Animator](ui-2d.md)を使います。

## Animation

`.animation.json`は位置、回転、拡縮を持つTransformキーフレームを保存する再利用可能なAnimation Clipです。
位置と拡縮は線形補間し、回転はラジアン単位で角度をまたぐ場合も最短方向に補間します。
同じClipはAssetManagerで共有キャッシュされます。

GameObjectへ「Transformアニメーター」を追加し、Asset Browserの`Anim`アセットをダブルクリックするか、Inspectorのドロップ領域へ渡すとClipを割り当てられます。
Inspectorでは再生速度、ループ、Play開始時の自動再生、プレビュー時刻、Clip再読み込みを操作できます。
設定はScene／Prefabへ保存され、コピーや複製にも引き継がれます。

Clip未設定時はInspectorの「新規Animation Clipを作成...」から`assets/animations`へ初期Keyframe付きClipを作成できます。
「Timelineを開く」では次の操作が可能です。

- Clip名、長さ、ループ設定の編集
- 時刻スライダーによるTransform補間プレビュー
- 現在時刻へのKeyframe追加と選択Keyframeの移動・削除
- 位置、回転、拡縮の数値編集
- Inspectorやギズモで調整した現在のGameObject TransformをKeyframeへ記録
- Clipの安全保存と再読み込み

Timelineの`*`は未保存変更を表します。
Timelineを閉じる、Sceneを切り替える、Undo／Redoする、またはPlayを開始すると、プレビュー前のTransformへ自動的に戻ります。

### Animation State Machine

`.animator.json`は複数のAnimation ClipをStateとしてまとめます。
TransformAnimatorでは各Stateの`clip`、ModelRendererではモデル内の`modelClip`を使用します。
各Stateには速度、ループを設定でき、TransitionはTrigger、正規化Exit Time、クロスフェード時間を使用します。
TriggerとExit Timeを両方指定した場合は、両条件を満たした時に遷移します。
TriggerなしのTransitionはExit Timeによる自動遷移です。

Asset BrowserではControllerを`State`として表示します。
Transformアニメーターへダブルクリック、右クリック、ドラッグ＆ドロップで割り当てられ、Inspectorには現在Stateと遷移状態が表示されます。
Controller使用中はState側のClip／Loopが優先されます。
通常のClip再生とTimelineプレビューでも、回転はクォータニオンSlerpで補間します。
オイラー角はJSONで編集しやすくするための入力形式で、実行時の正本には使いません。

スキン付きglTF／GLB／FBXのModelRendererにもControllerをドラッグ＆ドロップできます。
遷移中はボーンごとの平行移動と拡縮を線形補間し、回転をクォータニオンSlerpで補間します。
Inspectorには現在State、ブレンド中表示、Controllerに含まれるTriggerボタンが表示されます。

ControllerアセットをダブルクリックするとノードグラフEditorが開きます。
Stateノードの移動、Entry State指定、Transition追加、Trigger／Exit Time／遷移時間、速度、ループを視覚的に編集でき、`Ctrl+S`でControllerへ保存します。
Stateは右側のInspectorから1Dまたは2D Freeform Cartesian Blend Treeへ切り替えられます。
1DではFloat Parameterと各子Clipのしきい値、2DではX／Y Parameterと各子Clipの2D座標を設定します。
2D編集欄には座標軸とChild配置のプレビューも表示されます。

2D Blend Treeは現在座標から各Childまでの距離を使って連続ウェイトを計算し、Child座標と完全一致した場合はそのClipだけを100%にします。
1 Stateにつき最大16 Clip、遷移中は遷移元と遷移先を合わせて最大32 Clipの重み付きボーン姿勢を合成できます。

### Root MotionとAnimation Event

ModelRendererの「Root Motionを適用」を有効にすると、指定したスケルトンノード（未指定時は最上位ノード）の移動とY回転をGameObjectへ反映します。
描画するスケルトンから同じルート変位を除去するため、GameObject移動との二重適用を防ぎます。
ループ境界ではClip終端までの差分と先頭からの差分を合成します。
設定はScene／Prefabへ保存され、EditorのAnimationプレビュー中はGameObjectを移動しません。

ノードグラフのState Inspectorでは、正規化時刻0～1へAnimation Event名と文字列Payloadを追加できます。
再生中に時刻を通過するとModelRendererのキューへ入り、C++から取得できます。

```cpp
LamaPon::AnimationEventNotification event;
while (modelRenderer->PollAnimationEvent(event))
{
    // event.state / event.name / event.payload / event.normalizedTime
}
```

ループ境界、クロスフェード中の遷移元／遷移先、逆再生の時刻通過にも対応しています。

RuntimeからTriggerを送る場合:

```cpp
if (auto* animator =
    player.GetComponent<LamaPon::TransformAnimatorComponent>())
{
    animator->SetTrigger("Run");
}
```

`assets/animations/Accent.animator.json`は`Idle`と`Float`をExit Timeで往復し、0.3～0.4秒でクロスフェードします。
サンプルシーンと青いアクセントPrefabに設定済みです。
`assets/animations/AnimatedSausage.animator.json`はFBX内の`Base`、`Wiggle`、`Spin`を`MoveX`／`MoveY` Parameterで混ぜる2D Blend Treeと、Trigger遷移用の`Spin` Stateを使用します。
サンプルシーンのInspectorで2つの値と`Spin` Triggerを操作して確認できます。

サンプルの`assets/animations/AccentFloat.animation.json`は3キーフレームで上下移動、回転、拡縮を行います。
Nested Prefabの`BlueAccent.prefab.json`へ設定済みなので、「操作できる立方体」Prefabを配置してPlayすると確認できます。

## よくあるつまずき

- **Playしても動かない** — Clipが割り当て済みか、「Play開始時の自動再生」が
  ONかを確認します。
  Controller使用時はEntry Stateが正しいかも確認します。
- **Timelineで動かしたのにPlayで元に戻る** — Timelineのプレビューは
  終了時に自動で元のTransformへ戻る仕様です。
  `*`（未保存マーク）が出ている間はClipへ保存されていません。
  `Ctrl+S`または保存ボタンで確定します。
- **SetTriggerが効かない** — Trigger名がControllerの遷移条件と一致して
  いるか、そのStateから出る遷移にTriggerが設定されているかを確認します。
- **モデルのアニメが再生されない** — Controllerの各Stateで`modelClip`
  （モデル内のクリップ名）が正しいかを確認します。
  FBX/glTFのクリップ名はInspectorに表示されます。
- **キャラが二重に進む（Root Motion）** — Root Motion有効時はアニメ側の
  移動がGameObjectへ移されます。
  スクリプトでも同時に前進させると二重になるため、どちらか片方にします。

Clipの基本形式:

```json
{
  "format": "LamaPonAnimationClip",
  "version": 1,
  "name": "浮遊",
  "duration": 2.0,
  "loop": true,
  "keyframes": [
    {
      "time": 0.0,
      "position": [0.0, 0.0, 0.0],
      "rotation": [0.0, 0.0, 0.0],
      "scale": [1.0, 1.0, 1.0]
    }
  ]
}
```
