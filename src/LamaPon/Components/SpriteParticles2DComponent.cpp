#include "LamaPon/Components/SpriteParticles2DComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace LamaPon
{
    SpriteParticles2DComponent::SpriteParticles2DComponent(
        const std::uint32_t maxParticles,
        const DirectX::XMFLOAT2 lifetime,
        const DirectX::XMFLOAT2 speed,
        const DirectX::XMFLOAT2 startSize,
        const DirectX::XMFLOAT4 startColor,
        const DirectX::XMFLOAT4 endColor)
        : m_startColor(startColor)
        , m_endColor(endColor)
    {
        SetMaxParticles(maxParticles);
        SetLifetime(lifetime);
        SetStartSpeed(speed);
        SetStartSize(startSize);
        SetStartColor(startColor);
        SetEndColor(endColor);
    }

    void SpriteParticles2DComponent::SetMaxParticles(
        const std::uint32_t count) noexcept
    {
        m_maxParticles = std::clamp(
            count,
            1u,
            AbsoluteMaximumParticles);
        if (m_particles.size() > m_maxParticles)
        {
            m_particles.resize(m_maxParticles);
        }
        m_particles.reserve(m_maxParticles);
    }

    void SpriteParticles2DComponent::SetLifetime(
        const DirectX::XMFLOAT2& range) noexcept
    {
        m_lifetime.x = std::clamp(
            std::min(range.x, range.y),
            0.01f,
            120.0f);
        m_lifetime.y = std::clamp(
            std::max(range.x, range.y),
            m_lifetime.x,
            120.0f);
    }

    void SpriteParticles2DComponent::SetStartSpeed(
        const DirectX::XMFLOAT2& range) noexcept
    {
        m_startSpeed.x = std::clamp(
            std::min(range.x, range.y),
            0.0f,
            10000.0f);
        m_startSpeed.y = std::clamp(
            std::max(range.x, range.y),
            m_startSpeed.x,
            10000.0f);
    }

    void SpriteParticles2DComponent::SetStartSize(
        const DirectX::XMFLOAT2& range) noexcept
    {
        m_startSize.x = std::clamp(
            std::min(range.x, range.y),
            0.001f,
            1000.0f);
        m_startSize.y = std::clamp(
            std::max(range.x, range.y),
            m_startSize.x,
            1000.0f);
    }

    void SpriteParticles2DComponent::SetSizeGrowth(
        const float growth) noexcept
    {
        m_sizeGrowth = std::clamp(growth, -10000.0f, 10000.0f);
    }

    void SpriteParticles2DComponent::SetGravity(
        const DirectX::XMFLOAT2& gravity) noexcept
    {
        m_gravity = {
            std::clamp(gravity.x, -10000.0f, 10000.0f),
            std::clamp(gravity.y, -10000.0f, 10000.0f)
        };
    }

    void SpriteParticles2DComponent::SetDrag(const float drag) noexcept
    {
        m_drag = std::clamp(drag, 0.0f, 100.0f);
    }

    void SpriteParticles2DComponent::SetStartColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_startColor = {
            std::clamp(color.x, 0.0f, 16.0f),
            std::clamp(color.y, 0.0f, 16.0f),
            std::clamp(color.z, 0.0f, 16.0f),
            std::clamp(color.w, 0.0f, 1.0f)
        };
    }

    void SpriteParticles2DComponent::SetEndColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_endColor = {
            std::clamp(color.x, 0.0f, 16.0f),
            std::clamp(color.y, 0.0f, 16.0f),
            std::clamp(color.z, 0.0f, 16.0f),
            std::clamp(color.w, 0.0f, 1.0f)
        };
    }

    void SpriteParticles2DComponent::SetTexturePath(
        std::filesystem::path path)
    {
        m_texturePath = std::move(path);
        RefreshTexture();
    }

    void SpriteParticles2DComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        RefreshTexture();
    }

    void SpriteParticles2DComponent::RefreshTexture()
    {
        m_texture.reset();
        if (m_assets != nullptr && !m_texturePath.empty())
        {
            m_texture = m_assets->LoadTexture(m_texturePath);
        }
    }

    void SpriteParticles2DComponent::OnUpdate(
        const float deltaTime)
    {
        const float safeDeltaTime = std::max(deltaTime, 0.0f);
        for (auto& particle : m_particles)
        {
            particle.age += safeDeltaTime;
            particle.velocity.x += m_gravity.x * safeDeltaTime;
            particle.velocity.y += m_gravity.y * safeDeltaTime;
            const float dragFactor = std::clamp(
                1.0f - m_drag * safeDeltaTime,
                0.0f,
                1.0f);
            particle.velocity.x *= dragFactor;
            particle.velocity.y *= dragFactor;
            particle.position.x +=
                particle.velocity.x * safeDeltaTime;
            particle.position.y +=
                particle.velocity.y * safeDeltaTime;
            particle.rotation +=
                particle.angularVelocity * safeDeltaTime;
        }
        std::erase_if(
            m_particles,
            [](const Particle& particle)
            {
                return particle.age >= particle.lifetime;
            });
    }

    void SpriteParticles2DComponent::Emit(
        const std::uint32_t count)
    {
        using namespace DirectX;

        XMFLOAT4X4 world{};
        XMStoreFloat4x4(&world, Owner().WorldMatrix());
        const XMFLOAT2 origin{ world._41, world._42 };
        for (std::uint32_t index{};
            index < count && m_particles.size() < m_maxParticles;
            ++index)
        {
            const float angle = RandomRange(0.0f, XM_2PI);
            const float speed = RandomRange(
                m_startSpeed.x,
                m_startSpeed.y);
            m_particles.push_back({
                origin,
                { std::cos(angle) * speed,
                    std::sin(angle) * speed },
                0.0f,
                RandomRange(m_lifetime.x, m_lifetime.y),
                RandomRange(m_startSize.x, m_startSize.y),
                RandomRange(0.0f, XM_2PI),
                RandomRange(-8.0f, 8.0f)
            });
        }
    }

    float SpriteParticles2DComponent::Random01() noexcept
    {
        m_randomState =
            m_randomState * 1664525u + 1013904223u;
        return static_cast<float>(m_randomState & 0x00ffffffu)
            / static_cast<float>(0x01000000u);
    }

    float SpriteParticles2DComponent::RandomRange(
        const float minimum,
        const float maximum) noexcept
    {
        return minimum + (maximum - minimum) * Random01();
    }

    void SpriteParticles2DComponent::OnRender2D(
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        using namespace DirectX;

        if (m_particles.empty()
            || m_graphics == nullptr
            || Owner().GetComponent<UIRectTransformComponent>()
                != nullptr)
        {
            return;
        }

        const auto& offset = m_graphics->Sprite2DOffset();
        const float textureWidth = m_texture
            ? static_cast<float>(m_texture->width)
            : 1.0f;
        const float textureHeight = m_texture
            ? static_cast<float>(m_texture->height)
            : 1.0f;
        for (const auto& particle : m_particles)
        {
            const float normalizedAge = std::clamp(
                particle.age / particle.lifetime,
                0.0f,
                1.0f);
            const float size = std::max(
                particle.size + m_sizeGrowth * particle.age,
                0.001f);
            const XMFLOAT4 color{
                m_startColor.x
                    + (m_endColor.x - m_startColor.x)
                        * normalizedAge,
                m_startColor.y
                    + (m_endColor.y - m_startColor.y)
                        * normalizedAge,
                m_startColor.z
                    + (m_endColor.z - m_startColor.z)
                        * normalizedAge,
                m_startColor.w
                    + (m_endColor.w - m_startColor.w)
                        * normalizedAge
            };
            const XMFLOAT4 premultipliedColor{
                color.x * color.w,
                color.y * color.w,
                color.z * color.w,
                color.w
            };
            spriteBatch.Draw(
                m_texture
                    ? m_texture->view.Get()
                    : whiteTexture,
                XMFLOAT2{
                    particle.position.x + offset.x,
                    particle.position.y + offset.y },
                nullptr,
                XMLoadFloat4(&premultipliedColor),
                particle.rotation,
                XMFLOAT2{
                    textureWidth * 0.5f,
                    textureHeight * 0.5f },
                XMFLOAT2{
                    size / textureWidth,
                    size / textureHeight });
        }
    }
}
