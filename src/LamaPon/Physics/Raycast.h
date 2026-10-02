#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

namespace LamaPon
{
    struct Ray final
    {
        // レイの始点座標
        DirectX::XMFLOAT3 origin;
        // レイの進行方向
        DirectX::XMFLOAT3 direction;
    };

    // レイの正方向とAABBの重なりを求めます(ray: 同じ座標系のレイ, bounds: 判定する境界, distance: 進入距離の出力)。
    // 始点が境界内なら距離は0で、方向が単位ベクトルの場合に距離の単位が座標と一致します。
    // 未検出の場合はdistanceを変更しません。
    [[nodiscard]] bool RayIntersectsBounds(
        const Ray& ray,
        const Bounds3D& bounds,
        float& distance) noexcept;

    // 8頂点を変換して軸平行境界を求めます(bounds: 変換前の境界, transform: アフィン変換行列)。
    [[nodiscard]] Bounds3D TransformBounds(
        const Bounds3D& bounds,
        DirectX::FXMMATRIX transform) noexcept;
}
