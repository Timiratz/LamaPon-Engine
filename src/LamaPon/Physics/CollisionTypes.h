#pragma once

#include <DirectXMath.h>

namespace LamaPon
{
    class GameObject;

    struct Bounds2D final
    {
        // 軸平行境界の最小座標
        DirectX::XMFLOAT2 minimum;
        // 軸平行境界の最大座標
        DirectX::XMFLOAT2 maximum;
    };

    struct Bounds3D final
    {
        // 軸平行境界の最小座標
        DirectX::XMFLOAT3 minimum;
        // 軸平行境界の最大座標
        DirectX::XMFLOAT3 maximum;
    };

    struct CollisionEvent final
    {
        // 衝突相手の非所有参照
        GameObject& other;
        // 接触面の法線方向
        DirectX::XMFLOAT3 normal;
        // 接触点のワールド座標
        DirectX::XMFLOAT3 point;
        // 接触のめり込み深さ
        float penetration{};
        // トリガー接触の有無
        bool isTrigger{};
    };
}
