#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsDeviceShaderState.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Input/InputSystem.h"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

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

    RenderingApi GraphicsDevice::ActiveRenderingApi() const noexcept
    {
        return m_backend != nullptr
            ? m_backend->Api()
            : RenderingApi::DirectX11;
    }

    bool GraphicsDevice::TearingAllowed() const noexcept
    {
        return m_backend != nullptr
            && m_backend->TearingAllowed();
    }

    bool GraphicsDevice::IsInitialized() const noexcept
    {
        return m_backend != nullptr
            && m_backend->IsInitialized();
    }

    GraphicsDevice::GraphicsDevice()
        : m_resourceLeaseState(CreateResourceLeaseState(this))
        , m_services(std::make_unique<RuntimeServices>())
    {
    }
    GraphicsDevice::~GraphicsDevice()
    {
        CloseResourceLeaseGate();
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
        ReleaseResources(false);
    }

    void GraphicsDevice::QuiesceResourceWork() noexcept
    {
        // Shader workers borrow AssetManager, so join them while both the
        // AssetManager and graphics backend are still alive.
        const auto waitForShaders = [](auto& entries) noexcept
        {
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
                    // Resource release continues; clearing the entry below
                    // consumes any stored compilation failure.
                }
            }
        };
        waitForShaders(m_materialShaders);
        waitForShaders(m_skinnedMaterialShaders);

        if (m_services)
        {
            m_services->QuiesceGraphicsWork();
        }
    }

    void GraphicsDevice::ReleaseResources(
        const bool preserveAudio) noexcept
    {
        // No backend state is cleared until every worker that borrows the
        // current AssetManager or Device has stopped. Initialize closes the
        // resource lease gate before entering this phase.
        QuiesceResourceWork();

        // Backend所有のGPU計測driverより先に非所有参照を外します。
        m_gpuProfiler.Detach();
        if (m_backend)
        {
            // BackendのDevice/Contextを借りている高レベル資源より先に
            // 描画状態だけを解除し、COM本体は最後まで保持します。
            m_backend->PrepareForResourceRelease();
        }
        m_shadowMap.reset();
        m_spotShadowMap.reset();
        m_pointShadowMap.reset();
        m_instanceBuffer.Reset();
        m_depthPass = DepthPassKind::None;
        m_skinnedMaterialShaders.clear();
        m_materialShaders.clear();
        m_spriteShaders.clear();
        m_screenShaders.clear();
        m_computeShaders.clear();
        m_queuedScreenEffects.clear();
        m_litEffect.reset();
        m_skinnedLitEffect.reset();
        m_errorEffect.reset();
        m_skinnedErrorEffect.reset();
        m_spriteErrorEffect.reset();
        m_errorEffectUnavailable = false;
        m_skinnedErrorEffectUnavailable = false;
        m_spriteErrorEffectUnavailable = false;
        m_litFailure = {};
        m_skinnedLitFailure = {};
        m_environmentFailure = {};
        m_clustersFailure = {};
        m_sceneCompositionTarget.reset();
        ClearRenderTextures();
        m_environmentRenderer.reset();
        m_clusteredLights.reset();
        m_debugRenderer.reset();
        if (preserveAudio)
        {
            m_services->PrepareForGraphicsReinitialization();
        }
        else
        {
            m_services->Shutdown();
        }
        ResetApiResources();
        m_whiteTextureView.Reset();
        m_whiteTexture.Reset();
        m_lightingState = {};
        if (m_backend)
        {
            m_backend->Shutdown();
            m_backend.reset();
        }
        m_width = 0;
        m_height = 0;
        m_uiWidth = 0;
        m_uiHeight = 0;
        m_sprite2DOffset = {};
        m_sceneProjection = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        m_frameStatistics = {};
        m_memoryStatistics = {};
        m_lastMemoryStatisticsSample = {};
    }

    void GraphicsDevice::Initialize(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        Initialize(
            window,
            width,
            height,
            RenderingApi::DirectX11);
    }

    void GraphicsDevice::Initialize(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height,
        RenderingApi requestedApi)
    {
        // Sceneや独自rendererが旧Device資源を持つ間は、何も破棄する
        // 前に拒否します。描画API変更はプロセス再起動で反映する契約です。
        BeginResourceTransition();

        // 再初期化では、旧Deviceから作った高レベル資源を先にすべて
        // 破棄します。Backendだけを差し替えると、旧DeviceのSRVや
        // BlendStateが新しいContextへ残り得るためです。
        ReleaseResources(true);

        try
        {
            InitializeResources(
                window,
                width,
                height,
                requestedApi);
        }
        catch (...)
        {
            // 部分初期化したBackendや高レベル資源を残さず、
            // IsInitialized()が失敗後にtrueを返すことも防ぎます。
            // Audio is API-independent and remains available for a later
            // recovery attempt even when graphics initialization fails.
            ReleaseResources(true);
            EndResourceTransition();
            throw;
        }
        EndResourceTransition();
    }

    void GraphicsDevice::InitializeResources(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height,
        RenderingApi requestedApi)
    {

        // Backend選択はここへ集約します。ProjectSettingsや各起動経路は
        // 要求値を渡すだけにし、実効APIとフォールバック理由を一箇所で
        // 決定します。
        const GraphicsBackendSelection selection =
            SelectGraphicsBackend(requestedApi);
        switch (selection.fallbackReason)
        {
        case RenderingApiFallbackReason::None:
            break;
        case RenderingApiFallbackReason::NotImplemented:
            Logger::Instance().Warning(
                "DirectX 12 Experimentalは未実装のため、"
                "DirectX 11へフォールバックして起動します。");
            break;
        case RenderingApiFallbackReason::UnknownApi:
            Logger::Instance().Warning(
                "不明なRendering APIが指定されたため、"
                "DirectX 11へフォールバックして起動します。");
            break;
        case RenderingApiFallbackReason::Unsupported:
            Logger::Instance().Warning(
                "選択されたRendering APIを現在の環境で使用できないため、"
                "DirectX 11へフォールバックして起動します。");
            break;
        case RenderingApiFallbackReason::InitializationFailed:
            Logger::Instance().Warning(
                "選択されたRendering APIの初期化に失敗したため、"
                "DirectX 11へフォールバックして起動します。");
            break;
        }
        m_startupRenderingApi = selection.requestedApi;
        m_renderingApiFallbackReason =
            selection.fallbackReason;
        m_graphicsSettings.renderingApi =
            selection.requestedApi;

        m_width = std::max(width, 1u);
        m_height = std::max(height, 1u);
        m_uiWidth = m_width;
        m_uiHeight = m_height;
        m_sprite2DOffset = {};

        // 同じGraphicsDeviceを再初期化する場合も、旧Backendを
        // 破棄する前にprofilerの非所有参照を外します。
        m_gpuProfiler.Detach();
        m_backend = CreateGraphicsBackend(
            selection.activeApi);
        m_backend->Initialize(GraphicsBackendCreateInfo{
            static_cast<void*>(window),
            m_width,
            m_height,
            s_preferWarpAdapter,
            s_enableDebugLayer
        });
        m_gpuProfiler.Attach(
            m_backend->ProfilerBackend());

        RefreshMemoryStatistics(true);
        // エディター外でもFPS制限の状態を確認できるよう、ログへ記録します。
        if (!TearingAllowed())
        {
            Logger::Instance().Info(
                "ティアリング許可が使えない環境です。VSyncを切っても"
                "モニターのリフレッシュレートがFPSの上限になります。");
        }
        CreateWhiteTexture();
        CreateApiResources(m_backend->Api());
        m_services->Initialize(Device(), Context(), window,
            m_graphicsSettings.runtimeTextureCompression,
            *m_backend);
        m_debugRenderer = std::make_unique<DebugRenderer>(
            m_backend->CreateDebugDrawingBackend());
        m_shadowMap = std::make_unique<ShadowMap>();
        m_spotShadowMap = std::make_unique<ShadowMap>();
        m_pointShadowMap = std::make_unique<ShadowMap>();
        if (m_graphicsSettings.shadowsEnabled)
        {
            m_backend->InitializeShadowMap(
                *m_shadowMap,
                m_graphicsSettings.shadowResolution,
                m_graphicsSettings.shadowCascadeLimit,
                false);
            // スポット/ポイントはカスケードより解像度を落とします。
            const std::uint32_t localShadowResolution =
                std::max(
                    m_graphicsSettings.shadowResolution
                        / 2u,
                    256u);
            m_backend->InitializeShadowMap(
                *m_spotShadowMap,
                localShadowResolution,
                static_cast<std::uint32_t>(
                    MaximumSpotShadows),
                false);
            m_backend->InitializeShadowMap(
                *m_pointShadowMap,
                localShadowResolution,
                6u,
                true);
        }
        m_sceneCompositionTarget =
            std::make_unique<RenderTarget>();
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

            if (!m_backend)
            {
                return;
            }
            const auto video =
                m_backend->QueryVideoMemoryStatistics();
            if (!video.adapterAvailable)
            {
                return;
            }
            if (video.descriptionAvailable)
            {
                m_memoryStatistics.dedicatedVideoMemoryBytes =
                    video.dedicatedBytes;
                m_memoryStatistics.sharedSystemMemoryBytes =
                    video.sharedSystemBytes;
            }
            m_memoryStatistics.videoMemoryBudgetAvailable =
                video.localBudgetAvailable
                || video.nonLocalBudgetAvailable;
            if (video.localBudgetAvailable)
            {
                m_memoryStatistics.localVideoMemoryUsageBytes =
                    video.localUsageBytes;
                m_memoryStatistics.localVideoMemoryBudgetBytes =
                    video.localBudgetBytes;
            }
            if (video.nonLocalBudgetAvailable)
            {
                m_memoryStatistics.nonLocalVideoMemoryUsageBytes =
                    video.nonLocalUsageBytes;
                m_memoryStatistics.nonLocalVideoMemoryBudgetBytes =
                    video.nonLocalBudgetBytes;
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
                m_backend->InitializeShadowMap(
                    *m_shadowMap,
                    m_graphicsSettings.shadowResolution,
                    m_graphicsSettings.shadowCascadeLimit,
                    false);
                const std::uint32_t
                    localShadowResolution = std::max(
                        m_graphicsSettings
                            .shadowResolution / 2u,
                        256u);
                m_backend->InitializeShadowMap(
                    *m_spotShadowMap,
                    localShadowResolution,
                    static_cast<std::uint32_t>(
                        MaximumSpotShadows),
                    false);
                m_backend->InitializeShadowMap(
                    *m_pointShadowMap,
                    localShadowResolution,
                    6u,
                    true);
            }
        }
    }

    void GraphicsDevice::ApplyQualityPreset(
        const GraphicsQualityPreset preset)
    {
        auto settings = GraphicsSettingsForPreset(preset);
        settings.renderingApi =
            m_graphicsSettings.renderingApi;
        SetGraphicsSettings(settings);
    }

    float GraphicsDevice::AspectRatio() const noexcept
    {
        return static_cast<float>(m_width) / static_cast<float>(std::max(m_height, 1u));
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
        if (m_backend != nullptr)
        {
            return m_services->EnsureAssets(
                Device(),
                Context(),
                m_graphicsSettings.runtimeTextureCompression,
                *m_backend);
        }
        return m_services->EnsureAssets(
            Device(),
            Context(),
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

    DebugRenderer& GraphicsDevice::Debug() const
    {
        if (!m_debugRenderer)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *m_debugRenderer;
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

    void GraphicsDevice::CreateWhiteTexture()
    {
        constexpr std::array<std::uint8_t, 4> white{
            0xffu, 0xffu, 0xffu, 0xffu };
        auto texture = m_backend->CreateSolidRgba8Texture(white);
        auto view = m_backend->CreateShaderResourceView(texture);
        m_whiteTexture = std::move(texture);
        m_whiteTextureView = std::move(view);
    }
}
