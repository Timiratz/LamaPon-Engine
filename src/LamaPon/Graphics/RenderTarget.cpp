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
    // D3D11の描画先状態を借用し、不一致なら空を返す(target: 任意の描画先)。
    [[nodiscard]] LamaPon::Detail::D3D11RenderTargetState*
        AsD3D11State(LamaPon::RenderTarget* const target) noexcept
    {
        return target != nullptr
            ? dynamic_cast<LamaPon::Detail::D3D11RenderTargetState*>(
                LamaPon::Detail::RenderTargetBackendAccess::Get(*target))
            : nullptr;
    }

    // D3D11の描画先状態を借用し、不一致なら空を返す(target: 任意の描画先)。
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
    // 描画先の内部状態を借用し、未生成なら空を返す(target: 描画先)。
    RenderTargetBackendState*
        RenderTargetBackendAccess::Get(RenderTarget& target) noexcept
    {
        return target.m_backendState.get();
    }

    // 描画先の内部状態を借用し、未生成なら空を返す(target: 描画先)。
    const RenderTargetBackendState*
        RenderTargetBackendAccess::Get(
            const RenderTarget& target) noexcept
    {
        return target.m_backendState.get();
    }

    // 表示面への計算書込み指定を返す(target: 描画先)。
    bool RenderTargetBackendAccess::ComputeWritable(
        const RenderTarget& target) noexcept
    {
        return target.m_computeWritable;
    }

    // 公開用の履歴行列を更新する(target: 描画先, viewProjection: 履歴のビュー射影行列)。
    void RenderTargetBackendAccess::SetPublicHistoryViewProjection(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection) noexcept
    {
        target.m_publicHistoryViewProjection = viewProjection;
    }

    // 旧状態を解放して新しい状態を所有する(target: 描画先, state: 新しい内部状態)。
    void RenderTargetBackendAccess::Publish(
        RenderTarget& target,
        std::unique_ptr<RenderTargetBackendState> state) noexcept
    {
        target.m_backendState = std::move(state);
    }
}

// 旧APIから現在のカラーを借用する(target: 任意の描画先)。
// API 67以前のモジュールは版の検査前に旧メンバーを解決するため、互換エクスポートを保持する。
// x64ではthisと参照引数がポインターで渡されるため、旧メンバー名をこの関数群へ別名公開する。
// 旧モジュールの実行はAPI版の不一致で拒否する。
extern "C" void* LamaPonLegacyRenderTargetShaderResourceView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr ? state->ShaderResourceView() : nullptr;
}

// 旧APIから表示面を借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetDisplayShaderResourceView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->DisplayShaderResourceView()
        : nullptr;
}

// 旧APIから画面空間遮蔽を借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetAmbientOcclusionView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->AmbientOcclusionShaderResourceView()
        : nullptr;
}

// 旧APIからHDR色履歴を借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetColorHistoryView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->ColorHistoryShaderResourceView()
        : nullptr;
}

// 旧APIから反射用Hi-Z深度を借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetReflectionDepthPyramidView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->ReflectionDepthPyramidShaderResourceView()
        : nullptr;
}

// 旧APIから深度を借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetDepthView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->DepthShaderResourceView()
        : nullptr;
}

// 旧APIから複製した深度を借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetDepthCopyView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->m_depthCopyShaderResourceView.Get()
        : nullptr;
}

// 旧APIからHi-Z各段の描画先を借用する(target: 任意の描画先, mip: ミップ番号)。
extern "C" void* LamaPonLegacyRenderTargetReflectionMipTarget(
    const LamaPon::RenderTarget* const target,
    const std::uint32_t mip) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
            && mip < state->m_reflectionDepthPyramidTargets.size()
        ? state->m_reflectionDepthPyramidTargets[mip].Get()
        : nullptr;
}

// 旧APIからHi-Z各段の参照を借用する(target: 任意の描画先, mip: ミップ番号)。
extern "C" void* LamaPonLegacyRenderTargetReflectionMipView(
    const LamaPon::RenderTarget* const target,
    const std::uint32_t mip) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
            && mip < state->m_reflectionDepthPyramidMipViews.size()
        ? state->m_reflectionDepthPyramidMipViews[mip].Get()
        : nullptr;
}

// 旧APIから表示面の計算書込みを借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetDisplayUnorderedAccessView(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->m_displayUnorderedAccessView.Get()
        : nullptr;
}

// 旧APIから表示面のテクスチャを借用する(target: 任意の描画先)。
extern "C" void* LamaPonLegacyRenderTargetDisplayTexture(
    const LamaPon::RenderTarget* const target) noexcept
{
    // 借用するD3D11の描画先状態
    const auto* const state = AsD3D11State(target);
    return state != nullptr
        ? state->m_displayColorTexture.Get()
        : nullptr;
}

// 旧APIからブルームを適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定)。
extern "C" void LamaPonLegacyRenderTargetApplyBloom(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::BloomSettings* const settings)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr)
    {
        state->ApplyBloom(*renderer, *settings);
    }
}

// 旧APIから輪郭線を適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定, projection: 任意の射影行列)。
extern "C" void LamaPonLegacyRenderTargetApplyScreenOutline(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::ScreenOutlineSettings* const settings,
    const DirectX::XMFLOAT4X4* const projection)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && projection != nullptr)
    {
        state->ApplyScreenOutline(*renderer, *settings, *projection);
    }
}

// 旧APIからレンズフレアを適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定)。
extern "C" void LamaPonLegacyRenderTargetApplyScreenSpaceLensFlare(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::ScreenSpaceLensFlareSettings* const settings)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr)
    {
        state->ApplyScreenSpaceLensFlare(*renderer, *settings);
    }
}

// 旧APIからTAAを適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定, inputs: 任意のフレーム入力)。
extern "C" void LamaPonLegacyRenderTargetApplyTemporalAntiAliasing(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::TemporalAntiAliasingSettings* const settings,
    const LamaPon::EnvironmentRenderer::TemporalInputs* const inputs)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && inputs != nullptr)
    {
        state->ApplyTemporalAntiAliasing(
            *renderer, *settings, *inputs);
    }
}

// 旧APIから体積光を適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定, inputs: 任意のフレーム入力)。
extern "C" void LamaPonLegacyRenderTargetApplyVolumetricLight(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::VolumetricLightSettings* const settings,
    const LamaPon::EnvironmentRenderer::VolumetricInputs* const inputs)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && inputs != nullptr)
    {
        state->ApplyVolumetricLight(*renderer, *settings, *inputs);
    }
}

// 旧APIから被写界深度を適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定, projection: 任意の射影行列, sampleCount: サンプル数)。
extern "C" void LamaPonLegacyRenderTargetApplyDepthOfField(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::DepthOfFieldSettings* const settings,
    const DirectX::XMFLOAT4X4* const projection,
    const std::uint32_t sampleCount)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && projection != nullptr)
    {
        state->ApplyDepthOfField(
            *renderer, *settings, *projection, sampleCount);
    }
}

// 旧APIからブラーを適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定, inverseViewProjection: 任意の逆ビュー射影行列, viewProjection: 任意のビュー射影行列, sampleCount: サンプル数)。
extern "C" void LamaPonLegacyRenderTargetApplyMotionBlur(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::MotionBlurSettings* const settings,
    const DirectX::XMFLOAT4X4* const inverseViewProjection,
    const DirectX::XMFLOAT4X4* const viewProjection,
    const std::uint32_t sampleCount)
{
    // 借用するD3D11の描画先状態
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

// 旧APIからトーン変換を適用する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の効果設定)。
extern "C" void LamaPonLegacyRenderTargetApplyToneMapping(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::ColorGradingSettings* const settings)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr)
    {
        state->ApplyToneMapping(*renderer, *settings);
    }
}

// 旧APIからFXAAを適用する(target: 任意の描画先, renderer: 任意の処理器)。
extern "C" void LamaPonLegacyRenderTargetApplyFXAA(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr)
    {
        state->ApplyFXAA(*renderer);
    }
}

// 旧APIから画面空間遮蔽を生成する(target: 任意の描画先, renderer: 任意の処理器, settings: 任意の遮蔽設定, projection: 任意の射影行列, sampleCount: サンプル数)。
extern "C" bool LamaPonLegacyRenderTargetResolveAmbientOcclusion(
    LamaPon::RenderTarget* const target,
    LamaPon::EnvironmentRenderer* const renderer,
    const LamaPon::AmbientOcclusionSettings* const settings,
    const DirectX::XMFLOAT4X4* const projection,
    const std::uint32_t sampleCount)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && renderer != nullptr && settings != nullptr
        && projection != nullptr)
    {
        return state->ResolveAmbientOcclusion(
            *renderer, *settings, *projection, sampleCount);
    }
    return false;
}

// 旧APIから画面効果を適用する(target: 任意の描画先, effect: 任意の画面効果, auxiliaryTextures: 旧APIの補助参照配列, depthParameters: 任意の深度係数, depthUnprojection: 任意の深度逆射影係数, parameters: 任意の独自定数配列)。
extern "C" void LamaPonLegacyRenderTargetApplyScreenEffect(
    LamaPon::RenderTarget* const target,
    LamaPon::ScreenEffect* const effect,
    const void* const auxiliaryTextures,
    const DirectX::XMFLOAT4* const depthParameters,
    const DirectX::XMFLOAT4* const depthUnprojection,
    const std::array<DirectX::XMFLOAT4, 8>* const parameters)
{
    // 借用するD3D11の描画先状態
    if (auto* const state = AsD3D11State(target);
        state != nullptr && effect != nullptr
        && auxiliaryTextures != nullptr && depthParameters != nullptr
        && depthUnprojection != nullptr && parameters != nullptr)
    {
        // 旧APIの補助テクスチャ参照
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
