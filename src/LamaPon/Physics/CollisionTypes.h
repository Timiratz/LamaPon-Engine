#pragma once

#include <DirectXMath.h>

namespace LamaPon
{
    class GameObject;

    struct Bounds2D final
    {
        DirectX::XMFLOAT2 minimum;
        DirectX::XMFLOAT2 maximum;
    };

    struct Bounds3D final
    {
        DirectX::XMFLOAT3 minimum;
        DirectX::XMFLOAT3 maximum;
    };

    struct CollisionEvent final
    {
        GameObject& other;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT3 point;
        float penetration{};
        bool isTrigger{};
    };
}
