#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class SpriteBatch;
    }
}

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;

    // A small, world-space 2D burst emitter. It deliberately uses the same
    // SpriteBatch path as SpriteRenderer so it needs no particle shader or
    // texture: a 1x1 white texture is enough to draw every particle.
    class SpriteParticles2DComponent final : public Component
    {
    public:
        explicit SpriteParticles2DComponent(
            std::uint32_t maxParticles = 128,
            DirectX::XMFLOAT2 lifetime = { 0.25f, 0.65f },
            DirectX::XMFLOAT2 speed = { 24.0f, 72.0f },
            DirectX::XMFLOAT2 startSize = { 4.0f, 12.0f },
            DirectX::XMFLOAT4 startColor =
                { 1.0f, 0.85f, 0.35f, 1.0f },
            DirectX::XMFLOAT4 endColor =
                { 1.0f, 0.25f, 0.05f, 0.0f });

        void SetMaxParticles(std::uint32_t count) noexcept;
        void SetLifetime(
            const DirectX::XMFLOAT2& range) noexcept;
        void SetStartSpeed(
            const DirectX::XMFLOAT2& range) noexcept;
        void SetStartSize(
            const DirectX::XMFLOAT2& range) noexcept;
        void SetSizeGrowth(float growth) noexcept;
        void SetGravity(
            const DirectX::XMFLOAT2& gravity) noexcept;
        void SetDrag(float drag) noexcept;
        void SetStartColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetEndColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetTexturePath(std::filesystem::path path);
        void SetSortOrder(int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        [[nodiscard]] std::uint32_t MaxParticles() const noexcept
        {
            return m_maxParticles;
        }
        [[nodiscard]] const DirectX::XMFLOAT2& Lifetime() const noexcept
        {
            return m_lifetime;
        }
        [[nodiscard]] const DirectX::XMFLOAT2& StartSpeed() const noexcept
        {
            return m_startSpeed;
        }
        [[nodiscard]] const DirectX::XMFLOAT2& StartSize() const noexcept
        {
            return m_startSize;
        }
        [[nodiscard]] float SizeGrowth() const noexcept
        {
            return m_sizeGrowth;
        }
        [[nodiscard]] const DirectX::XMFLOAT2& Gravity() const noexcept
        {
            return m_gravity;
        }
        [[nodiscard]] float Drag() const noexcept
        {
            return m_drag;
        }
        [[nodiscard]] const DirectX::XMFLOAT4& StartColor() const noexcept
        {
            return m_startColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4& EndColor() const noexcept
        {
            return m_endColor;
        }
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }
        [[nodiscard]] std::size_t ActiveParticleCount() const noexcept
        {
            return m_particles.size();
        }

        // Emits one burst at the owner's current world-space position.
        void Emit(std::uint32_t count);
        void Clear() noexcept
        {
            m_particles.clear();
        }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpriteParticles2D";
        }
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        void OnRender2D(
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView* whiteTexture) override;

    private:
        struct Particle final
        {
            DirectX::XMFLOAT2 position;
            DirectX::XMFLOAT2 velocity;
            float age{};
            float lifetime{ 0.5f };
            float size{ 8.0f };
            float rotation{};
            float angularVelocity{};
        };

        [[nodiscard]] float Random01() noexcept;
        [[nodiscard]] float RandomRange(
            float minimum,
            float maximum) noexcept;
        void RefreshTexture();

        static constexpr std::uint32_t AbsoluteMaximumParticles = 4096;

        std::uint32_t m_maxParticles{ 128 };
        DirectX::XMFLOAT2 m_lifetime{ 0.25f, 0.65f };
        DirectX::XMFLOAT2 m_startSpeed{ 24.0f, 72.0f };
        DirectX::XMFLOAT2 m_startSize{ 4.0f, 12.0f };
        float m_sizeGrowth{ -4.0f };
        DirectX::XMFLOAT2 m_gravity{ 0.0f, 180.0f };
        float m_drag{ 0.8f };
        DirectX::XMFLOAT4 m_startColor{ 1.0f, 0.85f, 0.35f, 1.0f };
        DirectX::XMFLOAT4 m_endColor{ 1.0f, 0.25f, 0.05f, 0.0f };
        std::filesystem::path m_texturePath;
        std::shared_ptr<const TextureAsset> m_texture;
        std::vector<Particle> m_particles;
        GraphicsDevice* m_graphics{};
        AssetManager* m_assets{};
        int m_sortOrder{};
        std::uint32_t m_randomState{ 0x91e10da5u };
    };
}
