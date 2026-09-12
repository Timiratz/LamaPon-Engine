#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/D3D11RenderTargetState.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
    [[nodiscard]] const LamaPon::Detail::D3D11RenderTargetState&
        RequireCurrentOffscreenTarget(
        const LamaPon::GraphicsDevice& graphics,
        const LamaPon::RenderTarget& target,
        const char* const operation)
    {
        if (!graphics.IsInitialized())
        {
            throw std::logic_error(
                std::string(operation)
                + " requires an initialized device.");
        }
        if (!target.IsValid()
            || !graphics.IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !graphics.IsGraphicsViewCurrent(
                target.DepthViewHandle()))
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }

        const auto* const state = dynamic_cast<const
            LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr || !state->IsValid())
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }
        return *state;
    }

    [[nodiscard]] LamaPon::Detail::D3D11RenderTargetState&
        RequireCurrentOffscreenTarget(
            const LamaPon::GraphicsDevice& graphics,
            LamaPon::RenderTarget& target,
            const char* const operation)
    {
        static_cast<void>(RequireCurrentOffscreenTarget(
            graphics,
            static_cast<const LamaPon::RenderTarget&>(target),
            operation));
        auto* const state = dynamic_cast<
            LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr || !state->IsValid())
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }
        return *state;
    }

    [[nodiscard]] LamaPon::EnvironmentRenderer::TemporalInputs
        ToD3D11TemporalInputs(
            const LamaPon::TemporalAntiAliasingInputs& source)
    {
        LamaPon::EnvironmentRenderer::TemporalInputs result{};
        result.inverseViewProjection = source.inverseViewProjection;
        result.viewProjection = source.viewProjection;
        return result;
    }

    [[nodiscard]] LamaPon::EnvironmentRenderer::VolumetricInputs
        ToD3D11VolumetricInputs(
            const LamaPon::VolumetricLightInputs& source)
    {
        LamaPon::EnvironmentRenderer::VolumetricInputs result{};
        result.cascadeShadow = source.cascadeShadow;
        result.inverseViewProjection = source.inverseViewProjection;
        result.cameraPosition = source.cameraPosition;
        result.lightDirection = source.lightDirection;
        result.lightColor = source.lightColor;
        result.cascadeViewProjections = source.cascadeViewProjections;
        result.cascadeCount = source.cascadeCount;
        result.shadowBias = source.shadowBias;
        result.shadowResolution = source.shadowResolution;
        return result;
    }
}

namespace LamaPon
{
    void GraphicsDevice::CopyOffscreenTargetToBackBuffer(
        const RenderTarget& target)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            if (!target.IsValid()
                || !IsGraphicsViewCurrent(
                    target.CurrentColorViewHandle())
                || !IsGraphicsViewCurrent(
                    target.DisplayViewHandle()))
            {
                throw std::invalid_argument(
                    "CopyOffscreenTargetToBackBuffer requires a current "
                    "DirectX 12 target.");
            }

            // 基本D3D12 RenderTargetはLDRなので、表示用資源へ
            // 確定した画像を既存Sprite pipelineで画面全体へ
            // 転写します。HDRトーンマップは後続段階で
            // 専用fullscreen pipelineへ置き換えます。
            auto& mutableTarget = const_cast<RenderTarget&>(target);
            m_state->m_backend->PublishOffscreenTarget(mutableTarget);
            m_state->m_backend->BindBackBuffer();

            SpritePassDescription description;
            description.blend = SpriteBlendMode::Opaque;
            auto pass = BeginSpritePass(description);
            SpriteDrawRequest request;
            request.texture = target.DisplayViewHandle();
            request.scale = {
                static_cast<float>(Width())
                    / static_cast<float>(
                        std::max(target.Width(), 1u)),
                static_cast<float>(Height())
                    / static_cast<float>(
                        std::max(target.Height(), 1u)) };
            if (!pass.Draw(request))
            {
                throw std::runtime_error(
                    "The DirectX 12 scene composition texture was "
                    "rejected.");
            }
            pass.End();
            return;
        }

        static_cast<void>(RequireCurrentOffscreenTarget(
            *this,
            target,
            "CopyOffscreenTargetToBackBuffer"));
        auto& environment = Environment();
        auto* const source = TryResolveD3D11ShaderResourceView(
            target.CurrentColorViewHandle());
        if (source == nullptr)
        {
            throw std::invalid_argument(
                "CopyOffscreenTargetToBackBuffer requires a resolvable "
                "current color view.");
        }

        m_state->m_backend->BindBackBuffer();
        environment.CopyToBoundRenderTarget(source);
    }

    bool GraphicsDevice::ResolveOffscreenTargetAmbientOcclusion(
        RenderTarget& target,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // SSAO shaderはまだD3D11専用ですが、後続パスが
            // 同じ公開DepthViewを読めるようコピーは確定します。
            m_state->m_backend->CaptureOffscreenTargetDepth(target);
            return false;
        }
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ResolveOffscreenTargetAmbientOcclusion");
        if (!settings.enabled)
        {
            return false;
        }
        return targetState.ResolveAmbientOcclusion(
            Environment(),
            settings,
            projection,
            sampleCount);
    }

    void GraphicsDevice::ApplyOffscreenTargetTemporalAntiAliasing(
        RenderTarget& target,
        const TemporalAntiAliasingSettings& settings,
        const TemporalAntiAliasingInputs& inputs)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetTemporalAntiAliasing");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyTemporalAntiAliasing(
            Environment(),
            settings,
            ToD3D11TemporalInputs(inputs));
    }

    void GraphicsDevice::ApplyOffscreenTargetVolumetricLight(
        RenderTarget& target,
        const VolumetricLightSettings& settings,
        const VolumetricLightInputs& inputs)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetVolumetricLight");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyVolumetricLight(
            Environment(),
            settings,
            ToD3D11VolumetricInputs(inputs));
    }

    void GraphicsDevice::ApplyOffscreenTargetDepthOfField(
        RenderTarget& target,
        const DepthOfFieldSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetDepthOfField");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyDepthOfField(
            Environment(),
            settings,
            projection,
            sampleCount);
    }

    void GraphicsDevice::ApplyOffscreenTargetMotionBlur(
        RenderTarget& target,
        const MotionBlurSettings& settings,
        const DirectX::XMFLOAT4X4& inverseViewProjection,
        const DirectX::XMFLOAT4X4& viewProjection,
        const std::uint32_t sampleCount)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetMotionBlur");
        // disabledでもRenderTargetへ渡し、保持している前フレーム行列を
        // 無効化します。再度有効にした瞬間の大きなブレを防ぐためです。
        targetState.ApplyMotionBlur(
            Environment(),
            settings,
            inverseViewProjection,
            viewProjection,
            sampleCount);
    }

    void GraphicsDevice::ApplyOffscreenTargetBloom(
        RenderTarget& target,
        const BloomSettings& settings)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetBloom");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyBloom(Environment(), settings);
    }

    void GraphicsDevice::ApplyOffscreenTargetScreenSpaceLensFlare(
        RenderTarget& target,
        const ScreenSpaceLensFlareSettings& settings)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetScreenSpaceLensFlare");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyScreenSpaceLensFlare(
            Environment(),
            settings);
    }

    void GraphicsDevice::ApplyOffscreenTargetToneMapping(
        RenderTarget& target,
        const ColorGradingSettings& settings)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetToneMapping");
        targetState.ApplyToneMapping(Environment(), settings);
    }

    void GraphicsDevice::ApplyOffscreenTargetScreenOutline(
        RenderTarget& target,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetScreenOutline");
        if (!settings.enabled)
        {
            return;
        }
        targetState.ApplyScreenOutline(
            Environment(),
            settings,
            projection);
    }

    void GraphicsDevice::ApplyOffscreenTargetFXAA(
        RenderTarget& target)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetFXAA");
        targetState.ApplyFXAA(Environment());
    }

    float GraphicsDevice::UpdateOffscreenTargetAutoExposure(
        RenderTarget& target,
        const AutoExposureSettings& settings,
        const float deltaSeconds)
    {
        auto& targetState = RequireCurrentOffscreenTarget(
            *this,
            target,
            "UpdateOffscreenTargetAutoExposure");

        std::optional<float> measuredLuminance;
        if (settings.enabled)
        {
            measuredLuminance =
                m_state->m_backend->TryReadOffscreenTargetLuminance(target);
        }

        const float exposureStops = targetState.UpdateAutoExposure(
            Environment(),
            measuredLuminance,
            settings,
            deltaSeconds);

        if (settings.enabled)
        {
            m_state->m_backend->CaptureOffscreenTargetLuminance(target);
        }
        return exposureStops;
    }
}
