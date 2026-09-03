#include "LamaPon/Components/ParticleSystemComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <CommonStates.h>
#include <Effects.h>
#include <PrimitiveBatch.h>
#include <VertexTypes.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace LamaPon
{
    struct ParticleSystemComponent::
        InputLayoutHolder final
    {
        Microsoft::WRL::ComPtr<
            ID3D11InputLayout> value;
    };

    ParticleSystemComponent::
        ParticleSystemComponent(
            const std::uint32_t maxParticles,
            const float emissionRate,
            const DirectX::XMFLOAT2 lifetime,
            const DirectX::XMFLOAT2 speed,
            const DirectX::XMFLOAT2 startSize,
            const DirectX::XMFLOAT4 startColor,
            const DirectX::XMFLOAT4 endColor,
            const ParticleEmitterShape shape,
            std::filesystem::path texturePath)
        : m_maxParticles(maxParticles)
        , m_emissionRate(emissionRate)
        , m_lifetime(lifetime)
        , m_startSpeed(speed)
        , m_startSize(startSize)
        , m_startColor(startColor)
        , m_endColor(endColor)
        , m_shape(shape)
        , m_texturePath(std::move(texturePath))
    {
        SetMaxParticles(maxParticles);
        SetEmissionRate(emissionRate);
        SetLifetime(lifetime);
        SetStartSpeed(speed);
        SetStartSize(startSize);
        SetStartColor(startColor);
        SetEndColor(endColor);
        m_particles.reserve(m_maxParticles);
        m_renderOrder.reserve(m_maxParticles);
    }

    ParticleSystemComponent::
        ~ParticleSystemComponent() = default;

    void ParticleSystemComponent::
        SetMaxParticles(
            const std::uint32_t count) noexcept
    {
        m_maxParticles =
            std::clamp(
                count,
                1u,
                AbsoluteMaximumParticles);
        if (m_particles.size()
            > m_maxParticles)
        {
            m_particles.resize(
                m_maxParticles);
        }
        m_particles.reserve(
            m_maxParticles);
        m_renderOrder.reserve(
            m_maxParticles);
    }

    void ParticleSystemComponent::
        SetEmissionRate(
            const float rate) noexcept
    {
        m_emissionRate =
            std::clamp(
                rate,
                0.0f,
                10000.0f);
    }

    void ParticleSystemComponent::SetLifetime(
        const DirectX::XMFLOAT2& range) noexcept
    {
        m_lifetime.x =
            std::clamp(
                std::min(
                    range.x,
                    range.y),
                0.01f,
                120.0f);
        m_lifetime.y =
            std::clamp(
                std::max(
                    range.x,
                    range.y),
                m_lifetime.x,
                120.0f);
    }

    void ParticleSystemComponent::
        SetStartSpeed(
            const DirectX::XMFLOAT2& range) noexcept
    {
        m_startSpeed.x =
            std::clamp(
                std::min(
                    range.x,
                    range.y),
                0.0f,
                1000.0f);
        m_startSpeed.y =
            std::clamp(
                std::max(
                    range.x,
                    range.y),
                m_startSpeed.x,
                1000.0f);
    }

    void ParticleSystemComponent::
        SetStartSize(
            const DirectX::XMFLOAT2& range) noexcept
    {
        m_startSize.x =
            std::clamp(
                std::min(
                    range.x,
                    range.y),
                0.001f,
                100.0f);
        m_startSize.y =
            std::clamp(
                std::max(
                    range.x,
                    range.y),
                m_startSize.x,
                100.0f);
    }

    void ParticleSystemComponent::
        SetEndSizeMultiplier(
            const float multiplier) noexcept
    {
        m_endSizeMultiplier =
            std::clamp(
                multiplier,
                0.0f,
                10.0f);
    }

    void ParticleSystemComponent::SetStartColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_startColor = {
            std::clamp(color.x, 0.0f, 16.0f),
            std::clamp(color.y, 0.0f, 16.0f),
            std::clamp(color.z, 0.0f, 16.0f),
            std::clamp(color.w, 0.0f, 1.0f)
        };
    }

    void ParticleSystemComponent::SetEndColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_endColor = {
            std::clamp(color.x, 0.0f, 16.0f),
            std::clamp(color.y, 0.0f, 16.0f),
            std::clamp(color.z, 0.0f, 16.0f),
            std::clamp(color.w, 0.0f, 1.0f)
        };
    }

    void ParticleSystemComponent::SetGravity(
        const DirectX::XMFLOAT3& gravity) noexcept
    {
        m_gravity = {
            std::clamp(
                gravity.x,
                -1000.0f,
                1000.0f),
            std::clamp(
                gravity.y,
                -1000.0f,
                1000.0f),
            std::clamp(
                gravity.z,
                -1000.0f,
                1000.0f)
        };
    }

    void ParticleSystemComponent::
        SetEmitterSize(
            const DirectX::XMFLOAT3& size) noexcept
    {
        m_emitterSize = {
            std::clamp(
                std::abs(size.x),
                0.0f,
                1000.0f),
            std::clamp(
                std::abs(size.y),
                0.0f,
                1000.0f),
            std::clamp(
                std::abs(size.z),
                0.0f,
                1000.0f)
        };
    }

    void ParticleSystemComponent::SetConeAngle(
        const float radians) noexcept
    {
        m_coneAngle =
            std::clamp(
                std::abs(radians),
                0.0f,
                DirectX::XM_PIDIV2);
    }

    void ParticleSystemComponent::SetDuration(
        const float duration) noexcept
    {
        m_duration =
            std::clamp(
                duration,
                0.01f,
                3600.0f);
    }

    void ParticleSystemComponent::SetTexturePath(
        std::filesystem::path path)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr
            && !path.empty())
        {
            texture =
                m_assets->LoadTexture(path);
        }
        m_texturePath = std::move(path);
        m_texture = std::move(texture);
    }

    void ParticleSystemComponent::SetShaderPath(
        std::filesystem::path path)
    {
        m_shaderPath = std::move(path);
        m_shaderGeneration = 0;
        m_shaderError.clear();
    }

    void ParticleSystemComponent::SetAuxiliaryTexturePath(
        std::filesystem::path path)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr
            && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_auxiliaryTexturePath = std::move(path);
        m_auxiliaryTexture = std::move(texture);
    }

    void ParticleSystemComponent::SetCustomParameter(
        const std::size_t index,
        const DirectX::XMFLOAT4& value) noexcept
    {
        if (index < m_customParameters.size())
        {
            m_customParameters[index] = value;
        }
    }

    void ParticleSystemComponent::ReloadShader()
    {
        if (m_graphics != nullptr
            && !m_shaderPath.empty())
        {
            m_graphics->InvalidateCustomPixelShader(
                m_shaderPath);
        }
    }

    void ParticleSystemComponent::Play() noexcept
    {
        m_playing = true;
        if (!m_looping
            && m_emittingTime >= m_duration)
        {
            m_emittingTime = 0.0f;
        }
    }

    void ParticleSystemComponent::Stop(
        const bool clearParticles) noexcept
    {
        m_playing = false;
        m_spawnAccumulator = 0.0f;
        if (clearParticles)
        {
            m_particles.clear();
        }
    }

    void ParticleSystemComponent::Restart() noexcept
    {
        m_particles.clear();
        m_emittingTime = 0.0f;
        m_spawnAccumulator = 0.0f;
        m_randomState = 0x7f4a7c15u;
        m_playing = true;
    }

    void ParticleSystemComponent::Emit(
        const std::uint32_t count)
    {
        const std::uint32_t available =
            m_maxParticles
                - static_cast<std::uint32_t>(
                    m_particles.size());
        const std::uint32_t spawnCount =
            std::min(count, available);
        for (std::uint32_t index{};
            index < spawnCount;
            ++index)
        {
            SpawnOne();
        }
    }

    void ParticleSystemComponent::EmitParticle(
        const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT3& velocity,
        const float lifetime,
        const float startSize,
        const float rotation,
        const float angularVelocity)
    {
        if (m_particles.size() >= m_maxParticles)
        {
            return;
        }
        m_particles.push_back({
            position,
            velocity,
            0.0f,
            std::clamp(lifetime, 0.01f, 120.0f),
            std::clamp(startSize, 0.001f, 100.0f),
            rotation,
            angularVelocity
        });
    }

    void ParticleSystemComponent::UpdatePreview(
        const float deltaTime)
    {
        if (m_previewInEditor)
        {
            Simulate(deltaTime);
        }
    }

    void ParticleSystemComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        if (!m_texturePath.empty())
        {
            m_texture =
                m_assets->LoadTexture(
                    m_texturePath);
        }
        if (!m_auxiliaryTexturePath.empty())
        {
            m_auxiliaryTexture =
                m_assets->LoadTexture(
                    m_auxiliaryTexturePath);
        }

        m_effect =
            std::make_unique<
                DirectX::BasicEffect>(
                    graphics.Device());
        m_effect->SetVertexColorEnabled(true);
        m_effect->SetTextureEnabled(true);
        m_batch =
            std::make_unique<
                DirectX::PrimitiveBatch<
                    DirectX::
                        VertexPositionColorTexture>>(
                            graphics.Context(),
                            AbsoluteMaximumParticles
                                * 6,
                            AbsoluteMaximumParticles
                                * 4);
        m_inputLayout =
            std::make_unique<
                InputLayoutHolder>();

        const void* shaderByteCode{};
        std::size_t byteCodeLength{};
        m_effect->GetVertexShaderBytecode(
            &shaderByteCode,
            &byteCodeLength);
        const HRESULT result =
            graphics.Device()->
                CreateInputLayout(
                    DirectX::
                        VertexPositionColorTexture::
                            InputElements,
                    DirectX::
                        VertexPositionColorTexture::
                            InputElementCount,
                    shaderByteCode,
                    byteCodeLength,
                    m_inputLayout->value.
                        ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            throw std::runtime_error(
                "Failed to create Particle System input layout.");
        }
        m_playing = m_playOnStart;
    }

    void ParticleSystemComponent::OnUpdate(
        const float deltaTime)
    {
        Simulate(deltaTime);
    }

    void ParticleSystemComponent::Simulate(
        const float deltaTime)
    {
        const float step =
            std::clamp(
                deltaTime,
                0.0f,
                0.1f);
        if (step <= 0.0f)
        {
            return;
        }

        for (auto& particle : m_particles)
        {
            particle.age += step;
            particle.velocity.x +=
                m_gravity.x * step;
            particle.velocity.y +=
                m_gravity.y * step;
            particle.velocity.z +=
                m_gravity.z * step;
            particle.position.x +=
                particle.velocity.x * step;
            particle.position.y +=
                particle.velocity.y * step;
            particle.position.z +=
                particle.velocity.z * step;
            particle.rotation +=
                particle.angularVelocity
                * step;
        }
        std::erase_if(
            m_particles,
            [](const Particle& particle)
            {
                return particle.age
                    >= particle.lifetime;
            });

        if (!m_playing)
        {
            return;
        }
        m_emittingTime += step;
        bool canEmit = true;
        if (m_emittingTime >= m_duration)
        {
            if (m_looping)
            {
                m_emittingTime =
                    std::fmod(
                        m_emittingTime,
                        m_duration);
            }
            else
            {
                canEmit = false;
                m_playing = false;
            }
        }
        if (!canEmit
            || m_emissionRate <= 0.0f)
        {
            return;
        }

        m_spawnAccumulator +=
            m_emissionRate * step;
        const auto spawnCount =
            static_cast<std::uint32_t>(
                m_spawnAccumulator);
        m_spawnAccumulator -=
            static_cast<float>(spawnCount);
        Emit(spawnCount);
    }

    float ParticleSystemComponent::Random01() noexcept
    {
        m_randomState ^= m_randomState << 13;
        m_randomState ^= m_randomState >> 17;
        m_randomState ^= m_randomState << 5;
        return static_cast<float>(
            m_randomState
            & 0x00ffffffu)
            / static_cast<float>(
                0x01000000u);
    }

    float ParticleSystemComponent::RandomRange(
        const float minimum,
        const float maximum) noexcept
    {
        return minimum
            + (maximum - minimum)
                * Random01();
    }

    DirectX::XMFLOAT3
        ParticleSystemComponent::
            RandomUnitVector() noexcept
    {
        const float z =
            RandomRange(-1.0f, 1.0f);
        const float angle =
            RandomRange(
                0.0f,
                DirectX::XM_2PI);
        const float radius =
            std::sqrt(
                std::max(
                    1.0f - z * z,
                    0.0f));
        return {
            radius * std::cos(angle),
            z,
            radius * std::sin(angle)
        };
    }

    void ParticleSystemComponent::SpawnOne()
    {
        using namespace DirectX;

        XMMATRIX world =
            Owner().WorldMatrix();
        XMFLOAT3 localPosition{};
        XMFLOAT3 localDirection{
            0.0f,
            1.0f,
            0.0f
        };
        switch (m_shape)
        {
        case ParticleEmitterShape::Cone:
        {
            const float cosineMinimum =
                std::cos(m_coneAngle);
            const float cosine =
                RandomRange(
                    cosineMinimum,
                    1.0f);
            const float sine =
                std::sqrt(
                    std::max(
                        1.0f
                            - cosine * cosine,
                        0.0f));
            const float angle =
                RandomRange(
                    0.0f,
                    XM_2PI);
            localDirection = {
                sine * std::cos(angle),
                cosine,
                sine * std::sin(angle)
            };
            break;
        }
        case ParticleEmitterShape::Sphere:
            localDirection =
                RandomUnitVector();
            break;
        case ParticleEmitterShape::Box:
            localPosition = {
                RandomRange(
                    -m_emitterSize.x * 0.5f,
                    m_emitterSize.x * 0.5f),
                RandomRange(
                    -m_emitterSize.y * 0.5f,
                    m_emitterSize.y * 0.5f),
                RandomRange(
                    -m_emitterSize.z * 0.5f,
                    m_emitterSize.z * 0.5f)
            };
            localDirection =
                RandomUnitVector();
            break;
        }

        XMFLOAT3 position{};
        XMFLOAT3 direction{};
        XMStoreFloat3(
            &position,
            XMVector3TransformCoord(
                XMLoadFloat3(
                    &localPosition),
                world));
        XMStoreFloat3(
            &direction,
            XMVector3Normalize(
                XMVector3TransformNormal(
                    XMLoadFloat3(
                        &localDirection),
                    world)));
        const float speed =
            RandomRange(
                m_startSpeed.x,
                m_startSpeed.y);
        m_particles.push_back({
            position,
            {
                direction.x * speed,
                direction.y * speed,
                direction.z * speed
            },
            0.0f,
            RandomRange(
                m_lifetime.x,
                m_lifetime.y),
            RandomRange(
                m_startSize.x,
                m_startSize.y),
            RandomRange(
                0.0f,
                XM_2PI),
            RandomRange(
                -2.5f,
                2.5f)
        });
    }

    void ParticleSystemComponent::OnRender3D(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;

        if (m_particles.empty()
            || m_graphics == nullptr
            || !m_effect
            || !m_batch
            || !m_inputLayout)
        {
            return;
        }

        const XMMATRIX inverseView =
            XMMatrixInverse(nullptr, view);
        const XMVECTOR cameraRight =
            XMVector3Normalize(
                inverseView.r[0]);
        const XMVECTOR cameraUp =
            XMVector3Normalize(
                inverseView.r[1]);
        XMFLOAT3 cameraPosition{};
        XMStoreFloat3(
            &cameraPosition,
            inverseView.r[3]);

        m_renderOrder.resize(
            m_particles.size());
        std::iota(
            m_renderOrder.begin(),
            m_renderOrder.end(),
            std::size_t{});
        if (!m_additive)
        {
            std::ranges::sort(
                m_renderOrder,
                [this, cameraPosition](
                    const std::size_t left,
                    const std::size_t right)
                {
                    const XMVECTOR camera =
                        XMLoadFloat3(
                            &cameraPosition);
                    const XMVECTOR leftOffset =
                        XMVectorSubtract(
                            XMLoadFloat3(
                                &m_particles[
                                    left].position),
                            camera);
                    const XMVECTOR rightOffset =
                        XMVectorSubtract(
                            XMLoadFloat3(
                                &m_particles[
                                    right].position),
                            camera);
                    return XMVectorGetX(
                            XMVector3LengthSq(
                                leftOffset))
                        > XMVectorGetX(
                            XMVector3LengthSq(
                                rightOffset));
                });
        }

        auto& states =
            m_graphics->States();
        auto* context =
            m_graphics->Context();
        const float blendFactor[]{
            0.0f,
            0.0f,
            0.0f,
            0.0f
        };
        context->OMSetBlendState(
            m_additive
                ? states.Additive()
                : states.NonPremultiplied(),
            blendFactor,
            0xffffffffu);
        context->OMSetDepthStencilState(
            states.DepthRead(),
            0);
        context->RSSetState(
            states.CullNone());
        ID3D11SamplerState* samplers[]{
            states.LinearWrap()
        };
        context->PSSetSamplers(
            0,
            1,
            samplers);

        m_effect->SetWorld(
            XMMatrixIdentity());
        m_effect->SetView(view);
        m_effect->SetProjection(
            projection);
        m_effect->SetTexture(
            m_texture
                ? m_texture->view.Get()
                : m_graphics->
                    WhiteTexture());
        m_effect->Apply(context);
        context->IASetInputLayout(
            m_inputLayout->value.Get());

        bool customShaderApplied = false;
        if (!m_shaderPath.empty())
        {
            customShaderApplied =
                m_graphics->ApplyCustomPixelShader(
                    m_shaderPath,
                    m_customParameters,
                    &m_shaderGeneration,
                    &m_shaderError);
            if (customShaderApplied)
            {
                ID3D11ShaderResourceView* resources[]{
                    m_texture
                        ? m_texture->view.Get()
                        : m_graphics->WhiteTexture(),
                    m_auxiliaryTexture
                        ? m_auxiliaryTexture->view.Get()
                        : m_graphics->WhiteTexture()
                };
                context->PSSetShaderResources(
                    0,
                    static_cast<UINT>(
                        std::size(resources)),
                    resources);
            }
        }

        m_batch->Begin();
        for (const std::size_t particleIndex :
            m_renderOrder)
        {
            const auto& particle =
                m_particles[particleIndex];
            const float normalizedAge =
                std::clamp(
                    particle.age
                        / particle.lifetime,
                    0.0f,
                    1.0f);
            const float size =
                particle.startSize
                * (1.0f
                    + (m_endSizeMultiplier
                        - 1.0f)
                        * normalizedAge);
            const XMFLOAT4 color{
                m_startColor.x
                    + (m_endColor.x
                        - m_startColor.x)
                        * normalizedAge,
                m_startColor.y
                    + (m_endColor.y
                        - m_startColor.y)
                        * normalizedAge,
                m_startColor.z
                    + (m_endColor.z
                        - m_startColor.z)
                        * normalizedAge,
                m_startColor.w
                    + (m_endColor.w
                        - m_startColor.w)
                        * normalizedAge
            };
            const float cosine =
                std::cos(
                    particle.rotation);
            const float sine =
                std::sin(
                    particle.rotation);
            // 原作の地面エフェクトはXZ平面に置かれるため、
            // BillboardとHorizontalで回転の基準軸を切り替えます。
            const XMVECTOR baseRight =
                m_renderMode
                    == ParticleRenderMode::Horizontal
                ? g_XMIdentityR0
                : cameraRight;
            const XMVECTOR baseUp =
                m_renderMode
                    == ParticleRenderMode::Horizontal
                ? g_XMIdentityR2
                : cameraUp;
            const bool horizontal = m_renderMode
                == ParticleRenderMode::Horizontal;
            const XMVECTOR rotatedRight =
                XMVectorAdd(
                    XMVectorScale(
                        baseRight,
                        cosine),
                    XMVectorScale(
                        baseUp,
                        horizontal ? -sine : sine));
            const XMVECTOR rotatedUp =
                XMVectorAdd(
                    XMVectorScale(
                        baseRight,
                        horizontal ? sine : -sine),
                    XMVectorScale(
                        baseUp,
                        cosine));
            const XMVECTOR center =
                XMLoadFloat3(
                    &particle.position);
            const XMVECTOR right =
                XMVectorScale(
                    rotatedRight,
                    size * 0.5f);
            const XMVECTOR up =
                XMVectorScale(
                    rotatedUp,
                    size * 0.5f);
            XMFLOAT3 corners[4]{};
            XMStoreFloat3(
                &corners[0],
                XMVectorSubtract(
                    XMVectorSubtract(
                        center,
                        right),
                    up));
            XMStoreFloat3(
                &corners[1],
                XMVectorAdd(
                    XMVectorSubtract(
                        center,
                        up),
                    right));
            XMStoreFloat3(
                &corners[2],
                XMVectorAdd(
                    XMVectorAdd(
                        center,
                        right),
                    up));
            XMStoreFloat3(
                &corners[3],
                XMVectorAdd(
                    XMVectorSubtract(
                        center,
                        right),
                    up));
            m_batch->DrawQuad(
                VertexPositionColorTexture{
                    corners[0],
                    color,
                    { 0.0f, 1.0f } },
                VertexPositionColorTexture{
                    corners[1],
                    color,
                    { 1.0f, 1.0f } },
                VertexPositionColorTexture{
                    corners[2],
                    color,
                    { 1.0f, 0.0f } },
                VertexPositionColorTexture{
                    corners[3],
                    color,
                    { 0.0f, 0.0f } });
        }
        m_batch->End();

        if (customShaderApplied)
        {
            ID3D11ShaderResourceView* resources[]{
                nullptr,
                nullptr
            };
            context->PSSetShaderResources(
                0,
                static_cast<UINT>(
                    std::size(resources)),
                resources);
        }

        context->OMSetBlendState(
            states.Opaque(),
            blendFactor,
            0xffffffffu);
        context->OMSetDepthStencilState(
            states.DepthDefault(),
            0);
        context->RSSetState(
            states.CullCounterClockwise());
    }
}
