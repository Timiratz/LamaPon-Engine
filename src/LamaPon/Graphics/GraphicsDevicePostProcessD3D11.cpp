#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <stdexcept>
#include <string>

namespace
{
    void RequireCurrentOffscreenTarget(
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
                target.DepthViewHandle()))
        {
            throw std::invalid_argument(
                std::string(operation)
                + " requires a target owned by the active backend.");
        }
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
    bool GraphicsDevice::ResolveOffscreenTargetAmbientOcclusion(
        RenderTarget& target,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ResolveOffscreenTargetAmbientOcclusion");
        if (!settings.enabled)
        {
            return false;
        }
        return target.ResolveAmbientOcclusion(
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
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetTemporalAntiAliasing");
        if (!settings.enabled)
        {
            return;
        }
        target.ApplyTemporalAntiAliasing(
            Environment(),
            settings,
            ToD3D11TemporalInputs(inputs));
    }

    void GraphicsDevice::ApplyOffscreenTargetVolumetricLight(
        RenderTarget& target,
        const VolumetricLightSettings& settings,
        const VolumetricLightInputs& inputs)
    {
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetVolumetricLight");
        if (!settings.enabled)
        {
            return;
        }
        target.ApplyVolumetricLight(
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
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetDepthOfField");
        if (!settings.enabled)
        {
            return;
        }
        target.ApplyDepthOfField(
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
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetMotionBlur");
        // disabledでもRenderTargetへ渡し、保持している前フレーム行列を
        // 無効化します。再度有効にした瞬間の大きなブレを防ぐためです。
        target.ApplyMotionBlur(
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
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetBloom");
        if (!settings.enabled)
        {
            return;
        }
        target.ApplyBloom(Environment(), settings);
    }

    void GraphicsDevice::ApplyOffscreenTargetScreenSpaceLensFlare(
        RenderTarget& target,
        const ScreenSpaceLensFlareSettings& settings)
    {
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetScreenSpaceLensFlare");
        if (!settings.enabled)
        {
            return;
        }
        target.ApplyScreenSpaceLensFlare(
            Environment(),
            settings);
    }

    void GraphicsDevice::ApplyOffscreenTargetToneMapping(
        RenderTarget& target,
        const ColorGradingSettings& settings)
    {
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetToneMapping");
        target.ApplyToneMapping(Environment(), settings);
    }

    void GraphicsDevice::ApplyOffscreenTargetScreenOutline(
        RenderTarget& target,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetScreenOutline");
        if (!settings.enabled)
        {
            return;
        }
        target.ApplyScreenOutline(
            Environment(),
            settings,
            projection);
    }

    void GraphicsDevice::ApplyOffscreenTargetFXAA(
        RenderTarget& target)
    {
        RequireCurrentOffscreenTarget(
            *this,
            target,
            "ApplyOffscreenTargetFXAA");
        target.ApplyFXAA(Environment());
    }
}
