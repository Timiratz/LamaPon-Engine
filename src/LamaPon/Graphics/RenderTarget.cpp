#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"
#include "LamaPon/Graphics/ScreenEffect.h"

namespace
{
    [[nodiscard]] LamaPon::Detail::D3D11RenderTargetState*
        AsD3D11State(
            LamaPon::Detail::RenderTargetBackendState* const state)
        noexcept
    {
        return dynamic_cast<
            LamaPon::Detail::D3D11RenderTargetState*>(state);
    }

    [[nodiscard]] const LamaPon::Detail::D3D11RenderTargetState*
        AsD3D11State(
            const LamaPon::Detail::RenderTargetBackendState* const state)
        noexcept
    {
        return dynamic_cast<
            const LamaPon::Detail::D3D11RenderTargetState*>(state);
    }
}

namespace LamaPon
{
    RenderTarget::RenderTarget() noexcept = default;

    RenderTarget::~RenderTarget() noexcept = default;

    float RenderTarget::AdaptedLuminance() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_adaptedLuminance
            : 0.0f;
    }

    float RenderTarget::AutoExposureStops() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_autoExposureStops
            : 0.0f;
    }

    GraphicsViewHandle
        RenderTarget::AmbientOcclusionViewHandle() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_ambientOcclusionView
            : GraphicsViewHandle{};
    }

    GraphicsViewHandle
        RenderTarget::CurrentColorViewHandle() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_currentColorView
            : GraphicsViewHandle{};
    }

    GraphicsViewHandle
        RenderTarget::ColorHistoryViewHandle() const noexcept
    {
        return m_backendState != nullptr
                && m_backendState->m_historyValid
            ? m_backendState->m_colorHistoryView
            : GraphicsViewHandle{};
    }

    GraphicsViewHandle
        RenderTarget::TemporalHistoryViewHandle() const noexcept
    {
        return m_backendState != nullptr
                && m_backendState->m_temporalHistoryValid
            ? m_backendState->m_temporalHistoryView
            : GraphicsViewHandle{};
    }

    const DirectX::XMFLOAT4X4&
        RenderTarget::ColorHistoryViewProjection() const noexcept
    {
        return m_publicHistoryViewProjection;
    }

    ID3D11ShaderResourceView*
        RenderTarget::DepthCopyShaderResourceView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->m_depthCopyShaderResourceView.Get()
            : nullptr;
    }

    GraphicsViewHandle RenderTarget::DepthViewHandle() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_depthView
            : GraphicsViewHandle{};
    }

    GraphicsViewHandle
        RenderTarget::ReflectionDepthPyramidViewHandle() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_reflectionDepthPyramidViewHandle
            : GraphicsViewHandle{};
    }

    std::uint32_t
        RenderTarget::ReflectionDepthPyramidMipCount() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_reflectionDepthPyramidMipCount
            : 0u;
    }

    ID3D11RenderTargetView*
        RenderTarget::ReflectionDepthPyramidMipTarget(
            const std::uint32_t mip) const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
                && mip < state->m_reflectionDepthPyramidTargets.size()
            ? state->m_reflectionDepthPyramidTargets[mip].Get()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        RenderTarget::ReflectionDepthPyramidMipView(
            const std::uint32_t mip) const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
                && mip < state->m_reflectionDepthPyramidMipViews.size()
            ? state->m_reflectionDepthPyramidMipViews[mip].Get()
            : nullptr;
    }

    void RenderTarget::SetComputeWritable(const bool value) noexcept
    {
        m_computeWritable = value;
    }

    ID3D11UnorderedAccessView*
        RenderTarget::DisplayUnorderedAccessView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->m_displayUnorderedAccessView.Get()
            : nullptr;
    }

    ID3D11Texture2D* RenderTarget::DisplayTexture() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->m_displayColorTexture.Get()
            : nullptr;
    }

    GraphicsViewHandle RenderTarget::DisplayViewHandle() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_displayView
            : GraphicsViewHandle{};
    }

    std::uint32_t RenderTarget::Width() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_width
            : 0u;
    }

    std::uint32_t RenderTarget::Height() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_height
            : 0u;
    }

    float RenderTarget::AspectRatio() const noexcept
    {
        return static_cast<float>(Width())
            / static_cast<float>(std::max(Height(), 1u));
    }

    bool RenderTarget::IsValid() const noexcept
    {
        return m_backendState != nullptr
            && m_backendState->m_initialized;
    }

    ID3D11ShaderResourceView*
        RenderTarget::ShaderResourceView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->ShaderResourceView()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        RenderTarget::DisplayShaderResourceView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->DisplayShaderResourceView()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        RenderTarget::AmbientOcclusionShaderResourceView()
        const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->AmbientOcclusionShaderResourceView()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        RenderTarget::ColorHistoryShaderResourceView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->ColorHistoryShaderResourceView()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        RenderTarget::ReflectionDepthPyramidShaderResourceView()
        const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->ReflectionDepthPyramidShaderResourceView()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        RenderTarget::DepthShaderResourceView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->DepthShaderResourceView()
            : nullptr;
    }

    void RenderTarget::Bind(ID3D11DeviceContext* const context) const
    {
        if (const auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->Bind(context);
        }
    }

    void RenderTarget::Clear(
        ID3D11DeviceContext* const context,
        const float color[4]) const
    {
        if (const auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->Clear(context, color);
        }
    }

    void RenderTarget::CopyToDisplay(
        ID3D11DeviceContext* const context) const
    {
        if (const auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->CopyToDisplay(context);
        }
    }

    void RenderTarget::BindDepthOnly(
        ID3D11DeviceContext* const context) const
    {
        if (const auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->BindDepthOnly(context);
        }
    }

    void RenderTarget::CaptureDepthForReflections(
        ID3D11DeviceContext* const context) const
    {
        if (const auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->CaptureDepthForReflections(context);
        }
    }

    void RenderTarget::CaptureColorHistory(
        ID3D11DeviceContext* const context,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->CaptureColorHistory(context, viewProjection);
            if (state->m_historyValid)
            {
                m_publicHistoryViewProjection =
                    state->m_historyViewProjection;
            }
        }
    }

    void RenderTarget::CaptureTemporalHistory(
        ID3D11DeviceContext* const context,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->CaptureTemporalHistory(context, viewProjection);
        }
    }

    float RenderTarget::UpdateAutoExposure(
        EnvironmentRenderer& renderer,
        const std::optional<float> measuredLuminance,
        const AutoExposureSettings& settings,
        const float deltaSeconds)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            return state->UpdateAutoExposure(
                renderer,
                measuredLuminance,
                settings,
                deltaSeconds);
        }
        return 0.0f;
    }

    std::optional<float>
        RenderTarget::TryReadAutoExposureLuminance(
            ID3D11DeviceContext* const context)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            return state->TryReadAutoExposureLuminance(context);
        }
        return std::nullopt;
    }

    void RenderTarget::CaptureAutoExposureLuminance(
        ID3D11DeviceContext* const context)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->CaptureAutoExposureLuminance(context);
        }
    }

    void RenderTarget::ApplyBloom(
        EnvironmentRenderer& renderer,
        const BloomSettings& settings)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyBloom(renderer, settings);
        }
    }

    void RenderTarget::ApplyScreenOutline(
        EnvironmentRenderer& renderer,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyScreenOutline(renderer, settings, projection);
        }
    }

    void RenderTarget::ApplyScreenSpaceLensFlare(
        EnvironmentRenderer& renderer,
        const ScreenSpaceLensFlareSettings& settings)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyScreenSpaceLensFlare(renderer, settings);
        }
    }

    void RenderTarget::ApplyTemporalAntiAliasing(
        EnvironmentRenderer& renderer,
        const TemporalAntiAliasingSettings& settings,
        const EnvironmentRenderer::TemporalInputs& inputs)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyTemporalAntiAliasing(renderer, settings, inputs);
        }
    }

    void RenderTarget::ApplyVolumetricLight(
        EnvironmentRenderer& renderer,
        const VolumetricLightSettings& settings,
        const EnvironmentRenderer::VolumetricInputs& inputs)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyVolumetricLight(renderer, settings, inputs);
        }
    }

    void RenderTarget::ApplyDepthOfField(
        EnvironmentRenderer& renderer,
        const DepthOfFieldSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyDepthOfField(
                renderer,
                settings,
                projection,
                sampleCount);
        }
    }

    void RenderTarget::ApplyMotionBlur(
        EnvironmentRenderer& renderer,
        const MotionBlurSettings& settings,
        const DirectX::XMFLOAT4X4& inverseViewProjection,
        const DirectX::XMFLOAT4X4& viewProjection,
        const std::uint32_t sampleCount)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyMotionBlur(
                renderer,
                settings,
                inverseViewProjection,
                viewProjection,
                sampleCount);
        }
    }

    void RenderTarget::ApplyToneMapping(
        EnvironmentRenderer& renderer,
        const ColorGradingSettings& settings)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyToneMapping(renderer, settings);
        }
    }

    void RenderTarget::ApplyFXAA(EnvironmentRenderer& renderer)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyFXAA(renderer);
        }
    }

    bool RenderTarget::ResolveAmbientOcclusion(
        EnvironmentRenderer& renderer,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            return state->ResolveAmbientOcclusion(
                renderer,
                settings,
                projection,
                sampleCount);
        }
        return false;
    }

    void RenderTarget::ApplyScreenEffect(
        ScreenEffect& effect,
        const std::array<ID3D11ShaderResourceView*, 2>&
            auxiliaryTextures,
        const DirectX::XMFLOAT4& depthParameters,
        const DirectX::XMFLOAT4& depthUnprojection,
        const std::array<DirectX::XMFLOAT4, 8>& parameters)
    {
        if (auto* const state = AsD3D11State(m_backendState.get()))
        {
            state->ApplyScreenEffect(
                effect,
                auxiliaryTextures,
                depthParameters,
                depthUnprojection,
                parameters);
        }
    }
}
