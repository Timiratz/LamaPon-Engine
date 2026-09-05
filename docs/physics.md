# 物理と衝突判定

Rigidbody、Collider、Raycast、CharacterController、物理マテリアル、CCDの使い方を説明します。

[← ドキュメント一覧へ戻る](index.md)

## 1分でためす（落ちる箱とジャンプ）

1. 床用GameObjectに**3Dボックスコライダー**を追加し、横に広げます
  （Rigidbodyは付けません＝動かない床になります）。
2. 箱用GameObjectにMeshRenderer（立方体）、**3Dボックスコライダー**、
  **リジッドボディ**を追加し、床の上へ置きます。
3. Playすると重力で落ちて床に載ります。ジャンプさせるには次のスクリプトを
  箱へ追加します。

```cpp
#include "LamaPon/LamaPon.h"

class JumpBox final : public LamaPon::Script
{
public:
    void Update(float) override
    {
        // Space（既定のJump）で上向きに瞬間的な力を加えます
        if (Graphics().Input().WasPressed("Jump"))
        {
            auto* body =
                GetComponent<LamaPon::RigidbodyComponent>();
            if (body != nullptr)
            {
                body->AddForce(
                    { 0.0f, 6.0f, 0.0f },
                    LamaPon::ForceMode::Impulse);
            }
        }
    }

    void OnCollisionEnter(
        const LamaPon::CollisionEvent& event) override
    {
        // 何かに当たった瞬間に呼ばれます（着地判定などに）
    }
};

LAMAPON_SCRIPT(JumpBox);
```

人型キャラクターの移動は、自前でRigidbodyを操作するより**Character Controller**（後述）を使うのが簡単です。
WASDとSpaceが最初から使えます。

## RigidbodyとCollider

Inspectorの「コンポーネントを追加」から次を追加できます。

- 2Dボックスコライダー
- 2D円コライダー
- 2D多角形コライダー（Polygon Collider 2D）
- 3Dボックスコライダー
- 3Dカプセルコライダー
- 3Dスフィアコライダー
- 3D Convex Hullコライダー
- Mesh Collider（モデルの三角形メッシュ）
- リジッドボディ
- Character Controller

Rigidbodyは質量、速度、角速度、ローカル重心、移動／回転抵抗、重力、キネマティック、回転軸ごとの固定、衝突検出モードを設定できます。
力やトルクに加え、`AddForceAtPosition`で重心から離れた位置へ力を加えられます。
衝突解決も接触点の速度とボックス形状から求めた慣性を使うため、崖際に偏って載った箱、斜めに接触したOBB、摩擦で転がる物体が角運動を伴って反応します。

高速で移動する弾丸などは`Continuous (CCD)`を選ぶと、前の位置から移動先までをスイープし、最初の接触位置で止めて薄いColliderのすり抜けを抑えます。
通常の物体は負荷の小さい`Discrete`を使用します。
Colliderをトリガーにすると衝突イベントのみ発生し、位置の押し戻しは行いません。
各Colliderの「摩擦係数」と「反発係数」はPhysics Materialとしてシーンへ保存されます。
摩擦は接触面に沿う速度を減速させ、反発は0で跳ね返らず、1で強く跳ね返ります。

```cpp
auto& body = object.AddComponent<LamaPon::RigidbodyComponent>();
body.SetMass(2.0f);
body.SetCenterOfMass({ 0.0f, -0.2f, 0.0f });
body.AddForceAtPosition(
    { 0.0f, 8.0f, 0.0f },
    worldHitPoint,
    LamaPon::ForceMode::Impulse);
```

`ForceMode`は継続的な`Force`／`Acceleration`と、瞬間的な`Impulse`／`VelocityChange`に対応します。
`CollisionEvent::point`からEnter／Stay時のワールド接触点も取得できます。

Box同士の面接触は、3Dでは最大4点、2Dでは接触辺の最大2点をContact Manifoldとして生成します。
Manifold内の法線Impulseを8回、同じBroad Phase候補に含まれる全接触ペアを4回反復するため、上段の反力を下段の接触へ同じPhysics Substep内で伝え直せます。
十分低速で下から支持されたRigidbodyは自動的にSleepし、力、トルク、速度変更、別の物体からの衝突でWakeします。
C++の`Sleep()`／`WakeUp()`とInspectorの「休止させる」／「起こす」から手動操作もできます。

Windows Runtimeの物理演算は描画FPSと分離した固定間隔（既定60 Hz）で実行され、プロジェクトの`fixedTimeStep`で変更できます。
C++ Scriptでは、通常のアニメーションやUIは`Update(deltaTime)`で経過時間を使い、Rigidbodyへ継続的な力を加える処理は`FixedUpdate(fixedDeltaTime)`へ記述します。
既定設定では、追いつき上限に達しない限り30／60／144 FPSのどの場合も1秒間に60回の物理更新となり、描画FPSによる挙動の変化を抑えます。重いフレームは入力時間を最大0.1秒、固定更新を`maximumCatchUpSteps`回に制限します。捨てた時間は物理時計へ加算しません。

Rigidbodyの「描画を補間」は既定で有効です。
物理Transformは最新の確定状態を保持したまま、描画時だけ直前と現在の位置を線形補間し、回転をQuaternion Slerpで補間します。
Mesh、Sprite、Light、Cameraと子GameObjectへ自動適用されるため、Compound Colliderの子表示も親Rigidbodyへ滑らかに追従します。
Teleportなど物理外でTransformを直接変更した場合は履歴を同期し、古い位置から意図しない補間をしません。
不要な物体はInspectorまたは`RigidbodyComponent::SetInterpolate(false)`で無効化できます。
`Scene::PhysicsInterpolationAlpha()`と`GameObject::InterpolatedWorldMatrix()`から描画補間率／補間済み行列を独自Rendererでも利用できます。
これらは`LateUpdate`で参照します。一時停止のゼロ時間更新では表示済みの補間位置を保持し、停止中のTransform編集は履歴へ同期します。

```cpp
void FixedUpdate(float fixedDeltaTime) override
{
    auto* body = Owner().GetComponent<LamaPon::RigidbodyComponent>();
    body->AddForce(
        { 0.0f, 0.0f, 8.0f },
        LamaPon::ForceMode::Acceleration);
}
```

<a id="physics-presentation-time"></a>

### ゴースト・リプレイと描画時刻（Windows Runtime）

1フレームの順序は`Update`→（`FixedUpdate`→物理計算を0回以上）→`LateUpdate`→描画です。物理に同期するタイマーは`FixedUpdate`の引数を積算し、表示は`LateUpdate`で更新します。固定時刻をそのままゴーストの表示へ渡すと階段状に進み、補間された自車やカメラとずれます。`Update`の`deltaTime`を別途足す方法も、物理の追いつき上限に達すると先行するため使いません。

`Scene::PhysicsTiming()`は物理の刻み幅、補間率、実行済み時間、描画時刻をまとめて返します。当該フレームの値は`LateUpdate`で確定し、`Update`では前フレームの値です。進行中のゲーム固有タイマーは`InterpolateTime()`へ渡すと、Rigidbodyの描画遅延を考慮した非負の時刻になります。タイマーの開始位置をScene時計に合わせる必要はありません。

```cpp
// Script内の例。SampleGhostはゲーム側で実装する記録データの補間処理です。
double m_raceTime{};

void FixedUpdate(const float dt) override
{
    if (m_racing)
    {
        m_raceTime += static_cast<double>(dt);
    }
}

void LateUpdate(float) override
{
    if (m_racing)
    {
        const auto& timing = GetScene().PhysicsTiming();
        SampleGhost(timing.InterpolateTime(m_raceTime));
    }
}
```

`SampleGhost`は時刻を挟む2サンプルを選び、位置を線形補間、回転をQuaternion Slerpなどで補間します。タイマーを停止・リセットした際の表示、記録の先頭・末尾の扱いはゲーム側で決めます。既存ゴーストがこのAPIへ自動的に切り替わることはありません。補間済みのゴースト表示はRigidbodyによる二重補間を避けてください。

- `fixedDeltaTime`：当該フレームで採用した刻み幅。固定更新中の設定変更は次フレームから採用します。
- `simulatedTime`／`PresentationTime()`：`Scene::Clear()`以降に実行した物理時間／対応する描画時刻。実時間の時計ではありません。
- `discardedDeltaTime`／`discardedTime`：Sceneへ渡された時間のうち、上限で実行しなかった秒数の当該フレーム分／累計。CLIの実行状態の`physics`にも出力します。上流での時間制限、GPU負荷、配信負荷を直接測る値ではありません。

`Time::FixedDeltaTime()`と`Scene::FixedPhysicsDeltaTime()`は互換用の既定値です。実行中の刻みには固定更新の引数を使ってください。このAPIはWindows Runtime向けで、WebのPortable APIにはまだ提供していません。Sceneのレイアウト変更によりGame Module APIは16となり、SDK反映後はゲーム用DLLの再ビルドが必要です。

### 3D形状と接触

3Dボックスは回転を含むOBBとして15軸SATで判定します。
Capsule ColliderとSphere ColliderはBox／Capsule／Sphere間のすべての組み合わせに対応し、キャラクター、縦長の物体、ボール状の物体へ軽い当たり判定を設定できます。
Scene Viewのデバッグ表示も実際の回転ボックス、カプセル、スフィア形状に追従します。

Convex Hull ColliderはInspectorで頂点を直接編集する任意点群コライダーで、GJK（交差判定）とEPA（めり込み量・法線算出）による汎用アルゴリズムでBox／Capsule／Sphere／Hullのすべての組み合わせに対応します。
クレーンのブレードや傾いた岩など、既存の基本形状では表現しづらい凸多面体形状に使えます。

Rigidbodyを親GameObjectへ置き、Box／Capsule／Sphere Colliderを子GameObjectへ配置するとCompound Colliderとして動作します。
子の位置・回転・大きさでひとつの剛体形状を組み立てられ、同じ親Rigidbodyに属するCollider同士は自己衝突しません。
衝突による移動・回転・Sleepは親Rigidbodyへ適用されます。

Character ControllerはTransformの位置を足元として扱い、入力移動、重力、接地判定、ジャンプ、壁沿い移動、段差越えに対応します。
既定では`MoveHorizontal`、`MoveVertical`、`Jump` Actionを使用します。
スクリプトからは`Move()`と`Jump()`でも操作でき、Inspectorには接地状態と垂直速度がリアルタイム表示されます。

SceneにはColliderを調べるPhysics Queryがあります。

```cpp
LamaPon::PhysicsHit hit;
if (scene.Raycast(ray, 100.0f, hit))
{
    auto* object = hit.gameObject;
}

const auto nearby = scene.OverlapSphere(center, 2.0f);
```

`Raycast`、距離順の`RaycastAll`、`SphereCast`、`OverlapBox`、`OverlapSphere`を利用でき、Layer Mask、Triggerを含めるか、無視するGameObjectをクエリごとに指定できます。

独自コンポーネントでは次のイベントをオーバーライドできます。

```cpp
void OnCollisionEnter(const LamaPon::CollisionEvent& event) override;
void OnCollisionStay(const LamaPon::CollisionEvent& event) override;
void OnCollisionExit(const LamaPon::CollisionEvent& event) override;
```

2Dは軽量なワールドAABB、3DのNarrow PhaseはOBB／Capsule／Sphere判定です。
Physics QueryはBox、Capsule、Sphereを対象にし、現在は高速な外接Bounds判定を使用します。

Polygon Collider 2DはこのAABB簡略化の例外で、GameObjectの回転にそのまま追従する凸多角形として判定します（エッジ法線を軸としたSAT、接触面のクリッピングによる最大2点のマニフォールド）。
Inspectorで頂点を直接ドラッグ編集でき、斜面や凹凸のある足場などBoxでは表現しづらい形状に使えます。
頂点は3個以上で、必ず凸多角形になるようにしてください（凹形状は判定が破綻します）。
巻き順（時計回り／反時計回り）はどちらでも構いません。
組み合わせるBox Collider 2Dは引き続きAABBとして扱われます。

`Joint`コンポーネントでは接続先GameObjectと双方のローカルアンカーを指定できます。

- `Fixed`: アンカー位置、速度、角速度を固定
- `Hinge`: アンカー位置と軸外の回転を固定し、指定軸だけ回転
- `Spring`: 自然長、強さ、減衰を使って2物体を接続
- `接続した物体同士を衝突`: 無効時は接続ペアのCollider判定を省略

```cpp
auto& body = scene.CreateGameObject("Door");
body.AddComponent<LamaPon::RigidbodyComponent>();
auto& hinge = body.AddComponent<LamaPon::JointComponent>(
    LamaPon::JointType::Hinge,
    frame.Id(),
    DirectX::XMFLOAT3{ -0.5f, 0.0f, 0.0f },
    DirectX::XMFLOAT3{ 0.5f, 0.0f, 0.0f });
hinge.SetUseLimits(true);
hinge.SetLimits({ -75.0f, 75.0f });
hinge.SetUseMotor(true);
hinge.SetMotor({
    45.0f,  // 目標角速度（度/秒）
    20.0f   // 最大トルク
});
```

Fixed Jointは位置アンカーと角速度を固定し、Hingeは位置アンカーを拘束しながら初期姿勢を0度として最小／最大角度の範囲で回転します。
モーターは目標角速度まで最大トルクの範囲で加速し、角度制限の外側へは押し続けません。
Springは質量差を反映して伸縮します。
Joint Constraintは各Physics Substepで4回反復されるため、複数のJointを連結したときもアンカー誤差が伝播しにくくなっています。

Broad Phaseでは2D・3D Colliderを均一グリッドへ登録し、同じセルを共有するColliderだけをNarrow Phaseの候補にします。
複数セルにまたがるColliderのペアは一度だけ処理され、広大な床など4096セルを超えるColliderは取りこぼしを防ぐため安全な全候補比較へ自動的に切り替わります。

Inspectorの「シーン環境 → 物理 Broad Phase」ではセルサイズを0.25～100の範囲で調整できます。
再生中はCollider数、候補ペア数、実際のNarrow Phase回数、使用セル数、接触数をリアルタイム表示します。
セルサイズはシーンJSONの`physics.broadPhaseCellSize`へ保存されます。

## プロジェクト設定（物理）

**プロジェクト設定 → 物理**に、シーン全体へ効く調整値があります。
`.lamapon/project.json`へ保存され、書き出したゲームにも付いていきます。
既定値は、この設定を足す前にコードへ直書きされていた値と同じなので、触らなければ挙動は変わりません。

| 項目 | 既定 | 何をする |
|---|---|---|
| 重力 | `(0, -9.81, 0)` | Rigidbodyの「重力を使う」がオンのものへ掛かる加速度（m/s²）。**3軸とも指定できます**（横向きの重力や無重力も作れます） |
| 固定タイムステップ | `1/60`秒 | 物理を1回進める時間。`FixedUpdate`の間隔でもあります |
| 1フレームの最大回数 | `8` | 描画が遅れたときに取り戻す上限 |
| 反復回数 | `8` | 接触の解決を繰り返す回数 |
| 速さのしきい値 | `0.25` m/s | ここまで遅ければ「止まっている」とみなす |
| 角速度のしきい値 | `0.35` rad/s | 同上（回転） |
| 眠るまでの秒数 | `0.5` | しきい値を下回り続けてから眠るまで |
| DCDで安全な速さ | `40` m/s | これを超えたDCDの物体をログで知らせます |
| 超えたら頭打ちにする | オフ | オンにすると、上の速さを超えないよう速度を制限します |

### どれをいつ触るか

- **箱を積むと沈む・ぷるぷるする** → **反復回数**を増やします（16〜24あたり）。
  そのぶん重くなります
- **速い物体が薄い壁をすり抜ける** → 下の「すり抜け（DCDとCCD）」を
  見てください。
  固定タイムステップを小さくするのは**全部に効く**ので最後の手段です
- **止まってほしいのに微妙に動き続ける** → **速さのしきい値**を上げるか、
  **眠るまでの秒数**を短くします
- **動いているのに勝手に止まる** → 逆に、しきい値を下げます
- **水中・月面・無重力** → **重力**を変えます

### すり抜け（DCDとCCD）

当たり判定には2つの方式があり、**Rigidbodyごとに選びます**。

| 方式 | 何をする | 重さ |
|---|---|---|
| **Discrete (DCD)**（既定） | その瞬間の位置だけで重なりを見る | 軽い |
| **Continuous (CCD)** | 前の位置から今の位置まで**掃引**して、最初に当たる場所で止める | 重い |

DCDは「1歩で進む距離」が当たり判定の薄さを超えるとすり抜けます。
境目は次の式です。

```
すり抜ける速さ = 当たり判定の薄さ ÷ 固定タイムステップ
```

60Hz（`1/60`秒）で薄さ0.5mの壁なら、`0.5 ÷ (1/60) = 30 m/s` を超えると危険、ということです。

**CCDは必要な物体にだけ付けてください。** 弾、投擲物、高速で落ちるものなど、実際にすり抜けたものだけです。
全部に付けると重くなります（掃引は物体ごとに追加の判定が走ります）。
CCDは、キネマティック・眠っている物体・無効な物体では**そもそも走りません**。

どれに付けるべきかを探すために、プロジェクト設定の**「DCDで安全な速さ」**があります。
この速さを超えたDCDの物体は、名前付きでログに1回だけ警告が出ます。

```
離散判定（DCD）には速すぎます（85.3 m/s、上限 40.0 m/s）。
薄い壁をすり抜けることがあります。この物体のRigidbodyを
Continuous (CCD) へ変えてください: Bullet
```

自分のゲームに合わせるなら、**一番薄い当たり判定 ÷ 固定タイムステップ**を入れてください。
設定画面には「今の設定では1歩あたり何m進むか」も出ます。

**「超えたら頭打ちにする」は既定でオフ**です。
オンにすると、CCDを使わなくてもすり抜けにくくなりますが、落下が途中で頭打ちになるなど**見た目が変わります**。
まずは警告を見て、必要な物体をCCDへ移すのがおすすめです。

### 上げすぎると悪くなるもの

**「1フレームの最大回数」は増やさないでください。** 描画が重くて遅れたとき、取り戻すために物理を何回も進める設定です。
増やすと「重い →たくさん進める → もっと重い」という悪循環に入り、一度カクつくと戻らなくなります。
重いときは**遅れを捨てる**（＝スローモーションになる）方が、止まってしまうより安全です。

固定タイムステップも同じで、小さくすると1フレームあたりの回数が増えます。
`1/120`にすると計算量はおよそ倍です。

### スクリプトから読む

```cpp
#include "LamaPon/Physics/PhysicsSettings.h"

const auto& physics = LamaPon::ActivePhysicsSettings();
const float step = physics.fixedTimeStep;
```

実行中に一時的に変えることもできます（`SetActivePhysicsSettings`）。
0以下の刻み幅のような「進まなくなる値」は自動で丸められます。

## レイヤーの名前と衝突マトリクス

コライダーには0〜31の**レイヤー番号**があり、プロジェクト設定 →物理で**名前**を付けられます（例: 2=Player、3=Enemy）。
名前を付けたレイヤーはInspectorの「レイヤー」「衝突マスク」がドロップダウンになり、番号を覚える必要がなくなります。

なお、**新規プロジェクトの床（Ground）はレイヤー1**で作られます。
マトリクスで1を切るときは床が巻き込まれることに注意してください。

**衝突マトリクス**は「どのレイヤーとどのレイヤーが当たるか」の一覧表です（プロジェクト設定 → 物理）。
チェックを外したペアは当たりません。
既定は全部オンなので、触らなければ挙動は変わりません。

当たるかどうかは次の**両方**を通ったペアだけです。

1. お互いの「衝突マスク」が相手のレイヤーを含んでいる
  （コライダーごとの設定）
2. 衝突マトリクスでそのペアがオンになっている
  （プロジェクト全体の設定）

使い分けの目安: **ゲーム全体のルールはマトリクス**で（「敵どうしは当たらない」など）、**例外の個体だけマスク**で調整します。

- マトリクスが効くのは**接触の解決とトリガー、Character Controller**
  です。
  `Raycast` や `OverlapBox` などの問い合わせには効きません（問い合わせは呼び出し側が渡すLayer Maskだけで絞ります）
- `.lamapon/project.json` には `layerNames`（名前の一覧）と
  `collisionOff`（**当たらないペア**の一覧、`[[1,2]]` の形）で保存されます。
  既定（全部当たる）なら `collisionOff` は空です
- 名前はスクリプトの表示・エディター用で、判定は常に**番号**で
  行います。
  名前を変えても既存シーンの挙動は変わりません

## よくあるつまずき

- **床をすり抜けて落ちる** — 床か物体のどちらかに**Colliderが無い**のが
  ほとんどの原因です。
  Scene ViewでColliderは緑の枠で表示されるので目で確認できます。
  高速で動く物体のすり抜けは、Rigidbodyの衝突検出を`Continuous (CCD)`へ。
- **押しても動かない** — 「キネマティック」がONだと物理の影響を受けません。
  また静止し続けた物体はSleepします（力を加えれば自動で起きます）。
- **Rigidbody付きなのにTransformを直接動かしたら挙動が変** — Rigidbodyを
  持つ物体は`AddForce`／`SetVelocity`で動かします。
  ワープさせたい場合だけTransformを直接書き換えます（補間履歴は自動同期されます）。
- **OnCollisionEnterが呼ばれない** — ColliderがTriggerになっていると
  `OnTriggerEnter`側へ届きます。
  両方すり抜けたい場合はTrigger、ぶつけたい場合は通常Colliderです。
- **ジャンプや加速をUpdateに書いたらフレームレートで変わる** — 継続的に力を
  加える処理は固定間隔（既定60Hz）の`FixedUpdate`へ。
  押した瞬間の`Impulse`は`Update`でも問題ありません。
