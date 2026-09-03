#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

namespace LamaPon
{
    struct Ray final
    {
        DirectX::XMFLOAT3 origin;
        DirectX::XMFLOAT3 direction;
    };

    [[nodiscard]] bool RayIntersectsBounds(
        const Ray& ray,
        const Bounds3D& bounds,
        float& distance) noexcept;

    [[nodiscard]] Bounds3D TransformBounds(
        const Bounds3D& bounds,
        DirectX::FXMMATRIX transform) noexcept;
}
