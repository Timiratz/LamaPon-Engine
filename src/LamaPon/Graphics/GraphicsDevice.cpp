#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ComputeEffect.h"
#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderDiagnostics.h"
#include "LamaPon/Graphics/ShaderRenderState.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/SpriteEffect.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/SceneManager.h"

#include <CommonStates.h>
#include <SpriteBatch.h>

// IDXGIFactory5（ティアリング許可の問い合わせ）。d3d11.hが引く
// dxgi.hには入っていません。
#include <dxgi1_5.h>

#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <array>
#include <chrono>
#include <future>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

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

    // コンパイル失敗時にソースを読み、原因に対応する診断を追加します。
    [[nodiscard]] std::string DescribeShaderFailure(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const char* compilerMessage,
        const LamaPon::ShaderUsage usage)
    {
        std::string source;
        try
        {
            const auto bytes =
                assets.ReadFileBytes(shaderPath);
            source.assign(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }
        catch (const std::exception&)
        {
            // 読めなくても説明は返せます（includeの取りこぼしなど、
            // ソースを見なくても分かるものがあるため）。
        }
        return LamaPon::ExplainShaderError(
            compilerMessage != nullptr ? compilerMessage : "",
            source,
            usage);
    }

    // このアダプターがティアリング許可に対応しているか。対応が無い
    // 環境（Windows 10より前、リモートデスクトップ、一部の仮想GPU）
    // ではIDXGIFactory5そのものが取れないので、そのままfalseです。
    [[nodiscard]] bool QueryTearingSupport()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
        if (FAILED(CreateDXGIFactory1(
                IID_PPV_ARGS(factory.GetAddressOf()))))
        {
            return false;
        }
        BOOL allowed = FALSE;
        if (FAILED(factory->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowed,
                sizeof(allowed))))
        {
            return false;
        }
        return allowed != FALSE;
    }
}

namespace LamaPon
{
    // EXEとDLLで初期化フラグを共有するため、実体をDLL内へ一つだけ定義します。
    bool GraphicsDevice::s_preferWarpAdapter = false;
    bool GraphicsDevice::s_enableDebugLayer = false;

    void GraphicsDevice::SetPreferWarpAdapter(
        const bool prefer) noexcept
    {
        s_preferWarpAdapter = prefer;
    }

    void GraphicsDevice::SetEnableDebugLayer(
        const bool enable) noexcept
    {
        s_enableDebugLayer = enable;
    }

    bool GraphicsDevice::IsDebugLayerEnabled() noexcept
    {
        return s_enableDebugLayer;
    }

    struct GraphicsDevice::MaterialShaderEntry final
    {
        std::unique_ptr<LitEffect> effect;
        // このエントリーのバリアント（#pragma multi_compileの
        // キーワード）。同じHLSLでも組み合わせごとに別エントリーです。
        std::vector<std::string> keywords;
        // 非同期コンパイル中の待ち合わせ。std::asyncのfutureは
        // デストラクターが完了を待つので、GraphicsDeviceを畳んだ
        // ときにワーカーが取り残されることはありません。
        std::future<void> warming;
        // バイトコードの用意待ち。trueの間は標準Litで描きます。
        bool pending{};
        std::filesystem::file_time_type writeTime{};
        std::uint64_t generation{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::SpriteShaderEntry final
    {
        std::unique_ptr<SpriteEffect> effect;
        std::filesystem::file_time_type writeTime{};
        std::uint64_t generation{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::ScreenShaderEntry final
    {
        std::unique_ptr<ScreenEffect> effect;
        std::filesystem::file_time_type writeTime{};
        std::uint64_t generation{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::ComputeShaderEntry final
    {
        std::unique_ptr<ComputeEffect> effect;
        std::filesystem::file_time_type writeTime{};
        std::string error;
        std::chrono::steady_clock::time_point nextCheck{};
        bool observed{};
        bool sourceExists{};
        bool forceReload{};
    };

    struct GraphicsDevice::QueuedScreenEffect final
    {
        ScreenEffect* effect{};
        std::array<
            std::shared_ptr<const TextureAsset>,
            2> auxiliaryTextures{};
        ScreenEffect::CustomParameters parameters{};
        ScreenEffectPoint point{
            ScreenEffectPoint::AfterToneMapping };
    };

    GraphicsDevice::GraphicsDevice()
        : m_services(std::make_unique<RuntimeServices>())
    {
    }
    GraphicsDevice::~GraphicsDevice()
    {
        Shutdown();
    }

    void GraphicsDevice::RecordFrameStatistics(
        const float frameTimeSeconds,
        const float cpuTimeMilliseconds) noexcept
    {
        const float safeFrameTime =
            std::max(frameTimeSeconds, 0.000001f);
        constexpr float smoothing = 0.1f;
        if (m_frameStatistics.totalFrames == 0)
        {
            m_frameStatistics.frameTimeMilliseconds =
                safeFrameTime * 1000.0f;
            m_frameStatistics.cpuTimeMilliseconds =
                std::max(cpuTimeMilliseconds, 0.0f);
        }
        else
        {
            m_frameStatistics.frameTimeMilliseconds +=
                (safeFrameTime * 1000.0f
                    - m_frameStatistics
                        .frameTimeMilliseconds)
                * smoothing;
            m_frameStatistics.cpuTimeMilliseconds +=
                (std::max(cpuTimeMilliseconds, 0.0f)
                    - m_frameStatistics
                        .cpuTimeMilliseconds)
                * smoothing;
        }
        m_frameStatistics.framesPerSecond =
            1000.0f
            / std::max(
                m_frameStatistics.frameTimeMilliseconds,
                0.001f);
        ++m_frameStatistics.totalFrames;
    }

    void GraphicsDevice::Shutdown() noexcept
    {
        if (m_context)
        {
            m_context->ClearState();
            m_context->Flush();
        }
        m_shadowMap.reset();
        m_spotShadowMap.reset();
        m_pointShadowMap.reset();
        m_instanceBuffer.Reset();
        m_instanceBufferCapacity = 0;
        m_uiScissorRasterizer.Reset();
        m_skinnedMaterialShaders.clear();
        m_materialShaders.clear();
        m_spriteShaders.clear();
        m_screenShaders.clear();
        m_queuedScreenEffects.clear();
        m_litEffect.reset();
        m_sceneCompositionTarget.reset();
        m_renderTextures.clear();
        m_environmentRenderer.reset();
        m_clusteredLights.reset();
        m_debugRenderer.reset();
        m_services->Shutdown();
        m_commonStates.reset();
        m_spriteBatch.reset();
        m_whiteTexture.Reset();
        m_depthStencilView.Reset();
        m_depthTexture.Reset();
        m_renderTargetView.Reset();
        m_swapChain.Reset();
        m_context.Reset();
        m_device.Reset();
        m_width = 0;
        m_height = 0;
        m_sprite2DOffset = {};
    }

    void GraphicsDevice::Initialize(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        m_width = std::max(width, 1u);
        m_height = std::max(height, 1u);
        m_uiWidth = m_width;
        m_uiHeight = m_height;
        m_sprite2DOffset = {};

        // ティアリング許可が無いと、VSyncを切ってもモニターの
        // リフレッシュレートがそのままFPSの上限になります。フリップ
        // モデルでは提示が垂直同期の間隔で引き取られ、積める枚数
        // （BufferCount）を使い切った時点でPresentが待たされるためです。
        // ティアリングには、アダプター対応、スワップチェーン作成フラグ、
        // Presentの同期間隔0と提示フラグの組み合わせが必要です。
        m_tearingAllowed = QueryTearingSupport();

        DXGI_SWAP_CHAIN_DESC swapChainDescription{};
        swapChainDescription.BufferDesc.Width = m_width;
        swapChainDescription.BufferDesc.Height = m_height;
        swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDescription.SampleDesc.Count = 1;
        swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDescription.BufferCount = 2;
        swapChainDescription.OutputWindow = window;
        swapChainDescription.Windowed = TRUE;
        swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDescription.Flags = m_tearingAllowed
            ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
            : 0u;

        constexpr std::array featureLevels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const bool wantDebugLayer =
#if defined(_DEBUG)
            true;
#else
            s_enableDebugLayer;
#endif
        if (wantDebugLayer)
        {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        }

        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        const auto createDevice =
            [this,
                &swapChainDescription,
                &featureLevels,
                &selectedFeatureLevel](
                const D3D_DRIVER_TYPE driverType,
                const UINT deviceFlags)
        {
            return D3D11CreateDeviceAndSwapChain(
                nullptr,
                driverType,
                nullptr,
                deviceFlags,
                featureLevels.data(),
                static_cast<UINT>(featureLevels.size()),
                D3D11_SDK_VERSION,
                &swapChainDescription,
                m_swapChain.ReleaseAndGetAddressOf(),
                m_device.ReleaseAndGetAddressOf(),
                &selectedFeatureLevel,
                m_context.ReleaseAndGetAddressOf());
        };

        const D3D_DRIVER_TYPE primaryDriver =
            s_preferWarpAdapter
                ? D3D_DRIVER_TYPE_WARP
                : D3D_DRIVER_TYPE_HARDWARE;
        HRESULT result = createDevice(primaryDriver, flags);

        // SDKのデバッグレイヤーが利用できない場合は、フラグを外して再作成します。
        if (wantDebugLayer
            && result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = createDevice(primaryDriver, flags);
        }

        // 対応判定後もティアリング付き作成が失敗するドライバーでは、
        // 提示フラグを外して再作成します。描画結果は維持されますが、
        // FPS上限はリフレッシュレートへ戻ります。
        if (FAILED(result) && m_tearingAllowed)
        {
            m_tearingAllowed = false;
            swapChainDescription.Flags = 0;
            result = createDevice(primaryDriver, flags);
            if (SUCCEEDED(result))
            {
                Logger::Instance().Warning(
                    "ティアリング許可付きのスワップチェーンを作れ"
                    "なかったため、無効で起動しました。VSyncを切っても"
                    "モニターのリフレッシュレートがFPSの上限になります。");
            }
        }

        // GPUが使えない環境（VM・リモートデスクトップ・CI）では
        // WARP（CPUラスタライザ）へ自動フォールバックします。
        if (FAILED(result) && !s_preferWarpAdapter)
        {
            result = createDevice(
                D3D_DRIVER_TYPE_WARP,
                flags);
            if (SUCCEEDED(result))
            {
                Logger::Instance().Warning(
                    "GPUデバイスの作成に失敗したため、WARP"
                    "（CPU描画）で起動しました。描画性能は低下します。");
            }
        }

        ThrowIfFailed(result, "D3D11CreateDeviceAndSwapChain");

        // デバッガーなしでも確認できるよう、InfoQueueのメッセージを
        // エンジンログへ転送します。
        if ((flags & D3D11_CREATE_DEVICE_DEBUG) != 0)
        {
            if (SUCCEEDED(m_device.As(&m_infoQueue)))
            {
                Logger::Instance().Info(
                    "D3D11のデバッグレイヤーを有効にしました。"
                    "不正な描画はログへ出ます（描画は遅くなります）。");
            }
            else
            {
                Logger::Instance().Warning(
                    "D3D11のデバッグレイヤーは有効ですが、"
                    "InfoQueueを取得できませんでした。");
            }
        }
        else if (s_enableDebugLayer)
        {
            Logger::Instance().Warning(
                "--d3ddebug が指定されましたが、D3D11の"
                "デバッグレイヤーを有効にできませんでした。"
                "Windowsのオプション機能「グラフィックス ツール」が"
                "必要です。");
        }

        if (selectedFeatureLevel < D3D_FEATURE_LEVEL_11_0)
        {
            throw std::runtime_error("Direct3D feature level 11.0 is required.");
        }

        LogSelectedAdapter();
        RefreshMemoryStatistics(true);
        // エディター外でもFPS制限の状態を確認できるよう、ログへ記録します。
        if (!m_tearingAllowed)
        {
            Logger::Instance().Info(
                "ティアリング許可が使えない環境です。VSyncを切っても"
                "モニターのリフレッシュレートがFPSの上限になります。");
        }

        CreateSizeDependentResources();
        CreateWhiteTexture();
        m_spriteBatch = std::make_unique<DirectX::SpriteBatch>(m_context.Get());
        m_commonStates = std::make_unique<DirectX::CommonStates>(m_device.Get());
        {
            // UIクリッピング（ScrollView等）用のシザー有効
            // ラスタライザ。
            D3D11_RASTERIZER_DESC scissorDescription{};
            scissorDescription.FillMode =
                D3D11_FILL_SOLID;
            scissorDescription.CullMode = D3D11_CULL_NONE;
            scissorDescription.DepthClipEnable = TRUE;
            scissorDescription.ScissorEnable = TRUE;
            ThrowIfFailed(
                m_device->CreateRasterizerState(
                    &scissorDescription,
                    m_uiScissorRasterizer
                        .ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateRasterizerState");
        }
        m_services->Initialize(m_device.Get(), m_context.Get(), window,
            m_graphicsSettings.runtimeTextureCompression);
        m_debugRenderer = std::make_unique<DebugRenderer>(
            m_device.Get(),
            m_context.Get());
        m_shadowMap = std::make_unique<ShadowMap>();
        m_spotShadowMap = std::make_unique<ShadowMap>();
        m_pointShadowMap = std::make_unique<ShadowMap>();
        if (m_graphicsSettings.shadowsEnabled)
        {
            m_shadowMap->Initialize(
                m_device.Get(),
                m_graphicsSettings.shadowResolution,
                m_graphicsSettings.shadowCascadeLimit);
            // スポット/ポイントはカスケードより解像度を落とします。
            const std::uint32_t localShadowResolution =
                std::max(
                    m_graphicsSettings.shadowResolution
                        / 2u,
                    256u);
            m_spotShadowMap->Initialize(
                m_device.Get(),
                localShadowResolution,
                static_cast<std::uint32_t>(
                    MaximumSpotShadows));
            m_pointShadowMap->Initialize(
                m_device.Get(),
                localShadowResolution,
                6u,
                true);
        }
        m_sceneCompositionTarget =
            std::make_unique<RenderTarget>();
        m_gpuProfiler.Initialize(
            m_device.Get(),
            m_context.Get());
    }

    void GraphicsDevice::Resize(const std::uint32_t width, const std::uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
        {
            return;
        }

        m_width = width;
        m_height = height;
        m_uiWidth = width;
        m_uiHeight = height;

        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();
        m_depthTexture.Reset();
        m_context->Flush();

        // 作成時と同じフラグを渡し直さないと、リサイズした瞬間に
        // ティアリング許可が外れ、次のPresentがE_INVALIDARGで落ちます。
        ThrowIfFailed(
            m_swapChain->ResizeBuffers(
                0,
                m_width,
                m_height,
                DXGI_FORMAT_UNKNOWN,
                m_tearingAllowed
                    ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                    : 0u),
            "IDXGISwapChain::ResizeBuffers");

        CreateSizeDependentResources();
    }

    void GraphicsDevice::BeginFrame(const float clearColor[4])
    {
        m_gpuProfiler.OpenFrame();
        RefreshMemoryStatistics();
        // 大きいテクスチャの段階アップロードを予算内で進めます
        // （メインスレッドのフレーム先頭が唯一の転送ポイント）。
        if (auto* assets = TryAssets())
        {
            assets->PumpTextureUploads();
            assets->PumpModelUploads();
        }
        m_uiWidth = m_width;
        m_uiHeight = m_height;
        ID3D11RenderTargetView* renderTargets[]{ m_renderTargetView.Get() };
        m_context->OMSetRenderTargets(1, renderTargets, m_depthStencilView.Get());
        m_context->RSSetViewports(1, &m_viewport);
        m_context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
        m_context->ClearDepthStencilView(
            m_depthStencilView.Get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            1.0f,
            0);
    }

    DirectX::SpriteBatch& GraphicsDevice::BeginSprites()
    {
        m_uiScissorStack.clear();
        m_spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            m_commonStates->NonPremultiplied());
        return *m_spriteBatch;
    }

    DirectX::SpriteBatch& GraphicsDevice::BeginSprites(
        const std::filesystem::path& shaderPath,
        const std::array<DirectX::XMFLOAT4, 8>&
            customParameters,
        std::uint64_t* generation,
        std::string* error,
        const Sprite2DLighting* lighting)
    {
        if (generation != nullptr)
        {
            *generation = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        if (shaderPath.empty())
        {
            return BeginSprites();
        }

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        auto& entry = m_spriteShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<SpriteShaderEntry>();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                if (!sourceExists)
                {
                    entry->error =
                        "Sprite shader file was not found: "
                        + PathToUtf8(absolutePath);
                    entry->effect.reset();
                }
                else
                {
                    try
                    {
                        auto candidate =
                            std::make_unique<SpriteEffect>(
                                m_device.Get(),
                                m_context.Get(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_spriteShaderGeneration;
                        entry->error.clear();
                    }
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Sprite);
                        // 直前に成功したものを描き続けると、書き
                        // 間違えたシェーダーが前のまま出ます。
                        // 3Dと同じく捨てて代役に任せます。
                        entry->effect.reset();
                    }
                }
            }
        }

        if (generation != nullptr)
        {
            *generation = entry->generation;
        }
        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {
            // コンパイル失敗を視認できるよう、マゼンタの代替表示を使います。
            if (!entry->error.empty())
            {
                if (auto* const placeholder =
                        SpriteErrorPlaceholder())
                {
                    placeholder->SetParameters(
                        customParameters);
                    placeholder->SetLights(Sprite2DLighting{});
                    m_spriteBatch->Begin(
                        DirectX::SpriteSortMode_Deferred,
                        m_commonStates->NonPremultiplied(),
                        nullptr,
                        nullptr,
                        nullptr,
                        [placeholder]()
                        {
                            placeholder->Apply();
                        });
                    return *m_spriteBatch;
                }
            }
            return BeginSprites();
        }

        entry->effect->SetParameters(customParameters);
        // 灯りを渡されなかった呼び出しでは空にします。前の描画の
        // 一覧が残っていると、Light2Dを消したのにまだ光る、という
        // 見え方になるためです。
        entry->effect->SetLights(
            lighting != nullptr
                ? *lighting
                : Sprite2DLighting{});
        auto* effect = entry->effect.get();
        m_spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            m_commonStates->NonPremultiplied(),
            nullptr,
            nullptr,
            nullptr,
            [effect]()
            {
                effect->Apply();
            });
        return *m_spriteBatch;
    }

    bool GraphicsDevice::ApplyCustomPixelShader(
        const std::filesystem::path& shaderPath,
        const std::array<
            DirectX::XMFLOAT4,
            8>& customParameters,
        std::uint64_t* generation,
        std::string* error) const
    {
        if (generation != nullptr)
        {
            *generation = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        if (shaderPath.empty())
        {
            return false;
        }

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        auto& entry = m_spriteShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<SpriteShaderEntry>();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                if (!sourceExists)
                {
                    entry->error =
                        "Custom pixel shader file was not found: "
                        + PathToUtf8(absolutePath);
                    entry->effect.reset();
                }
                else
                {
                    try
                    {
                        auto candidate =
                            std::make_unique<SpriteEffect>(
                                m_device.Get(),
                                m_context.Get(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_spriteShaderGeneration;
                        entry->error.clear();
                    }
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Sprite);
                        // スプライトと同じく、失敗したら直前の
                        // シェーダーは残しません。
                        entry->effect.reset();
                    }
                }
            }
        }

        if (generation != nullptr)
        {
            *generation = entry->generation;
        }
        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {
            // スプライトと同じく、失敗はマゼンタで知らせます。
            if (!entry->error.empty())
            {
                if (auto* const placeholder =
                        SpriteErrorPlaceholder())
                {
                    placeholder->SetParameters(
                        customParameters);
                    placeholder->Apply();
                    return true;
                }
            }
            return false;
        }

        entry->effect->SetParameters(customParameters);
        entry->effect->Apply();
        return true;
    }

    void GraphicsDevice::InvalidateCustomPixelShader(
        const std::filesystem::path& shaderPath) const
    {
        InvalidateSpriteShader(shaderPath);
    }

    void GraphicsDevice::EndSprites()
    {
        m_spriteBatch->End();
        m_uiScissorStack.clear();
    }

    void GraphicsDevice::PushUIScissor(
        const float minimumX,
        const float minimumY,
        const float maximumX,
        const float maximumY)
    {
        D3D11_RECT scissor{
            static_cast<LONG>(
                std::max(minimumX, 0.0f)),
            static_cast<LONG>(
                std::max(minimumY, 0.0f)),
            static_cast<LONG>(
                std::max(maximumX, 0.0f)),
            static_cast<LONG>(
                std::max(maximumY, 0.0f)) };
        // 入れ子は交差矩形にします。
        if (!m_uiScissorStack.empty())
        {
            const auto& outer = m_uiScissorStack.back();
            scissor.left =
                std::max(scissor.left, outer.left);
            scissor.top =
                std::max(scissor.top, outer.top);
            scissor.right =
                std::min(scissor.right, outer.right);
            scissor.bottom =
                std::min(scissor.bottom, outer.bottom);
        }
        scissor.right =
            std::max(scissor.right, scissor.left);
        scissor.bottom =
            std::max(scissor.bottom, scissor.top);
        m_uiScissorStack.push_back(scissor);

        // 進行中のバッチを確定してからシザー状態へ切り替えます。
        m_spriteBatch->End();
        m_context->RSSetScissorRects(1, &scissor);
        m_spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            m_commonStates->NonPremultiplied(),
            nullptr,
            nullptr,
            m_uiScissorRasterizer.Get());
    }

    void GraphicsDevice::PopUIScissor()
    {
        if (m_uiScissorStack.empty())
        {
            return;
        }
        m_uiScissorStack.pop_back();
        m_spriteBatch->End();
        if (m_uiScissorStack.empty())
        {
            m_spriteBatch->Begin(
                DirectX::SpriteSortMode_Deferred,
                m_commonStates->NonPremultiplied());
            return;
        }
        m_context->RSSetScissorRects(
            1,
            &m_uiScissorStack.back());
        m_spriteBatch->Begin(
            DirectX::SpriteSortMode_Deferred,
            m_commonStates->NonPremultiplied(),
            nullptr,
            nullptr,
            m_uiScissorRasterizer.Get());
    }

    ID3D11Buffer* GraphicsDevice::AcquireInstanceBuffer(
        const void* data,
        const std::size_t bytes)
    {
        if (data == nullptr
            || bytes == 0
            || !IsInitialized())
        {
            return nullptr;
        }

        if (!m_instanceBuffer
            || m_instanceBufferCapacity < bytes)
        {
            const std::size_t capacity = std::max({
                bytes,
                m_instanceBufferCapacity * 2,
                static_cast<std::size_t>(4096) });
            D3D11_BUFFER_DESC description{};
            description.ByteWidth =
                static_cast<UINT>(capacity);
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags =
                D3D11_BIND_VERTEX_BUFFER;
            description.CPUAccessFlags =
                D3D11_CPU_ACCESS_WRITE;
            if (FAILED(m_device->CreateBuffer(
                    &description,
                    nullptr,
                    m_instanceBuffer
                        .ReleaseAndGetAddressOf())))
            {
                m_instanceBuffer.Reset();
                m_instanceBufferCapacity = 0;
                return nullptr;
            }
            m_instanceBufferCapacity = capacity;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(
                m_instanceBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped)))
        {
            return nullptr;
        }
        std::memcpy(mapped.pData, data, bytes);
        m_context->Unmap(m_instanceBuffer.Get(), 0);
        return m_instanceBuffer.Get();
    }

    void GraphicsDevice::DrainDebugMessages()
    {
        if (!m_infoQueue)
        {
            return;
        }

        const auto stored =
            m_infoQueue->GetNumStoredMessages();
        for (UINT64 index = 0; index < stored; ++index)
        {
            SIZE_T length = 0;
            if (FAILED(m_infoQueue->GetMessage(
                    index,
                    nullptr,
                    &length))
                || length == 0)
            {
                continue;
            }
            std::vector<std::byte> storage(length);
            auto* const message =
                reinterpret_cast<D3D11_MESSAGE*>(
                    storage.data());
            if (FAILED(m_infoQueue->GetMessage(
                    index,
                    message,
                    &length)))
            {
                continue;
            }

            const std::string text(
                message->pDescription,
                message->DescriptionByteLength > 0
                    ? message->DescriptionByteLength - 1
                    : 0);
            ++m_debugMessagesLogged;
            switch (message->Severity)
            {
            case D3D11_MESSAGE_SEVERITY_CORRUPTION:
            case D3D11_MESSAGE_SEVERITY_ERROR:
                Logger::Instance().Error("D3D11: " + text);
                break;
            case D3D11_MESSAGE_SEVERITY_WARNING:
                Logger::Instance().Warning("D3D11: " + text);
                break;
            default:
                Logger::Instance().Info("D3D11: " + text);
                break;
            }
        }
        m_infoQueue->ClearStoredMessages();
    }

    void GraphicsDevice::EndFrame()
    {
        // Presentより前に流します。デバイスを失う描画があった場合、
        // その理由はこのメッセージ側に出ていることが多いためです。
        DrainDebugMessages();
        m_gpuProfiler.CloseFrame();
        // DXGI_PRESENT_ALLOW_TEARINGは同期間隔0とセットでしか使えません
        // （VSync有効時に渡すとPresentがE_INVALIDARGを返します）。
        // ALLOW_TEARINGを指定するとリフレッシュレートを超えられます。上限は
        // Application側のフレームペーサー（targetFrameRate）が持ちます。
        const bool immediate =
            !m_graphicsSettings.vSyncEnabled;
        const HRESULT presented = m_swapChain->Present(
            immediate ? 0u : 1u,
            (immediate && m_tearingAllowed)
                ? DXGI_PRESENT_ALLOW_TEARING
                : 0u);
        // デバイスを失ったときは、Presentの戻り値ではなく
        // GetDeviceRemovedReasonの方に本当の理由が入ります。
        // 数字だけ投げると「HRESULT 2289696802」のような、
        // 手がかりの無いメッセージになります。
        if (presented == DXGI_ERROR_DEVICE_REMOVED
            || presented == DXGI_ERROR_DEVICE_RESET)
        {
            const HRESULT reason = m_device
                ? m_device->GetDeviceRemovedReason()
                : presented;
            std::ostringstream message;
            message
                << "The graphics device was lost while"
                   " presenting a frame (Present=0x"
                << std::hex << std::uppercase
                << static_cast<unsigned long>(presented)
                << ", reason=0x"
                << static_cast<unsigned long>(reason)
                << "). This usually means the driver rejected"
                   " the previous draw call. Run with"
                   " --d3ddebug to see which one.";
            throw std::runtime_error(message.str());
        }
        ThrowIfFailed(presented, "IDXGISwapChain::Present");
    }

    std::vector<std::uint8_t>
        GraphicsDevice::CaptureBackBuffer(
            std::uint32_t& width,
            std::uint32_t& height) const
    {
        if (!IsInitialized() || m_swapChain == nullptr)
        {
            throw std::logic_error(
                "CaptureBackBuffer requires an initialized device.");
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(
            m_swapChain->GetBuffer(
                0,
                IID_PPV_ARGS(
                    backBuffer.ReleaseAndGetAddressOf())),
            "IDXGISwapChain::GetBuffer");

        D3D11_TEXTURE2D_DESC description{};
        backBuffer->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        ThrowIfFailed(
            m_device->CreateTexture2D(
                &description,
                nullptr,
                staging.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(staging)");
        m_context->CopyResource(
            staging.Get(),
            backBuffer.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(
            m_context->Map(
                staging.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped),
            "ID3D11DeviceContext::Map(staging)");

        width = description.Width;
        height = description.Height;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width)
            * height
            * 4);
        for (std::uint32_t row = 0; row < height; ++row)
        {
            std::memcpy(
                pixels.data()
                    + static_cast<std::size_t>(row)
                        * width * 4,
                static_cast<const std::uint8_t*>(
                    mapped.pData)
                    + static_cast<std::size_t>(row)
                        * mapped.RowPitch,
                static_cast<std::size_t>(width) * 4);
        }
        m_context->Unmap(staging.Get(), 0);
        return pixels;
    }

    RenderTarget& GraphicsDevice::AcquireRenderTexture(
        const std::string& name,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const std::uint32_t safeWidth =
            width == 0 ? 1 : width;
        const std::uint32_t safeHeight =
            height == 0 ? 1 : height;

        auto& slot = m_renderTextures[name];
        if (!slot)
        {
            slot = std::make_unique<RenderTarget>();
        }
        // Resizeは同じサイズなら何もしません（作り直しの判定は
        // RenderTarget側が持っています）。
        slot->Resize(
            m_device.Get(),
            safeWidth,
            safeHeight);
        return *slot;
    }

    RenderTarget& GraphicsDevice::AcquireComputeTexture(
        const std::string& name,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        const std::uint32_t safeWidth =
            width == 0 ? 1 : width;
        const std::uint32_t safeHeight =
            height == 0 ? 1 : height;

        auto& slot = m_renderTextures[name];
        if (!slot)
        {
            slot = std::make_unique<RenderTarget>();
        }
        // UAVのバインドフラグは作成時にしか決められないので、
        // Resizeより前に印を付けます。カメラの描画先として先に
        // 既存の同名テクスチャには作成フラグを追加できないため、
        // カメラ描画先とは異なる名前を使用します。
        slot->SetComputeWritable(true);
        slot->Resize(
            m_device.Get(),
            safeWidth,
            safeHeight);
        return *slot;
    }

    const RenderTarget* GraphicsDevice::FindRenderTexture(
        const std::string& name) const noexcept
    {
        const auto entry = m_renderTextures.find(name);
        if (entry == m_renderTextures.end())
        {
            return nullptr;
        }
        return entry->second.get();
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::RenderTextureView(
            const std::string& name) const noexcept
    {
        const auto* target = FindRenderTexture(name);
        if (target == nullptr
            || !target->IsValid())
        {
            return nullptr;
        }
        return target->DisplayShaderResourceView();
    }

    bool GraphicsDevice::ReleaseRenderTexture(
        const std::string& name)
    {
        return m_renderTextures.erase(name) > 0;
    }

    void GraphicsDevice::ClearRenderTextures() noexcept
    {
        m_renderTextures.clear();
    }

    std::vector<std::string>
        GraphicsDevice::RenderTextureNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_renderTextures.size());
        for (const auto& [name, target] : m_renderTextures)
        {
            static_cast<void>(target);
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    void GraphicsDevice::BeginSceneComposition(
        const float clearColor[4])
    {
        m_sceneCompositionTarget->Resize(
            m_device.Get(),
            RenderWidth(),
            RenderHeight());
        m_sceneCompositionTarget->Bind(
            m_context.Get());
        m_sceneCompositionTarget->Clear(
            m_context.Get(),
            clearColor);
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading)
    {
        EndSceneComposition(
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            VolumetricLightFrame{},
            TemporalAntiAliasingFrame{});
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading)
    {
        EndSceneComposition(
            bloom,
            lensFlare,
            colorGrading,
            VolumetricLightFrame{},
            TemporalAntiAliasingFrame{});
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal)
    {
        EndSceneComposition(
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            volumetric,
            temporal);
    }

    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal)
    {
        PostProcessFrame frame{};
        frame.bloom = bloom;
        frame.lensFlare = lensFlare;
        frame.colorGrading = colorGrading;
        frame.volumetric = volumetric;
        frame.temporal = temporal;
        EndSceneComposition(frame);
    }

    void GraphicsDevice::EndSceneComposition(
        const PostProcessFrame& frame)
    {
        // RunPostProcessが計測区間を開始するため、呼び出し側では開始しません。
        RunPostProcess(
            *this,
            *m_sceneCompositionTarget,
            frame,
            [this](
                RenderTarget& target,
                const ScreenEffectPoint point)
            {
                ApplyQueuedScreenEffects(target, point);
            });
        m_gpuProfiler.BeginSection("画面へ転送");
        Environment().Copy(
            m_sceneCompositionTarget->
                ShaderResourceView(),
            m_renderTargetView.Get(),
            m_width,
            m_height);
        m_gpuProfiler.EndSection();
    }

    void GraphicsDevice::LogSelectedAdapter() const
    {
        // 性能診断でGPUとWARPを区別できるよう、起動時のアダプター名を記録します。
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device.As(&dxgiDevice)))
        {
            return;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(
                adapter.GetAddressOf()))
            || adapter == nullptr)
        {
            return;
        }
        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description)))
        {
            return;
        }

        // WARPは固定のベンダー／デバイスIDで名乗ります。
        const bool isWarp =
            description.VendorId == 0x1414u
            && description.DeviceId == 0x8cu;
        std::string name;
        for (const auto character : description.Description)
        {
            if (character == L'\0')
            {
                break;
            }
            name.push_back(
                character < 128
                    ? static_cast<char>(character)
                    : '?');
        }
        const auto videoMemoryMegabytes =
            static_cast<std::uint64_t>(
                description.DedicatedVideoMemory)
            / (1024u * 1024u);

        auto message = "描画アダプター: " + name
            + "（VRAM " + std::to_string(
                videoMemoryMegabytes)
            + " MB）";
        if (isWarp)
        {
            Logger::Instance().Warning(
                message
                + " ※WARP（CPU描画）です。GPUを使っていないため"
                  "描画性能は大幅に低下します。");
        }
        else
        {
            Logger::Instance().Info(message);
        }
    }

    void GraphicsDevice::ApplyQueuedScreenEffects(
        RenderTarget& target,
        const ScreenEffectPoint point)
    {
        // 対象地点にエフェクトが無い場合は、深度変換係数の計算を省略します。
        if (std::ranges::none_of(
                m_queuedScreenEffects,
                [point](const QueuedScreenEffect& queued)
                {
                    return queued.point == point;
                }))
        {
            return;
        }
        // 深度を距離へ直す係数。式は距離＝y/(深度+x)で、SSRの
        // Hi-Z作成（PSReflectionDepthLinearize）と同じものです。
        // SSRの深度変換と同じ式を使い、変換規則を一致させます。
        // 射影はこの画像を描いたときのもの
        // （TAAのずらし込み＝深度バッファと噛み合う方）。
        const auto& projection = SceneProjection();
        const DirectX::XMFLOAT4 depthParameters{
            projection._33,
            projection._43,
            1.0f,
            0.0f
        };
        // ビュー空間の位置（＝法線の再構成）用。SSAOが持っている
        // AmbientOcclusionProjectionのzwと同じ中身です。
        // 0除算よけの1e-6は、射影が空のときに無限大を配らないため。
        const DirectX::XMFLOAT4 depthUnprojection{
            1.0f / (std::abs(projection._11) > 1e-6f
                ? projection._11
                : 1.0f),
            1.0f / (std::abs(projection._22) > 1e-6f
                ? projection._22
                : 1.0f),
            0.0f,
            0.0f
        };
        for (const auto& queued : m_queuedScreenEffects)
        {
            if (queued.effect == nullptr
                || queued.point != point)
            {
                continue;
            }
            const std::array<ID3D11ShaderResourceView*, 2>
                auxiliaryViews{
                    queued.auxiliaryTextures[0]
                        ? queued.auxiliaryTextures[0]->view.Get()
                        : WhiteTexture(),
                    queued.auxiliaryTextures[1]
                        ? queued.auxiliaryTextures[1]->view.Get()
                        : WhiteTexture()
                };
            target.ApplyScreenEffect(
                *queued.effect,
                auxiliaryViews,
                depthParameters,
                depthUnprojection,
                queued.parameters);
        }
        // 現在の地点で適用したエフェクトだけを取り除きます。同じフレームの
        // 後続地点に登録されたエフェクトはキューへ残します。
        std::erase_if(
            m_queuedScreenEffects,
            [point](const QueuedScreenEffect& queued)
            {
                return queued.point == point;
            });
    }

    bool GraphicsDevice::QueueScreenEffect(
        const ScreenEffectRequest& request,
        std::uint64_t* generation,
        std::string* error)
    {
        if (generation != nullptr)
        {
            *generation = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        if (request.shader.empty())
        {
            return false;
        }

        const auto absolutePath =
            Assets().ResolvePath(request.shader)
                .lexically_normal();
        auto& entry = m_screenShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<ScreenShaderEntry>();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                if (!sourceExists)
                {
                    entry->error =
                        "Screen effect shader file was not found: "
                        + PathToUtf8(absolutePath);
                }
                else
                {
                    try
                    {
                        auto candidate =
                            std::make_unique<ScreenEffect>(
                                m_device.Get(),
                                m_context.Get(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_screenShaderGeneration;
                        entry->error.clear();
                    }
                    catch (const std::exception& exception)
                    {
                        // 再コンパイルに失敗しても、直前の正常なシェーダーは維持します。
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::ScreenEffect);
                    }
                }
            }
        }

        if (generation != nullptr)
        {
            *generation = entry->generation;
        }
        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {
            return false;
        }

        QueuedScreenEffect queued{};
        queued.effect = entry->effect.get();
        queued.parameters = request.customParameters;
        queued.point = request.point;
        for (std::size_t index = 0;
            index < request.auxiliaryTextures.size();
            ++index)
        {
            if (!request.auxiliaryTextures[index].empty())
            {
                queued.auxiliaryTextures[index] =
                    Assets().LoadTexture(
                        request.auxiliaryTextures[index]);
            }
        }
        m_queuedScreenEffects.emplace_back(
            std::move(queued));
        return true;
    }

    bool GraphicsDevice::DispatchComputeEffect(
        const ComputeEffectRequest& request,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (request.shader.empty()
            || request.outputTexture.empty()
            || request.outputWidth == 0
            || request.outputHeight == 0)
        {
            if (error != nullptr)
            {
                *error =
                    "A compute effect needs a shader, an"
                    " output texture name and a non-zero"
                    " size.";
            }
            return false;
        }

        const auto absolutePath =
            Assets().ResolvePath(request.shader)
                .lexically_normal();
        auto& entry = m_computeShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<ComputeShaderEntry>();
        }

        // 更新の見張り方はScreenEffectと同じです（保存したら
        // 作り直す、失敗しても直前の正常な版を残す）。
        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                if (!sourceExists)
                {
                    entry->error =
                        "Compute effect shader file was not"
                        " found: "
                        + PathToUtf8(absolutePath);
                }
                else
                {
                    try
                    {
                        entry->effect =
                            std::make_unique<ComputeEffect>(
                                m_device.Get(),
                                m_context.Get(),
                                Assets(),
                                absolutePath);
                        entry->error.clear();
                    }
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Compute);
                    }
                }
            }
        }

        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {
            return false;
        }

        // 書き込み先。UAVが要るので、Resizeの前に印を付けます
        // （バインドフラグは作成時にしか決められません）。
        auto& target = AcquireComputeTexture(
            request.outputTexture,
            request.outputWidth,
            request.outputHeight);
        if (target.DisplayUnorderedAccessView() == nullptr)
        {
            if (error != nullptr)
            {
                *error =
                    "The compute output texture could not"
                    " be created for writing.";
            }
            return false;
        }

        std::array<ID3D11ShaderResourceView*, 2> inputs{};
        for (std::size_t index = 0;
            index < request.inputTextures.size();
            ++index)
        {
            if (request.inputTextures[index].empty())
            {
                inputs[index] = WhiteTexture();
                continue;
            }
            const auto texture = Assets().LoadTexture(
                request.inputTextures[index]);
            inputs[index] = texture
                ? texture->view.Get()
                : WhiteTexture();
        }

        m_gpuProfiler.BeginSection("Compute");
        entry->effect->Dispatch(
            inputs,
            target.DisplayUnorderedAccessView(),
            target.Width(),
            target.Height(),
            request.customParameters);
        m_gpuProfiler.EndSection();
        return true;
    }

    void GraphicsDevice::InvalidateComputeEffectShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty() || !TryAssets())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath)
                .lexically_normal();
        const auto found =
            m_computeShaders.find(absolutePath);
        if (found != m_computeShaders.end()
            && found->second)
        {
            found->second->forceReload = true;
        }
    }

    void GraphicsDevice::InvalidateScreenEffectShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty() || !TryAssets())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath)
                .lexically_normal();
        const auto found =
            m_screenShaders.find(absolutePath);
        if (found != m_screenShaders.end()
            && found->second)
        {
            found->second->forceReload = true;
        }
    }

    void GraphicsDevice::DrawLoadingScreen(
        const float progress,
        const SceneLoadingScreenSettings& settings,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!settings.enabled)
        {
            return;
        }

        using namespace DirectX;
        const std::uint32_t canvasWidth =
            width == 0 ? m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_height : height;
        const auto premultiplied =
            [](const XMFLOAT4& color) noexcept
            {
                return XMFLOAT4{
                    color.x * color.w,
                    color.y * color.w,
                    color.z * color.w,
                    color.w
                };
            };
        const auto drawRectangle =
            [this, &premultiplied](
                SpriteBatch& sprites,
                const float x,
                const float y,
                const float width,
                const float height,
                const XMFLOAT4& color)
            {
                const auto tint =
                    premultiplied(color);
                sprites.Draw(
                    m_whiteTexture.Get(),
                    XMFLOAT2{ x, y },
                    nullptr,
                    XMLoadFloat4(&tint),
                    0.0f,
                    XMFLOAT2{},
                    XMFLOAT2{
                        std::max(width, 0.0f),
                        std::max(height, 0.0f)
                    });
            };

        auto& sprites = BeginSprites();
        drawRectangle(
            sprites,
            0.0f,
            0.0f,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight),
            settings.backgroundColor);

        const float barWidth =
            std::min(
                std::clamp(
                    static_cast<float>(
                        canvasWidth) * 0.58f,
                    240.0f,
                    760.0f),
                static_cast<float>(
                    canvasWidth) * 0.9f);
        const float barHeight = 18.0f;
        const float barX =
            (static_cast<float>(canvasWidth) - barWidth)
            * 0.5f;
        const float barY =
            static_cast<float>(canvasHeight) * 0.62f;
        drawRectangle(
            sprites,
            barX,
            barY,
            barWidth,
            barHeight,
            settings.barBackgroundColor);
        drawRectangle(
            sprites,
            barX,
            barY,
            barWidth
                * std::clamp(progress, 0.0f, 1.0f),
            barHeight,
            settings.barFillColor);

        std::string label = settings.message;
        if (settings.showPercentage)
        {
            label += " ";
            label += std::to_string(
                static_cast<int>(
                    std::lround(
                        std::clamp(
                            progress,
                            0.0f,
                            1.0f)
                        * 100.0f)));
            label += "%";
        }
        const float textWidth =
            std::min(
                static_cast<float>(canvasWidth) * 0.8f,
                720.0f);
        const TextLayoutOptions layout{
            { textWidth, 64.0f },
            TextHorizontalAlignment::Center,
            TextVerticalAlignment::Center,
            false
        };
        const auto text = Assets().LoadTextTexture(
            label,
            "Yu Gothic UI",
            30.0f,
            layout);
        if (text && text->view)
        {
            // 白で生成した文字テクスチャへ描画時の色を掛けます。
            sprites.Draw(
                text->view.Get(),
                XMFLOAT2{
                    (static_cast<float>(canvasWidth)
                        - static_cast<float>(
                            text->width))
                        * 0.5f,
                    barY - 86.0f
                },
                nullptr,
                PremultipliedTextColor(
                    settings.textColor));
        }
        EndSprites();
    }

    void GraphicsDevice::DrawStartupLogo(
        const std::filesystem::path& logoPath,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (logoPath.empty() || !Assets().FileExists(logoPath))
        {
            return;
        }

        std::shared_ptr<const TextureAsset> logo;
        try
        {
            // テクスチャは最初のフレームでキャッシュされるため、起動処理で
            // 小さな画像を読み込むのは一度だけです。
            logo = Assets().LoadTexture(logoPath);
        }
        catch (const std::exception&)
        {
            return;
        }
        if (!logo || !logo->view || logo->width == 0 || logo->height == 0)
        {
            return;
        }

        const std::uint32_t canvasWidth =
            width == 0 ? m_width : width;
        const std::uint32_t canvasHeight =
            height == 0 ? m_height : height;
        const float maximumWidth = std::min(
            static_cast<float>(canvasWidth) * 0.32f,
            360.0f);
        const float maximumHeight = std::min(
            static_cast<float>(canvasHeight) * 0.42f,
            360.0f);
        const float scale = std::min(
            maximumWidth / static_cast<float>(logo->width),
            maximumHeight / static_cast<float>(logo->height));
        const float drawWidth =
            static_cast<float>(logo->width) * scale;
        const float drawHeight =
            static_cast<float>(logo->height) * scale;
        const float x =
            (static_cast<float>(canvasWidth) - drawWidth) * 0.5f;
        const float y =
            static_cast<float>(canvasHeight) * 0.30f
            - drawHeight * 0.5f;

        auto& sprites = BeginSprites();
        const DirectX::XMFLOAT4 white{ 1.0f, 1.0f, 1.0f, 1.0f };
        sprites.Draw(
            logo->view.Get(),
            DirectX::XMFLOAT2{ x, y },
            nullptr,
            DirectX::XMLoadFloat4(&white),
            0.0f,
            DirectX::XMFLOAT2{},
            DirectX::XMFLOAT2{ scale, scale });
        EndSprites();
    }

    void GraphicsDevice::RefreshMemoryStatistics(
        const bool force) noexcept
    {
        try
        {
            const auto now = std::chrono::steady_clock::now();
            if (!force
                && m_lastMemoryStatisticsSample
                    != std::chrono::steady_clock::time_point{}
                && now - m_lastMemoryStatisticsSample
                    < std::chrono::milliseconds(500))
            {
                return;
            }
            m_lastMemoryStatisticsSample = now;

            PROCESS_MEMORY_COUNTERS_EX process{};
            process.cb = sizeof(process);
            if (GetProcessMemoryInfo(
                    GetCurrentProcess(),
                    reinterpret_cast<
                        PROCESS_MEMORY_COUNTERS*>(&process),
                    sizeof(process)))
            {
                m_memoryStatistics.processWorkingSetBytes =
                    static_cast<std::uint64_t>(
                        process.WorkingSetSize);
                m_memoryStatistics.processPrivateBytes =
                    static_cast<std::uint64_t>(
                        process.PrivateUsage);
            }

            MEMORYSTATUSEX system{};
            system.dwLength = sizeof(system);
            if (GlobalMemoryStatusEx(&system))
            {
                m_memoryStatistics.systemPhysicalTotalBytes =
                    system.ullTotalPhys;
                m_memoryStatistics.systemPhysicalUsedBytes =
                    system.ullTotalPhys - system.ullAvailPhys;
            }

            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (!m_device
                || FAILED(m_device.As(&dxgiDevice))
                || FAILED(dxgiDevice->GetAdapter(
                    adapter.GetAddressOf()))
                || !adapter)
            {
                return;
            }

            DXGI_ADAPTER_DESC description{};
            if (SUCCEEDED(adapter->GetDesc(&description)))
            {
                m_memoryStatistics.dedicatedVideoMemoryBytes =
                    static_cast<std::uint64_t>(
                        description.DedicatedVideoMemory);
                m_memoryStatistics.sharedSystemMemoryBytes =
                    static_cast<std::uint64_t>(
                        description.SharedSystemMemory);
            }

            Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
            if (FAILED(adapter.As(&adapter3)) || !adapter3)
            {
                m_memoryStatistics.videoMemoryBudgetAvailable = false;
                return;
            }
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
            const bool hasLocal = SUCCEEDED(
                adapter3->QueryVideoMemoryInfo(
                    0,
                    DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                    &local));
            const bool hasNonLocal = SUCCEEDED(
                adapter3->QueryVideoMemoryInfo(
                    0,
                    DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                    &nonLocal));
            m_memoryStatistics.videoMemoryBudgetAvailable =
                hasLocal || hasNonLocal;
            if (hasLocal)
            {
                m_memoryStatistics.localVideoMemoryUsageBytes =
                    local.CurrentUsage;
                m_memoryStatistics.localVideoMemoryBudgetBytes =
                    local.Budget;
            }
            if (hasNonLocal)
            {
                m_memoryStatistics.nonLocalVideoMemoryUsageBytes =
                    nonLocal.CurrentUsage;
                m_memoryStatistics.nonLocalVideoMemoryBudgetBytes =
                    nonLocal.Budget;
            }
        }
        catch (...)
        {
            // 性能表示の失敗で描画を止めません。
        }
    }

    void GraphicsDevice::SetGraphicsSettings(
        const GraphicsSettings& settings)
    {
        const auto clamped =
            ClampGraphicsSettings(settings);
        const bool recreateShadows =
            clamped.shadowsEnabled
                != m_graphicsSettings.shadowsEnabled
            || clamped.shadowResolution
                != m_graphicsSettings.shadowResolution
            || clamped.shadowCascadeLimit
                != m_graphicsSettings.shadowCascadeLimit;
        m_graphicsSettings = clamped;
        // 以降に読み込まれるテクスチャへ圧縮設定を反映します
        // （生成済みテクスチャはそのまま）。
        if (auto* assets = TryAssets())
        {
            assets->SetRuntimeTextureCompressionEnabled(
                clamped.runtimeTextureCompression);
        }
        if (IsInitialized() && recreateShadows)
        {
            m_shadowMap =
                std::make_unique<ShadowMap>();
            m_spotShadowMap =
                std::make_unique<ShadowMap>();
            m_pointShadowMap =
                std::make_unique<ShadowMap>();
            if (m_graphicsSettings.shadowsEnabled)
            {
                m_shadowMap->Initialize(
                    m_device.Get(),
                    m_graphicsSettings.shadowResolution,
                    m_graphicsSettings.shadowCascadeLimit);
                const std::uint32_t
                    localShadowResolution = std::max(
                        m_graphicsSettings
                            .shadowResolution / 2u,
                        256u);
                m_spotShadowMap->Initialize(
                    m_device.Get(),
                    localShadowResolution,
                    static_cast<std::uint32_t>(
                        MaximumSpotShadows));
                m_pointShadowMap->Initialize(
                    m_device.Get(),
                    localShadowResolution,
                    6u,
                    true);
            }
        }
    }

    void GraphicsDevice::ApplyQualityPreset(
        const GraphicsQualityPreset preset)
    {
        SetGraphicsSettings(
            GraphicsSettingsForPreset(preset));
    }

    float GraphicsDevice::AspectRatio() const noexcept
    {
        return static_cast<float>(m_width) / static_cast<float>(std::max(m_height, 1u));
    }

    void GraphicsDevice::SetSprite2DOffset(
        const DirectX::XMFLOAT2& offset) noexcept
    {
        m_sprite2DOffset = offset;
    }

    const DirectX::XMFLOAT2& GraphicsDevice::Sprite2DOffset() const noexcept
    {
        return m_sprite2DOffset;
    }

    std::uint32_t GraphicsDevice::RenderWidth() const noexcept
    {
        return std::max(
            static_cast<std::uint32_t>(
                std::lround(
                    static_cast<float>(m_width)
                    * m_graphicsSettings.renderScale)),
            1u);
    }

    std::uint32_t GraphicsDevice::RenderHeight() const noexcept
    {
        return std::max(
            static_cast<std::uint32_t>(
                std::lround(
                    static_cast<float>(m_height)
                    * m_graphicsSettings.renderScale)),
            1u);
    }

    AssetManager& GraphicsDevice::Assets() const
    {
        return m_services->EnsureAssets(m_device.Get(), m_context.Get(),
            m_graphicsSettings.runtimeTextureCompression);
    }

    AssetManager* GraphicsDevice::TryAssets() const noexcept
    {
        return m_services->TryAssets();
    }

    AudioSystem& GraphicsDevice::Audio() const
    {
        return m_services->Audio();
    }

    InputSystem& GraphicsDevice::Input() const
    {
        return m_services->Input();
    }

    DirectX::CommonStates& GraphicsDevice::States() const
    {
        if (!m_commonStates)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *m_commonStates;
    }

    ID3D11BlendState* GraphicsDevice::AdditiveBlendPreservingAlpha() const
    {
        if (!m_additiveBlendPreservingAlpha)
        {
            m_additiveBlendPreservingAlpha =
                CreateAdditiveBlendPreservingAlpha(m_device.Get());
        }
        return m_additiveBlendPreservingAlpha.Get();
    }

    DebugRenderer& GraphicsDevice::Debug() const
    {
        if (!m_debugRenderer)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *m_debugRenderer;
    }

    namespace
    {
        // 失敗した組み込みシェーダーの再試行間隔です。連続コンパイルを避けつつ、
        // 修正後に復帰できる時間として設定します。
        constexpr double BuiltInRetrySeconds = 2.0;

        [[nodiscard]] double SteadySeconds() noexcept
        {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count();
        }
    }

    // 組み込みシェーダーの失敗を記録して毎フレームの再コンパイルを防ぎ、
    // 再試行間隔後に復帰を試みます。代替描画経路が無いため失敗は送出し、
    // Application側が描画失敗を処理してエディターUIを継続します。
    template <typename T, typename Factory>
    T& GraphicsDevice::BuildBuiltIn(
        std::unique_ptr<T>& slot,
        BuiltInFailure& failure,
        Factory&& factory) const
    {
        if (slot)
        {
            return *slot;
        }
        const double now = SteadySeconds();
        if (!failure.message.empty()
            && now - failure.lastAttempt < BuiltInRetrySeconds)
        {
            // 再試行時刻までは記録済みの失敗を返し、コンパイルを省略します。
            throw std::runtime_error(failure.message);
        }
        failure.lastAttempt = now;
        try
        {
            slot = factory();
        }
        catch (const std::exception& exception)
        {
            // ログはApplicationの描画ループが1箇所で出します
            // （組み込みシェーダー以外の描画失敗も同じ扱いに
            // したいので、種類ごとに書き分けません）。
            failure.message = exception.what();
            throw;
        }
        failure.message.clear();
        return *slot;
    }

    EnvironmentRenderer&
        GraphicsDevice::Environment() const
    {
        return BuildBuiltIn(
            m_environmentRenderer,
            m_environmentFailure,
            [this]
            {
                return std::make_unique<EnvironmentRenderer>(
                    m_device.Get(),
                    m_context.Get(),
                    Assets(),
                    Assets().ResolvePath(
                        "shaders/LamaPonEnvironment.hlsl"));
            });
    }

    ClusteredLights& GraphicsDevice::Clusters() const
    {
        if (!m_clusteredLights)
        {
            // カリングCSがプロジェクトに無い場合は、互換性維持のため
            // エンジン同梱のアセットから読み込みます。
            constexpr const char* relativePath =
                "shaders/LamaPonLightCulling.hlsl";
            auto shaderPath =
                Assets().ResolvePath(relativePath);
            if (!Assets().FileExists(shaderPath))
            {
                shaderPath = ExecutableDirectory()
                    / "assets"
                    / relativePath;
            }
            return BuildBuiltIn(
                m_clusteredLights,
                m_clustersFailure,
                [this, shaderPath]
                {
                    return std::make_unique<ClusteredLights>(
                        m_device.Get(),
                        Assets(),
                        shaderPath);
                });
        }
        return *m_clusteredLights;
    }

    LitEffect& GraphicsDevice::Lit() const
    {
        return BuildBuiltIn(
            m_litEffect,
            m_litFailure,
            [this]
            {
                return std::make_unique<LitEffect>(
                    m_device.Get(),
                    m_context.Get(),
                    Assets(),
                    Assets().ResolvePath(
                        "shaders/LamaPonLit.hlsl"));
            });
    }

    LitEffect& GraphicsDevice::SkinnedLit() const
    {
        return BuildBuiltIn(
            m_skinnedLitEffect,
            m_skinnedLitFailure,
            [this]
            {
                return std::make_unique<LitEffect>(
                    m_device.Get(),
                    m_context.Get(),
                    Assets(),
                    Assets().ResolvePath(
                        "shaders/LamaPonLit.hlsl"),
                    true);
            });
    }

    LitEffect* GraphicsDevice::ShaderErrorPlaceholder(
        const bool skinned) const
    {
        auto& effect =
            skinned ? m_skinnedErrorEffect : m_errorEffect;
        auto& unavailable = skinned
            ? m_skinnedErrorEffectUnavailable
            : m_errorEffectUnavailable;
        // 代替シェーダーを設定した回数をFrameStatisticsへ記録します。
        if (effect)
        {
            ++m_frameStatistics.shaderFallbackDraws;
            return effect.get();
        }
        if (unavailable)
        {
            return nullptr;
        }

        // プロジェクトに代替シェーダーが無い場合はエンジン同梱版を使い、
        // プロジェクトの版に関係なく同じ失敗表示を提供します。
        constexpr const char* relativePath =
            "shaders/LamaPonShaderError.hlsl";
        auto shaderPath = Assets().ResolvePath(relativePath);
        if (!Assets().FileExists(shaderPath))
        {
            shaderPath =
                ExecutableDirectory() / "assets" / relativePath;
        }

        try
        {
            effect = std::make_unique<LitEffect>(
                m_device.Get(),
                m_context.Get(),
                Assets(),
                shaderPath,
                skinned);
        }
        catch (const std::exception&)
        {
            // 代役すら用意できないときは標準Litのままにします。
            // 知らせ方が無いだけで、描画は続けられます。
            unavailable = true;
            return nullptr;
        }
        ++m_frameStatistics.shaderFallbackDraws;
        return effect.get();
    }

    SpriteEffect* GraphicsDevice::SpriteErrorPlaceholder() const
    {
        // 3D側と同じく、渡した回数を数えます
        // （FrameStatistics::shaderFallbackDrawsを参照）。
        if (m_spriteErrorEffect)
        {
            ++m_frameStatistics.shaderFallbackDraws;
            return m_spriteErrorEffect.get();
        }
        if (m_spriteErrorEffectUnavailable)
        {
            return nullptr;
        }

        constexpr const char* relativePath =
            "shaders/LamaPonSpriteError.hlsl";
        auto shaderPath = Assets().ResolvePath(relativePath);
        if (!Assets().FileExists(shaderPath))
        {
            shaderPath =
                ExecutableDirectory() / "assets" / relativePath;
        }

        try
        {
            m_spriteErrorEffect =
                std::make_unique<SpriteEffect>(
                    m_device.Get(),
                    m_context.Get(),
                    Assets(),
                    shaderPath);
        }
        catch (const std::exception&)
        {
            m_spriteErrorEffectUnavailable = true;
            return nullptr;
        }
        ++m_frameStatistics.shaderFallbackDraws;
        return m_spriteErrorEffect.get();
    }

    bool GraphicsDevice::IsShaderCompiling(
        const std::filesystem::path& shaderPath,
        const ShaderKeywordSet& keywords) const
    {
        if (shaderPath.empty())
        {
            return false;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        const auto variantKey = normalized.Key();
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        const auto found = m_materialShaders.find(cacheKey);
        return found != m_materialShaders.end()
            && found->second->pending;
    }

    const ShaderVariantDeclaration&
        GraphicsDevice::ShaderVariantsFor(
            const std::filesystem::path& shaderPath) const
    {
        static const ShaderVariantDeclaration empty;
        if (shaderPath.empty())
        {
            return empty;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        const auto found = m_shaderVariants.find(absolutePath);
        if (found != m_shaderVariants.end())
        {
            return found->second;
        }
        ShaderVariantDeclaration declaration;
        try
        {
            if (Assets().FileExists(absolutePath))
            {
                const auto source =
                    Assets().ReadFileBytes(absolutePath);
                declaration = ParseShaderVariants(
                    std::string_view{
                        reinterpret_cast<const char*>(
                            source.data()),
                        source.size() });
            }
        }
        catch (const std::exception&)
        {
            // 読み取り失敗時は宣言なしとして扱い、Inspectorの表示を継続します。
            declaration = {};
        }
        return m_shaderVariants
            .emplace(absolutePath, std::move(declaration))
            .first->second;
    }

    LitEffect& GraphicsDevice::MaterialShader(
        const std::filesystem::path& shaderPath,
        std::uint64_t& generation,
        std::string& error,
        const ShaderKeywordSet& keywords) const
    {
        generation = 0;
        error.clear();
        if (shaderPath.empty())
        {
            return Lit();
        }

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 宣言に無いキーワードは落とします。シェーダーを差し替えた
        // 後のマテリアルが、存在しないキーワードでコンパイルを
        // 走らせないようにするためです。
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        // 同じHLSLでもバリアントごとに別のエントリーです。キーは
        // 「パス?キーワード」で、キーワードは常に整列済みなので
        // 同じ組み合わせなら必ず同じキーになります。
        const auto variantKey = normalized.Key();
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        auto& entry = m_materialShaders[cacheKey];
        if (!entry)
        {
            entry = std::make_unique<MaterialShaderEntry>();
            entry->keywords = normalized.Keywords();
        }

        // コンパイル失敗を明示するため標準Litではなくマゼンタの代替表示を使います。
        const auto resolve = [this, &entry]() -> LitEffect&
        {
            if (entry->effect)
            {
                return *entry->effect;
            }
            if (!entry->error.empty())
            {
                if (auto* const placeholder =
                        ShaderErrorPlaceholder(false))
                {
                    return *placeholder;
                }
            }
            return Lit();
        };

        // 非同期コンパイル完了後にLitEffectを組み立てます
        // （キャッシュに当たるので一瞬で終わります）。
        if (entry->pending)
        {
            if (entry->warming.valid()
                && entry->warming.wait_for(
                        std::chrono::seconds(0))
                    == std::future_status::ready)
            {
                entry->warming.get();
                entry->pending = false;
                try
                {
                    entry->effect =
                        std::make_unique<LitEffect>(
                            m_device.Get(),
                            m_context.Get(),
                            Assets(),
                            absolutePath,
                            false,
                            entry->keywords);
                    entry->generation =
                        ++m_materialShaderGeneration;
                    entry->error.clear();
                }
                catch (const std::exception& exception)
                {
                    entry->error = DescribeShaderFailure(
                        Assets(),
                        absolutePath,
                        exception.what(),
                        ShaderUsage::Material);
                }
            }
            else
            {
                // 非同期コンパイルの完了までは標準Litで描画を継続します。
                generation = entry->generation;
                error.clear();
                return Lit();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (entry->observed
            && !entry->forceReload
            && now < entry->nextCheck)
        {
            generation = entry->generation;
            error = entry->error;
            return resolve();
        }
        entry->nextCheck = now + std::chrono::milliseconds(250);

        // アーカイブで配布したゲームには変更監視の対象となる展開済みファイルが
        // ありません。ホットリロードの更新日時確認はエディター上の展開済み
        // アセットだけに行い、アーカイブ内のシェーダーは一度だけ読み込みます。
        const bool archived = Assets().IsArchived();
        std::error_code fileError;
        const bool sourceExists = Assets().FileExists(absolutePath);
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(
                absolutePath,
                fileError)
            : std::filesystem::file_time_type{};
        const bool changed = !entry->observed
            || entry->forceReload
            || entry->sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry->writeTime != writeTime);
        if (changed)
        {
            entry->observed = true;
            entry->forceReload = false;
            entry->sourceExists = sourceExists;
            entry->writeTime = writeTime;
            if (!sourceExists)
            {
                entry->error =
                    "Shader file was not found: "
                    + PathToUtf8(absolutePath);
                entry->effect.reset();
            }
            else
            {
                try
                {
                    // アーカイブ（書き出したゲーム）は全部事前
                    // コンパイル済みなので待ち時間が無く、かつ
                    // アーカイブ読み取りはスレッド安全ではないため
                    // 同期のままにします。
                    if (m_asyncShaderCompilation
                        && !Assets().IsArchived())
                    {
                        // effectを破棄すると、失敗時は代替表示へ切り替わります。
                        auto* const assets = &Assets();
                        const auto path = absolutePath;
                        const auto keywordList = entry->keywords;
                        entry->effect.reset();
                        entry->pending = true;
                        entry->error.clear();
                        entry->warming = std::async(
                            std::launch::async,
                            [assets, path, keywordList]
                            {
                                WarmShaderCache(
                                    *assets,
                                    path,
                                    keywordList);
                            });
                        return Lit();
                    }
                    auto candidate = std::make_unique<LitEffect>(
                        m_device.Get(),
                        m_context.Get(),
                        Assets(),
                        absolutePath,
                        false,
                        entry->keywords);
                    entry->effect = std::move(candidate);
                    entry->generation =
                        ++m_materialShaderGeneration;
                    entry->error.clear();
                }
                catch (const std::exception& exception)
                {
                    entry->error = DescribeShaderFailure(
                        Assets(),
                        absolutePath,
                        exception.what(),
                        ShaderUsage::Material);
                    // 再コンパイル失敗を視認できるよう、直前のシェーダーを破棄して
                    // 代替表示へ切り替えます。
                    entry->effect.reset();
                }
            }
        }

        generation = entry->generation;
        error = entry->error;
        return resolve();
    }

    LitEffect* GraphicsDevice::SkinnedMaterialShader(
        const std::filesystem::path& shaderPath,
        std::uint64_t& generation,
        std::string& error,
        const ShaderKeywordSet& keywords) const
    {
        generation = 0;
        error.clear();
        if (shaderPath.empty())
        {
            return nullptr;
        }

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 通常マテリアルと同じく、バリアントごとに別エントリーです。
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        const auto variantKey = normalized.Key();
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        auto& entry = m_skinnedMaterialShaders[cacheKey];
        if (!entry)
        {
            entry = std::make_unique<MaterialShaderEntry>();
            entry->keywords = normalized.Keywords();
        }

        // 通常マテリアルと同じく、失敗時はマゼンタの代替表示を使い、
        // シェーダーを作成できなかったモデルを画面上で特定できます。
        const auto resolve = [this, &entry]() -> LitEffect*
        {
            if (entry->effect)
            {
                return entry->effect.get();
            }
            if (!entry->error.empty())
            {
                return ShaderErrorPlaceholder(true);
            }
            return nullptr;
        };

        const auto now = std::chrono::steady_clock::now();
        if (entry->observed
            && !entry->forceReload
            && now < entry->nextCheck)
        {
            generation = entry->generation;
            error = entry->error;
            return resolve();
        }
        entry->nextCheck = now + std::chrono::milliseconds(250);

        // アーカイブで配布したゲームには変更監視の対象となる展開済みファイルが
        // ありません。ホットリロードの更新日時確認はエディター上の展開済み
        // アセットだけに行い、アーカイブ内のシェーダーは一度だけ読み込みます。
        const bool archived = Assets().IsArchived();
        std::error_code fileError;
        const bool sourceExists = Assets().FileExists(absolutePath);
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(
                absolutePath,
                fileError)
            : std::filesystem::file_time_type{};
        const bool changed = !entry->observed
            || entry->forceReload
            || entry->sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry->writeTime != writeTime);
        if (changed)
        {
            entry->observed = true;
            entry->forceReload = false;
            entry->sourceExists = sourceExists;
            entry->writeTime = writeTime;
            if (!sourceExists)
            {
                entry->error =
                    "Shader file was not found: "
                    + PathToUtf8(absolutePath);
                entry->effect.reset();
            }
            else
            {
                try
                {
                    auto candidate = std::make_unique<LitEffect>(
                        m_device.Get(),
                        m_context.Get(),
                        Assets(),
                        absolutePath,
                        true,
                        entry->keywords);
                    entry->effect = std::move(candidate);
                    entry->generation =
                        ++m_materialShaderGeneration;
                    entry->error.clear();
                }
                catch (const std::exception& exception)
                {
                    entry->error = DescribeShaderFailure(
                        Assets(),
                        absolutePath,
                        exception.what(),
                        ShaderUsage::Material);
                    // 通常マテリアルと同じく、失敗したら直前の
                    // シェーダーは残しません。
                    entry->effect.reset();
                }
            }
        }

        generation = entry->generation;
        error = entry->error;
        return resolve();
    }

    void GraphicsDevice::InvalidateMaterialShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 宣言そのものも読み直します（multi_compileの行を
        // 足し引きしたときに追従するため）。
        m_shaderVariants.erase(absolutePath);
        // バリアントごとに別エントリーなので、そのHLSLから作られた
        // ものを全部立て直します（キーは「パス?キーワード」）。
        const auto prefix = absolutePath.wstring();
        for (auto& [key, value] : m_materialShaders)
        {
            const auto text = key.wstring();
            if (text == prefix
                || (text.rfind(prefix, 0) == 0
                    && text.size() > prefix.size()
                    && text[prefix.size()] == L'?'))
            {
                value->forceReload = true;
            }
        }
        for (auto& [key, value] : m_skinnedMaterialShaders)
        {
            const auto text = key.wstring();
            if (text == prefix
                || (text.rfind(prefix, 0) == 0
                    && text.size() > prefix.size()
                    && text[prefix.size()] == L'?'))
            {
                value->forceReload = true;
            }
        }
    }

    void GraphicsDevice::InvalidateSpriteShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        const auto found =
            m_spriteShaders.find(absolutePath);
        if (found != m_spriteShaders.end())
        {
            found->second->forceReload = true;
        }
    }

    ShadowMap& GraphicsDevice::Shadows() const
    {
        if (!m_shadowMap)
        {
            throw std::logic_error(
                "GraphicsDevice has not been initialized.");
        }

        return *m_shadowMap;
    }

    ShadowMap& GraphicsDevice::SpotShadows() const
    {
        if (!m_spotShadowMap)
        {
            throw std::logic_error(
                "GraphicsDevice has not been initialized.");
        }

        return *m_spotShadowMap;
    }

    ShadowMap& GraphicsDevice::PointShadows() const
    {
        if (!m_pointShadowMap)
        {
            throw std::logic_error(
                "GraphicsDevice has not been initialized.");
        }

        return *m_pointShadowMap;
    }

    void GraphicsDevice::CreateSizeDependentResources()
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(
            m_swapChain->GetBuffer(
                0,
                IID_PPV_ARGS(backBuffer.ReleaseAndGetAddressOf())),
            "IDXGISwapChain::GetBuffer");

        ThrowIfFailed(
            m_device->CreateRenderTargetView(
                backBuffer.Get(),
                nullptr,
                m_renderTargetView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRenderTargetView");

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = m_width;
        depthDescription.Height = m_height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        ThrowIfFailed(
            m_device->CreateTexture2D(
                &depthDescription,
                nullptr,
                m_depthTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(depth)");

        ThrowIfFailed(
            m_device->CreateDepthStencilView(
                m_depthTexture.Get(),
                nullptr,
                m_depthStencilView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateDepthStencilView");

        m_viewport.TopLeftX = 0.0f;
        m_viewport.TopLeftY = 0.0f;
        m_viewport.Width = static_cast<float>(m_width);
        m_viewport.Height = static_cast<float>(m_height);
        m_viewport.MinDepth = 0.0f;
        m_viewport.MaxDepth = 1.0f;
    }

    void GraphicsDevice::CreateWhiteTexture()
    {
        constexpr std::uint32_t whitePixel = 0xffffffffu;

        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = 1;
        textureDescription.Height = 1;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = &whitePixel;
        initialData.SysMemPitch = sizeof(whitePixel);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            m_device->CreateTexture2D(
                &textureDescription,
                &initialData,
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(white)");

        ThrowIfFailed(
            m_device->CreateShaderResourceView(
                texture.Get(),
                nullptr,
                m_whiteTexture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(white)");
    }
}
