# Navigation MeshとAI Pathfinding

NavMesh Surfaceのベイクと、NavMesh Agentによる経路移動を説明します。

[← ドキュメント一覧へ戻る](index.md)

## 1分でためす（プレイヤーを追いかける敵）

1. 床と障害物を配置します（どちらも3D Box Colliderなどを付けます）。
2. 空のGameObjectへ**NavMesh Surface**を追加し、Surfaceサイズを床に合わせて
  「**3D ColliderからBake**」を押します。
  Scene Viewに青（歩ける）と赤（障害物）のセルが表示されれば成功です。
3. プレイヤーのGameObjectのTagを`Player`にします（プロジェクト設定で登録）。
4. 敵のGameObjectへ**NavMesh Agent**と次のスクリプトを追加してPlayします。

```cpp
#include "LamaPon/LamaPon.h"

class ChaseAI final : public LamaPon::Script
{
public:
    void Start() override
    {
        // 経路計算は毎フレームやらず、0.3秒ごとに更新します
        InvokeRepeating(0.0f, 0.3f, [this] { UpdateDestination(); });
    }

private:
    void UpdateDestination()
    {
        auto* player = FindWithTag("Player");
        auto* agent =
            GetComponent<LamaPon::NavMeshAgentComponent>();
        if (player != nullptr && agent != nullptr)
        {
            // Bake済みSurfaceを自動で選んで経路を計算します
            agent->SetDestination(
                player->GetTransform().position);
        }
    }
};

LAMAPON_SCRIPT(ChaseAI);
```

特定のSurfaceを指定したい場合は`agent->SetDestination(destination, *navMesh)`も使えます。

## NavMesh Surface（歩ける場所のBake）

GameObjectへ`NavMesh Surface`を追加すると、XZ平面上へ軽量なグリッドNavMeshを作成できます。
InspectorではSurfaceサイズ、セルサイズ、Agent半径・高さを設定し、「3D ColliderからBake」で現在のSceneにある非TriggerのBox／Capsule／Sphere Colliderを障害物へ変換します。
SurfaceのY位置より下にある床Colliderは障害物にならず、Agentの高さへ突き出したColliderだけをラスタライズします。

Scene Viewでは歩行可能セルを青、障害セルを赤で表示します。
グリッドは1軸最大128セルに制限され、Bake結果はScene／Prefabへ保存されます。

高さの違う複数フロアには、フロアごとにNavMesh Surfaceを用意します。
`SetDestination(destination)`はAgentに最も近い高さのSurfaceを自動選択するため、複数フロアの移動もそのまま動きます。

## NavMesh Agent（経路に沿った移動）

移動させるGameObjectへ`NavMesh Agent`を追加し、移動速度、停止距離、目的地を設定します。
「経路を計算」を押すと、Bake済みSurface上で8方向A*探索を実行します。
斜め移動では障害物の角をすり抜けないよう隣接セルを確認し、経路は視線が通る区間をまとめて直線化（スムージング）されます。
黄色い経路を確認してPlayすると、Agentが経路に沿って移動します。

NavMesh設定、障害セル、Agentの目的地と経路はJSONへ保存され、GameObject複製とPrefab配置でも維持されます。
サンプルSceneにはBake済みSurfaceと黄色いAgentがあり、Playすると障害物を迂回して移動します。

## よくあるつまずき

- **Agentが動かない** — Surfaceを**Bakeしたか**が最初の確認ポイントです。
  障害物を動かした後も再Bakeが必要です（Bake結果は自動更新されません）。
- **目的地に着く前に止まる** — Agentの「停止距離」が大きすぎないか確認します。
- **壁を無視して直進する** — 壁のColliderがTriggerになっていないか、
  BakeのタイミングでColliderが存在したかを確認します（青赤の表示で分かります）。
- **遠くの目的地へ行けない** — Surfaceの範囲外です。グリッドは1軸最大128セル
  なので、広いマップはセルサイズを大きくするか、Surfaceを分割します。
- **経路がガタガタする** — セルサイズを小さくすると滑らかになりますが、
  Bake時間とメモリが増えます。
  まずは既定値のままスムージングに任せるのがおすすめです。
