#include "LamaPon/Core/Application.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/BuildInfo.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Core/RuntimeIntegrity.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Online/OnlinePersistenceCoordinator.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Online/NetworkSceneBridge.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"
#include "LamaPon/Scripting/GameModuleHost.h"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace
{
    // 目標FPSまで待機します(frameStart: フレーム開始時刻, targetFrameRate: 目標FPS、0は制限なし)。
    void PaceFrame(
        const std::chrono::steady_clock::time_point frameStart,
        const std::uint32_t targetFrameRate)
    {
        if (targetFrameRate == 0)
        {
            return;
        }

        // 目標フレーム間隔
        const auto frameDuration =
            std::chrono::duration<double>(
                1.0
                / static_cast<double>(targetFrameRate));
        // フレーム終了の目標時刻
        const auto deadline =
            frameStart
            + std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                    frameDuration);
        // スピン待機への切替余裕
        constexpr auto spinMargin =
            std::chrono::microseconds(500);
        // スリープ終了時刻
        const auto sleepDeadline =
            deadline - spinMargin;
        if (std::chrono::steady_clock::now()
            < sleepDeadline)
        {
            std::this_thread::sleep_until(
                sleepDeadline);
        }
        while (std::chrono::steady_clock::now()
            < deadline)
        {
            std::this_thread::yield();
        }
    }
}

namespace LamaPon
{
    Application::Application(
        std::wstring title,
        const std::uint32_t width,
        const std::uint32_t height,
        std::string persistenceName)
        : m_window(title, width, height)
        , m_persistenceName(
            persistenceName.empty()
                ? WideToUtf8(title)
                : std::move(persistenceName))
    {
    }

    void Application::ReportRenderFailure(
        const std::string& message)
    {
        if (m_lastRenderFailure == message)
        {
            return;
        }
        m_lastRenderFailure = message;
        Logger::Instance().Error(
            "描画に失敗したため、このフレームをスキップしました。"
            "アプリケーションは動作を継続します: "
            + message);
    }

    Application::~Application()
    {
        Logger::Instance().Info(
            "LamaPonを終了します。");
        // 破棄済みサービスへスクリプトが触れないよう、共有参照を先に解除します。
        if (m_onlineServices
            && ActiveOnlineServices() == m_onlineServices.get())
        {
            SetActiveOnlineServices(nullptr);
        }
        if (m_playerPrefs
            && ActivePlayerPrefs() == m_playerPrefs.get())
        {
            SetActivePlayerPrefs(nullptr);
        }
        if (m_onlineServices)
        {
            static_cast<void>(
                Detail::OnlinePersistenceAccess::Detach(
                    *m_onlineServices));
            // アカウント保存の保留復旧を破棄前に一度再試行します。
            Detail::OnlinePersistenceAccess::EndFrame(
                *m_onlineServices);
            // 終了前の保存復旧を管理する参照
            const auto* const persistence =
                Detail::OnlinePersistenceAccess::Coordinator(
                    *m_onlineServices);
            if (persistence
                && persistence->HasPendingRecovery())
            {
                Logger::Instance().Error(
                    "アカウントのPlayerPrefsを終了前に保存できませんでした。"
                    "ゲストデータへは安全に復帰しています。");
            }
        }
        if (m_playerPrefs
            && m_playerPrefs->IsDirty())
        {
            try
            {
                m_playerPrefs->Save();
                Logger::Instance().Info(
                    "PlayerPrefsを保存しました。");
            }
            // 終了前の設定保存の失敗理由
            catch (const std::exception& exception)
            {
                Logger::Instance().Error(
                    std::string(
                        "PlayerPrefsを保存できませんでした: ")
                    + exception.what());
            }
        }
        m_window.SetMessageCallback({});
        m_layer.reset();
        if (m_networkSceneBridge) m_networkSceneBridge->Reset();
        m_networkSceneBridge.reset();
        m_networkSession.reset();
        m_scene.reset();
        m_gameModule.reset();
        m_onlineServices.reset();
        m_saveData.reset();
        m_playerPrefs.reset();
        m_graphics.Shutdown();

        if (m_comInitialized)
        {
            CoUninitialize();
        }
        Logger::Instance().CloseFile();
    }

    void Application::Initialize(const HINSTANCE instance)
    {
        Initialize(instance, RenderingApi::DirectX11);
    }

    void Application::Initialize(
        const HINSTANCE instance,
        const RenderingApi requestedApi)
    {
        Initialize(
            instance,
            requestedApi,
            GraphicsStartupProfile::FullRenderer);
    }

    void Application::Initialize(
        const HINSTANCE instance,
        const RenderingApi requestedApi,
        const GraphicsStartupProfile startupProfile)
    {
        // 実行バイナリの親ディレクトリ
        const auto executableDirectory = ExecutableDirectory();
        if (executableDirectory.empty())
        {
            throw std::runtime_error("GetModuleFileNameW failed.");
        }
        // 描画初期化の警告も記録できるよう、デバイス作成前に診断出力を開きます。
        static_cast<void>(
            Logger::Instance().SetFilePath(
                executableDirectory
                    / L"LamaPon.log"));

        // エディターのシェルダイアログを使えるよう、COMをSTAで初期化します。
        // COM初期化の結果コード
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(comResult))
        {
            m_comInitialized = true;
        }
        else if (comResult != RPC_E_CHANGED_MODE)
        {
            throw std::runtime_error("CoInitializeEx failed.");
        }

        // 最新の描画サイズを予約します(width: 変更後の幅ピクセル, height: 変更後の高さピクセル)。
        m_window.SetResizeCallback(
            [this](const std::uint32_t width, const std::uint32_t height)
            {
                if (width != 0 && height != 0)
                {
                    // Win32のメッセージ処理中にはGPU資源を作り直さず、次の描画前に最後のサイズだけを適用します。
                    m_pendingWidth = width;
                    m_pendingHeight = height;
                    m_resizePending = true;
                }
            });

        // レイヤーへ終了可否を確認し、拒否された場合はウィンドウを閉じません。
        m_window.SetCloseCallback(
            [this]
            {
                return m_layer == nullptr
                    || m_layer->ConfirmClose();
            });

        m_window.Create(instance);
        m_graphics.Initialize(
            m_window.Handle(),
            m_window.ClientWidth(),
            m_window.ClientHeight(),
            requestedApi,
            startupProfile);
        m_resizePending = false;
        m_graphics.Assets().SetAssetRoot(
            executableDirectory / L"assets");
        // 起動時のコンパイルを省くため、同梱のシェーダーキャッシュを検索対象にします。
        AddShaderCacheSearchDirectory(
            executableDirectory / L"shader-cache");
        Logger::Instance().Info(
            "LamaPonを初期化しました: "
            + FormatBuildLabel());

        // このゲームのユーザー保存領域
        const auto userData =
            UserDataDirectory(m_persistenceName);
        m_playerPrefs =
            std::make_unique<PlayerPrefs>(
                userData / L"PlayerPrefs.json");
        m_saveData =
            std::make_unique<SaveDataStore>(
                userData / L"Saves");
        try
        {
            m_playerPrefs->Load();
        }
        // 起動時の設定読み込みの失敗理由
        catch (const std::exception& exception)
        {
            Logger::Instance().Warning(
                std::string(
                    "PlayerPrefsを読み込めないため、"
                    "元ファイルを変更せず空の設定を使用します: ")
                    + exception.what());
        }

        m_onlineServices = std::make_unique<OnlineServices>();
        Detail::OnlinePersistenceAccess::Attach(
            *m_onlineServices,
            *m_playerPrefs,
            *m_saveData,
            userData);
        // スクリプトが使う設定とオンラインサービスの共有参照を登録します。
        SetActivePlayerPrefs(m_playerPrefs.get());
        SetActiveOnlineServices(m_onlineServices.get());

        m_gameModule =
            std::make_unique<GameModuleHost>();
        // 配布物の改ざん検知結果の理由
        std::string integrityReason;
        // 書き出し済み配布物では、ランタイムとGame Moduleの整合性を検証してから読み込みます。
        if (!RuntimeIntegrity::VerifyExportedArtifacts(
                executableDirectory,
                integrityReason))
        {
            Logger::Instance().Warning(
                "配布物の整合性検証に失敗したため、"
                "Game Moduleを読み込みません: "
                + integrityReason);
        }
        else if (!m_gameModule->Load(
                executableDirectory
                    / L"LamaPonGameModule.dll"))
        {
            Logger::Instance().Warning(
                "LamaPonGameModule.dll was not loaded: "
                + m_gameModule->LastError());
        }
        else
        {
            Logger::Instance().Info(
                "Game Moduleを読み込みました。");
        }
        m_scene = std::make_unique<Scene>(m_graphics);
        m_networkSession = std::make_unique<NetworkSession>();
        m_networkSceneBridge = std::make_unique<NetworkSceneBridge>(*m_scene, *m_networkSession);
        SetActiveNetworkSession(m_networkSession.get());
        SetActiveNetworkSceneBridge(m_networkSceneBridge.get());
        // ゲーム表示サイズを変更します(width: 幅ピクセル, height: 高さピクセル)。
        // 現在のゲーム表示サイズを幅と高さの組で返します。
        m_scene->SetWindowSizeCallbacks(
            [this](const std::uint32_t width, const std::uint32_t height)
            {
                return SetWindowSize(width, height);
            },
            [this]
            {
                return WindowSize();
            });
    }

    bool Application::SetWindowSize(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (width == 0 || height == 0
            || width > 16384 || height > 16384)
        {
            return false;
        }
        if (m_layer)
        {
            return m_layer->IsPlaying()
                && m_layer->SetGameViewSize(width, height);
        }
        return m_window.SetClientSize(width, height);
    }

    std::pair<std::uint32_t, std::uint32_t>
        Application::WindowSize() const noexcept
    {
        if (m_layer && m_layer->IsPlaying())
        {
            return m_layer->GameViewSize();
        }
        return { m_window.ClientWidth(), m_window.ClientHeight() };
    }

    void Application::ApplyPendingResize()
    {
        if (!m_resizePending || !m_graphics.IsInitialized())
        {
            return;
        }
        m_graphics.Resize(m_pendingWidth, m_pendingHeight);
        m_resizePending = false;
    }

    void Application::AttachLayer(
        std::unique_ptr<ApplicationLayer> layer)
    {
        if (!m_scene)
        {
            throw std::logic_error(
                "Application::Initialize must be called before AttachLayer.");
        }
        if (!layer)
        {
            throw std::invalid_argument(
                "Application::AttachLayer requires a valid layer.");
        }

        m_layer = std::move(layer);

        // メッセージをレイヤーへ転送します(window: 受信ウィンドウ, message: メッセージ番号, wParam: 第1引数, lParam: 第2引数)。
        m_window.SetMessageCallback(
            [this](const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam)
            {
                return m_layer != nullptr
                    && m_layer->HandleMessage(window, message, wParam, lParam);
            });
    }

    void Application::SetStartupSplashScreenEnabled(
        const bool enabled) noexcept
    {
        m_startupSplashScreenEnabled = enabled;
    }

    int Application::Run()
    {
        if (!m_scene)
        {
            throw std::logic_error("Application::Initialize must be called before Run.");
        }

        // メインループで処理するメッセージ
        MSG message{};
        // 前フレームの開始時刻
        auto previousTime = std::chrono::steady_clock::now();
        while (message.message != WM_QUIT)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }

            // 通信が停止中の最小化では、次のウィンドウイベントまで待機します。
            if (m_window.IsMinimized()
                && (!m_networkSession || m_networkSession->State() == NetworkState::Stopped
                    || m_networkSession->State() == NetworkState::Error))
            {
                WaitMessage();
                previousTime = std::chrono::steady_clock::now();
                continue;
            }

            // 現在のフレームの開始時刻
            const auto currentTime = std::chrono::steady_clock::now();
            // 前フレームからの経過秒数
            const std::chrono::duration<float> elapsed = currentTime - previousTime;
            previousTime = currentTime;
            // 通信などに渡す実経過秒数
            const float rawDeltaTime =
                std::max(elapsed.count(), 0.0f);
            // 最長0.1秒に制限した経過時間
            const float deltaTime =
                std::min(rawDeltaTime, 0.1f);
            Time::Detail::AdvanceFrame(deltaTime);
            Profiler::Instance().BeginFrame();

            {
                LAMAPON_PROFILE_SCOPE("Audio");
                try
                {
                    static_cast<void>(
                        m_graphics.Audio().Update());
                }
                // 音声更新に失敗した理由
                catch (const std::exception& exception)
                {
                    // デバイス切り替え後の復旧中は、オーディオエンジンから例外が送出されることがあります。
                    Logger::Instance().Warning(
                        std::string{ "音声の更新に失敗しました: " }
                        + exception.what());
                }
            }
            {
                LAMAPON_PROFILE_SCOPE("Online");
                if (m_networkSession) m_networkSession->Update(rawDeltaTime);
                if (m_onlineServices)
                {
                    m_onlineServices->Update(rawDeltaTime);
                }
            }
            {
                LAMAPON_PROFILE_SCOPE("GameModule");
                if (m_gameModule)
                {
                    m_gameModule->PollHotReload(deltaTime);
                }
            }

            {
                LAMAPON_PROFILE_SCOPE("Editor");
                if (m_layer && !m_window.IsMinimized())
                {
                    m_layer->BeginFrame();
                    m_layer->Draw();
                }
            }

            {
                LAMAPON_PROFILE_SCOPE("Input");
                // レイヤーから受け取る入力状態
                InputSnapshot remoteInput;
                if (m_layer != nullptr
                    && m_layer->ConsumeInputSnapshot(remoteInput))
                {
                    m_graphics.Input().UpdateFromSnapshot(
                        remoteInput);
                }
                else
                {
                    m_graphics.Input().Update(
                        !m_layer || !m_layer->WantsKeyboard());
                }
            }
            {
                LAMAPON_PROFILE_SCOPE("Simulation");
                // 今フレームにゲーム更新を進めるか
                // 一時停止中の1回更新要求はここで消費します。
                const bool shouldSimulate =
                    m_layer == nullptr
                    || (m_layer->IsPlaying()
                        && (!m_layer->IsPaused()
                            || m_layer
                                ->ConsumeSimulationStep()));
                if (shouldSimulate)
                {
                    if (m_networkSceneBridge) m_networkSceneBridge->BeforeSimulation(rawDeltaTime);
                    // ゲーム更新には時間倍率適用済みの経過秒数を渡します。
                    m_scene->Update(Time::DeltaTime());
                    if (m_networkSceneBridge) m_networkSceneBridge->AfterSimulation(rawDeltaTime);
                }
            }

            if (!m_window.IsMinimized())
            {
                LAMAPON_PROFILE_SCOPE("Render");
                ApplyPendingResize();
                if (m_layer)
                {
                    // 描画失敗を記録し、エディターUIを継続します。
                    try
                    {
                        m_layer->RenderSceneViews();
                    }
                    // 描画失敗の診断内容
                    catch (const std::exception& exception)
                    {
                        ReportRenderFailure(exception.what());
                    }
                    m_graphics.BeginFrame(m_clearColor);
                    m_layer->Render();
                }
                else
                {
                    m_graphics.BeginFrame(m_clearColor);
                    // 描画失敗を記録し、ゲームの更新を継続します。
                    try
                    {
                        m_scene->RenderGameFrame(m_clearColor);
                        // シーン切替の管理
                        const auto& scenes =
                            m_scene->Scenes();
                        // 読み込み画面はUIの上に重ね、遷移の覆いはScene側で描きます。
                        m_graphics.DrawLoadingScreen(
                            scenes.TransitionFrame(),
                            scenes.LoadingScreen());
                        if (scenes.IsLoading())
                        {
                            if (m_startupSplashScreenEnabled)
                            {
                                m_graphics.DrawStartupLogo();
                            }
                        }
                        else if (m_startupSplashScreenEnabled)
                        {
                            // 初回の読み込みが終われば、以後の読み込み画面では起動ロゴを表示しません。
                            m_startupSplashScreenEnabled = false;
                        }
                        // デバッグ表示の更新には時間倍率を掛けない実経過時間を使います。
                        m_debugOverlay.Update(
                            m_graphics,
                            *m_scene,
                            rawDeltaTime);
                    }
                    // 描画失敗の診断内容
                    catch (const std::exception& exception)
                    {
                        ReportRenderFailure(exception.what());
                    }
                }
                m_graphics.EndFrame();
            }

            // このフレームで確定した保存を、次のネットワーク処理より前に同期層へ渡します。
            if (m_onlineServices)
            {
                Detail::OnlinePersistenceAccess::EndFrame(
                    *m_onlineServices);
            }

            // CPUによるフレーム処理の終了時刻
            const auto cpuEnd =
                std::chrono::steady_clock::now();
            // 待機を除くCPU処理時間、ミリ秒
            const float cpuMilliseconds =
                std::chrono::duration<float, std::milli>(
                    cpuEnd - currentTime).count();
            m_graphics.RecordFrameStatistics(
                rawDeltaTime,
                cpuMilliseconds);
            Profiler::Instance().EndFrame();
            // 描画設定の目標フレーム数毎秒
            const auto requestedRate = m_graphics.Settings().targetFrameRate;
            // 最小化時のフレーム上限、毎秒
            // 最小化中はVSync待機がないため、処理頻度を最大60Hzに制限します。
            const auto frameRate = m_window.IsMinimized()
                ? (requestedRate == 0 ? 60u : std::min(requestedRate, 60u)) : requestedRate;
            PaceFrame(currentTime, frameRate);
        }

        return static_cast<int>(message.wParam);
    }

    Scene& Application::ActiveScene() const
    {
        if (!m_scene)
        {
            throw std::logic_error("Application has not been initialized.");
        }

        return *m_scene;
    }

    PlayerPrefs& Application::Preferences() const
    {
        if (!m_playerPrefs)
        {
            throw std::logic_error(
                "Application has not been initialized.");
        }
        return *m_playerPrefs;
    }

    SaveDataStore& Application::Saves() const
    {
        if (!m_saveData)
        {
            throw std::logic_error(
                "Application has not been initialized.");
        }
        return *m_saveData;
    }

    OnlineServices& Application::Online() const
    {
        if (!m_onlineServices)
        {
            throw std::logic_error(
                "Application has not been initialized.");
        }
        return *m_onlineServices;
    }

    InputSystem& Application::Input() const
    {
        return m_graphics.Input();
    }

    GameModuleHost& Application::GameModule() const
    {
        if (!m_gameModule)
        {
            throw std::logic_error(
                "Application has not been initialized.");
        }
        return *m_gameModule;
    }

    const DirectX::Keyboard::State&
        Application::KeyboardState() const
    {
        return m_graphics.Input().KeyboardState();
    }

    void Application::SetClearColor(
        const float red,
        const float green,
        const float blue,
        const float alpha) noexcept
    {
        m_clearColor[0] = red;
        m_clearColor[1] = green;
        m_clearColor[2] = blue;
        m_clearColor[3] = alpha;
    }
}

namespace LamaPon
{
    NetworkSession& Application::Network() const
    {
        if (!m_networkSession) throw std::logic_error("Application is not initialized.");
        return *m_networkSession;
    }
    NetworkSceneBridge& Application::NetworkScene() const
    {
        if (!m_networkSceneBridge) throw std::logic_error("Application is not initialized.");
        return *m_networkSceneBridge;
    }
}
