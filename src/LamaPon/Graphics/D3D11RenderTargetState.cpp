#include "LamaPon/Graphics/D3D11RenderTargetState.h"
#include "LamaPon/Graphics/AutoExposureAdaptation.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/ScreenEffect.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
    // 失敗したHRESULTを例外に変える(result: 実行結果, operation: 操作名)。
    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }
}

namespace LamaPon
{
    ID3D11ShaderResourceView*
        Detail::D3D11RenderTargetState::ShaderResourceView() const noexcept
    {
        return m_shaderResourceView.Get();
    }

    ID3D11ShaderResourceView*
        Detail::D3D11RenderTargetState::DisplayShaderResourceView() const noexcept
    {
        return m_displayShaderResourceView.Get();
    }

    ID3D11ShaderResourceView*
        Detail::D3D11RenderTargetState::AmbientOcclusionShaderResourceView()
        const noexcept
    {
        return m_occlusionBlurShaderResourceView.Get();
    }

    ID3D11ShaderResourceView*
        Detail::D3D11RenderTargetState::ColorHistoryShaderResourceView()
        const noexcept
    {
        return m_historyValid
            ? m_historyShaderResourceView.Get()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        Detail::D3D11RenderTargetState::ReflectionDepthPyramidShaderResourceView()
        const noexcept
    {
        return m_reflectionDepthPyramidView.Get();
    }

    ID3D11ShaderResourceView*
        Detail::D3D11RenderTargetState::DepthShaderResourceView() const noexcept
    {
        return m_depthShaderResourceView.Get();
    }

    void Detail::D3D11RenderTargetState::Resize(
        ID3D11Device* device,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        // 一以上に補正した要求幅
        const std::uint32_t requestedWidth = std::max(width, 1u);
        // 一以上に補正した要求高さ
        const std::uint32_t requestedHeight = std::max(height, 1u);
        if (requestedWidth == m_width
            && requestedHeight == m_height
            && m_ownerDevice.Get() == device
            && IsValid())
        {
            return;
        }

        m_initialized = false;
        m_ownerDevice.Reset();
        m_currentColorView.Reset();
        m_postColorView.Reset();
        m_displayView.Reset();
        m_ambientOcclusionView.Reset();
        m_colorHistoryView.Reset();
        m_reflectionDepthPyramidViewHandle.Reset();
        m_depthView.Reset();
        m_temporalHistoryView.Reset();
        m_colorTexture.Reset();
        m_renderTargetView.Reset();
        m_shaderResourceView.Reset();
        m_postColorTexture.Reset();
        m_postRenderTargetView.Reset();
        m_postShaderResourceView.Reset();
        m_displayColorTexture.Reset();
        m_displayShaderResourceView.Reset();
        m_displayUnorderedAccessView.Reset();
        m_depthTexture.Reset();
        m_depthStencilView.Reset();
        m_depthShaderResourceView.Reset();
        m_occlusionTexture.Reset();
        m_occlusionRenderTargetView.Reset();
        m_occlusionShaderResourceView.Reset();
        m_occlusionBlurTexture.Reset();
        m_occlusionBlurRenderTargetView.Reset();
        m_occlusionBlurShaderResourceView.Reset();
        m_streakTexture.Reset();
        m_streakRenderTargetView.Reset();
        m_streakShaderResourceView.Reset();
        m_streakBlurTexture.Reset();
        m_streakBlurRenderTargetView.Reset();
        m_streakBlurShaderResourceView.Reset();
        m_depthOfFieldTexture.Reset();
        m_depthOfFieldRenderTargetView.Reset();
        m_depthOfFieldShaderResourceView.Reset();
        m_depthOfFieldBlurTexture.Reset();
        m_depthOfFieldBlurRenderTargetView.Reset();
        m_depthOfFieldBlurShaderResourceView.Reset();
        m_luminanceTexture.Reset();
        m_luminanceRenderTargetView.Reset();
        m_luminanceShaderResourceView.Reset();
        m_luminanceStagingTexture.Reset();
        m_luminanceMipLevels = 0;
        // 資源の再生成時は測定を破棄し、次の測定値から露出を開始する。
        m_luminanceStagingReady = false;
        m_adaptedLuminance = 0.0f;
        m_autoExposureStops = 0.0f;
        // サイズ変更前の行列による過剰なブラーを防ぐため、履歴を無効にする。
        m_motionBlurPreviousViewProjection = {};
        m_motionBlurPreviousValid = false;
        m_historyTexture.Reset();
        m_historyShaderResourceView.Reset();
        m_depthCopyTexture.Reset();
        m_depthCopyShaderResourceView.Reset();
        m_reflectionDepthPyramidTexture.Reset();
        m_reflectionDepthPyramidView.Reset();
        m_reflectionDepthPyramidTargets.clear();
        m_reflectionDepthPyramidMipViews.clear();
        m_reflectionDepthPyramidMipCount = 0;
        m_temporalHistoryTexture.Reset();
        m_temporalHistoryShaderResourceView.Reset();
        m_temporalHistoryValid = false;
        m_temporalHistoryViewProjection = {};
        // サイズ変更前のカラー履歴を無効にする。
        m_historyValid = false;

        m_width = requestedWidth;
        m_height = requestedHeight;
        m_occlusionWidth = std::max(m_width / 2u, 1u);
        m_occlusionHeight = std::max(m_height / 2u, 1u);
        // フレアの筋は四分の一解像度で生成する。
        m_streakWidth = std::max(m_width / 4u, 1u);
        m_streakHeight = std::max(m_height / 4u, 1u);
        // ぼけの縁の段差を抑えるため、被写界深度の作業資源は半解像度にする。
        m_depthOfFieldWidth = std::max(m_width / 2u, 1u);
        m_depthOfFieldHeight = std::max(m_height / 2u, 1u);
        // 輝度を四分の一解像度で測定し、ミップ生成で平均する。
        m_luminanceWidth = std::max(m_width / 4u, 1u);
        m_luminanceHeight = std::max(m_height / 4u, 1u);

        // HDRカラー資源の仕様
        D3D11_TEXTURE2D_DESC colorDescription{};
        colorDescription.Width = m_width;
        colorDescription.Height = m_height;
        colorDescription.MipLevels = 1;
        colorDescription.ArraySize = 1;
        // トーン変換まで一を超える色を保持するため、HDR形式を使う。
        colorDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        colorDescription.SampleDesc.Count = 1;
        colorDescription.Usage = D3D11_USAGE_DEFAULT;
        colorDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ThrowIfFailed(
            device->CreateTexture2D(
                &colorDescription,
                nullptr,
                m_colorTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(render target)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_colorTexture.Get(),
                nullptr,
                m_renderTargetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView(offscreen)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_colorTexture.Get(),
                nullptr,
                m_shaderResourceView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(offscreen)");
        // HDR履歴はコピーだけで更新するため、描画先のバインドフラグを付けない。
        // HDR色履歴の仕様
        D3D11_TEXTURE2D_DESC historyDescription =
            colorDescription;
        historyDescription.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(
            device->CreateTexture2D(
                &historyDescription,
                nullptr,
                m_historyTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(SSR history)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_historyTexture.Get(),
                nullptr,
                m_historyShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(SSR history)");


        ThrowIfFailed(
            device->CreateTexture2D(
                &historyDescription,
                nullptr,
                m_temporalHistoryTexture
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(TAA history)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_temporalHistoryTexture.Get(),
                nullptr,
                m_temporalHistoryShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(TAA history)");


        // 半解像度の遮蔽資源の仕様
        D3D11_TEXTURE2D_DESC occlusionDescription{};
        occlusionDescription.Width = m_occlusionWidth;
        occlusionDescription.Height = m_occlusionHeight;
        occlusionDescription.MipLevels = 1;
        occlusionDescription.ArraySize = 1;
        occlusionDescription.Format = DXGI_FORMAT_R8_UNORM;
        occlusionDescription.SampleDesc.Count = 1;
        occlusionDescription.Usage = D3D11_USAGE_DEFAULT;
        occlusionDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(
            device->CreateTexture2D(
                &occlusionDescription,
                nullptr,
                m_occlusionTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(SSAO)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_occlusionTexture.Get(),
                nullptr,
                m_occlusionRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView(SSAO)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_occlusionTexture.Get(),
                nullptr,
                m_occlusionShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(SSAO)");
        ThrowIfFailed(
            device->CreateTexture2D(
                &occlusionDescription,
                nullptr,
                m_occlusionBlurTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(SSAO blur)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_occlusionBlurTexture.Get(),
                nullptr,
                m_occlusionBlurRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView(SSAO blur)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_occlusionBlurTexture.Get(),
                nullptr,
                m_occlusionBlurShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(SSAO blur)");

        // フレアの筋はHDR形式の二つの作業資源を交互に使う。
        // フレアの筋資源の仕様
        D3D11_TEXTURE2D_DESC streakDescription{};
        streakDescription.Width = m_streakWidth;
        streakDescription.Height = m_streakHeight;
        streakDescription.MipLevels = 1;
        streakDescription.ArraySize = 1;
        streakDescription.Format =
            DXGI_FORMAT_R16G16B16A16_FLOAT;
        streakDescription.SampleDesc.Count = 1;
        streakDescription.Usage = D3D11_USAGE_DEFAULT;
        streakDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(
            device->CreateTexture2D(
                &streakDescription,
                nullptr,
                m_streakTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(lens flare streak)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_streakTexture.Get(),
                nullptr,
                m_streakRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView"
            "(lens flare streak)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_streakTexture.Get(),
                nullptr,
                m_streakShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(lens flare streak)");
        ThrowIfFailed(
            device->CreateTexture2D(
                &streakDescription,
                nullptr,
                m_streakBlurTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D"
            "(lens flare streak blur)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_streakBlurTexture.Get(),
                nullptr,
                m_streakBlurRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView"
            "(lens flare streak blur)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_streakBlurTexture.Get(),
                nullptr,
                m_streakBlurShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(lens flare streak blur)");

        // 符号付きの錯乱円半径をアルファへ持つため、RGBA16F形式を使う。
        // 被写界深度の作業資源仕様
        D3D11_TEXTURE2D_DESC depthOfFieldDescription{};
        depthOfFieldDescription.Width = m_depthOfFieldWidth;
        depthOfFieldDescription.Height = m_depthOfFieldHeight;
        depthOfFieldDescription.MipLevels = 1;
        depthOfFieldDescription.ArraySize = 1;
        depthOfFieldDescription.Format =
            DXGI_FORMAT_R16G16B16A16_FLOAT;
        depthOfFieldDescription.SampleDesc.Count = 1;
        depthOfFieldDescription.Usage = D3D11_USAGE_DEFAULT;
        depthOfFieldDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(
            device->CreateTexture2D(
                &depthOfFieldDescription,
                nullptr,
                m_depthOfFieldTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(depth of field)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_depthOfFieldTexture.Get(),
                nullptr,
                m_depthOfFieldRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView"
            "(depth of field)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_depthOfFieldTexture.Get(),
                nullptr,
                m_depthOfFieldShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(depth of field)");
        ThrowIfFailed(
            device->CreateTexture2D(
                &depthOfFieldDescription,
                nullptr,
                m_depthOfFieldBlurTexture
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D"
            "(depth of field blur)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_depthOfFieldBlurTexture.Get(),
                nullptr,
                m_depthOfFieldBlurRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView"
            "(depth of field blur)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_depthOfFieldBlurTexture.Get(),
                nullptr,
                m_depthOfFieldBlurShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(depth of field blur)");

        // 全ミップの自動生成で対数輝度を平均するため、RGBA16F形式を使う。
        // 対数輝度資源の仕様
        D3D11_TEXTURE2D_DESC luminanceDescription{};
        luminanceDescription.Width = m_luminanceWidth;
        luminanceDescription.Height = m_luminanceHeight;
        luminanceDescription.MipLevels = 0;
        luminanceDescription.ArraySize = 1;
        luminanceDescription.Format =
            DXGI_FORMAT_R16G16B16A16_FLOAT;
        luminanceDescription.SampleDesc.Count = 1;
        luminanceDescription.Usage = D3D11_USAGE_DEFAULT;
        luminanceDescription.BindFlags =
            D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE;
        luminanceDescription.MiscFlags =
            D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ThrowIfFailed(
            device->CreateTexture2D(
                &luminanceDescription,
                nullptr,
                m_luminanceTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(luminance)");
        // 非二冪の寸法にも対応するため、生成後の資源からミップ数を取得する。
        // 生成された輝度資源の仕様
        D3D11_TEXTURE2D_DESC createdLuminance{};
        m_luminanceTexture->GetDesc(&createdLuminance);
        m_luminanceMipLevels = createdLuminance.MipLevels;
        // 最初のミップだけへ描画し、残りはミップ生成で埋める。
        // 対数輝度の描画先仕様
        D3D11_RENDER_TARGET_VIEW_DESC luminanceTargetDescription{};
        luminanceTargetDescription.Format =
            luminanceDescription.Format;
        luminanceTargetDescription.ViewDimension =
            D3D11_RTV_DIMENSION_TEXTURE2D;
        luminanceTargetDescription.Texture2D.MipSlice = 0;
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_luminanceTexture.Get(),
                &luminanceTargetDescription,
                m_luminanceRenderTargetView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView(luminance)");
        // GenerateMipsが全段を埋められるよう、全ミップの参照を作る。
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_luminanceTexture.Get(),
                nullptr,
                m_luminanceShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(luminance)");

        // 一画素の輝度読取資源の仕様
        D3D11_TEXTURE2D_DESC luminanceStaging{};
        luminanceStaging.Width = 1;
        luminanceStaging.Height = 1;
        luminanceStaging.MipLevels = 1;
        luminanceStaging.ArraySize = 1;
        luminanceStaging.Format = luminanceDescription.Format;
        luminanceStaging.SampleDesc.Count = 1;
        luminanceStaging.Usage = D3D11_USAGE_STAGING;
        luminanceStaging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ThrowIfFailed(
            device->CreateTexture2D(
                &luminanceStaging,
                nullptr,
                m_luminanceStagingTexture
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(luminance staging)");

        ThrowIfFailed(
            device->CreateTexture2D(
                &colorDescription,
                nullptr,
                m_postColorTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(post process)");
        ThrowIfFailed(
            device->CreateRenderTargetView(
                m_postColorTexture.Get(),
                nullptr,
                m_postRenderTargetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView(post process)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_postColorTexture.Get(),
                nullptr,
                m_postShaderResourceView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(post process)");

        // 表示面をポスト処理の切替えから独立させ、必要な場合だけ計算書込みを許可する。
        // 表示面の資源仕様
        D3D11_TEXTURE2D_DESC displayDescription =
            colorDescription;
        if (m_computeWritable)
        {
            displayDescription.BindFlags |=
                D3D11_BIND_UNORDERED_ACCESS;
        }
        ThrowIfFailed(
            device->CreateTexture2D(
                &displayDescription,
                nullptr,
                m_displayColorTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(display)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_displayColorTexture.Get(),
                nullptr,
                m_displayShaderResourceView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(display)");
        if (m_computeWritable)
        {
            // 表示面の計算書込み参照仕様
            D3D11_UNORDERED_ACCESS_VIEW_DESC accessView{};
            accessView.Format = displayDescription.Format;
            accessView.ViewDimension =
                D3D11_UAV_DIMENSION_TEXTURE2D;
            ThrowIfFailed(
                device->CreateUnorderedAccessView(
                    m_displayColorTexture.Get(),
                    &accessView,
                    m_displayUnorderedAccessView
                        .ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateUnorderedAccessView"
                "(display)");
        }

        // 深度資源の仕様
        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = m_width;
        depthDescription.Height = m_height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        // 深度を描画と読取の両方に使うため、互換形式を持つTYPELESS資源で作る。
        depthDescription.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.Usage = D3D11_USAGE_DEFAULT;
        depthDescription.BindFlags =
            D3D11_BIND_DEPTH_STENCIL
            | D3D11_BIND_SHADER_RESOURCE;

        ThrowIfFailed(
            device->CreateTexture2D(
                &depthDescription,
                nullptr,
                m_depthTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(offscreen depth)");
        // 深度ステンシル描画先の仕様
        D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
        depthViewDescription.Format =
            DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthViewDescription.ViewDimension =
            D3D11_DSV_DIMENSION_TEXTURE2D;
        ThrowIfFailed(
            device->CreateDepthStencilView(
                m_depthTexture.Get(),
                &depthViewDescription,
                m_depthStencilView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilView(offscreen)");
        // 深度の描画と同時に読むSSRには、プリパス後に複製した資源を使う。
        // 複製深度資源の仕様
        D3D11_TEXTURE2D_DESC depthCopyDescription =
            depthDescription;
        depthCopyDescription.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(
            device->CreateTexture2D(
                &depthCopyDescription,
                nullptr,
                m_depthCopyTexture
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(SSR depth)");

        // 深度読取参照の仕様
        D3D11_SHADER_RESOURCE_VIEW_DESC depthResourceDescription{};
        depthResourceDescription.Format =
            DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthResourceDescription.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURE2D;
        depthResourceDescription.Texture2D.MipLevels = 1;
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_depthCopyTexture.Get(),
                &depthResourceDescription,
                m_depthCopyShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(SSR depth)");
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_depthTexture.Get(),
                &depthResourceDescription,
                m_depthShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(offscreen depth)");

        // Hi-Zにはカメラからの距離を持ち、各ミップは前段の二対二画素の最小値を使う。
        {
            // Hi-Zに必要なミップ段数
            std::uint32_t mipCount = 1;
            // ミップ計算中の辺長
            for (std::uint32_t size =
                    std::max(m_width, m_height);
                size > 1;
                size >>= 1)
            {
                ++mipCount;
            }
            // Hi-Z深度資源の仕様
            D3D11_TEXTURE2D_DESC pyramidDescription{};
            pyramidDescription.Width = m_width;
            pyramidDescription.Height = m_height;
            pyramidDescription.MipLevels = mipCount;
            pyramidDescription.ArraySize = 1;
            pyramidDescription.Format = DXGI_FORMAT_R32_FLOAT;
            pyramidDescription.SampleDesc.Count = 1;
            pyramidDescription.Usage = D3D11_USAGE_DEFAULT;
            pyramidDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            ThrowIfFailed(
                device->CreateTexture2D(
                    &pyramidDescription,
                    nullptr,
                    m_reflectionDepthPyramidTexture
                        .ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(hi-z pyramid)");
            ThrowIfFailed(
                device->CreateShaderResourceView(
                    m_reflectionDepthPyramidTexture.Get(),
                    nullptr,
                    m_reflectionDepthPyramidView
                        .ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView"
                "(hi-z pyramid)");
            m_reflectionDepthPyramidTargets.resize(mipCount);
            m_reflectionDepthPyramidMipViews.resize(mipCount);
            // 作成するミップ番号
            for (std::uint32_t mip = 0;
                // Hi-Zに必要なミップ段数
                mip < mipCount;
                ++mip)
            {
                // 各ミップの描画先仕様
                D3D11_RENDER_TARGET_VIEW_DESC
                    targetDescription{};
                targetDescription.Format =
                    DXGI_FORMAT_R32_FLOAT;
                targetDescription.ViewDimension =
                    D3D11_RTV_DIMENSION_TEXTURE2D;
                targetDescription.Texture2D.MipSlice = mip;
                ThrowIfFailed(
                    device->CreateRenderTargetView(
                        m_reflectionDepthPyramidTexture
                            .Get(),
                        &targetDescription,
                        m_reflectionDepthPyramidTargets[mip]
                            .ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreateRenderTargetView"
                    "(hi-z pyramid)");
                // 各ミップの読取参照仕様
                D3D11_SHADER_RESOURCE_VIEW_DESC
                    mipViewDescription{};
                mipViewDescription.Format =
                    DXGI_FORMAT_R32_FLOAT;
                mipViewDescription.ViewDimension =
                    D3D11_SRV_DIMENSION_TEXTURE2D;
                mipViewDescription.Texture2D
                    .MostDetailedMip = mip;
                mipViewDescription.Texture2D.MipLevels = 1;
                ThrowIfFailed(
                    device->CreateShaderResourceView(
                        m_reflectionDepthPyramidTexture
                            .Get(),
                        &mipViewDescription,
                        m_reflectionDepthPyramidMipViews[mip]
                            .ReleaseAndGetAddressOf()),
                    "ID3D11Device::CreateShaderResourceView"
                    "(hi-z pyramid mip)");
            }
        }

        m_viewport.TopLeftX = 0.0f;
        m_viewport.TopLeftY = 0.0f;
        m_viewport.Width = static_cast<float>(m_width);
        m_viewport.Height = static_cast<float>(m_height);
        m_viewport.MinDepth = 0.0f;
        m_viewport.MaxDepth = 1.0f;
        m_ownerDevice = device;
        m_reflectionDepthPyramidMipCount =
            static_cast<std::uint32_t>(
                m_reflectionDepthPyramidTargets.size());
        m_initialized = true;
    }

    void Detail::D3D11RenderTargetState::CaptureDepthForReflections(
        ID3D11DeviceContext* const context) const
    {
        if (context == nullptr
            || m_depthCopyTexture == nullptr
            || m_depthTexture == nullptr)
        {
            return;
        }
        context->CopyResource(
            m_depthCopyTexture.Get(),
            m_depthTexture.Get());
    }

    void Detail::D3D11RenderTargetState::CaptureColorHistory(
        ID3D11DeviceContext* const context,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (context == nullptr
            || m_historyTexture == nullptr
            || m_colorTexture == nullptr)
        {
            return;
        }

        context->CopyResource(
            m_historyTexture.Get(),
            m_colorTexture.Get());
        m_historyViewProjection = viewProjection;
        m_historyValid = true;
    }

    void Detail::D3D11RenderTargetState::CaptureTemporalHistory(
        ID3D11DeviceContext* const context,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (context == nullptr
            || m_temporalHistoryTexture == nullptr
            || m_colorTexture == nullptr)
        {
            return;
        }
        context->CopyResource(
            m_temporalHistoryTexture.Get(),
            m_colorTexture.Get());
        m_temporalHistoryViewProjection = viewProjection;
        m_temporalHistoryValid = true;
    }

    void Detail::D3D11RenderTargetState::CopyToDisplay(
        ID3D11DeviceContext* context) const
    {
        if (m_displayColorTexture != nullptr
            && m_colorTexture != nullptr)
        {
            context->CopyResource(
                m_displayColorTexture.Get(),
                m_colorTexture.Get());
        }
    }

    void Detail::D3D11RenderTargetState::Bind(ID3D11DeviceContext* context) const
    {
        // カラー読取の解除用参照
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        context->PSSetShaderResources(0, 1, nullResource);

        // 設定するカラー描画先
        ID3D11RenderTargetView* renderTargets[]{ m_renderTargetView.Get() };
        context->OMSetRenderTargets(1, renderTargets, m_depthStencilView.Get());
        context->RSSetViewports(1, &m_viewport);
    }

    void Detail::D3D11RenderTargetState::BindDepthOnly(
        ID3D11DeviceContext* context) const
    {
        // 深度の読取と書込が競合しないよう、Litが読むPSのt0〜t15を先に解除する。
        // 深度描画前の解除用参照列
        ID3D11ShaderResourceView* nullResources[16]{};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(std::size(nullResources)),
            nullResources);

        context->OMSetRenderTargets(
            0,
            nullptr,
            m_depthStencilView.Get());
        context->RSSetViewports(1, &m_viewport);
    }

    void Detail::D3D11RenderTargetState::Clear(
        ID3D11DeviceContext* context,
        const float color[4]) const
    {
        context->ClearRenderTargetView(m_renderTargetView.Get(), color);
        context->ClearDepthStencilView(
            m_depthStencilView.Get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            1.0f,
            0);
    }

    float Detail::D3D11RenderTargetState::AspectRatio() const noexcept
    {
        return static_cast<float>(m_width)
            / static_cast<float>(std::max(m_height, 1u));
    }

    void Detail::D3D11RenderTargetState::SwapPostProcessBuffers() noexcept
    {
        std::swap(m_colorTexture, m_postColorTexture);
        std::swap(m_renderTargetView, m_postRenderTargetView);
        std::swap(m_shaderResourceView, m_postShaderResourceView);
        std::swap(m_currentColorView, m_postColorView);
    }

    void Detail::D3D11RenderTargetState::ApplyBloom(
        EnvironmentRenderer& renderer,
        const BloomSettings& settings)
    {
        if (!IsValid() || !settings.enabled)
        {
            return;
        }
        renderer.ApplyBloom(
            m_shaderResourceView.Get(),
            m_postRenderTargetView.Get(),
            m_width,
            m_height,
            settings);
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyScreenOutline(
        EnvironmentRenderer& renderer,
        const ScreenOutlineSettings& settings,
        const DirectX::XMFLOAT4X4& projection)
    {
        if (!IsValid()
            || !settings.enabled
            || m_depthShaderResourceView == nullptr
            || std::abs(projection._11) < 1e-6f
            || std::abs(projection._22) < 1e-6f)
        {
            return;
        }
        renderer.ApplyScreenOutline(
            m_shaderResourceView.Get(),
            m_depthShaderResourceView.Get(),
            m_postRenderTargetView.Get(),
            m_width,
            m_height,
            settings,
            projection);
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyScreenSpaceLensFlare(
        EnvironmentRenderer& renderer,
        const ScreenSpaceLensFlareSettings& settings)
    {
        if (!IsValid() || !settings.enabled)
        {
            return;
        }
        // 四分の一解像度の二資源を交互に使い、筋の探索幅を広げる。
        renderer.BuildLensFlareStreaks(
            m_shaderResourceView.Get(),
            m_streakRenderTargetView.Get(),
            m_streakShaderResourceView.Get(),
            m_streakBlurRenderTargetView.Get(),
            m_streakBlurShaderResourceView.Get(),
            m_streakWidth,
            m_streakHeight,
            settings);
        renderer.ApplyScreenSpaceLensFlare(
            m_shaderResourceView.Get(),
            m_postRenderTargetView.Get(),
            m_width,
            m_height,
            settings,
            renderer.LastLensFlareStreakResource());
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyFXAA(
        EnvironmentRenderer& renderer)
    {
        if (!IsValid())
        {
            return;
        }
        renderer.ApplyFXAA(
            m_shaderResourceView.Get(),
            m_postRenderTargetView.Get(),
            m_width,
            m_height);
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyTemporalAntiAliasing(
        EnvironmentRenderer& renderer,
        const TemporalAntiAliasingSettings& settings,
        const EnvironmentRenderer::TemporalInputs& inputs)
    {
        if (!IsValid() || !settings.enabled)
        {
            return;
        }

        // ポスト処理前に深度の描画先を外し、この描画先の深度を読む。
        // 描画先の資源を補った入力
        auto resolved = inputs;
        resolved.depth = m_depthView;
        resolved.history = m_temporalHistoryValid
            ? m_temporalHistoryView
            : GraphicsViewHandle{};
        // 他のビューの履歴を混ぜないよう、この描画先の前回行列を使う。
        resolved.previousViewProjection =
            m_temporalHistoryViewProjection;
        resolved.previousValid = m_temporalHistoryValid;

        if (renderer.ApplyTemporalAntiAliasing(
                m_shaderResourceView.Get(),
                m_postRenderTargetView.Get(),
                m_width,
                m_height,
                settings,
                resolved))
        {
            SwapPostProcessBuffers();
        }
    }

    void Detail::D3D11RenderTargetState::ApplyVolumetricLight(
        EnvironmentRenderer& renderer,
        const VolumetricLightSettings& settings,
        const EnvironmentRenderer::VolumetricInputs&
            inputs)
    {
        if (!IsValid()
            || m_depthShaderResourceView == nullptr)
        {
            return;
        }

        // 描画先の資源を補った入力
        auto resolved = inputs;
        resolved.depth = m_depthView;
        if (!renderer.ApplyVolumetricLight(
                m_shaderResourceView.Get(),
                m_postRenderTargetView.Get(),
                m_width,
                m_height,
                settings,
                resolved))
        {
            return;
        }
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyDepthOfField(
        EnvironmentRenderer& renderer,
        const DepthOfFieldSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (!IsValid()
            || !settings.enabled
            || m_depthShaderResourceView == nullptr)
        {
            return;
        }

        // 描画先の作業資源を持つ入力
        EnvironmentRenderer::DepthOfFieldInputs inputs{};
        inputs.depth = m_depthShaderResourceView.Get();
        inputs.projection = projection;
        inputs.prepareTarget =
            m_depthOfFieldRenderTargetView.Get();
        inputs.prepareResource =
            m_depthOfFieldShaderResourceView.Get();
        inputs.blurTarget =
            m_depthOfFieldBlurRenderTargetView.Get();
        inputs.blurResource =
            m_depthOfFieldBlurShaderResourceView.Get();
        inputs.halfWidth = m_depthOfFieldWidth;
        inputs.halfHeight = m_depthOfFieldHeight;
        inputs.sampleCount = sampleCount;

        if (!renderer.ApplyDepthOfField(
                m_shaderResourceView.Get(),
                m_postRenderTargetView.Get(),
                m_width,
                m_height,
                settings,
                inputs))
        {
            return;
        }
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyMotionBlur(
        EnvironmentRenderer& renderer,
        const MotionBlurSettings& settings,
        const DirectX::XMFLOAT4X4& inverseViewProjection,
        const DirectX::XMFLOAT4X4& viewProjection,
        const std::uint32_t sampleCount)
    {
        if (!IsValid()
            || m_depthShaderResourceView == nullptr)
        {
            return;
        }
        if (!settings.enabled)
        {
            // 再有効化時の過剰なブラーを防ぐため、無効な間は行列の履歴を捨てる。
            m_motionBlurPreviousValid = false;
            return;
        }

        // 描画先の作業資源を持つ入力
        EnvironmentRenderer::MotionBlurInputs inputs{};
        inputs.depth = m_depthShaderResourceView.Get();
        inputs.inverseViewProjection = inverseViewProjection;
        inputs.previousViewProjection =
            m_motionBlurPreviousViewProjection;
        inputs.previousValid = m_motionBlurPreviousValid;
        inputs.sampleCount = sampleCount;

        if (renderer.ApplyMotionBlur(
                m_shaderResourceView.Get(),
                m_postRenderTargetView.Get(),
                m_width,
                m_height,
                settings,
                inputs))
        {
            SwapPostProcessBuffers();
        }

        // 効果を適用しなかった初回も行列を保存し、次のフレームで使う。
        m_motionBlurPreviousViewProjection = viewProjection;
        m_motionBlurPreviousValid = true;
    }

    float Detail::D3D11RenderTargetState::UpdateAutoExposure(
        EnvironmentRenderer& renderer,
        const std::optional<float> measuredLuminance,
        const AutoExposureSettings& settings,
        const float deltaSeconds)
    {
        if (!IsValid()
            || m_luminanceTexture == nullptr
            || m_luminanceStagingTexture == nullptr
            || m_luminanceMipLevels == 0)
        {
            return 0.0f;
        }
        if (!settings.enabled)
        {
            // 自動露出を無効にした場合は、読み残しと順応状態を破棄する。
            m_luminanceStagingReady = false;
            ResetAutoExposure(*this);
            return 0.0f;
        }


        // 順応後の露出補正段数
        const float exposureStops = AdvanceAutoExposure(
            *this,
            measuredLuminance,
            settings,
            deltaSeconds);

        // 輝度を描いた後に読取コピーを発行し、結果は後のフレームで読む。
        renderer.RenderLuminance(
            m_shaderResourceView.Get(),
            m_luminanceRenderTargetView.Get(),
            m_luminanceShaderResourceView.Get(),
            m_luminanceWidth,
            m_luminanceHeight);
        return exposureStops;
    }

    std::optional<float>
        Detail::D3D11RenderTargetState::TryReadAutoExposureLuminance(
            ID3D11DeviceContext* const context)
    {
        if (context == nullptr
            || !m_luminanceStagingReady
            || m_luminanceStagingTexture == nullptr)
        {
            return std::nullopt;
        }

        // CPU読取領域の情報
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // 輝度領域の読取結果
        const HRESULT mapResult = context->Map(
            m_luminanceStagingTexture.Get(),
            0,
            D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT,
            &mapped);
        if (FAILED(mapResult) || mapped.pData == nullptr)
        {
            return std::nullopt;
        }

        // 読み戻した半精度の値列
        const auto* const halfValues =
            static_cast<const DirectX::PackedVector::HALF*>(
                mapped.pData);
        // 平均対数輝度
        const float averageLogLuminance =
            DirectX::PackedVector::XMConvertHalfToFloat(
                halfValues[0]);
        context->Unmap(m_luminanceStagingTexture.Get(), 0);
        // 対数平均から線形輝度へ戻して、描画基盤の共通契約に合わせる。
        return std::exp(averageLogLuminance);
    }

    void Detail::D3D11RenderTargetState::CaptureAutoExposureLuminance(
        ID3D11DeviceContext* const context)
    {
        if (context == nullptr
            || m_luminanceTexture == nullptr
            || m_luminanceStagingTexture == nullptr
            || m_luminanceMipLevels == 0)
        {
            return;
        }

        // 一画素の最小ミップだけをCPU読取用の資源へコピーする。
        context->CopySubresourceRegion(
            m_luminanceStagingTexture.Get(),
            0,
            0,
            0,
            0,
            m_luminanceTexture.Get(),
            m_luminanceMipLevels - 1,
            nullptr);
        m_luminanceStagingReady = true;
    }

    bool Detail::D3D11RenderTargetState::ResolveAmbientOcclusion(
        EnvironmentRenderer& renderer,
        const AmbientOcclusionSettings& settings,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t sampleCount)
    {
        if (!IsValid()
            || !settings.enabled
            || m_depthShaderResourceView == nullptr
            || m_occlusionRenderTargetView == nullptr
            || m_occlusionBlurRenderTargetView == nullptr)
        {
            return false;
        }

        // 遮蔽の生成に失敗した場合は、遮蔽なしとして扱う。
        if (!renderer.RenderAmbientOcclusion(
            m_depthShaderResourceView.Get(),
            m_occlusionRenderTargetView.Get(),
            m_occlusionWidth,
            m_occlusionHeight,
            settings,
            projection,
            sampleCount))
        {
            return false;
        }

        // 深度を考慮して遮蔽を平滑化し、Litが環境光へ反映する。
        renderer.BlurAmbientOcclusion(
            m_occlusionShaderResourceView.Get(),
            m_depthShaderResourceView.Get(),
            m_occlusionBlurRenderTargetView.Get(),
            m_occlusionWidth,
            m_occlusionHeight);
        return true;
    }

    void Detail::D3D11RenderTargetState::ApplyScreenEffect(
        ScreenEffect& effect,
        const std::array<ID3D11ShaderResourceView*, 2>&
            auxiliaryTextures,
        const DirectX::XMFLOAT4& depthParameters,
        const DirectX::XMFLOAT4& depthUnprojection,
        const std::array<DirectX::XMFLOAT4, 8>&
            parameters)
    {
        if (!IsValid())
        {
            return;
        }
        // ポスト処理前に深度描画先を外し、深度を直接読む。
        effect.Apply(
            m_shaderResourceView.Get(),
            auxiliaryTextures,
            m_depthShaderResourceView.Get(),
            depthParameters,
            depthUnprojection,
            m_postRenderTargetView.Get(),
            m_width,
            m_height,
            parameters);
        SwapPostProcessBuffers();
    }

    void Detail::D3D11RenderTargetState::ApplyToneMapping(
        EnvironmentRenderer& renderer,
        const ColorGradingSettings& settings)
    {
        if (!IsValid())
        {
            return;
        }
        renderer.ApplyToneMapping(
            m_shaderResourceView.Get(),
            m_postRenderTargetView.Get(),
            m_width,
            m_height,
            settings);
        SwapPostProcessBuffers();
    }
}
