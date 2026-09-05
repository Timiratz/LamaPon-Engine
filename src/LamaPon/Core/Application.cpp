#include "LamaPon/Core/Application.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Input/InputSystem.h"
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
    void PaceFrame(
        const std::chrono::steady_clock::time_point frameStart,
        const std::uint32_t targetFrameRate)
    {
        if (targetFrameRate == 0)
        {
            return;
        }

        const auto frameDuration =
            std::chrono::duration<double>(
                1.0
                / static_cast<double>(targetFrameRate));
        const auto deadline =
            frameStart
            + std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                    frameDuration);
        constexpr auto spinMargin =
            std::chrono::microseconds(500);
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
        // 同じ失敗を毎フレーム出しません。壊れている間、ログが
        // 埋まって他が読めなくなるためです。直って再び壊れたときは
        // 内容が変わるので、また出ます。
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
        // 先に登録を外します。破棄済みのPlayerPrefsへScriptが
        // 触れないようにするためです。
        SetActivePlayerPrefs(nullptr);
        if (m_playerPrefs
            && m_playerPrefs->IsDirty())
        {
            try
            {
                m_playerPrefs->Save();
                Logger::Instance().Info(
                    "PlayerPrefsを保存しました。");
            }
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
        m_scene.reset();
        m_gameModule.reset();
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
        // エディターが使うフォルダー選択などのシェルダイアログは、呼び出し元の
        // スレッドがシングルスレッドアパートメントであることを前提とします。
        // COINIT_MULTITHREADEDではSHBrowseForFolderWが応答しなくなります。
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(comResult))
        {
            m_comInitialized = true;
        }
        else if (comResult != RPC_E_CHANGED_MODE)
        {
            throw std::runtime_error("CoInitializeEx failed.");
        }

        m_window.SetResizeCallback(
            [this](const std::uint32_t width, const std::uint32_t height)
            {
                if (m_graphics.IsInitialized())
                {
                    m_graphics.Resize(width, height);
                }
            });

        // 閉じる前にレイヤー（エディター等）へ確認します。
        // 未保存の変更がある場合はここで警告が出ます。
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
            m_window.ClientHeight());
        const auto executableDirectory = ExecutableDirectory();
        if (executableDirectory.empty())
        {
            throw std::runtime_error("GetModuleFileNameW failed.");
        }
        m_graphics.Assets().SetAssetRoot(
            executableDirectory / L"assets");
        // 書き出し時に同梱した事前コンパイル済みシェーダー。これが
        // あると、プレイヤーの初回起動でもコンパイルが1本も走りません
        // （無ければ従来どおり初回だけコンパイルして、以後は
        // %LOCALAPPDATA%側のキャッシュが効きます）。
        AddShaderCacheSearchDirectory(
            executableDirectory / L"shader-cache");
        static_cast<void>(
            Logger::Instance().SetFilePath(
                executableDirectory
                    / L"LamaPon.log"));
        Logger::Instance().Info(
            "LamaPonを初期化しました。");

        const auto userData =
            UserDataDirectory(m_persistenceName);
        m_playerPrefs =
            std::make_unique<PlayerPrefs>(
                userData / L"PlayerPrefs.json");
        m_saveData =
            std::make_unique<SaveDataStore>(
                userData / L"Saves");
        // C++ Scriptから設定値を読み書きできるように登録します
        // （Scriptはここを通してハイスコア等を保存します）。
        SetActivePlayerPrefs(m_playerPrefs.get());
        try
        {
            m_playerPrefs->Load();
        }
        catch (const std::exception& exception)
        {
            m_playerPrefs->DeleteAll();
            Logger::Instance().Warning(
                std::string(
                    "PlayerPrefsを読み込めないため、"
                    "空の設定を使用します: ")
                + exception.what());
        }

        m_gameModule =
            std::make_unique<GameModuleHost>();
        if (!m_gameModule->Load(
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

        MSG message{};
        auto previousTime = std::chrono::steady_clock::now();

        while (message.message != WM_QUIT)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }

            const auto currentTime = std::chrono::steady_clock::now();
            const std::chrono::duration<float> elapsed = currentTime - previousTime;
            previousTime = currentTime;
            const float rawDeltaTime =
                std::max(elapsed.count(), 0.0f);
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
                catch (const std::exception& exception)
                {
                    // デバイス切り替え後の復旧中は、オーディオエンジンから
                    // 例外が送出されることがあります。
                    Logger::Instance().Warning(
                        std::string{ "音声の更新に失敗しました: " }
                        + exception.what());
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
                if (m_layer)
                {
                    m_layer->BeginFrame();
                    m_layer->Draw();
                }
            }

            {
                LAMAPON_PROFILE_SCOPE("Input");
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
                // 一時停止中は更新を通しません。ただし「次のフレーム」が
                // 押されていれば1回だけ通します（要求はここで消費）。
                const bool shouldSimulate =
                    m_layer == nullptr
                    || (m_layer->IsPlaying()
                        && (!m_layer->IsPaused()
                            || m_layer
                                ->ConsumeSimulationStep()));
                if (shouldSimulate)
                {
                    // timeScale適用済みのdeltaTimeで
                    // ゲームプレイを進めます。
                    m_scene->Update(Time::DeltaTime());
                }
            }

            {
                LAMAPON_PROFILE_SCOPE("Render");
                if (m_layer)
                {
                    // シーン描画の例外はここで処理し、エディターUIを
                    // 継続します。ログを確認しながら原因を修正でき、
                    // 修正後はGraphicsDeviceが描画資源を再作成します。
                    try
                    {
                        m_layer->RenderSceneViews();
                    }
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
                    // ゲーム実行時も描画失敗をログに記録し、更新処理を
                    // 継続します。
                    try
                    {
                        m_graphics.BeginSceneComposition(
                            m_clearColor);
                        // 3DだけをHDRターゲットへ描き、UIは色変換後に
                        // 重ねる。ターゲットを渡すのは、深度プリパスと
                        // SSAOをライティングより前に走らせるためです。
                        m_scene->RenderMainCamera(
                            m_graphics.AspectRatio(),
                            false,
                            m_graphics.SceneCompositionTarget());
                        m_graphics.EndSceneComposition(
                            m_scene->PostProcessFrameData());
                        m_scene->Render2D();
                        const auto& scenes =
                            m_scene->Scenes();
                        if (scenes.IsLoading())
                        {
                            m_graphics.DrawLoadingScreen(
                                scenes.LoadProgress(),
                                scenes.LoadingScreen());
                            if (m_startupSplashScreenEnabled)
                            {
                                m_graphics.DrawStartupLogo();
                            }
                        }
                        else if (m_startupSplashScreenEnabled)
                        {
                            // 起動ロゴを表示するのは初回だけです。以降のシーン
                            // 読み込みでは通常の読み込み画面を使います。
                            m_startupSplashScreenEnabled = false;
                        }
                        // F1でデバッグオーバーレイを表示します
                        // （timeScaleの影響を受けないよう実時間で更新）。
                        m_debugOverlay.Update(
                            m_graphics,
                            *m_scene,
                            rawDeltaTime);
                    }
                    catch (const std::exception& exception)
                    {
                        ReportRenderFailure(exception.what());
                    }
                }
                m_graphics.EndFrame();
            }

            const auto cpuEnd =
                std::chrono::steady_clock::now();
            const float cpuMilliseconds =
                std::chrono::duration<float, std::milli>(
                    cpuEnd - currentTime).count();
            m_graphics.RecordFrameStatistics(
                rawDeltaTime,
                cpuMilliseconds);
            Profiler::Instance().EndFrame();
            PaceFrame(
                currentTime,
                m_graphics.Settings()
                    .targetFrameRate);
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
