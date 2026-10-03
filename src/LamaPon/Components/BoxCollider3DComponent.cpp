#include "LamaPon/Components/BoxCollider3DComponent.h"

#include "LamaPon/Physics/PhysicsSettings.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <limits>

namespace LamaPon
{
    BoxCollider3DComponent::BoxCollider3DComponent(
        const DirectX::XMFLOAT3 size,
        const DirectX::XMFLOAT3 offset,
        const bool isTrigger,
        const std::uint32_t layer,
        const std::uint32_t collisionMask,
        PhysicsMaterial material) noexcept
        : m_size(size)
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    OrientedBox3D BoxCollider3DComponent::WorldBox() const noexcept
    {
        using namespace DirectX;
        // 所有物体のワールド変換
        const XMMATRIX world = Owner().WorldMatrix();
        // 変換した箱のワールド中心
        XMFLOAT3 center{};
        XMStoreFloat3(
            &center,
            XMVector3TransformCoord(
                XMLoadFloat3(&m_offset),
                world));
        // 正規化したワールドXYZ軸
        std::array<XMFLOAT3, 3> axes;
        // XYZ軸のワールド拡大倍率
        XMFLOAT3 scales{};
        // ローカルXYZ単位軸
        const std::array localAxes{
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
            XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
        };
        // 変換するXYZ軸の番号
        for (std::size_t index{}; index < 3; ++index)
        {
            // ワールド変換した軸
            const auto transformed =
                XMVector3TransformNormal(
                    localAxes[index],
                    world);
            // 軸のワールド拡大倍率
            const float scale =
                XMVectorGetX(
                    XMVector3Length(transformed));
            (&scales.x)[index] = scale;
            XMStoreFloat3(
                &axes[index],
                scale > 0.000001f
                    ? XMVectorScale(
                        transformed,
                        1.0f / scale)
                    : localAxes[index]);
        }
        return {
            center,
            axes,
            {
                std::abs(m_size.x * scales.x) * 0.5f,
                std::abs(m_size.y * scales.y) * 0.5f,
                std::abs(m_size.z * scales.z) * 0.5f
            }
        };
    }

    Bounds3D BoxCollider3DComponent::WorldBounds() const noexcept
    {
        return BoundsOf(WorldBox());
    }

    bool BoxCollider3DComponent::CanCollideWith(
        const BoxCollider3DComponent& other) const noexcept
    {

        return (m_collisionMask & (1u << other.m_layer)) != 0
            && (other.m_collisionMask & (1u << m_layer)) != 0
            && LayersCanCollide(m_layer, other.m_layer);
    }

    void BoxCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        // 形状のデバッグRGBA色
        const DirectX::XMVECTOR color = m_isTrigger
            ? DirectX::XMVectorSet(1.0f, 0.75f, 0.1f, 1.0f)
            : DirectX::XMVectorSet(0.1f, 1.0f, 0.35f, 1.0f);
        // ワールド箱の8隅
        const auto corners = CornersOf(WorldBox());
        // 箱の12辺の端点列
        std::array<DirectX::XMFLOAT3, 24> lines;
        // 次に書き込む線分端点番号
        std::size_t lineIndex{};
        // 辺を調べる隅の番号
        for (std::size_t corner{}; corner < corners.size(); ++corner)
        {
            // 隣接隅を選ぶ軸ビット
            for (const std::size_t bit : { 1u, 2u, 4u })
            {
                // 隣接する隅の番号
                const std::size_t neighbor = corner ^ bit;
                if (corner < neighbor)
                {
                    lines[lineIndex++] = corners[corner];
                    lines[lineIndex++] = corners[neighbor];
                }
            }
        }
        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
