#include "LamaPon/Physics/Raycast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace LamaPon
{
    bool RayIntersectsBounds(
        const Ray& ray,
        const Bounds3D& bounds,
        float& distance) noexcept
    {
        constexpr float epsilon = 0.000001f;
        float nearest = 0.0f;
        float farthest = std::numeric_limits<float>::max();

        const std::array origins{ ray.origin.x, ray.origin.y, ray.origin.z };
        const std::array directions{
            ray.direction.x,
            ray.direction.y,
            ray.direction.z
        };
        const std::array minimums{
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z
        };
        const std::array maximums{
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z
        };

        for (std::size_t axis = 0; axis < origins.size(); ++axis)
        {
            if (std::abs(directions[axis]) <= epsilon)
            {
                if (origins[axis] < minimums[axis]
                    || origins[axis] > maximums[axis])
                {
                    return false;
                }
                continue;
            }

            const float inverseDirection = 1.0f / directions[axis];
            float entry = (minimums[axis] - origins[axis]) * inverseDirection;
            float exit = (maximums[axis] - origins[axis]) * inverseDirection;
            if (entry > exit)
            {
                std::swap(entry, exit);
            }

            nearest = std::max(nearest, entry);
            farthest = std::min(farthest, exit);
            if (nearest > farthest)
            {
                return false;
            }
        }

        distance = nearest;
        return true;
    }

    Bounds3D TransformBounds(
        const Bounds3D& bounds,
        DirectX::FXMMATRIX transform) noexcept
    {
        using namespace DirectX;

        const std::array corners{
            XMFLOAT3{ bounds.minimum.x, bounds.minimum.y, bounds.minimum.z },
            XMFLOAT3{ bounds.maximum.x, bounds.minimum.y, bounds.minimum.z },
            XMFLOAT3{ bounds.minimum.x, bounds.maximum.y, bounds.minimum.z },
            XMFLOAT3{ bounds.maximum.x, bounds.maximum.y, bounds.minimum.z },
            XMFLOAT3{ bounds.minimum.x, bounds.minimum.y, bounds.maximum.z },
            XMFLOAT3{ bounds.maximum.x, bounds.minimum.y, bounds.maximum.z },
            XMFLOAT3{ bounds.minimum.x, bounds.maximum.y, bounds.maximum.z },
            XMFLOAT3{ bounds.maximum.x, bounds.maximum.y, bounds.maximum.z }
        };

        XMFLOAT3 transformed{};
        XMStoreFloat3(
            &transformed,
            XMVector3Transform(XMLoadFloat3(&corners.front()), transform));

        Bounds3D result{ transformed, transformed };
        for (std::size_t index = 1; index < corners.size(); ++index)
        {
            XMStoreFloat3(
                &transformed,
                XMVector3Transform(XMLoadFloat3(&corners[index]), transform));
            result.minimum.x = std::min(result.minimum.x, transformed.x);
            result.minimum.y = std::min(result.minimum.y, transformed.y);
            result.minimum.z = std::min(result.minimum.z, transformed.z);
            result.maximum.x = std::max(result.maximum.x, transformed.x);
            result.maximum.y = std::max(result.maximum.y, transformed.y);
            result.maximum.z = std::max(result.maximum.z, transformed.z);
        }

        return result;
    }
}
