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
        std::uint32_t layerMask{
            0xffffffffu };
        bool includeTriggers{};
        std::uint64_t ignoredGameObjectId{};
    };

    struct PhysicsHit final
    {
        GameObject* gameObject{};
        BoxCollider3DComponent* collider{};
        DirectX::XMFLOAT3 point{};
        DirectX::XMFLOAT3 normal{};
        float distance{};
        CapsuleCollider3DComponent* capsuleCollider{};
        SphereCollider3DComponent* sphereCollider{};
        ConvexHullCollider3DComponent* hullCollider{};
        // 既存の集成初期化を壊さないため、新フィールドは末尾へ
        // 追加します。
        MeshCollider3DComponent* meshCollider{};
    };

    struct PhysicsOverlapHit final
    {
        GameObject* gameObject{};
        BoxCollider3DComponent* collider{};
        CapsuleCollider3DComponent* capsuleCollider{};
        SphereCollider3DComponent* sphereCollider{};
        ConvexHullCollider3DComponent* hullCollider{};
        MeshCollider3DComponent* meshCollider{};
    };
}
