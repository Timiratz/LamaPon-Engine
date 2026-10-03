#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/D3D12EnvironmentPrefilter.h"
#include "LamaPon/Graphics/D3D12SpriteRenderer.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Resources.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderRenderState.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <CommonStates.h>
#include <SpriteBatch.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

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

    // D3D11基盤へ変換し、不一致なら空を返す(backend: 描画基盤)。
    [[nodiscard]] LamaPon::D3D11Backend*
        AsD3D11Backend(
            LamaPon::GraphicsBackend* const backend) noexcept
    {
        return dynamic_cast<LamaPon::D3D11Backend*>(backend);
    }

    // 描画可能なキューブ参照か調べる(device: 描画デバイス, view: 確認する参照, expectedFormat: 必須形式、UNKNOWNは任意, expectedSize: 形式指定時の辺長, expectedMipLevels: 形式指定時の段数)。
    [[nodiscard]] bool IsCubeShaderResource(
        ID3D11Device* const device,
        ID3D11ShaderResourceView* const view,
        const DXGI_FORMAT expectedFormat = DXGI_FORMAT_UNKNOWN,
        const std::uint32_t expectedSize = 0,
        const std::uint32_t expectedMipLevels = 0) noexcept
    {
        if (device == nullptr || view == nullptr)
        {
            return false;
        }

        // キューブ参照の仕様
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        view->GetDesc(&viewDescription);
        if (viewDescription.ViewDimension
                != D3D11_SRV_DIMENSION_TEXTURECUBE
            || viewDescription.TextureCube.MostDetailedMip != 0
            || viewDescription.TextureCube.MipLevels == 0)
        {
            return false;
        }

        // 参照元の描画資源
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.ReleaseAndGetAddressOf());
        // 参照元の二次元テクスチャ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource == nullptr || FAILED(resource.As(&texture)))
        {
            return false;
        }
        // テクスチャの仕様
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        // 参照するミップ段数
        auto viewMipLevels =
            viewDescription.TextureCube.MipLevels;
        if (viewMipLevels == std::numeric_limits<UINT>::max())
        {
            viewMipLevels = description.MipLevels;
        }
        if (description.Width == 0
            || description.Width != description.Height
            || description.ArraySize != 6
            || description.MipLevels == 0
            || viewMipLevels > description.MipLevels
            || description.SampleDesc.Count != 1
            || (description.MiscFlags
                & D3D11_RESOURCE_MISC_TEXTURECUBE) == 0
            || (description.BindFlags
                & D3D11_BIND_SHADER_RESOURCE) == 0)
        {
            return false;
        }

        if (expectedFormat != DXGI_FORMAT_UNKNOWN)
        {
            return viewDescription.Format == expectedFormat
                && description.Format == expectedFormat
                && description.Width == expectedSize
                && description.MipLevels == expectedMipLevels
                && viewMipLevels == expectedMipLevels;
        }

        // 形式が対応する用途
        UINT formatSupport{};
        // 必要な形式対応用途
        constexpr UINT RequiredFormatSupport =
            D3D11_FORMAT_SUPPORT_TEXTURECUBE
            | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        return viewDescription.Format != DXGI_FORMAT_UNKNOWN
            && SUCCEEDED(device->CheckFormatSupport(
                viewDescription.Format,
                &formatSupport))
            && (formatSupport & RequiredFormatSupport)
                == RequiredFormatSupport;
    }

    // 規定形式の環境マップを取り込む(backend: 描画基盤, native: 鏡面・拡散の参照)。
    [[nodiscard]] LamaPon::PrefilteredEnvironmentViews
        ImportPrefilteredEnvironmentViews(
            LamaPon::D3D11Backend& backend,
            const LamaPon::EnvironmentRenderer::
                OwnedPrefilteredEnvironment& native)
    {
        // 規定の最大ミップ番号
        constexpr auto ExpectedMaximumMip = static_cast<float>(
            LamaPon::EnvironmentRenderer::
                PrefilteredSpecularMipLevels - 1);
        if (!IsCubeShaderResource(
                backend.Device(),
                native.specular.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                LamaPon::EnvironmentRenderer::PrefilteredSpecularSize,
                LamaPon::EnvironmentRenderer::
                    PrefilteredSpecularMipLevels)
            || !IsCubeShaderResource(
                backend.Device(),
                native.irradiance.Get(),
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                LamaPon::EnvironmentRenderer::
                    PrefilteredIrradianceSize,
                LamaPon::EnvironmentRenderer::
                    PrefilteredIrradianceMipLevels)
            || native.specularMaximumMip != ExpectedMaximumMip)
        {
            return {};
        }

        // 鏡面・拡散の両方の参照を取り込んでから結果を公開する。
        // 取り込む鏡面反射参照
        auto specular = backend.ImportShaderResourceViewHandle(
            native.specular.Get());
        // 取り込む拡散反射参照
        auto irradiance = backend.ImportShaderResourceViewHandle(
            native.irradiance.Get());
        return {
            std::move(specular),
            std::move(irradiance),
            native.specularMaximumMip
        };
    }
}

namespace LamaPon::Detail
{
    // 画像描画とUI切抜き用資源を作る(device: 描画デバイス, context: 描画コンテキスト)。
    GraphicsDeviceD3D11Resources::GraphicsDeviceD3D11Resources(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context)
        : spriteBatch(
            std::make_unique<DirectX::SpriteBatch>(context))
        , commonStates(
            std::make_unique<DirectX::CommonStates>(device))
    {

        // UI切抜き用描画状態の仕様
        D3D11_RASTERIZER_DESC scissorDescription{};
        scissorDescription.FillMode =
            D3D11_FILL_SOLID;
        scissorDescription.CullMode = D3D11_CULL_NONE;
        scissorDescription.DepthClipEnable = TRUE;
        scissorDescription.ScissorEnable = TRUE;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &scissorDescription,
                uiScissorRasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState");
    }

    // ワーカーの終了を待ち、描画資源を解放する。
    GraphicsDeviceD3D11Resources::~GraphicsDeviceD3D11Resources()
    {
        Reset();
    }

    // 高水準資源を先に、続いて描画サービスとネイティブ資源を解放する。
    void GraphicsDeviceD3D11Resources::Reset() noexcept
    {
        ResetHighLevelResources();
        // 描画サービスをネイティブ描画状態より先に解放する。
        renderServices.reset();
        spriteShaderCallback = {};
        spriteBatchOwner = D3D11SpriteBatchOwner::None;
        spriteBatchToken = 0;
        spriteBatchNativeBegun = false;
        spriteBlendState = nullptr;
        skyPrefilteredSpecular.Reset();
        skyPrefilteredIrradiance.Reset();
        skyPrefilteredMaximumMip = 0.0f;
        additiveBlendPreservingAlpha.Reset();
        uiScissorRasterizer.Reset();
        uiScissorStack.clear();
        spriteViewPins.clear();
        spriteTexturePins.clear();
        commonStates.reset();
        spriteBatch.reset();
    }

    // 材質シェーダーの非同期コンパイルが終了するまで待つ。
    void GraphicsDeviceD3D11Resources::QuiesceResourceWork() noexcept
    {
        // 非同期コンパイルの終了を待つ(entries: シェーダーキャッシュ)。
        const auto waitForShaders = [](auto& entries) noexcept
        {
            // path: キャッシュの識別名、entry: コンパイル状態
            for (auto& [path, entry] : entries)
            {
                static_cast<void>(path);
                if (!entry || !entry->warming.valid())
                {
                    continue;
                }
                try
                {
                    entry->warming.wait();
                }
                catch (...)
                {
                    // 終了処理では待機の例外を外へ伝播させない。
                }
            }
        };
        waitForShaders(materialShaders);
        waitForShaders(skinnedMaterialShaders);
    }

    // ワーカーの終了を待ち、効果・キュー・保持参照を解放する。
    void GraphicsDeviceD3D11Resources::
        ResetHighLevelResources() noexcept
    {
        QuiesceResourceWork();
        // コールバックと保持参照を効果の所有元より先に解放する。
        spriteShaderCallback = {};
        spriteViewPins.clear();
        spriteTexturePins.clear();
        // キューが保持する効果の世代をキャッシュより先に解放する。
        queuedScreenEffects.clear();
        shadowMap.reset();
        spotShadowMap.reset();
        pointShadowMap.reset();
        skinnedMaterialShaders.clear();
        materialShaders.clear();
        spriteShaders.clear();
        screenShaders.clear();
        computeShaders.clear();
        litEffect.reset();
        skinnedLitEffect.reset();
        errorEffect.reset();
        skinnedErrorEffect.reset();
        spriteErrorEffect.reset();
        errorEffectUnavailable = false;
        skinnedErrorEffectUnavailable = false;
        spriteErrorEffectUnavailable = false;
        litFailure = {};
        skinnedLitFailure = {};
        environmentFailure = {};
        environmentRenderer.reset();
        skyPrefilteredSpecular.Reset();
        skyPrefilteredIrradiance.Reset();
        skyPrefilteredMaximumMip = 0.0f;
    }

    // 借用する描画サービスを返し、未生成なら空を返す。
    GraphicsRenderServices*
        GraphicsDeviceD3D11Resources::TryRenderServices() noexcept
    {
        return renderServices.get();
    }

    // 既存の影資源を置き換えて初期化する(backend: 描画基盤, settings: 影の品質設定)。
    void GraphicsDeviceD3D11Resources::RecreateShadowMaps(
        GraphicsBackend& backend,
        const GraphicsSettings& settings)
    {
        shadowMap = std::make_unique<ShadowMap>();
        spotShadowMap = std::make_unique<ShadowMap>();
        pointShadowMap = std::make_unique<ShadowMap>();
        if (!settings.shadowsEnabled)
        {
            return;
        }

        backend.InitializeShadowMap(
            *shadowMap,
            settings.shadowResolution,
            settings.shadowCascadeLimit,
            false);
        // 点・スポット影の解像度
        const std::uint32_t localShadowResolution = std::max(
            settings.shadowResolution / 2u,
            256u);
        backend.InitializeShadowMap(
            *spotShadowMap,
            localShadowResolution,
            static_cast<std::uint32_t>(MaximumSpotShadows),
            false);
        backend.InitializeShadowMap(
            *pointShadowMap,
            localShadowResolution,
            6u,
            true);
    }

    // 借用する平行光源の影資源を返す。
    ShadowMap* GraphicsDeviceD3D11Resources::
        TryDirectionalShadowMap() const noexcept
    {
        return shadowMap.get();
    }

    // 借用するスポット光源の影資源を返す。
    ShadowMap* GraphicsDeviceD3D11Resources::
        TrySpotShadowMap() const noexcept
    {
        return spotShadowMap.get();
    }

    // 借用する点光源の影資源を返す。
    ShadowMap* GraphicsDeviceD3D11Resources::
        TryPointShadowMap() const noexcept
    {
        return pointShadowMap.get();
    }

    // D3D11固有資源を生成する(device: 描画デバイス, context: 描画コンテキスト, backend: 寿命が長い基盤)。
    std::unique_ptr<GraphicsDeviceApiResources>
        CreateD3D11GraphicsDeviceApiResources(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context,
            GraphicsBackend& backend)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "DirectX 11 API resources require an initialized device "
                "and context.");
        }

        // API固有の描画資源
        auto resources =
            std::make_unique<GraphicsDeviceD3D11Resources>(
                device,
                context);
        resources->renderServices =
            CreateD3D11GraphicsRenderServices(
                device,
                context,
                backend);
        return resources;
    }
}

namespace LamaPon
{
    // 稼働中の基盤に対応するAPI資源を生成する(activeApi: 稼働中の描画API)。
    void GraphicsDevice::CreateApiResources(
        const RenderingApi activeApi)
    {
        if (m_state->m_apiResources != nullptr)
        {
            throw std::logic_error(
                "Graphics API resources are already initialized.");
        }
        if (m_state->m_backend == nullptr
            || m_state->m_backend->Api() != activeApi)
        {
            throw std::logic_error(
                "Graphics API resources require the active backend.");
        }

        // 生成中のAPI固有資源
        std::unique_ptr<Detail::GraphicsDeviceApiResources>
            resources;
        switch (activeApi)
        {
        case RenderingApi::DirectX11:
            resources =
                Detail::CreateD3D11GraphicsDeviceApiResources(
                    Device(),
                    Context(),
                    *m_state->m_backend);
            break;
        case RenderingApi::DirectX12Experimental:
            resources =
                Detail::CreateD3D12GraphicsDeviceApiResources(
                    *m_state->m_backend);
            break;
        case RenderingApi::Auto:
        default:
            throw std::logic_error(
                "API resources for the active rendering API are not "
                "implemented.");
        }
        if (resources == nullptr || resources->Api() != activeApi)
        {
            throw std::logic_error(
                "The graphics API resource factory returned an "
                "incompatible resource owner.");
        }

        // 生成とAPI整合性の確認が成功してから資源を公開する。
        m_state->m_apiResources = std::move(resources);
    }

    // API固有資源を解放して所有元を空にする。
    void GraphicsDevice::ResetApiResources() noexcept
    {
        if (m_state->m_apiResources)
        {
            m_state->m_apiResources->Reset();
            m_state->m_apiResources.reset();
        }
    }

    // 現在のD3D11資源を借用し、不一致や未生成なら空を返す。
    Detail::GraphicsDeviceD3D11Resources*
        GraphicsDevice::TryD3D11ApiResources() const noexcept
    {
        if (m_state->m_backend == nullptr
            || m_state->m_backend->Api() != RenderingApi::DirectX11
            || m_state->m_apiResources == nullptr
            || m_state->m_apiResources->Api()
                != RenderingApi::DirectX11)
        {
            return nullptr;
        }
        return dynamic_cast<Detail::GraphicsDeviceD3D11Resources*>(
            m_state->m_apiResources.get());
    }

    // D3D11資源を借用し、未生成なら例外を送出する。
    Detail::GraphicsDeviceD3D11Resources&
        GraphicsDevice::RequireD3D11ApiResources()
    {
        // API固有の描画資源
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            throw std::logic_error(
                "DirectX 11 API resources are not initialized.");
        }
        return *resources;
    }

    // D3D11資源を借用し、未生成なら例外を送出する。
    const Detail::GraphicsDeviceD3D11Resources&
        GraphicsDevice::RequireD3D11ApiResources() const
    {
        // API固有の描画資源
        const auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            throw std::logic_error(
                "DirectX 11 API resources are not initialized.");
        }
        return *resources;
    }

    // 借用するD3D11デバイスを返し、他のAPIなら空を返す。
    ID3D11Device* GraphicsDevice::Device() const noexcept
    {
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        return backend != nullptr
            ? backend->Device()
            : nullptr;
    }

    // 借用するD3D11コンテキストを返し、他のAPIなら空を返す。
    ID3D11DeviceContext* GraphicsDevice::Context() const noexcept
    {
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        return backend != nullptr
            ? backend->Context()
            : nullptr;
    }

    // 現在の基盤で描画可能なキューブか調べる(cubemap: 確認する参照)。
    bool GraphicsDevice::IsSampleableCubeView(
        const GraphicsViewHandle& cubemap) const noexcept
    {
        // D3D12の描画基盤
        if (const auto* const d3d12 = dynamic_cast<const D3D12Backend*>(
                m_state->m_backend.get());
            d3d12 != nullptr)
        {
            // 現在の基盤の世代に属する正方形のキューブ参照を確認する。
            // 解決したシェーダー参照
            const auto binding = d3d12->TryResolveShaderResource(cubemap);
            return binding.has_value()
                && binding->dimension == D3D12_SRV_DIMENSION_TEXTURECUBE
                && binding->width != 0u
                && binding->width == binding->height;
        }
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        if (!cubemap || backend == nullptr)
        {
            return false;
        }
        try
        {
            return IsCubeShaderResource(
                backend->Device(),
                backend->ResolveShaderResourceView(cubemap));
        }
        catch (...)
        {
            return false;
        }
    }

    // 有効なキューブまたはグラデーションの空を描く(view: ビュー行列, projection: 射影行列, settings: 空の設定, cubemap: 任意のキューブ参照, sun: 任意の太陽設定)。
    void GraphicsDevice::DrawSky(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const SkySettings& settings,
        const GraphicsViewHandle& cubemap,
        const SkySunDescription* const sun) const
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // キューブ参照が無効ならグラデーションの空へ戻す。
            // API固有の描画資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // D3D12の画像描画器
            auto* const renderer = resources != nullptr
                ? resources->TrySpriteRenderer()
                : nullptr;
            if (renderer == nullptr)
            {
                throw std::logic_error(
                    "Sky rendering requires an active backend.");
            }
            renderer->DrawSky(
                view,
                projection,
                settings,
                cubemap,
                sun,
                m_state->m_whiteTextureView);
            return;
        }
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr || backend->Device() == nullptr)
        {
            throw std::logic_error(
                "Sky rendering requires an active backend.");
        }

        // 描画するキューブの参照
        ID3D11ShaderResourceView* nativeCubemap{};
        if (cubemap)
        {
            try
            {
                // 解決したキューブ参照
                auto* const resolved =
                    backend->ResolveShaderResourceView(cubemap);
                if (IsCubeShaderResource(
                        backend->Device(),
                        resolved))
                {
                    nativeCubemap = resolved;
                }
            }
            catch (...)
            {
                // 古い世代や他の基盤の参照は手続き生成の空へ戻す。
            }
        }

        // 空描画用の太陽設定
        EnvironmentRenderer::SkySun nativeSun{};
        // 任意の太陽設定参照
        const EnvironmentRenderer::SkySun* nativeSunPointer{};
        if (sun != nullptr)
        {
            nativeSun.directionToSun = sun->directionToSun;
            nativeSun.color = sun->color;
            nativeSun.angularRadius = sun->angularRadius;
            nativeSunPointer = &nativeSun;
        }
        Environment().DrawSky(
            view,
            projection,
            settings,
            nativeCubemap,
            nativeSunPointer);
    }

    // 反射用深度を生成し、失敗なら偽を返す(target: 描画先, projectionZ: 射影行列の深度係数, projectionW: 射影行列の深度定数)。
    bool GraphicsDevice::TryBuildReflectionDepthPyramid(
        RenderTarget& target,
        const float projectionZ,
        const float projectionW) const noexcept
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 深度ピラミッドの生成に失敗した場合はSSRを無効にする。
            // API固有の描画資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // D3D12の画像描画器
            auto* const renderer = resources != nullptr
                ? resources->TrySpriteRenderer()
                : nullptr;
            if (renderer == nullptr
                || !target.IsValid()
                || !IsGraphicsViewCurrent(target.DepthViewHandle())
                || !IsGraphicsViewCurrent(
                    target.ReflectionDepthPyramidViewHandle()))
            {
                return false;
            }
            try
            {
                return renderer->BuildReflectionDepthPyramid(
                    target,
                    m_state->m_whiteTextureView,
                    projectionZ,
                    projectionW);
            }
            catch (...)
            {
                return false;
            }
        }
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr
            || !target.IsValid()
            || !backend->IsViewCurrent(target.DepthViewHandle())
            || !backend->IsViewCurrent(
                target.ReflectionDepthPyramidViewHandle()))
        {
            return false;
        }
        try
        {
            Environment().BuildReflectionDepthPyramid(
                target,
                projectionZ,
                projectionW);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // D3D11で環境光の係数をベイクする(renderFace: キューブ各面の描画処理)。
    std::optional<std::array<float, 12>>
        GraphicsDevice::BakeIrradianceProbe(
            const EnvironmentProbeFaceRenderer& renderFace) const
    {
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr || backend->Device() == nullptr)
        {
            throw std::logic_error(
                "Irradiance probe baking requires an active backend.");
        }
        return Environment().BakeIrradianceProbe(renderFace);
    }

    // 白テクスチャを借用し、参照の解決失敗なら空を返す。
    ID3D11ShaderResourceView*
        GraphicsDevice::WhiteTexture() const noexcept
    {
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr)
        {
            return nullptr;
        }
        try
        {
            return backend->ResolveShaderResourceView(
                m_state->m_whiteTextureView);
        }
        catch (...)
        {
            // 互換用の非送出取得では参照の解決失敗も空として返す。
            return nullptr;
        }
    }

    // 現在のD3D11ネイティブ参照を借用する(view: 解決する中立参照)。
    ID3D11ShaderResourceView*
        GraphicsDevice::ResolveD3D11ShaderResourceView(
            const GraphicsViewHandle& view) const
    {
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr)
        {
            if (view)
            {
                throw std::invalid_argument(
                    "A non-empty shader-resource view requires an active "
                    "DirectX 11 backend.");
            }
            return nullptr;
        }
        return backend->ResolveShaderResourceView(view);
    }

    // D3D11参照を借用し、解決失敗なら空を返す(view: 解決する中立参照)。
    ID3D11ShaderResourceView*
        GraphicsDevice::TryResolveD3D11ShaderResourceView(
            const GraphicsViewHandle& view) const noexcept
    {
        try
        {
            return ResolveD3D11ShaderResourceView(view);
        }
        catch (...)
        {
            return nullptr;
        }
    }

    // 中立参照を優先してD3D11参照を借用する(resources: 保持する資源のスナップショット)。
    ID3D11ShaderResourceView*
        GraphicsDevice::TryResolveD3D11ShaderResourceView(
            const TextureResourceSnapshot& resources)
            const noexcept
    {
        if (resources.shaderResourceView)
        {
            // 中立参照が無効でも、旧ネイティブ参照へ戻さない。
            return TryResolveD3D11ShaderResourceView(
                resources.shaderResourceView);
        }
        if (resources.texture)
        {
            // 旧ネイティブ参照を使えるのは、中立参照が両方空の場合だけとする。
            return nullptr;
        }

        // 旧APIのテクスチャ参照
        auto* const legacy =
            resources.d3d11ShaderResourceView.Get();
        if (legacy == nullptr)
        {
            return nullptr;
        }
        // 旧参照の所有デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        legacy->GetDevice(owner.ReleaseAndGetAddressOf());
        return owner.Get() == Device() ? legacy : nullptr;
    }

    // 現在のD3D11ネイティブバッファーを借用する(buffer: 解決する中立バッファー)。
    ID3D11Buffer* GraphicsDevice::ResolveD3D11Buffer(
        const GraphicsBufferHandle& buffer) const
    {
        // D3D11の描画基盤
        const auto* const backend =
            AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr)
        {
            if (buffer)
            {
                throw std::invalid_argument(
                    "A non-empty buffer requires an active DirectX 11 "
                    "backend.");
            }
            return nullptr;
        }
        return backend->ResolveBuffer(buffer);
    }

    // D3D11参照を現在の基盤へ取り込む(view: 保持するネイティブ参照)。
    GraphicsViewHandle
        GraphicsDevice::ImportD3D11ShaderResourceView(
            ID3D11ShaderResourceView* const view)
    {
        if (view == nullptr)
        {
            return {};
        }
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr)
        {
            throw std::logic_error(
                "Importing a DirectX 11 shader-resource view requires "
                "an active DirectX 11 backend.");
        }
        return backend->ImportShaderResourceViewHandle(view);
    }

    // D3D11用環境マップのキャッシュを読み込む(key: キャッシュ識別子)。
    EnvironmentRenderer::OwnedPrefilteredEnvironment
        GraphicsDevice::TryLoadCachedEnvironment(
            const std::uint64_t key) const
    {
        return EnvironmentCache::TryLoad(Device(), key);
    }

    // 稼働中のAPIで環境プローブのベイクを準備する。
    void GraphicsDevice::PrepareEnvironmentProbeBake() const
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // プローブの描画先とキューブは初回ベイクで生成する。
            // API固有の描画資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            if (resources == nullptr
                || resources->TryEnvironmentPrefilter() == nullptr)
            {
                throw std::logic_error(
                    "Environment probe baking requires an active backend.");
            }
            return;
        }
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr || backend->Device() == nullptr)
        {
            throw std::logic_error(
                "Environment probe baking requires an active backend.");
        }
        Environment().PrepareProbeBake();
    }

    // 反射用環境マップをベイクする(renderFace: キューブ各面の描画処理, cacheKey: 任意のキャッシュ識別子)。
    PrefilteredEnvironmentViews
        GraphicsDevice::BakeReflectionProbeViews(
            const EnvironmentProbeFaceRenderer& renderFace,
            const std::optional<std::uint64_t> cacheKey) const
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // API固有の描画資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 環境マップの畳み込み器
            auto* const prefilter = resources != nullptr
                ? resources->TryEnvironmentPrefilter()
                : nullptr;
            if (prefilter == nullptr)
            {
                throw std::logic_error(
                    "Reflection probe baking requires an active backend.");
            }
            // D3D12はD3D11用ディスクキャッシュを使わず、GPUでベイクする。
            // 生成した環境マップ参照
            auto result = prefilter->BakeReflectionProbe(renderFace);
            if (!result.IsValid())
            {
                throw std::runtime_error(
                    "Reflection probe baking produced invalid environment "
                    "views.");
            }
            return result;
        }
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr || backend->Device() == nullptr)
        {
            throw std::logic_error(
                "Reflection probe baking requires an active backend.");
        }

        // ネイティブ環境マップ参照
        const auto native = Environment().BakeReflectionProbe(
            renderFace,
            cacheKey);
        // 生成した環境マップ参照
        auto result = ImportPrefilteredEnvironmentViews(
            *backend,
            native);
        if (!result.IsValid())
        {
            throw std::runtime_error(
                "Reflection probe baking produced invalid environment views.");
        }
        return result;
    }

    // 環境マップのキャッシュを取り込み、失敗なら空を返す(key: キャッシュ識別子)。
    PrefilteredEnvironmentViews
        GraphicsDevice::TryLoadCachedEnvironmentViews(
            const std::uint64_t key) const noexcept
    {
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        if (backend == nullptr || backend->Device() == nullptr)
        {
            return {};
        }
        try
        {
            // ネイティブ環境マップ参照
            const auto native = EnvironmentCache::TryLoad(
                backend->Device(),
                key);
            return ImportPrefilteredEnvironmentViews(
                *backend,
                native);
        }
        catch (...)
        {
            return {};
        }
    }

    // 環境マップを畳み込み、失敗なら空を返す(source: 元のキューブ参照, cacheKey: キャッシュ識別子)。
    PrefilteredEnvironmentViews
        GraphicsDevice::TryGetPrefilteredEnvironmentViews(
            const GraphicsViewHandle& source,
            const std::uint64_t cacheKey) const noexcept
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 入力参照とキャッシュ識別子が同じ間は畳み込み結果を再利用する。
            // API固有の描画資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 環境マップの畳み込み器
            auto* const prefilter = resources != nullptr
                ? resources->TryEnvironmentPrefilter()
                : nullptr;
            if (prefilter == nullptr || !IsSampleableCubeView(source))
            {
                return {};
            }
            try
            {
                return prefilter->Prefilter(source, cacheKey);
            }
            catch (...)
            {
                return {};
            }
        }
        // D3D11の描画基盤
        auto* const backend = AsD3D11Backend(m_state->m_backend.get());
        // D3D11の描画資源
        auto* const apiResources = TryD3D11ApiResources();
        if (!source || backend == nullptr || apiResources == nullptr)
        {
            return {};
        }

        try
        {
            // 畳み込む元キューブ参照
            auto* const nativeSource =
                backend->ResolveShaderResourceView(source);
            if (!IsCubeShaderResource(
                    backend->Device(),
                    nativeSource))
            {
                return {};
            }

            // ネイティブ環境マップ参照
            const auto native = Environment()
                .GetPrefilteredEnvironment(
                    nativeSource,
                    cacheKey);
            // 規定の最大ミップ番号
            constexpr auto ExpectedMaximumMip = static_cast<float>(
                EnvironmentRenderer::PrefilteredSpecularMipLevels - 1);
            if (!IsCubeShaderResource(
                    backend->Device(),
                    native.specular,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    EnvironmentRenderer::PrefilteredSpecularSize,
                    EnvironmentRenderer::PrefilteredSpecularMipLevels)
                || !IsCubeShaderResource(
                    backend->Device(),
                    native.irradiance,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    EnvironmentRenderer::PrefilteredIrradianceSize,
                    EnvironmentRenderer::PrefilteredIrradianceMipLevels)
                || native.specularMaximumMip != ExpectedMaximumMip)
            {
                return {};
            }

            // 既存参照を再利用できる
            bool reuseHandles =
                apiResources->skyPrefilteredSpecular
                && apiResources->skyPrefilteredIrradiance
                && apiResources->skyPrefilteredMaximumMip
                    == native.specularMaximumMip;
            if (reuseHandles)
            {
                try
                {
                    reuseHandles =
                        backend->ResolveShaderResourceView(
                            apiResources->skyPrefilteredSpecular)
                            == native.specular
                        && backend->ResolveShaderResourceView(
                            apiResources->skyPrefilteredIrradiance)
                            == native.irradiance;
                }
                catch (...)
                {
                    reuseHandles = false;
                }
            }
            if (!reuseHandles)
            {
                // 取り込む鏡面反射参照
                auto specular = backend->ImportShaderResourceViewHandle(
                    native.specular);
                // 取り込む拡散反射参照
                auto irradiance = backend->ImportShaderResourceViewHandle(
                    native.irradiance);
                apiResources->skyPrefilteredSpecular =
                    std::move(specular);
                apiResources->skyPrefilteredIrradiance =
                    std::move(irradiance);
                apiResources->skyPrefilteredMaximumMip =
                    native.specularMaximumMip;
            }

            return {
                apiResources->skyPrefilteredSpecular,
                apiResources->skyPrefilteredIrradiance,
                apiResources->skyPrefilteredMaximumMip
            };
        }
        catch (...)
        {
            return {};
        }
    }

    // GI係数を転送して旧D3D11参照を保持する(width: 格子の横数, height: 格子の縦数, depth: 格子の奥行数, coefficients: 半精度の係数列)。
    std::array<Microsoft::WRL::ComPtr<
        ID3D11ShaderResourceView>, 3>
        GraphicsDevice::UploadBakedGlobalIllumination(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t depth,
        const std::span<const std::uint16_t> coefficients) const noexcept
    {
        // 保持する旧API用GI参照
        std::array<Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView>, 3> legacyViews;
        // 中立形式のGI参照
        const auto neutralViews =
            UploadBakedGlobalIlluminationViews(
                width,
                height,
                depth,
                coefficients);
        // GI参照の要素番号
        for (std::size_t index{};
            index < neutralViews.size();
            ++index)
        {
            // 解決したGIのネイティブ参照
            auto* const nativeView =
                TryResolveD3D11ShaderResourceView(
                    neutralViews[index]);
            if (nativeView == nullptr)
            {
                return {};
            }
            legacyViews[index] = nativeView;
        }
        return legacyViews;
    }

    // 旧画像描画の終了までテクスチャを保持する(resources: 保持する資源スナップショット)。
    ID3D11ShaderResourceView*
        GraphicsDevice::PinD3D11TextureForSpriteBatch(
            std::shared_ptr<const TextureResourceSnapshot> resources)
    {
        // D3D11の描画資源
        auto& apiResources = RequireD3D11ApiResources();
        if (apiResources.spriteBatchOwner
                != Detail::D3D11SpriteBatchOwner::Legacy
            || !apiResources.spriteBatchNativeBegun)
        {
            throw std::logic_error(
                "A legacy SpriteBatch texture can only be pinned during "
                "an active legacy sprite pass.");
        }
        if (resources == nullptr)
        {
            return nullptr;
        }
        // 保持するネイティブ参照
        auto* const view =
            TryResolveD3D11ShaderResourceView(*resources);
        if (view != nullptr)
        {
            apiResources.spriteTexturePins.emplace_back(
                std::move(resources));
        }
        return view;
    }

    // インスタンス情報を転送してD3D11バッファーを借用する(data: 転送元, bytes: 転送バイト数)。
    ID3D11Buffer* GraphicsDevice::AcquireInstanceBuffer(
        const void* data,
        const std::size_t bytes)
    {
        if (data == nullptr || bytes == 0)
        {
            return nullptr;
        }
        return ResolveD3D11Buffer(
            AcquireInstanceBufferHandle(
                std::span{
                    static_cast<const std::byte*>(data),
                    bytes }));
    }

    // 描画テクスチャのD3D11参照を借用する(name: テクスチャ名)。
    ID3D11ShaderResourceView*
        GraphicsDevice::RenderTextureView(
            const std::string& name) const noexcept
    {
        return TryResolveD3D11ShaderResourceView(
            RenderTextureViewHandle(name));
    }

    // 共通の描画状態を借用し、未初期化なら例外を送出する。
    DirectX::CommonStates& GraphicsDevice::States() const
    {
        // API固有の描画資源
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr || !resources->commonStates)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *resources->commonStates;
    }

    // アルファ保持の加算混合を遅延生成して借用し、API資源がなければ空を返す。
    ID3D11BlendState* GraphicsDevice::AdditiveBlendPreservingAlpha() const
    {
        // API固有の描画資源
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            return nullptr;
        }
        if (!resources->additiveBlendPreservingAlpha)
        {
            resources->additiveBlendPreservingAlpha =
                CreateAdditiveBlendPreservingAlpha(Device());
        }
        return resources->additiveBlendPreservingAlpha.Get();
    }
}
