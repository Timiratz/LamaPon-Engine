#include "LamaPon/Components/SpriteMaskComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace LamaPon
{
    SpriteMaskComponent::SpriteMaskComponent(
        const SpriteMaskShape shape,
        const DirectX::XMFLOAT2 size) noexcept
        : m_shape(shape)
        , m_size(size)
    {
        SetSize(size);
    }

    void SpriteMaskComponent::SetSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_size = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    DirectX::XMFLOAT2
        SpriteMaskComponent::WorldPosition() const noexcept
    {
        DirectX::XMFLOAT3 translation{};
        DirectX::XMStoreFloat3(
            &translation,
            Owner().WorldMatrix().r[3]);
        return { translation.x, translation.y };
    }

    void SpriteMaskComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        const auto center = WorldPosition();
        const DirectX::XMVECTOR color =
            DirectX::XMVectorSet(1.0f, 0.35f, 0.85f, 1.0f);

        if (m_shape == SpriteMaskShape::Rectangle)
        {
            const float halfWidth = m_size.x * 0.5f;
            const float halfHeight = m_size.y * 0.5f;
            const std::array<DirectX::XMFLOAT3, 8> lines{
                DirectX::XMFLOAT3{
                    center.x - halfWidth,
                    center.y - halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x + halfWidth,
                    center.y - halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x + halfWidth,
                    center.y - halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x + halfWidth,
                    center.y + halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x + halfWidth,
                    center.y + halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x - halfWidth,
                    center.y + halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x - halfWidth,
                    center.y + halfHeight, 0.0f },
                DirectX::XMFLOAT3{
                    center.x - halfWidth,
                    center.y - halfHeight, 0.0f }
            };
            graphics.Debug().DrawLines(
                lines, color, view, projection);
            return;
        }

        const float radius = m_size.x * 0.5f;
        constexpr int SegmentCount = 24;
        std::array<
            DirectX::XMFLOAT3,
            SegmentCount * 2> lines{};
        for (int segment = 0;
            segment < SegmentCount;
            ++segment)
        {
            const float angleA =
                DirectX::XM_2PI
                * static_cast<float>(segment)
                / static_cast<float>(SegmentCount);
            const float angleB =
                DirectX::XM_2PI
                * static_cast<float>(segment + 1)
                / static_cast<float>(SegmentCount);
            lines[segment * 2] = {
                center.x + std::cos(angleA) * radius,
                center.y + std::sin(angleA) * radius,
                0.0f };
            lines[segment * 2 + 1] = {
                center.x + std::cos(angleB) * radius,
                center.y + std::sin(angleB) * radius,
                0.0f };
        }
        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
