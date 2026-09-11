#pragma once

// DirectX 11専用のRenderTarget stateです。共通stateと分けることで、
// 将来のD3D12 BackendはD3D11型をincludeせず実装できます。
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace LamaPon
{
    class ScreenEffect;
}

namespace LamaPon::Detail
{
    struct D3D11RenderTargetState final : RenderTargetBackendState
    {
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_initialized && m_renderTargetView != nullptr;
        }

        [[nodiscard]] ID3D11ShaderResourceView*
            ShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            DisplayShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            AmbientOcclusionShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            ColorHistoryShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            ReflectionDepthPyramidShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthShaderResourceView() const noexcept;

        void Resize(
            ID3D11Device* device,
            std::uint32_t width,
            std::uint32_t height);
        void Bind(ID3D11DeviceContext* context) const;
        void Clear(
            ID3D11DeviceContext* context,
            const float color[4]) const;
        void CopyToDisplay(ID3D11DeviceContext* context) const;
        void BindDepthOnly(ID3D11DeviceContext* context) const;
        void CaptureDepthForReflections(
            ID3D11DeviceContext* context) const;
        void CaptureColorHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        void CaptureTemporalHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        [[nodiscard]] float UpdateAutoExposure(
            EnvironmentRenderer& renderer,
            std::optional<float> measuredLuminance,
            const AutoExposureSettings& settings,
            float deltaSeconds);
        [[nodiscard]] std::optional<float>
            TryReadAutoExposureLuminance(
                ID3D11DeviceContext* context);
        void CaptureAutoExposureLuminance(
            ID3D11DeviceContext* context);
        [[nodiscard]] float AspectRatio() const noexcept;
        void SwapPostProcessBuffers() noexcept;

        void ApplyBloom(
            EnvironmentRenderer& renderer,
            const BloomSettings& settings);
        void ApplyScreenOutline(
            EnvironmentRenderer& renderer,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        void ApplyScreenSpaceLensFlare(
            EnvironmentRenderer& renderer,
            const ScreenSpaceLensFlareSettings& settings);
        void ApplyTemporalAntiAliasing(
            EnvironmentRenderer& renderer,
            const TemporalAntiAliasingSettings& settings,
            const EnvironmentRenderer::TemporalInputs& inputs);
        void ApplyVolumetricLight(
            EnvironmentRenderer& renderer,
            const VolumetricLightSettings& settings,
            const EnvironmentRenderer::VolumetricInputs& inputs);
        void ApplyDepthOfField(
            EnvironmentRenderer& renderer,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void ApplyMotionBlur(
            EnvironmentRenderer& renderer,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& viewProjection,
            std::uint32_t sampleCount);
        void ApplyToneMapping(
            EnvironmentRenderer& renderer,
            const ColorGradingSettings& settings);
        void ApplyFXAA(EnvironmentRenderer& renderer);
        [[nodiscard]] bool ResolveAmbientOcclusion(
            EnvironmentRenderer& renderer,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void ApplyScreenEffect(
            ScreenEffect& effect,
            const std::array<ID3D11ShaderResourceView*, 2>&
                auxiliaryTextures,
            const DirectX::XMFLOAT4& depthParameters,
            const DirectX::XMFLOAT4& depthUnprojection,
            const std::array<DirectX::XMFLOAT4, 8>& parameters);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_shaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_postColorTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_postRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_postShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_displayColorTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_displayShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_displayUnorderedAccessView;
        bool m_computeWritable{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            m_depthStencilView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthShaderResourceView;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_occlusionTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_occlusionRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_occlusionShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_occlusionBlurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_occlusionBlurRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_occlusionBlurShaderResourceView;
        std::uint32_t m_occlusionWidth{};
        std::uint32_t m_occlusionHeight{};

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_streakTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_streakRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_streakShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_streakBlurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_streakBlurRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_streakBlurShaderResourceView;
        std::uint32_t m_streakWidth{};
        std::uint32_t m_streakHeight{};

        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthOfFieldTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_depthOfFieldRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthOfFieldShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthOfFieldBlurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_depthOfFieldBlurRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthOfFieldBlurShaderResourceView;
        std::uint32_t m_depthOfFieldWidth{};
        std::uint32_t m_depthOfFieldHeight{};

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_luminanceTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_luminanceRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_luminanceShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_luminanceStagingTexture;
        std::uint32_t m_luminanceWidth{};
        std::uint32_t m_luminanceHeight{};
        std::uint32_t m_luminanceMipLevels{};
        bool m_luminanceStagingReady{};
        D3D11_VIEWPORT m_viewport{};

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_historyTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_historyShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_temporalHistoryTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_temporalHistoryShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthCopyTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthCopyShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_reflectionDepthPyramidTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_reflectionDepthPyramidView;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>
            m_reflectionDepthPyramidTargets;
        std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
            m_reflectionDepthPyramidMipViews;

        // 同じ寸法でもBackendのDeviceが変わった場合は作り直します。
        Microsoft::WRL::ComPtr<ID3D11Device> m_ownerDevice;
    };
}
