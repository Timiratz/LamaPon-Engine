#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class BasicEffect;
        class CommonStates;
        template<typename TVertex>
        class PrimitiveBatch;
        struct VertexPositionColorTexture;
    }
}

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;

    enum class ParticleEmitterShape
    {
        Cone,
        Sphere,
        Box
    };

    // パーティクルの板ポリゴンを、どの面へ向けて描画するかを指定します。
    enum class ParticleRenderMode
    {
        Billboard,
        Horizontal
    };

    class ParticleSystemComponent final
        : public Component
    {
    public:
        static constexpr std::size_t
            CustomParameterCount = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        explicit ParticleSystemComponent(
            std::uint32_t maxParticles = 512,
            float emissionRate = 36.0f,
            DirectX::XMFLOAT2 lifetime =
                { 0.8f, 1.8f },
            DirectX::XMFLOAT2 speed =
                { 1.0f, 3.0f },
            DirectX::XMFLOAT2 startSize =
                { 0.08f, 0.22f },
            DirectX::XMFLOAT4 startColor =
                { 0.20f, 0.72f, 1.0f, 1.0f },
            DirectX::XMFLOAT4 endColor =
                { 0.04f, 0.18f, 0.55f, 0.0f },
            ParticleEmitterShape shape =
                ParticleEmitterShape::Cone,
            std::filesystem::path texturePath = {});
        ~ParticleSystemComponent() override;

        void SetMaxParticles(
            std::uint32_t count) noexcept;
        void SetEmissionRate(float rate) noexcept;
        void SetLifetime(
            const DirectX::XMFLOAT2& range) noexcept;
        void SetStartSpeed(
            const DirectX::XMFLOAT2& range) noexcept;
        void SetStartSize(
            const DirectX::XMFLOAT2& range) noexcept;
        void SetEndSizeMultiplier(
            float multiplier) noexcept;
        void SetStartColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetEndColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetGravity(
            const DirectX::XMFLOAT3& gravity) noexcept;
        void SetEmitterShape(
            ParticleEmitterShape shape) noexcept
        {
            m_shape = shape;
        }
        void SetRenderMode(
            ParticleRenderMode mode) noexcept
        {
            m_renderMode = mode;
        }
        void SetEmitterSize(
            const DirectX::XMFLOAT3& size) noexcept;
        void SetConeAngle(float radians) noexcept;
        void SetDuration(float duration) noexcept;
        void SetLooping(bool looping) noexcept
        {
            m_looping = looping;
        }
        void SetPlayOnStart(bool enabled) noexcept
        {
            m_playOnStart = enabled;
            if (m_particles.empty()
                && m_emittingTime == 0.0f)
            {
                m_playing = enabled;
            }
        }
        void SetPreviewInEditor(
            bool enabled) noexcept
        {
            m_previewInEditor = enabled;
        }
        void SetAdditive(bool additive) noexcept
        {
            m_additive = additive;
        }
        void SetTexturePath(
            std::filesystem::path path);
        // パーティクルの頂点生成と寿命補間はエンジン側に残し、
        // ピクセル表現だけをプロジェクトのHLSLへ差し替えます。
        void SetShaderPath(
            std::filesystem::path path);
        void SetAuxiliaryTexturePath(
            std::filesystem::path path);
        void SetCustomParameter(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept;
        void ReloadShader();

        [[nodiscard]] std::uint32_t
            MaxParticles() const noexcept
        {
            return m_maxParticles;
        }
        [[nodiscard]] float
            EmissionRate() const noexcept
        {
            return m_emissionRate;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            Lifetime() const noexcept
        {
            return m_lifetime;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            StartSpeed() const noexcept
        {
            return m_startSpeed;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            StartSize() const noexcept
        {
            return m_startSize;
        }
        [[nodiscard]] float
            EndSizeMultiplier() const noexcept
        {
            return m_endSizeMultiplier;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            StartColor() const noexcept
        {
            return m_startColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            EndColor() const noexcept
        {
            return m_endColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            Gravity() const noexcept
        {
            return m_gravity;
        }
        [[nodiscard]] ParticleEmitterShape
            EmitterShape() const noexcept
        {
            return m_shape;
        }
        [[nodiscard]] ParticleRenderMode
            RenderMode() const noexcept
        {
            return m_renderMode;
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmitterSize() const noexcept
        {
            return m_emitterSize;
        }
        [[nodiscard]] float
            ConeAngle() const noexcept
        {
            return m_coneAngle;
        }
        [[nodiscard]] float Duration() const noexcept
        {
            return m_duration;
        }
        [[nodiscard]] bool Looping() const noexcept
        {
            return m_looping;
        }
        [[nodiscard]] bool
            PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }
        [[nodiscard]] bool
            PreviewInEditor() const noexcept
        {
            return m_previewInEditor;
        }
        [[nodiscard]] bool Additive() const noexcept
        {
            return m_additive;
        }
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_shaderPath;
        }
        [[nodiscard]] const std::filesystem::path&
            AuxiliaryTexturePath() const noexcept
        {
            return m_auxiliaryTexturePath;
        }
        [[nodiscard]] const CustomParameters&
            CustomShaderParameters() const noexcept
        {
            return m_customParameters;
        }
        [[nodiscard]] DirectX::XMFLOAT4
            CustomParameter(
                const std::size_t index) const noexcept
        {
            return index < m_customParameters.size()
                ? m_customParameters[index]
                : DirectX::XMFLOAT4{};
        }
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }
        [[nodiscard]] std::size_t
            ActiveParticleCount() const noexcept
        {
            return m_particles.size();
        }
        [[nodiscard]] bool IsPlaying() const noexcept
        {
            return m_playing;
        }

        void Play() noexcept;
        void Stop(bool clearParticles = true) noexcept;
        void Restart() noexcept;
        void Emit(std::uint32_t count);
        // 原作など決め打ちのVFX配置を再現するため、ワールド座標と
        // 初速・寿命・大きさを指定して1粒だけ生成できます。
        void EmitParticle(
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& velocity,
            float lifetime,
            float startSize,
            float rotation = 0.0f,
            float angularVelocity = 0.0f);
        void UpdatePreview(float deltaTime);

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "ParticleSystem";
        }

    protected:
        void OnInitialize(
            GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        void OnRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX
                projection) override;

    private:
        struct Particle final
        {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT3 velocity;
            float age{};
            float lifetime{ 1.0f };
            float startSize{ 0.1f };
            float rotation{};
            float angularVelocity{};
        };

        void Simulate(float deltaTime);
        void SpawnOne();
        [[nodiscard]] float Random01() noexcept;
        [[nodiscard]] float RandomRange(
            float minimum,
            float maximum) noexcept;
        [[nodiscard]] DirectX::XMFLOAT3
            RandomUnitVector() noexcept;

        static constexpr std::uint32_t
            AbsoluteMaximumParticles = 4096;

        std::uint32_t m_maxParticles;
        float m_emissionRate;
        DirectX::XMFLOAT2 m_lifetime;
        DirectX::XMFLOAT2 m_startSpeed;
        DirectX::XMFLOAT2 m_startSize;
        float m_endSizeMultiplier{ 0.15f };
        DirectX::XMFLOAT4 m_startColor;
        DirectX::XMFLOAT4 m_endColor;
        DirectX::XMFLOAT3 m_gravity{
            0.0f,
            -1.2f,
            0.0f
        };
        ParticleEmitterShape m_shape;
        ParticleRenderMode m_renderMode{
            ParticleRenderMode::Billboard
        };
        DirectX::XMFLOAT3 m_emitterSize{
            1.0f,
            1.0f,
            1.0f
        };
        float m_coneAngle{
            DirectX::XMConvertToRadians(25.0f)
        };
        float m_duration{ 5.0f };
        bool m_looping{ true };
        bool m_playOnStart{ true };
        bool m_previewInEditor{ true };
        bool m_additive{ true };
        bool m_playing{ true };
        float m_emittingTime{};
        float m_spawnAccumulator{};
        std::uint32_t m_randomState{
            0x7f4a7c15u
        };
        std::filesystem::path m_texturePath;
        std::shared_ptr<const TextureAsset> m_texture;
        std::filesystem::path m_shaderPath;
        std::filesystem::path m_auxiliaryTexturePath;
        std::shared_ptr<const TextureAsset>
            m_auxiliaryTexture;
        CustomParameters m_customParameters{};
        std::uint64_t m_shaderGeneration{};
        std::string m_shaderError;
        std::vector<Particle> m_particles;
        std::vector<std::size_t> m_renderOrder;
        GraphicsDevice* m_graphics{};
        AssetManager* m_assets{};
        std::unique_ptr<DirectX::BasicEffect> m_effect;
        std::unique_ptr<DirectX::PrimitiveBatch<
            DirectX::VertexPositionColorTexture>> m_batch;
        struct InputLayoutHolder;
        std::unique_ptr<InputLayoutHolder> m_inputLayout;
    };
}
