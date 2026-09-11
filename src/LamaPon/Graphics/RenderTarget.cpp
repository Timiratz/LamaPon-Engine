#include "LamaPon/Graphics/RenderTarget.h"

#include "LamaPon/Graphics/D3D11RenderTargetState.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"
#include "LamaPon/Graphics/ScreenEffect.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace
{
    [[nodiscard]] LamaPon::Detail::D3D11RenderTargetState*
        AsD3D11State(LamaPon::RenderTarget* const target) noexcept
    {
        return target != nullptr
            ? dynamic_cast<LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(*target))
            : nullptr;
    }

    [[nodiscard]] const LamaPon::Detail::D3D11RenderTargetState*
        AsD3D11State(const LamaPon::RenderTarget* const target) noexcept
    {
        return target != nullptr
            ? dynamic_cast<const LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(*target))
            : nullptr;
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

    void RenderTarget::SetComputeWritable(const bool value) noexcept
    {
        m_computeWritable = value;
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
}

namespace LamaPon::Detail
{
    RenderTargetBackendState*
        RenderTargetBackendAccess::Get(RenderTarget& target) noexcept
    {
        return target.m_backendState.get();
    }

    const RenderTargetBackendState*
        RenderTargetBackendAccess::Get(
            const RenderTarget& target) noexcept
    {
        return target.m_backendState.get();
    }

    bool RenderTargetBackendAccess::ComputeWritable(
        const RenderTarget& target) noexcept
    {
        return target.m_computeWritable;
    }

    void RenderTargetBackendAccess::SetPublicHistoryViewProjection(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection) noexcept
    {
        target.m_publicHistoryViewProjection = viewProjection;
    }

    void RenderTargetBackendAccess::Publish(
        RenderTarget& target,
        std::unique_ptr<RenderTargetBackendState> state) noexcept
    {
        target.m_backendState = std::move(state);
    }
}

// API 67以前のGame ModuleはAPI version検査より先に旧member importを
// 解決します。x64ではmemberのthisと参照引数はいずれもpointerとして
// 渡されるため、API-neutralな公開ヘッダーへD3D11型を戻さずにこの
// loader互換thunkへaliasできます。実際の旧module利用はversion不一致で
// 拒否されますが、getterと処理の従来動作も可能な範囲で維持します。
extern "C" void* LamaPonLegacyRenderTargetShaderResourceView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr ? state->ShaderResourceView() : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetDisplayShaderResourceView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->DisplayShaderResourceView()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetAmbientOcclusionView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->AmbientOcclusionShaderResourceView()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetColorHistoryView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->ColorHistoryShaderResourceView()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetReflectionDepthPyramidView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->ReflectionDepthPyramidShaderResourceView()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetDepthView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->DepthShaderResourceView()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetDepthCopyView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->m_depthCopyShaderResourceView.Get()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetReflectionMipTarget(
    const LamaPon::RenderTarget* const target,
    const std::uint32_t mip) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
            && mip < state->m_reflectionDepthPyramidTargets.size()
        ? state->m_reflectionDepthPyramidTargets[mip].Get()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetReflectionMipView(
    const LamaPon::RenderTarget* const target,
    const std::uint32_t mip) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
            && mip < state->m_reflectionDepthPyramidMipViews.size()
        ? state->m_reflectionDepthPyramidMipViews[mip].Get()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetDisplayUnorderedAccessView(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->m_displayUnorderedAccessView.Get()
        : nullptr;
}

extern "C" void* LamaPonLegacyRenderTargetDisplayTexture(
    const LamaPon::RenderTarget* const target) noexcept
{
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->m_displayColorTexture.Get()
        : nullptr;
}

extern "C" void LamaPonLegacyRenderTargetApplyBloom(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::BloomSettings* const settings)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr)
    {
        state->ApplyBloom(*renderer, *settings);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyScreenOutline(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::ScreenOutlineSettings* const settings,
    const DirectX::XMFLOAT4X4* const projection)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && projection != nullptr)
    {
        state->ApplyScreenOutline(*renderer, *settings, *projection);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyScreenSpaceLensFlare(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::ScreenSpaceLensFlareSettings* const settings)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr)
    {
        state->ApplyScreenSpaceLensFlare(*renderer, *settings);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyTemporalAntiAliasing(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::TemporalAntiAliasingSettings* const settings,
    const LamaPon::EnvironmentRenderer::TemporalInputs* const inputs)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && inputs != nullptr)
    {
        state->ApplyTemporalAntiAliasing(
            *renderer, *settings, *inputs);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyVolumetricLight(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::VolumetricLightSettings* const settings,
    const LamaPon::EnvironmentRenderer::VolumetricInputs* const inputs)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && inputs != nullptr)
    {
        state->ApplyVolumetricLight(*renderer, *settings, *inputs);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyDepthOfField(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::DepthOfFieldSettings* const settings,
    const DirectX::XMFLOAT4X4* const projection,
    const std::uint32_t sampleCount)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && projection != nullptr)
    {
        state->ApplyDepthOfField(
            *renderer, *settings, *projection, sampleCount);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyMotionBlur(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::MotionBlurSettings* const settings,
    const DirectX::XMFLOAT4X4* const inverseViewProjection,
    const DirectX::XMFLOAT4X4* const viewProjection,
    const std::uint32_t sampleCount)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && inverseViewProjection != nullptr && viewProjection != nullptr)
    {
        state->ApplyMotionBlur(
            *renderer,
            *settings,
            *inverseViewProjection,
            *viewProjection,
            sampleCount);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyToneMapping(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::ColorGradingSettings* const settings)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr)
    {
        state->ApplyToneMapping(*renderer, *settings);
    }
}

extern "C" void LamaPonLegacyRenderTargetApplyFXAA(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr)
    {
        state->ApplyFXAA(*renderer);
    }
}

extern "C" bool LamaPonLegacyRenderTargetResolveAmbientOcclusion(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::AmbientOcclusionSettings* const settings,
    const DirectX::XMFLOAT4X4* const projection,
    const std::uint32_t sampleCount)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && projection != nullptr)
    {
        return state->ResolveAmbientOcclusion(
            *renderer, *settings, *projection, sampleCount);
    }
    return false;
}

extern "C" void LamaPonLegacyRenderTargetApplyScreenEffect(
    LamaPon::RenderTarget* const target,
    LamaPon::ScreenEffect* const effect,
    const void* const auxiliaryTextures,
    const DirectX::XMFLOAT4* const depthParameters,
    const DirectX::XMFLOAT4* const depthUnprojection,
    const std::array<DirectX::XMFLOAT4, 8>* const parameters)
{
    if (auto* const state = AsD3D11State(target);
        state != nullptr && effect != nullptr
        && auxiliaryTextures != nullptr && depthParameters != nullptr
        && depthUnprojection != nullptr && parameters != nullptr)
    {
        const auto& views = *static_cast<const std::array<
            ID3D11ShaderResourceView*, 2>*>(auxiliaryTextures);
        state->ApplyScreenEffect(
            *effect,
            views,
            *depthParameters,
            *depthUnprojection,
            *parameters);
    }
}
