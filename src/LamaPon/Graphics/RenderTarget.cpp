#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/ScreenEffect.h"

// 自動露出がミップの1x1（RGBA16F）をCPUで読むため、
// half→floatの変換が必要です。
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
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
    void RenderTarget::Resize(
        ID3D11Device* device,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const std::uint32_t requestedWidth = std::max(width, 1u);
        const std::uint32_t requestedHeight = std::max(height, 1u);
        if (requestedWidth == m_width
            && requestedHeight == m_height
            && IsValid())
        {
            return;
        }

        m_colorTexture.Reset();
        m_renderTargetView.Reset();
        m_shaderResourceView.Reset();
        m_postColorTexture.Reset();
        m_postRenderTargetView.Reset();
        m_postShaderResourceView.Reset();
        m_displayColorTexture.Reset();
        m_displayShaderResourceView.Reset();
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
        // 大きさが変わったら測定中の値は捨てます。次に測れた値へ
        // そのまま飛ぶので、露出が数秒かけて追いつく必要はありません。
        m_luminanceStagingReady = false;
        m_adaptedLuminance = 0.0f;
        m_autoExposureStops = 0.0f;
        // 前フレームの行列も無効です（画面の大きさが変わった直後に
        // 使うと画面全体が伸びます）。
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
        m_temporalHistoryTexture.Reset();
        m_temporalHistoryShaderResourceView.Reset();
        m_temporalHistoryValid = false;
        m_temporalHistoryViewProjection = {};
        // 大きさが変わったら前フレームの絵は使えません。
        m_historyValid = false;

        m_width = requestedWidth;
        m_height = requestedHeight;
        m_occlusionWidth = std::max(m_width / 2u, 1u);
        m_occlusionHeight = std::max(m_height / 2u, 1u);
        // 筋は輪郭が要らないので1/4で十分です。ここを上げても
        // 見た目はほぼ変わらず、タップ数だけ増えます。
        m_streakWidth = std::max(m_width / 4u, 1u);
        m_streakHeight = std::max(m_height / 4u, 1u);
        // DoFのぼかしは半解像度。1/4まで落とすとぼけの縁が階段状に
        // 見えるので、ここはSSAOと同じ半分に留めます。
        m_depthOfFieldWidth = std::max(m_width / 2u, 1u);
        m_depthOfFieldHeight = std::max(m_height / 2u, 1u);
        // 明るさの測定は1/4解像度。以降の平均はミップ連鎖に任せるので、
        // ここを上げても結果はほぼ変わらず、転送量だけ増えます。
        m_luminanceWidth = std::max(m_width / 4u, 1u);
        m_luminanceHeight = std::max(m_height / 4u, 1u);

        D3D11_TEXTURE2D_DESC colorDescription{};
        colorDescription.Width = m_width;
        colorDescription.Height = m_height;
        colorDescription.MipLevels = 1;
        colorDescription.ArraySize = 1;
        // ブルームとトーンマッピングが終わるまで、1.0を超える値を保ちます。
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
        // SSR用に前フレームの色を保持します。描画先にはしないため、
        // D3D11_BIND_SHADER_RESOURCEだけを指定します。
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

        // TAAの履歴もSSRの履歴と同じくSRVだけを作成します。
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

        // SSAO用の半解像度バッファ（遮蔽率だけなので1チャンネル）。
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

        // レンズフレアの筋（1/4解像度のping-pong）。HDRのまま
        // 扱うので形式はカラーと同じです。
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

        // 被写界深度の作業用（半解像度）。アルファへ符号付きCoCを
        // 入れるので、形式はカラーと同じRGBA16Fです。
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

        // 自動露出の明るさ測定（1/4解像度）。MipLevels=0で全ミップを
        // 作らせ、GENERATE_MIPSを付けてGenerateMipsで平均させます。
        // 形式をカラーと同じRGBA16Fにしているのは、ミップの自動生成が
        // 確実に使える形式に揃えるためです（1チャンネル形式は環境に
        // よって自動生成に対応しません）。
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
        // 実際に作られたミップ数を控えます。1/4解像度は2のべき乗とは
        // 限らないので、自分で計算せずデバイスに聞くのが確実です。
        D3D11_TEXTURE2D_DESC createdLuminance{};
        m_luminanceTexture->GetDesc(&createdLuminance);
        m_luminanceMipLevels = createdLuminance.MipLevels;
        // 描画先はミップ0だけです（以降はGenerateMipsが埋めます）。
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
        // SRVは全ミップを見るものにします。GenerateMipsはこの
        // ビューの範囲しか埋めないので、ミップ0だけのビューを渡すと
        // 何も起きません。
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_luminanceTexture.Get(),
                nullptr,
                m_luminanceShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(luminance)");
        // 1x1ミップをCPUへ渡すためのSTAGING。読むのは4チャンネル
        // ぶんの8バイトだけです。
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

        // 表示専用テクスチャ。ポスト処理のswapに左右されない
        // 安定したSRVをImGui等へ渡すために使います。
        //
        // Compute Shaderの書き込み先にするときだけUAVフラグを追加します。
        // 通常の描画先には不要なバインドフラグを付けず、ドライバーの
        // 最適化を妨げないようにします。
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
        m_displayUnorderedAccessView.Reset();
        if (m_computeWritable)
        {
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

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = m_width;
        depthDescription.Height = m_height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        // SSAOがシェーダーから深度を読むため、TYPELESSで作って
        // 深度ビューとシェーダービューの両方を張ります。
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
        // SSR用の深度コピー。Litパス中は深度がDSVとして刺さって
        // いるため、同じリソースをSRVとしても読むことはできません
        // （D3D11がSRVを黙ってnullにします）。プリパスの直後に
        // ここへ複製して、読む側はこちらを見ます。
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

        // SSRのHi-Z用の深度ピラミッド。深度を「カメラからの距離」へ
        // 直した値を全ミップで持ち、各ミップは2x2の最小値です。
        // レイは「この区画の最も手前よりレイ全体が手前」なら区画ごと
        // 飛ばせるので、何も無い空間を大股で越えられます。
        {
            std::uint32_t mipCount = 1;
            for (std::uint32_t size =
                    std::max(m_width, m_height);
                size > 1;
                size >>= 1)
            {
                ++mipCount;
            }
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
            for (std::uint32_t mip = 0;
                mip < mipCount;
                ++mip)
            {
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
    }

    void RenderTarget::CaptureDepthForReflections(
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

    void RenderTarget::CaptureColorHistory(
        ID3D11DeviceContext* const context,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (context == nullptr
            || m_historyTexture == nullptr
            || m_colorTexture == nullptr)
        {
            return;
        }
        // 同じ形式・同じ大きさなので丸ごとコピーで済みます。
        context->CopyResource(
            m_historyTexture.Get(),
            m_colorTexture.Get());
        m_historyViewProjection = viewProjection;
        m_historyValid = true;
    }

    void RenderTarget::CopyToDisplay(
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

    void RenderTarget::Bind(ID3D11DeviceContext* context) const
    {
        ID3D11ShaderResourceView* nullResource[]{ nullptr };
        context->PSSetShaderResources(0, 1, nullResource);

        ID3D11RenderTargetView* renderTargets[]{ m_renderTargetView.Get() };
        context->OMSetRenderTargets(1, renderTargets, m_depthStencilView.Get());
        context->RSSetViewports(1, &m_viewport);
    }

    void RenderTarget::BindDepthOnly(
        ID3D11DeviceContext* context) const
    {
        // 深度とSSAOのテクスチャを読んだままにしていると、この後の
        // プリパスとSSAOでそれらを描画先にできません（同じリソースの
        // 読みと書きは同時にできず、D3D11が黙って読み側を外します）。
        // Litシェーダーが使うt0〜t15をまとめて外しておきます。
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

    void RenderTarget::Clear(
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

    float RenderTarget::AspectRatio() const noexcept
    {
        return static_cast<float>(m_width)
            / static_cast<float>(std::max(m_height, 1u));
    }

    void RenderTarget::ApplyBloom(
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyScreenOutline(
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyScreenSpaceLensFlare(
        EnvironmentRenderer& renderer,
        const ScreenSpaceLensFlareSettings& settings)
    {
        if (!IsValid() || !settings.enabled)
        {
            return;
        }
        // 先に1/4解像度で筋を作ります。ストライドを4倍ずつ広げて
        // 3回書き戻すので、ping-pongで受け渡します。
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyFXAA(
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyTemporalAntiAliasing(
        EnvironmentRenderer& renderer,
        ID3D11DeviceContext* const context,
        const TemporalAntiAliasingSettings& settings,
        const EnvironmentRenderer::TemporalInputs& inputs)
    {
        if (!IsValid() || !settings.enabled)
        {
            return;
        }

        // 深度はこのターゲットのものを使います。ポスト処理の時点では
        // 深度が描画先として外れているので、そのまま読めます。
        auto resolved = inputs;
        resolved.depth = m_depthShaderResourceView.Get();
        resolved.history = m_temporalHistoryValid
            ? m_temporalHistoryShaderResourceView.Get()
            : nullptr;
        // 前フレームの行列はこのビューが自分で覚えているものを
        // 使います（ビューをまたいで共有すると壊れます）。
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
            std::swap(m_colorTexture, m_postColorTexture);
            std::swap(
                m_renderTargetView,
                m_postRenderTargetView);
            std::swap(
                m_shaderResourceView,
                m_postShaderResourceView);
        }

        // 解決した画像を次フレームの履歴へ保存します。
        // 初回も現在の画像を保存し、次フレームから履歴を利用できる
        // 状態にします。
        if (context != nullptr
            && m_temporalHistoryTexture != nullptr
            && m_colorTexture != nullptr)
        {
            context->CopyResource(
                m_temporalHistoryTexture.Get(),
                m_colorTexture.Get());
            m_temporalHistoryViewProjection =
                inputs.viewProjection;
            m_temporalHistoryValid = true;
        }
    }

    void RenderTarget::ApplyVolumetricLight(
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
        // 深度はこのターゲットが持っているものを使います
        // （呼ぶ側が知らなくて良いように、ここで差し込みます）。
        auto resolved = inputs;
        resolved.depth = m_depthShaderResourceView.Get();
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyDepthOfField(
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
        // 深度と作業用テクスチャはこのターゲットが持っているものを
        // 使います（呼ぶ側が知らなくて良いように、ここで差し込みます）。
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyMotionBlur(
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
            // 切っている間に控え続けると、入れ直した最初のフレームで
            // 「何十フレームぶんも動いた」ことになって画面全体が
            // 一瞬伸びます。無効にします。
            m_motionBlurPreviousValid = false;
            return;
        }

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
            std::swap(m_colorTexture, m_postColorTexture);
            std::swap(
                m_renderTargetView,
                m_postRenderTargetView);
            std::swap(
                m_shaderResourceView,
                m_postShaderResourceView);
        }

        // エフェクトを適用しなかった初回も行列を保存し、
        // 次フレームのモーション判定に使える状態にします。
        m_motionBlurPreviousViewProjection = viewProjection;
        m_motionBlurPreviousValid = true;
    }

    float RenderTarget::UpdateAutoExposure(
        EnvironmentRenderer& renderer,
        ID3D11DeviceContext* const context,
        const AutoExposureSettings& settings,
        const float deltaSeconds)
    {
        if (!IsValid()
            || context == nullptr
            || m_luminanceTexture == nullptr
            || m_luminanceStagingTexture == nullptr
            || m_luminanceMipLevels == 0)
        {
            return 0.0f;
        }
        if (!settings.enabled)
        {
            // 順応をやめたら状態も捨てます。入れ直したときは、最初に
            // 測れた明るさへそのまま飛ばしたいためです（何秒も前の
            // 値から追いつかせると暗転や白飛びから始まります）。
            m_luminanceStagingReady = false;
            m_adaptedLuminance = 0.0f;
            m_autoExposureStops = 0.0f;
            return 0.0f;
        }

        const float minimumLuminance = std::max(
            settings.minimumLuminance,
            0.0001f);
        const float maximumLuminance = std::max(
            settings.maximumLuminance,
            minimumLuminance);

        // 前フレームの測定結果を読みます。GPUの完了を待たず、結果が
        // 未完成ならそのフレームの更新を見送ります。
        if (m_luminanceStagingReady)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mapResult = context->Map(
                m_luminanceStagingTexture.Get(),
                0,
                D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT,
                &mapped);
            if (SUCCEEDED(mapResult) && mapped.pData != nullptr)
            {
                const auto* const halfValues =
                    static_cast<
                        const DirectX::PackedVector::HALF*>(
                            mapped.pData);
                const float averageLogLuminance =
                    DirectX::PackedVector::XMConvertHalfToFloat(
                        halfValues[0]);
                context->Unmap(
                    m_luminanceStagingTexture.Get(),
                    0);

                // 対数平均なので戻してから使います。
                const float measured = std::clamp(
                    std::exp(averageLogLuminance),
                    minimumLuminance,
                    maximumLuminance);
                if (m_adaptedLuminance <= 0.0f)
                {
                    // 初回は測定値を直接採用し、起動直後の
                    // 不要な露出変化を避けます。
                    m_adaptedLuminance = measured;
                }
                else
                {
                    // 明所と暗所で異なる順応速度を適用します。
                    const float speed = measured > m_adaptedLuminance
                        ? std::max(settings.speedToBright, 0.0f)
                        : std::max(settings.speedToDark, 0.0f);
                    // 指数補間により順応時間をフレームレートから分離し、
                    // 大きなdeltaTimeでも行き過ぎを防ぎます。
                    const float blend = speed > 0.0f
                        ? 1.0f - std::exp(
                            -std::max(deltaSeconds, 0.0f) * speed)
                        : 0.0f;
                    m_adaptedLuminance +=
                        (measured - m_adaptedLuminance) * blend;
                }
                // 露出は段数（exp2で効く）なのでlog2で渡します。
                m_autoExposureStops = std::log2(
                    std::max(settings.keyValue, 0.0001f)
                    / std::max(m_adaptedLuminance, 0.0001f));
            }
        }

        // 現在のフレームを測定し、結果を次のフレームで読みます。
        renderer.RenderLuminance(
            m_shaderResourceView.Get(),
            m_luminanceRenderTargetView.Get(),
            m_luminanceShaderResourceView.Get(),
            m_luminanceWidth,
            m_luminanceHeight);
        // いちばん小さいミップ（1x1）だけをCPUの読める場所へ移します。
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
        return m_autoExposureStops;
    }

    bool RenderTarget::ResolveAmbientOcclusion(
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

        // (1)半解像度で遮蔽を求めます。射影が復元不能な場合などは
        // falseが返るので、遮蔽なしとして扱います。
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

        // (2)深度を見るブラーでザラつきを均します。ここまでで完成で、
        // カラーへの反映はLitシェーダーが環境光項に対して行います。
        renderer.BlurAmbientOcclusion(
            m_occlusionShaderResourceView.Get(),
            m_depthShaderResourceView.Get(),
            m_occlusionBlurRenderTargetView.Get(),
            m_occlusionWidth,
            m_occlusionHeight);
        return true;
    }

    void RenderTarget::ApplyScreenEffect(
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
        // ここはポスト処理なのでDSVは外れています。深度をコピー
        // せずそのまま読めるのはそのためです。
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }

    void RenderTarget::ApplyToneMapping(
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
        std::swap(
            m_colorTexture,
            m_postColorTexture);
        std::swap(
            m_renderTargetView,
            m_postRenderTargetView);
        std::swap(
            m_shaderResourceView,
            m_postShaderResourceView);
    }
}
