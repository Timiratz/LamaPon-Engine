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
        // 軸に平行と見なす方向の閾値
        constexpr float epsilon = 0.000001f;
        // 全軸で有効な進入距離の下限
        float nearest = 0.0f;
        // 全軸で有効な退出距離の上限
        float farthest = std::numeric_limits<float>::max();

        // レイ始点の軸別成分
        const std::array origins{ ray.origin.x, ray.origin.y, ray.origin.z };
        // レイ方向の軸別成分
        const std::array directions{
            ray.direction.x,
            ray.direction.y,
            ray.direction.z
        };
        // 境界の軸別最小座標
        const std::array minimums{
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z
        };
        // 境界の軸別最大座標
        const std::array maximums{
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z
        };

        // スラブを検査する軸の番号
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

            // レイ方向の軸成分の逆数
            const float inverseDirection = 1.0f / directions[axis];
            // 現在軸の進入距離
            float entry = (minimums[axis] - origins[axis]) * inverseDirection;
            // 現在軸の退出距離
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

        // 変換前境界の8頂点
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

        // 現在の変換後頂点
        XMFLOAT3 transformed{};
        XMStoreFloat3(
            &transformed,
            XMVector3Transform(XMLoadFloat3(&corners.front()), transform));

        // 変換後の頂点を含む境界
        Bounds3D result{ transformed, transformed };
        // 変換する境界頂点の番号
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
