#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsBackendPackage.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/RenderTarget.h"
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
    // 初期化指定の実体はEXEとDLLで共有するためDLL内だけに定義します。
    // ソフトウェア描画の優先指定
    bool GraphicsDevice::s_preferWarpAdapter = false;
    // 検証レイヤーの有効化指定
    bool GraphicsDevice::s_enableDebugLayer = false;

    // 次回初期化のアダプター優先指定を設定します(prefer: ソフトウェア描画の優先)。
    void GraphicsDevice::SetPreferWarpAdapter(
        const bool prefer) noexcept
    {
        s_preferWarpAdapter = prefer;
    }

    // 次回初期化の検証レイヤー指定を設定します(enable: 有効化するか)。
    void GraphicsDevice::SetEnableDebugLayer(
        const bool enable) noexcept
    {
        s_enableDebugLayer = enable;
    }

    // 検証レイヤーの初期化指定を返します。
    bool GraphicsDevice::IsDebugLayerEnabled() noexcept
    {
        return s_enableDebugLayer;
    }

    // 実効APIを返し、バックエンドがなければD3D11です。
    RenderingApi GraphicsDevice::ActiveRenderingApi() const noexcept
    {
        return m_state->m_backend != nullptr
            ? m_state->m_backend->Api()
            : RenderingApi::DirectX11;
    }

    // 現在のバックエンドがティアリングを許可するか返します。
    bool GraphicsDevice::TearingAllowed() const noexcept
    {
        return m_state->m_backend != nullptr
            && m_state->m_backend->TearingAllowed();
    }

    // バックエンドの初期化状態を返します。
    bool GraphicsDevice::IsInitialized() const noexcept
    {
        return m_state->m_backend != nullptr
            && m_state->m_backend->IsInitialized();
    }

    // 描画デバイスの共有状態を構築します。
    GraphicsDevice::GraphicsDevice()
        : m_state(std::make_unique<State>(this))
    {
    }
    // リース受付を閉じて全資源を解放します。
    GraphicsDevice::~GraphicsDevice()
    {
        CloseResourceLeaseGate();
        Shutdown();
    }

    // 現在の深度パスを設定します(kind: 深度描画の種類)。
    void GraphicsDevice::SetDepthPass(
        const DepthPassKind kind) noexcept
    {
        m_state->m_depthPass = kind;
    }

    // 現在の深度パスの種類を返します。
    DepthPassKind GraphicsDevice::DepthPass() const noexcept
    {
        return m_state->m_depthPass;
    }

    // 現在が深度のみのパスか返します。
    bool GraphicsDevice::IsDepthOnlyPass() const noexcept
    {
        return m_state->m_depthPass != DepthPassKind::None;
    }

    // シーンのHDR描画先を借用します。
    RenderTarget*
        GraphicsDevice::SceneCompositionTarget() const noexcept
    {
        return m_state->m_sceneCompositionTarget.get();
    }

    // シーンの射影行列を保存します(projection: 現在の射影行列)。
    void GraphicsDevice::SetSceneProjection(
        const DirectX::XMFLOAT4X4& projection) noexcept
    {
        m_state->m_sceneProjection = projection;
    }

    // 保存したシーンの射影行列を参照します。
    const DirectX::XMFLOAT4X4&
        GraphicsDevice::SceneProjection() const noexcept
    {
        return m_state->m_sceneProjection;
    }

    // 現在の描画設定を参照します。
    const GraphicsSettings& GraphicsDevice::Settings() const noexcept
    {
        return m_state->m_graphicsSettings;
    }

    // 起動時に保存した要求APIを返します。
    RenderingApi GraphicsDevice::StartupRenderingApi() const noexcept
    {
        return m_state->m_startupRenderingApi;
    }

    // 起動時の代替API選択理由を返します。
    RenderingApiFallbackReason
        GraphicsDevice::RenderingApiFallback() const noexcept
    {
        return m_state->m_renderingApiFallbackReason;
    }

    // 初期化済みのD3D12バックエンドか返します。
    bool GraphicsDevice::IsD3D12ExperimentalBootstrap() const noexcept
    {
        return IsInitialized()
            && ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental;
    }

    // フレーム時間と描画回数の統計を参照します。
    const FrameStatistics& GraphicsDevice::FrameStats() const noexcept
    {
        return m_state->m_frameStatistics;
    }

    // 最後に採取したメモリ統計を参照します。
    const GraphicsMemoryStatistics&
        GraphicsDevice::MemoryStats() const noexcept
    {
        return m_state->m_memoryStatistics;
    }

    // フレームの代替シェーダー使用回数をリセットします。
    void GraphicsDevice::ResetShaderFallbackDraws() noexcept
    {
        m_state->m_frameStatistics.shaderFallbackDraws = 0;
    }

    // 所有する白い代替画像のハンドルを返します。
    GraphicsTextureHandle
        GraphicsDevice::WhiteTextureHandle() const noexcept
    {
        return m_state->m_whiteTexture;
    }

    // 白い代替画像の読み取りビューを返します。
    GraphicsViewHandle
        GraphicsDevice::WhiteTextureViewHandle() const noexcept
    {
        return m_state->m_whiteTextureView;
    }

    // 所有するGPU計測器を参照します。
    GpuProfiler& GraphicsDevice::Gpu() noexcept
    {
        return m_state->m_gpuProfiler;
    }

    // 所有する描画記録器を参照します。
    FrameDebugger& GraphicsDevice::FrameDebug() noexcept
    {
        return m_state->m_frameDebugger;
    }

    // 非同期シェーダー準備の使用を設定します(enabled: 非同期準備の有効化)。
    void GraphicsDevice::SetAsyncShaderCompilationEnabled(
        const bool enabled) noexcept
    {
        m_state->m_asyncShaderCompilation = enabled;
    }

    // 非同期シェーダー準備の使用指定を返します。
    bool GraphicsDevice::IsAsyncShaderCompilationEnabled() const noexcept
    {
        return m_state->m_asyncShaderCompilation;
    }

    // 現在のライト情報をコピーします(lighting: 描画に使用するライト一覧)。
    void GraphicsDevice::SetLightingState(
        const LightingState& lighting) noexcept
    {
        m_state->m_lightingState = lighting;
    }

    // 保存した現在のライト情報を参照します。
    const LightingState& GraphicsDevice::Lighting() const noexcept
    {
        return m_state->m_lightingState;
    }

    // バックバッファの幅をピクセル単位で返します。
    std::uint32_t GraphicsDevice::Width() const noexcept
    {
        return m_state->m_width;
    }

    // バックバッファの高をピクセル単位で返します。
    std::uint32_t GraphicsDevice::Height() const noexcept
    {
        return m_state->m_height;
    }

    // UIの基準寸法を設定します(width: 幅で0は1, height: 高で0は1)。
    void GraphicsDevice::SetUIViewportSize(
        const std::uint32_t width,
        const std::uint32_t height) noexcept
    {
        m_state->m_uiWidth = width == 0 ? 1 : width;
        m_state->m_uiHeight = height == 0 ? 1 : height;
    }

    // UIの基準幅をピクセル単位で返します。
    std::uint32_t GraphicsDevice::UIWidth() const noexcept
    {
        return m_state->m_uiWidth;
    }

    // UIの基準高をピクセル単位で返します。
    std::uint32_t GraphicsDevice::UIHeight() const noexcept
    {
        return m_state->m_uiHeight;
    }

    // 時間を平滑化してフレーム統計を更新します(frameTimeSeconds: フレーム時間の秒数, cpuTimeMilliseconds: CPU処理時間のミリ秒)。
    void GraphicsDevice::RecordFrameStatistics(
        const float frameTimeSeconds,
        const float cpuTimeMilliseconds) noexcept
    {
        // 下限を補正したフレーム秒数
        const float safeFrameTime =
            std::max(frameTimeSeconds, 0.000001f);
        // 表示時間の平滑化係数
        constexpr float smoothing = 0.1f;
        if (m_state->m_frameStatistics.totalFrames == 0)
        {
            m_state->m_frameStatistics.frameTimeMilliseconds =
                safeFrameTime * 1000.0f;
            m_state->m_frameStatistics.cpuTimeMilliseconds =
                std::max(cpuTimeMilliseconds, 0.0f);
        }
        else
        {
            m_state->m_frameStatistics.frameTimeMilliseconds +=
                (safeFrameTime * 1000.0f
                    - m_state->m_frameStatistics
                        .frameTimeMilliseconds)
                * smoothing;
            m_state->m_frameStatistics.cpuTimeMilliseconds +=
                (std::max(cpuTimeMilliseconds, 0.0f)
                    - m_state->m_frameStatistics
                        .cpuTimeMilliseconds)
                * smoothing;
        }
        m_state->m_frameStatistics.framesPerSecond =
            1000.0f
            / std::max(
                m_state->m_frameStatistics.frameTimeMilliseconds,
                0.001f);
        ++m_state->m_frameStatistics.totalFrames;
    }

    // 音声を含む全資源を停止して解放します。
    void GraphicsDevice::Shutdown() noexcept
    {
        ReleaseResources(false);
    }

    // API資源と実行サービスを借用する作業を停止します。
    void GraphicsDevice::QuiesceResourceWork() noexcept
    {
        // AssetManagerとバックエンドを借用するワーカーを、両者の解放前に停止します。
        if (m_state->m_apiResources)
        {
            m_state->m_apiResources->QuiesceResourceWork();
        }

        if (m_state->m_services)
        {
            m_state->m_services->QuiesceGraphicsWork();
        }
    }

    // 借用作業を停止して高レベル資源から解放します(preserveAudio: 再初期化用に音声を保持)。
    void GraphicsDevice::ReleaseResources(
        const bool preserveAudio) noexcept
    {
        // 再初期化ではリース受付を閉じてから、資源を借用する全ワーカーの停止を待ちます。
        QuiesceResourceWork();

        // バックエンド所有の計測器を破棄する前に借用参照を外します。
        m_state->m_gpuProfiler.Detach();
        if (m_state->m_backend)
        {
            // 高レベル資源より先に描画状態を解除し、DeviceとContextの実体は最後まで保持します。
            m_state->m_backend->PrepareForResourceRelease();
        }
        if (m_state->m_apiResources)
        {
            m_state->m_apiResources->ResetHighLevelResources();
        }
        m_state->m_instanceBuffer.Reset();
        m_state->m_depthPass = DepthPassKind::None;
        m_state->m_lightingState = {};
        m_state->m_clusteredLights.reset();
        m_state->m_clustersFailure = {};
        m_state->m_sceneCompositionTarget.reset();
        ClearRenderTextures();
        m_state->m_debugRenderer.reset();
        if (preserveAudio)
        {
            m_state->m_services->PrepareForGraphicsReinitialization();
        }
        else
        {
            m_state->m_services->Shutdown();
        }
        ResetApiResources();
        m_state->m_whiteTextureView.Reset();
        m_state->m_whiteTexture.Reset();
        if (m_state->m_backend)
        {
            m_state->m_backend->Shutdown();
            m_state->m_backend.reset();
        }
        m_state->m_width = 0;
        m_state->m_height = 0;
        m_state->m_uiWidth = 0;
        m_state->m_uiHeight = 0;
        m_state->m_sprite2DOffset = {};
        m_state->m_sceneProjection = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        m_state->m_frameStatistics = {};
        m_state->m_memoryStatistics = {};
        m_state->m_lastMemoryStatisticsSample = {};
    }

    // D3D11で描画を初期化します(window: 描画先のHWND, width: 初期画像幅, height: 初期画像高)。
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

    // 要求APIで描画を初期化します(window: 描画先のHWND, width: 初期画像幅, height: 初期画像高, requestedApi: 要求する描画API)。
    void GraphicsDevice::Initialize(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height,
        RenderingApi requestedApi)
    {
        Initialize(
            window,
            width,
            height,
            requestedApi,
            GraphicsStartupProfile::FullRenderer);
    }

    // 借用中の資源がないことを確認して初期化します(window: 描画先のHWND, width: 初期画像幅, height: 初期画像高, requestedApi: 要求する描画API, profile: 互換用の起動指定)。
    void GraphicsDevice::Initialize(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height,
        const RenderingApi requestedApi,
        const GraphicsStartupProfile profile)
    {
        // 資源を借用中なら破棄前に拒否し、描画APIの変更はプロセス再起動で反映します。
        BeginResourceTransition();

        // 旧SRVや描画状態を新しいContextへ残さないよう、高レベル資源から解放します。
        ReleaseResources(true);

        try
        {
            InitializeResources(
                window,
                width,
                height,
                requestedApi,
                profile);
        }
        catch (...)
        {
            // 初期化失敗では部分資源を全て解放して未初期化へ戻し、再試行用の音声は保持します。
            ReleaseResources(true);
            EndResourceTransition();
            throw;
        }
        EndResourceTransition();
    }

    // バックエンドと共有資源を生成します(window: 描画先のHWND, width: 初期画像幅で0は1, height: 初期画像高で0は1, requestedApi: 要求する描画API, profile: 互換用の起動指定)。
    // D3D12起動時のruntime_errorでは全資源を解放してからD3D11で再試行します。
    void GraphicsDevice::InitializeResources(
        const HWND window,
        const std::uint32_t width,
        const std::uint32_t height,
        const RenderingApi requestedApi,
        const GraphicsStartupProfile profile)
    {
        // 選択したAPIを初期化します(selection: 起動APIと代替理由)。
        const auto initializeCandidate =
            [this, window, width, height, profile](
                const GraphicsBackendSelection& selection)
        {
            switch (selection.fallbackReason)
            {
            case RenderingApiFallbackReason::None:
                break;
            case RenderingApiFallbackReason::NotImplemented:
                Logger::Instance().Warning(
                    "選択されたRendering APIはこのbuildで未実装のため、"
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
                    "DirectX 12 Experimentalの初期化に失敗したため、"
                    "DirectX 11へフォールバックして起動します。");
                break;
            }
            m_state->m_startupRenderingApi = selection.requestedApi;
            m_state->m_renderingApiFallbackReason =
                selection.fallbackReason;
            m_state->m_graphicsStartupProfile = profile;
            m_state->m_graphicsSettings.renderingApi =
                selection.requestedApi;

            m_state->m_width = std::max(width, 1u);
            m_state->m_height = std::max(height, 1u);
            m_state->m_uiWidth = m_state->m_width;
            m_state->m_uiHeight = m_state->m_height;
            m_state->m_sprite2DOffset = {};

            // バックエンド差し替え前に計測器の借用参照を外します。
            m_state->m_gpuProfiler.Detach();
            m_state->m_backend = CreateGraphicsBackend(
                selection.activeApi);
            m_state->m_backend->Initialize(GraphicsBackendCreateInfo{
                static_cast<void*>(window),
                m_state->m_width,
                m_state->m_height,
                s_preferWarpAdapter,
                s_enableDebugLayer
            });
            m_state->m_gpuProfiler.Attach(
                m_state->m_backend->ProfilerBackend());

            RefreshMemoryStatistics(true);

            if (!TearingAllowed())
            {
                Logger::Instance().Info(
                    "ティアリング許可が使えない環境です。VSyncを切っても"
                    "モニターのリフレッシュレートがFPSの上限になります。");
            }
            // 実効APIのD3D12指定
            const bool d3d12ExperimentalRenderer = selection.activeApi
                == RenderingApi::DirectX12Experimental;
            // 画像指定なしの描画用に、API共通の白い代替画像を生成します。
            CreateWhiteTexture();
            CreateApiResources(m_state->m_backend->Api());
            m_state->m_services->Initialize(
                Device(),
                Context(),
                window,
                m_state->m_graphicsSettings.runtimeTextureCompression,
                *m_state->m_backend);
            m_state->m_debugRenderer = std::make_unique<DebugRenderer>(
                m_state->m_backend->CreateDebugDrawingBackend());
            m_state->m_apiResources->RecreateShadowMaps(
                *m_state->m_backend,
                m_state->m_graphicsSettings);
            m_state->m_sceneCompositionTarget =
                std::make_unique<RenderTarget>();
            if (d3d12ExperimentalRenderer)
            {
                Logger::Instance().Warning(
                    "DirectX 12 Experimental rendererで起動しています。"
                    "GameとEditorのScene、2D/UI、3D Mesh/Model、shadow、"
                    "HDR/post-process、custom shader、particle、debug "
                    "drawingに対応しています。実験的なrendererのため、"
                    "DirectX 11と一致しない拡張機能が残る場合があります。");
            }
        };

        // 要求から解決した起動API
        auto selection = SelectGraphicsBackend(
            requestedApi,
            profile);
        if (selection.activeApi
            == RenderingApi::DirectX12Experimental)
        {
            // バックエンドパッケージの状態
            const auto package = ActivateGraphicsBackendPackage(
                selection.activeApi,
                VersionString);
            if (package.state == GraphicsBackendPackageState::Ready)
            {
                Logger::Instance().Info(
                    "DirectX 12バックエンドパッケージをロードしました: "
                    + package.descriptor.version);
            }
            else if (package.state
                == GraphicsBackendPackageState::Missing)
            {
                // パッケージ未導入の場合だけ、同梱のD3D12実装を使います。
                Logger::Instance().Warning(
                    "DirectX 12バックエンドパッケージが未導入のため、"
                    "移行用の組み込み実装を使用します。");
            }
            else
            {
                Logger::Instance().Warning(
                    "DirectX 12バックエンドパッケージを使用できないため、"
                    "DirectX 11へフォールバックします: "
                    + package.message);
                selection.activeApi = RenderingApi::DirectX11;
                selection.fallbackReason =
                    package.state
                            == GraphicsBackendPackageState::IncompatibleAbi
                        ? RenderingApiFallbackReason::NotImplemented
                        : RenderingApiFallbackReason::Unsupported;
            }
        }
        try
        {
            initializeCandidate(selection);
        }
        // D3D12初期化の再試行対象例外
        catch (const std::runtime_error& exception)
        {
            // D3D11による再試行の有無
            const bool retryWithD3D11 = selection.activeApi
                == RenderingApi::DirectX12Experimental;
            if (!retryWithD3D11)
            {
                throw;
            }

            Logger::Instance().Warning(
                std::string(
                    "DirectX 12 Experimental rendererを開始できないため、"
                    "DirectX 11へフォールバックします: ")
                + exception.what());
            // 借用元を含むD3D12資源を順に全て解放してから、D3D11で再試行します。
            ReleaseResources(true);
            initializeCandidate({
                selection.requestedApi,
                RenderingApi::DirectX11,
                RenderingApiFallbackReason::InitializationFailed
            });
        }
    }

    // 取得できたメモリ統計を500ms間隔で更新します(force: 採取間隔を無視する指定)。
    void GraphicsDevice::RefreshMemoryStatistics(
        const bool force) noexcept
    {
        try
        {
            // 現在の統計採取時刻
            const auto now = std::chrono::steady_clock::now();
            if (!force
                && m_state->m_lastMemoryStatisticsSample
                    != std::chrono::steady_clock::time_point{}
                && now - m_state->m_lastMemoryStatisticsSample
                    < std::chrono::milliseconds(500))
            {
                return;
            }
            m_state->m_lastMemoryStatisticsSample = now;

            // プロセスのメモリ統計
            PROCESS_MEMORY_COUNTERS_EX process{};
            process.cb = sizeof(process);
            if (GetProcessMemoryInfo(
                    GetCurrentProcess(),
                    reinterpret_cast<
                        PROCESS_MEMORY_COUNTERS*>(&process),
                    sizeof(process)))
            {
                m_state->m_memoryStatistics.processWorkingSetBytes =
                    static_cast<std::uint64_t>(
                        process.WorkingSetSize);
                m_state->m_memoryStatistics.processPrivateBytes =
                    static_cast<std::uint64_t>(
                        process.PrivateUsage);
            }

            // 物理メモリの使用状態
            MEMORYSTATUSEX system{};
            system.dwLength = sizeof(system);
            if (GlobalMemoryStatusEx(&system))
            {
                m_state->m_memoryStatistics.systemPhysicalTotalBytes =
                    system.ullTotalPhys;
                m_state->m_memoryStatistics.systemPhysicalUsedBytes =
                    system.ullTotalPhys - system.ullAvailPhys;
            }

            if (!m_state->m_backend)
            {
                return;
            }
            // アダプター容量とOS予算
            const auto video =
                m_state->m_backend->QueryVideoMemoryStatistics();
            if (!video.adapterAvailable)
            {
                return;
            }
            if (video.descriptionAvailable)
            {
                m_state->m_memoryStatistics.dedicatedVideoMemoryBytes =
                    video.dedicatedBytes;
                m_state->m_memoryStatistics.sharedSystemMemoryBytes =
                    video.sharedSystemBytes;
            }
            m_state->m_memoryStatistics.videoMemoryBudgetAvailable =
                video.localBudgetAvailable
                || video.nonLocalBudgetAvailable;
            if (video.localBudgetAvailable)
            {
                m_state->m_memoryStatistics.localVideoMemoryUsageBytes =
                    video.localUsageBytes;
                m_state->m_memoryStatistics.localVideoMemoryBudgetBytes =
                    video.localBudgetBytes;
            }
            if (video.nonLocalBudgetAvailable)
            {
                m_state->m_memoryStatistics.nonLocalVideoMemoryUsageBytes =
                    video.nonLocalUsageBytes;
                m_state->m_memoryStatistics.nonLocalVideoMemoryBudgetBytes =
                    video.nonLocalBudgetBytes;
            }
        }
        catch (...)
        {
            // 性能表示の失敗で描画を止めません。
        }
    }

    // 設定を補正し、必要なら影資源を再生成します(settings: 新しい描画設定)。
    // API変更は再起動で反映し、影の再生成に失敗しても先に保存した設定は戻しません。
    void GraphicsDevice::SetGraphicsSettings(
        const GraphicsSettings& settings)
    {
        // 有効範囲へ補正した描画設定
        const auto clamped =
            ClampGraphicsSettings(settings);
        // 影資源の再生成要否
        const bool recreateShadows =
            clamped.shadowsEnabled
                != m_state->m_graphicsSettings.shadowsEnabled
            || clamped.shadowResolution
                != m_state->m_graphicsSettings.shadowResolution
            || clamped.shadowCascadeLimit
                != m_state->m_graphicsSettings.shadowCascadeLimit;
        m_state->m_graphicsSettings = clamped;
        // 圧縮設定は以後に読む画像へ反映し、生成済み画像は再生成しません。
        // 圧縮設定を反映する資産管理
        if (auto* assets = TryAssets())
        {
            assets->SetRuntimeTextureCompressionEnabled(
                clamped.runtimeTextureCompression);
        }
        if (IsInitialized() && recreateShadows)
        {
            m_state->m_lightingState.directionalShadow.enabled = false;
            m_state->m_lightingState.directionalShadow.texture.Reset();
            // 無効にするスポット影
            for (auto& spotShadow : m_state->m_lightingState.spotShadows)
            {
                spotShadow.enabled = false;
            }
            m_state->m_lightingState.spotShadowTexture.Reset();
            m_state->m_lightingState.pointShadow.enabled = false;
            m_state->m_lightingState.pointShadow.texture.Reset();
            m_state->m_apiResources->RecreateShadowMaps(
                *m_state->m_backend,
                m_state->m_graphicsSettings);
        }
    }

    // 描画APIの指定を保持して品質を設定します(preset: 適用する品質プリセット)。
    void GraphicsDevice::ApplyQualityPreset(
        const GraphicsQualityPreset preset)
    {
        // API指定を保持する品質設定
        auto settings = GraphicsSettingsForPreset(preset);
        settings.renderingApi =
            m_state->m_graphicsSettings.renderingApi;
        SetGraphicsSettings(settings);
    }

    // 画面の幅を高さの下限1で割った縦横比を返します。
    float GraphicsDevice::AspectRatio() const noexcept
    {
        return static_cast<float>(m_state->m_width) / static_cast<float>(std::max(m_state->m_height, 1u));
    }

    // 描画倍率を反映した画像幅を最小1で返します。
    std::uint32_t GraphicsDevice::RenderWidth() const noexcept
    {
        return std::max(
            static_cast<std::uint32_t>(
                std::lround(
                    static_cast<float>(m_state->m_width)
                    * m_state->m_graphicsSettings.renderScale)),
            1u);
    }

    // 描画倍率を反映した画像高を最小1で返します。
    std::uint32_t GraphicsDevice::RenderHeight() const noexcept
    {
        return std::max(
            static_cast<std::uint32_t>(
                std::lround(
                    static_cast<float>(m_state->m_height)
                    * m_state->m_graphicsSettings.renderScale)),
            1u);
    }

    // 必要なら資産管理器を生成して参照します。
    AssetManager& GraphicsDevice::Assets() const
    {
        if (m_state->m_backend != nullptr)
        {
            return m_state->m_services->EnsureAssets(
                Device(),
                Context(),
                m_state->m_graphicsSettings.runtimeTextureCompression,
                *m_state->m_backend);
        }
        return m_state->m_services->EnsureAssets(
            Device(),
            Context(),
            m_state->m_graphicsSettings.runtimeTextureCompression);
    }

    // 生成済みの資産管理器を借用し、なければnullptrです。
    AssetManager* GraphicsDevice::TryAssets() const noexcept
    {
        return m_state->m_services->TryAssets();
    }

    // 実行サービスが所有する音声システムを参照します。
    AudioSystem& GraphicsDevice::Audio() const
    {
        return m_state->m_services->Audio();
    }

    // 実行サービスが所有する入力システムを参照します。
    InputSystem& GraphicsDevice::Input() const
    {
        return m_state->m_services->Input();
    }

    // 線分描画器を参照し、未初期化ならlogic_errorです。
    DebugRenderer& GraphicsDevice::Debug() const
    {
        if (!m_state->m_debugRenderer)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *m_state->m_debugRenderer;
    }

    // 平行光用の影描画先を借用し、未初期化ならlogic_errorです。
    ShadowMap& GraphicsDevice::Shadows() const
    {
        // 借用する種類別の影描画先
        auto* const shadowMap = m_state->m_apiResources
            ? m_state->m_apiResources->TryDirectionalShadowMap()
            : nullptr;
        if (shadowMap == nullptr)
        {
            throw std::logic_error(
                "GraphicsDevice has not been initialized.");
        }

        return *shadowMap;
    }

    // スポット光用の影描画先を借用し、未初期化ならlogic_errorです。
    ShadowMap& GraphicsDevice::SpotShadows() const
    {
        // 借用する種類別の影描画先
        auto* const shadowMap = m_state->m_apiResources
            ? m_state->m_apiResources->TrySpotShadowMap()
            : nullptr;
        if (shadowMap == nullptr)
        {
            throw std::logic_error(
                "GraphicsDevice has not been initialized.");
        }

        return *shadowMap;
    }

    // 点光源用の影描画先を借用し、未初期化ならlogic_errorです。
    ShadowMap& GraphicsDevice::PointShadows() const
    {
        // 借用する種類別の影描画先
        auto* const shadowMap = m_state->m_apiResources
            ? m_state->m_apiResources->TryPointShadowMap()
            : nullptr;
        if (shadowMap == nullptr)
        {
            throw std::logic_error(
                "GraphicsDevice has not been initialized.");
        }

        return *shadowMap;
    }

    // 白い画像とSRVを両方生成してから公開します。
    void GraphicsDevice::CreateWhiteTexture()
    {
        // 不透明な白のRGBA8値
        constexpr std::array<std::uint8_t, 4> white{
            0xffu, 0xffu, 0xffu, 0xffu };
        // 生成する白い画像
        auto texture = m_state->m_backend->CreateSolidRgba8Texture(white);
        // 白い画像の読み取りビュー
        auto view = m_state->m_backend->CreateShaderResourceView(texture);
        m_state->m_whiteTexture = std::move(texture);
        m_state->m_whiteTextureView = std::move(view);
    }
}
