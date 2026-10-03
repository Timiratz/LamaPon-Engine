#include "LamaPon/Components/Light2DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace LamaPon
{
    Light2DComponent::Light2DComponent(
        const DirectX::XMFLOAT3 color,
        const float intensity,
        const float radius) noexcept
        : m_color(color)
        , m_intensity(intensity)
        , m_radius(radius)
    {
        SetColor(color);
        SetIntensity(intensity);
        SetRadius(radius);
    }

    void Light2DComponent::SetColor(
        const DirectX::XMFLOAT3& color) noexcept
    {
        m_color = {
            std::clamp(color.x, 0.0f, 1.0f),
            std::clamp(color.y, 0.0f, 1.0f),
            std::clamp(color.z, 0.0f, 1.0f)
        };
    }

    void Light2DComponent::SetIntensity(
        const float intensity) noexcept
    {
        m_intensity = std::clamp(intensity, 0.0f, 16.0f);
    }

    void Light2DComponent::SetRadius(
        const float radius) noexcept
    {
        m_radius = std::clamp(radius, 1.0f, 100000.0f);
    }

    DirectX::XMFLOAT2
        Light2DComponent::WorldPosition() const noexcept
    {
        // 光源のワールド位置
        DirectX::XMFLOAT3 translation{};
        DirectX::XMStoreFloat3(
            &translation,
            Owner().WorldMatrix().r[3]);
        return { translation.x, translation.y };
    }

    void Light2DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        // 光の影響円のワールド中心
        const auto center = WorldPosition();
        // 影響円の分割数
        constexpr int SegmentCount = 24;
        // 影響円の線分端点列
        std::array<
            DirectX::XMFLOAT3,
            SegmentCount * 2> lines{};
        // 影響円の線分番号
        for (int segment = 0;
            // 影響円の分割数
            segment < SegmentCount;
            ++segment)
        {
            // 線分始点の角度ラジアン
            const float angleA =
                DirectX::XM_2PI
                * static_cast<float>(segment)
                / static_cast<float>(SegmentCount);
            // 線分終点の角度ラジアン
            const float angleB =
                DirectX::XM_2PI
                * static_cast<float>(segment + 1)
                / static_cast<float>(SegmentCount);
            lines[segment * 2] = {
                center.x + std::cos(angleA) * m_radius,
                center.y + std::sin(angleA) * m_radius,
                0.0f };
            lines[segment * 2 + 1] = {
                center.x + std::cos(angleB) * m_radius,
                center.y + std::sin(angleB) * m_radius,
                0.0f };
        }
        // デバッグ線のRGBA色
        const DirectX::XMVECTOR color = DirectX::XMVectorSet(
            m_color.x, m_color.y, m_color.z, 1.0f);
        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
