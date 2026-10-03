#include "LamaPon/Components/BoxCollider2DComponent.h"

#include "LamaPon/Physics/PhysicsSettings.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <limits>

namespace LamaPon
{
    BoxCollider2DComponent::BoxCollider2DComponent(
        const DirectX::XMFLOAT2 size,
        const DirectX::XMFLOAT2 offset,
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

    Bounds2D BoxCollider2DComponent::WorldBounds() const noexcept
    {
        using namespace DirectX;

        // ローカル箱のX半幅
        const float halfWidth = std::abs(m_size.x) * 0.5f;
        // ローカル箱のY半幅
        const float halfHeight = std::abs(m_size.y) * 0.5f;
        // ローカル箱の4隅
        const std::array corners{
            XMFLOAT3{ m_offset.x - halfWidth, m_offset.y - halfHeight, 0.0f },
            XMFLOAT3{ m_offset.x + halfWidth, m_offset.y - halfHeight, 0.0f },
            XMFLOAT3{ m_offset.x - halfWidth, m_offset.y + halfHeight, 0.0f },
            XMFLOAT3{ m_offset.x + halfWidth, m_offset.y + halfHeight, 0.0f }
        };

        // 変換後のXY軸平行境界
        Bounds2D bounds{
            { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() },
            { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() }
        };

        // 所有物体のワールド変換
        const XMMATRIX world = Owner().WorldMatrix();
        // 変換するローカル隅位置
        for (const auto& corner : corners)
        {
            // ワールド変換した隅位置
            XMFLOAT3 transformed{};
            XMStoreFloat3(
                &transformed,
                XMVector3TransformCoord(XMLoadFloat3(&corner), world));
            bounds.minimum.x = std::min(bounds.minimum.x, transformed.x);
            bounds.minimum.y = std::min(bounds.minimum.y, transformed.y);
            bounds.maximum.x = std::max(bounds.maximum.x, transformed.x);
            bounds.maximum.y = std::max(bounds.maximum.y, transformed.y);
        }

        return bounds;
    }

    bool BoxCollider2DComponent::CanCollideWith(
        const BoxCollider2DComponent& other) const noexcept
    {

        return (m_collisionMask & (1u << other.m_layer)) != 0
            && (other.m_collisionMask & (1u << m_layer)) != 0
            && LayersCanCollide(m_layer, other.m_layer);
    }

    void BoxCollider2DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        // 形状のデバッグRGBA色
        const DirectX::XMVECTOR color = m_isTrigger
            ? DirectX::XMVectorSet(1.0f, 0.75f, 0.1f, 1.0f)
            : DirectX::XMVectorSet(0.1f, 1.0f, 0.35f, 1.0f);
        graphics.Debug().DrawBounds(WorldBounds(), color, view, projection);
    }
}
