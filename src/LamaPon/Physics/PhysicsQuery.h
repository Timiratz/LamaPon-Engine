#pragma once

#include "LamaPon/Physics/Raycast.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace LamaPon
{
    class BoxCollider3DComponent;
    class CapsuleCollider3DComponent;
    class SphereCollider3DComponent;
    class ConvexHullCollider3DComponent;
    class MeshCollider3DComponent;
    class GameObject;

    struct PhysicsQueryFilter final
    {
        // 検索する衝突レイヤーのビット列
        std::uint32_t layerMask{
            0xffffffffu };
        // トリガーも検索するか
        bool includeTriggers{};
        // 除外するオブジェクトID
        std::uint64_t ignoredGameObjectId{};
    };

    // オブジェクトとコライダーは非所有参照で、新しいフィールドは集成初期化を保つため末尾へ追加します。
    struct PhysicsHit final
    {
        // 検出したオブジェクト
        GameObject* gameObject{};
        // 検出した箱コライダー
        BoxCollider3DComponent* collider{};
        // 検出点のワールド座標
        DirectX::XMFLOAT3 point{};
        // 検出面のワールド法線
        DirectX::XMFLOAT3 normal{};
        // レイ始点からの検出距離
        float distance{};
        // 検出したカプセルコライダー
        CapsuleCollider3DComponent* capsuleCollider{};
        // 検出した球コライダー
        SphereCollider3DComponent* sphereCollider{};
        // 検出した凸包コライダー
        ConvexHullCollider3DComponent* hullCollider{};
        // 検出したメッシュコライダー
        MeshCollider3DComponent* meshCollider{};
    };

    // 検出したオブジェクトとコライダーを非所有参照で保持します。
    struct PhysicsOverlapHit final
    {
        // 重なったオブジェクト
        GameObject* gameObject{};
        // 重なった箱コライダー
        BoxCollider3DComponent* collider{};
        // 重なったカプセルコライダー
        CapsuleCollider3DComponent* capsuleCollider{};
        // 重なった球コライダー
        SphereCollider3DComponent* sphereCollider{};
        // 重なった凸包コライダー
        ConvexHullCollider3DComponent* hullCollider{};
        // 重なったメッシュコライダー
        MeshCollider3DComponent* meshCollider{};
    };
}
