#include "LamaPon/Components/SphereCollider3DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace LamaPon
{
    SphereCollider3DComponent::
        SphereCollider3DComponent(
            const float radius,
            const DirectX::XMFLOAT3 offset,
            const bool isTrigger,
            const std::uint32_t layer,
            const std::uint32_t collisionMask,
            PhysicsMaterial material) noexcept
        : m_radius(std::max(radius, 0.01f))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void SphereCollider3DComponent::SetRadius(
        const float value) noexcept
    {
        m_radius = std::max(value, 0.01f);
    }

    Sphere3D
        SphereCollider3DComponent::WorldSphere()
            const noexcept
    {
        using namespace DirectX;
        // 所有物体のワールド変換
        const auto world = Owner().WorldMatrix();
        // 球のワールド中心
        XMFLOAT3 center{};
        XMStoreFloat3(
            &center,
            XMVector3TransformCoord(
                XMLoadFloat3(&m_offset),
                world));
        // X軸のワールド拡大倍率
        const float scaleX = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(
                        1.0f,
                        0.0f,
                        0.0f,
                        0.0f),
                    world)));
        // Y軸のワールド拡大倍率
        const float scaleY = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(
                        0.0f,
                        1.0f,
                        0.0f,
                        0.0f),
                    world)));
        // Z軸のワールド拡大倍率
        const float scaleZ = XMVectorGetX(
            XMVector3Length(
                XMVector3TransformNormal(
                    XMVectorSet(
                        0.0f,
                        0.0f,
                        1.0f,
                        0.0f),
                    world)));
        return {
            center,
            m_radius
                * std::max({
                    scaleX,
                    scaleY,
                    scaleZ
                })
        };
    }

    Bounds3D
        SphereCollider3DComponent::WorldBounds()
            const noexcept
    {
        return BoundsOf(WorldSphere());
    }

    void SphereCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;
        // ワールド球の形状
        const auto sphere = WorldSphere();
        // 形状のデバッグRGBA色
        const auto color = m_isTrigger
            ? XMVectorSet(
                1.0f,
                0.75f,
                0.1f,
                1.0f)
            : XMVectorSet(
                0.35f,
                0.9f,
                0.55f,
                1.0f);
        // 各輪郭円の分割数
        constexpr int segments = 24;
        // 球の3輪郭円の線分端点列
        std::vector<XMFLOAT3> lines;
        lines.reserve(segments * 6);
        // 輪郭面の番号でXY・XZ・YZ
        for (int plane{}; plane < 3; ++plane)
        {
            // 輪郭円の線分番号
            for (int index{}; index < segments;
                ++index)
            {
                // 線分始点の角度ラジアン
                const float firstAngle =
                    XM_2PI
                    * static_cast<float>(index)
                    / static_cast<float>(segments);
                // 線分終点の角度ラジアン
                const float secondAngle =
                    XM_2PI
                    * static_cast<float>(index + 1)
                    / static_cast<float>(segments);
                // 輪郭面の円周位置を返します(angle: 円周角ラジアン)。
                const auto point =
                    [&](const float angle)
                {
                    // 輪郭円のワールド位置
                    XMFLOAT3 result = sphere.center;
                    // 面の第1軸の円周位置
                    const float first =
                        std::cos(angle)
                        * sphere.radius;
                    // 面の第2軸の円周位置
                    const float second =
                        std::sin(angle)
                        * sphere.radius;
                    if (plane == 0)
                    {
                        result.x += first;
                        result.y += second;
                    }
                    else if (plane == 1)
                    {
                        result.x += first;
                        result.z += second;
                    }
                    else
                    {
                        result.y += first;
                        result.z += second;
                    }
                    return result;
                };
                lines.push_back(point(firstAngle));
                lines.push_back(point(secondAngle));
            }
        }
        graphics.Debug().DrawLines(
            lines,
            color,
            view,
            projection);
    }
}
