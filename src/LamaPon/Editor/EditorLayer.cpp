#include "LamaPon/Editor/BgmLoopPanel.h"
#include "LamaPon/Editor/EditorLayer.h"
#include "LamaPon/Editor/GameExportDialog.h"

#include "LamaPon/Editor/EditorLayerShared.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/PngWriter.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Editor/UiRecorder.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"

#include <commdlg.h>
// WM_DROPFILES（DragAcceptFiles/DragQueryFileW）とShellExecuteWに必要。
#include <shellapi.h>
#include <imgui.h>
// EditorLayer.hはjson_fwdのみのため、unique_ptr<nlohmann::json>
// メンバーを破棄するこの翻訳単位では完全型が必要です。
#include <nlohmann/json.hpp>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <ImGuizmo.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>

using namespace LamaPon::EditorDetail;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

namespace
{
    constexpr float ToolbarHeight = 52.0f;

    bool HasSceneExtension(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".scene.json");
    }

    bool HasPrefabExtension(const std::filesystem::path& path)
    {
        return Lowercase(LamaPon::PathToUtf8(path.filename())).ends_with(".prefab.json");
    }

    bool LoadJapaneseFont(ImGuiIO& inputOutput)
    {
        constexpr std::array fontCandidates{
            "C:/Windows/Fonts/YuGothM.ttc",
            "C:/Windows/Fonts/meiryo.ttc",
            "C:/Windows/Fonts/msgothic.ttc"
        };

        ImFontConfig fontConfig{};
        fontConfig.FontNo = 0;
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 1;

        for (const char* fontPath : fontCandidates)
        {
            if (std::filesystem::exists(fontPath)
                && inputOutput.Fonts->AddFontFromFileTTF(
                    fontPath,
                    18.0f,
                    &fontConfig,
                    nullptr) != nullptr)
            {
                return true;
            }
        }

        inputOutput.Fonts->AddFontDefault();
        return false;
    }

    std::uint64_t HashProjectMenuManifest(const std::string_view text)
    {
        std::uint64_t value = 1469598103934665603ull;
        for (const unsigned char byte : text)
        {
            value ^= byte;
            value *= 1099511628211ull;
        }
        return value;
    }

    std::vector<std::string> SplitProjectMenuPath(
        const std::string_view path)
    {
        std::vector<std::string> result;
        std::size_t begin = 0;
        while (begin <= path.size())
        {
            const std::size_t separator = path.find('/', begin);
            const std::size_t end = separator == std::string_view::npos
                ? path.size()
                : separator;
            const std::string_view part = path.substr(begin, end - begin);
            if (part.empty() || part == "." || part == "..")
            {
                throw std::runtime_error(
                    "メニューパスに空欄または . / .. は使えません");
            }
            result.emplace_back(part);
            if (result.back().size() > 96u)
            {
                throw std::runtime_error(
                    "メニュー名は96バイト以内にしてください");
            }
            if (separator == std::string_view::npos)
            {
                break;
            }
            begin = separator + 1u;
        }
        if (result.empty() || result.size() > 8u)
        {
            throw std::runtime_error(
                "メニューパスは1～8階層にしてください");
        }
        return result;
    }

    std::wstring QuoteWindowsArgument(const std::wstring_view argument)
    {
        if (argument.empty())
        {
            return L"\"\"";
        }
        if (argument.find_first_of(L" \t\n\v\"")
            == std::wstring_view::npos)
        {
            return std::wstring{ argument };
        }

        std::wstring result{ L'\"' };
        std::size_t backslashes = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2u + 1u, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2u, L'\\');
        result.push_back(L'\"');
        return result;
    }

}

namespace LamaPon
{
    EditorLayer::EditorLayer(
        const HWND window,
        GraphicsDevice& graphics,
        Scene& scene,
        PlayerPrefs& playerPrefs,
        SaveDataStore& saveData,
        std::filesystem::path scenePath,
        std::filesystem::path engineRoot,
        std::string buildConfiguration)
        : m_window(window)
        , m_graphics(graphics)
        , m_scene(scene)
        , m_playerPrefs(playerPrefs)
        , m_saveData(saveData)
        , m_scenePath(std::move(scenePath))
        , m_engineRoot(std::filesystem::weakly_canonical(
            std::move(engineRoot)))
        , m_buildConfiguration(std::move(buildConfiguration))
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        auto& inputOutput = ImGui::GetIO();
        inputOutput.ConfigFlags |=
            ImGuiConfigFlags_NavEnableKeyboard
            | ImGuiConfigFlags_DockingEnable;
        const auto layoutPath =
            EditorSettingsPath().parent_path()
            / "imgui-layout.ini";
        std::filesystem::create_directories(
            layoutPath.parent_path());
        m_imguiIniPath = PathToUtf8(layoutPath);
        inputOutput.IniFilename =
            m_imguiIniPath.c_str();
        inputOutput.ConfigWindowsMoveFromTitleBarOnly = true;
        const bool japaneseFontLoaded = LoadJapaneseFont(inputOutput);

        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;

        m_win32Initialized = ImGui_ImplWin32_Init(window);
        if (!m_win32Initialized)
        {
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui Win32 backend initialization failed.");
        }
        m_dx11Initialized = ImGui_ImplDX11_Init(graphics.Device(), graphics.Context());
        if (!m_dx11Initialized)
        {
            ImGui_ImplWin32_Shutdown();
            m_win32Initialized = false;
            ImGui::DestroyContext();
            throw std::runtime_error("ImGui DirectX 11 backend initialization failed.");
        }
        ResetHistory();
        MarkSceneSaved();
        // 起動経路では直前に SetAssetRoot が走査を終えています。
        RefreshAssets(true);
        CreateDefaultEditorPresets();
        std::string projectSettingsError;
        try
        {
            if (!LoadProjectConfiguration())
            {
                const auto relativeScene =
                    m_scenePath.lexically_relative(
                        m_graphics.Assets().AssetRoot());
                if (!relativeScene.empty())
                {
                    m_projectSettings.startupScene =
                        relativeScene;
                }
                SaveProjectConfiguration();
            }
        }
        catch (const std::exception& exception)
        {
            projectSettingsError =
                std::string{
                    "プロジェクト設定を読み込めませんでした: "
                }
                + exception.what();
        }
        try
        {
            const bool settingsLoaded = LoadEditorSettings();
            if (!projectSettingsError.empty())
            {
                SetStatus(projectSettingsError, true);
            }
            else
            {
                SetStatus(
                    settingsLoaded
                        ? "エディター設定を復元しました"
                        : (japaneseFontLoaded
                            ? "日本語エディターを起動しました"
                            : "日本語フォントが見つからないため既定フォントを使用します"),
                    !japaneseFontLoaded);
            }
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "エディター設定を読み込めませんでした: " }
                    + exception.what(),
                true);
        }
        DragAcceptFiles(m_window, TRUE);
    }

    EditorLayer::~EditorLayer()
    {
        // パッケージの取得/インストールスレッドを先に終わらせます。
        JoinPackageWorker();

        DragAcceptFiles(m_window, FALSE);

        if (m_gameModuleBuildProcess != nullptr)
        {
            CloseHandle(m_gameModuleBuildProcess);
            m_gameModuleBuildProcess = nullptr;
        }

        try
        {
            SaveEditorSettings();
        }
        catch (const std::exception&)
        {
        }

        if (m_dx11Initialized)
        {
            ImGui_ImplDX11_Shutdown();
        }
        if (m_win32Initialized)
        {
            ImGui_ImplWin32_Shutdown();
        }

        if (ImGui::GetCurrentContext() != nullptr)
        {
            if (!m_imguiIniPath.empty())
            {
                ImGui::SaveIniSettingsToDisk(
                    m_imguiIniPath.c_str());
            }
            ImGui::DestroyContext();
        }
    }

    bool EditorLayer::HandleMessage(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) const
    {
        if (message == WM_DROPFILES)
        {
            const auto drop = reinterpret_cast<HDROP>(wParam);
            PendingExternalAssetDrop pending;
            POINT clientPosition{};
            if (DragQueryPoint(drop, &clientPosition))
            {
                pending.screenPosition = clientPosition;
                ClientToScreen(
                    window,
                    &pending.screenPosition);
            }

            try
            {
                const UINT fileCount = DragQueryFileW(
                    drop,
                    0xFFFFFFFF,
                    nullptr,
                    0);
                pending.sources.reserve(fileCount);
                for (UINT index = 0; index < fileCount; ++index)
                {
                    const UINT length = DragQueryFileW(
                        drop,
                        index,
                        nullptr,
                        0);
                    std::wstring path(length + 1, L'\0');
                    if (DragQueryFileW(
                            drop,
                            index,
                            path.data(),
                            static_cast<UINT>(path.size()))
                        != 0)
                    {
                        path.resize(length);
                        pending.sources.emplace_back(
                            std::move(path));
                    }
                }
                if (!pending.sources.empty())
                {
                    m_pendingExternalAssetDrops.push_back(
                        std::move(pending));
                }
            }
            catch (...)
            {
            }
            DragFinish(drop);
            return true;
        }
        return ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
    }

    void EditorLayer::BeginFrame()
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // リモート操作: 入力の注入はImGui::NewFrame()の前に
        // 行います（このフレームのイベントキューへ載せるため）。
        if (!m_screenshotRequest.remoteDirectory.empty())
        {
            // UIの記録（dump / click-label用）。前フレームの記録を
            // 確定し、このフレームの記録を始めます。
            UiRecorder::SetEnabled(true);
            UiRecorder::NextFrame();
            // コマンドの注入より先に入れ直します（新しい位置が
            // 来たときはそちらが後ろに並んで勝ちます）。
            ReapplyRemoteMousePosition();
            if (!m_remoteMacro.empty())
            {
                // マクロ実行中は新しいコマンドを受けません
                // （手順の途中に別の入力が割り込むと壊れるため）。
                RunRemoteMacro();
            }
            else
            {
                PollRemoteCommands();
            }
        }
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        ProcessPendingAssetImports();

        // スクリーンショットモード: 1フレーム目にUIを開く指示を
        // 適用します（0フレーム目はまだレイアウトが無いため）。
        if (!m_screenshotRequest.imagePath.empty())
        {
            ++m_screenshotFrame;
            if (m_screenshotFrame == 1)
            {
                ApplyScreenshotIntent();
            }
        }
    }

    void EditorLayer::InjectMousePosition(
        const float x,
        const float y)
    {
        // 実カーソルは動かしません。ImGui_ImplWin32が毎フレーム
        // 実カーソル位置を報告して引き戻すので、位置を保持して
        // ReapplyRemoteMousePositionで毎フレーム入れ直します。
        m_remoteMouseHeld = true;
        m_remoteMouseX = x;
        m_remoteMouseY = y;
        ImGui::GetIO().AddMousePosEvent(x, y);
    }

    void EditorLayer::ReapplyRemoteMousePosition()
    {
        if (!m_remoteMouseHeld)
        {
            return;
        }
        // ImGui_ImplWin32_NewFrame()の後に積むので、バックエンドが
        // 報告した実カーソル位置より後ろに並び、こちらが勝ちます
        // （キューは順に適用され、最後のMousePosが残るため）。
        ImGui::GetIO().AddMousePosEvent(
            m_remoteMouseX,
            m_remoteMouseY);
    }

    void EditorLayer::RunRemoteMacro()
    {
        for (const auto& step : m_remoteMacro)
        {
            if (step.frame == m_remoteMacroFrame)
            {
                step.action();
            }
        }
        ++m_remoteMacroFrame;
        // 全ステップのフレームを過ぎたら完了。state.jsonの書き込みは
        // Render側が「マクロが空」を条件に行います。
        std::uint32_t lastFrame = 0;
        for (const auto& step : m_remoteMacro)
        {
            lastFrame = std::max(lastFrame, step.frame);
        }
        if (m_remoteMacroFrame > lastFrame)
        {
            m_remoteMacro.clear();
            m_remoteMacroFrame = 0;
        }
    }

    bool EditorLayer::ConsumeInputSnapshot(
        InputSnapshot& snapshot) noexcept
    {
        if (!m_playing
            || m_remoteInputFrames == 0
            || !m_remoteInputSnapshot.has_value())
        {
            return false;
        }
        snapshot = std::move(*m_remoteInputSnapshot);
        --m_remoteInputFrames;
        if (m_remoteInputFrames == 0)
        {
            m_remoteInputSnapshot.reset();
        }
        return true;
    }

    nlohmann::json EditorLayer::BuildRemoteRuntimeState() const
    {
        const auto& frameStats = m_graphics.FrameStats();
        const auto& memoryStats = m_graphics.MemoryStats();
        const auto& gpu = m_graphics.Gpu();
        const auto& pipeline =
            gpu.LatestPipelineStatistics();
        const auto& physics = m_scene.PhysicsStats();
        const auto& visibility = m_scene.VisibilityStats();

        nlohmann::json runtimeState{
            { "playing", m_playing },
            { "paused", m_paused },
            { "scene",
                PathToUtf8(
                    m_scene.Scenes().CurrentScenePath()) },
            { "objectCount", m_scene.GameObjects().size() },
            { "time", {
                { "deltaTime", Time::DeltaTime() },
                { "unscaledDeltaTime",
                    Time::UnscaledDeltaTime() },
                { "timeSinceStartup",
                    Time::TimeSinceStartup() },
                { "unscaledTimeSinceStartup",
                    Time::UnscaledTimeSinceStartup() },
                { "frameCount", Time::FrameCount() },
                { "timeScale", Time::TimeScale() },
                { "pausedByTimeScale", Time::IsPaused() },
            } },
            { "frame", {
                { "fps", frameStats.framesPerSecond },
                { "frameTimeMilliseconds",
                    frameStats.frameTimeMilliseconds },
                { "cpuTimeMilliseconds",
                    frameStats.cpuTimeMilliseconds },
                { "gpuTimeMilliseconds",
                    gpu.LatestFrameMilliseconds() },
                { "totalFrames", frameStats.totalFrames },
                { "shaderFallbackDraws",
                    frameStats.shaderFallbackDraws },
            } },
            { "memory", {
                { "processWorkingSetBytes",
                    memoryStats.processWorkingSetBytes },
                { "processPrivateBytes",
                    memoryStats.processPrivateBytes },
                { "systemPhysicalUsedBytes",
                    memoryStats.systemPhysicalUsedBytes },
                { "systemPhysicalTotalBytes",
                    memoryStats.systemPhysicalTotalBytes },
                { "dedicatedVideoMemoryBytes",
                    memoryStats.dedicatedVideoMemoryBytes },
                { "sharedSystemMemoryBytes",
                    memoryStats.sharedSystemMemoryBytes },
                { "localVideoMemoryUsageBytes",
                    memoryStats.localVideoMemoryUsageBytes },
                { "localVideoMemoryBudgetBytes",
                    memoryStats.localVideoMemoryBudgetBytes },
                { "nonLocalVideoMemoryUsageBytes",
                    memoryStats.nonLocalVideoMemoryUsageBytes },
                { "nonLocalVideoMemoryBudgetBytes",
                    memoryStats.nonLocalVideoMemoryBudgetBytes },
                { "videoMemoryBudgetAvailable",
                    memoryStats.videoMemoryBudgetAvailable },
            } },
            { "gpuPipeline", {
                { "valid", pipeline.valid },
                { "inputAssemblerVertices",
                    pipeline.inputAssemblerVertices },
                { "inputAssemblerPrimitives",
                    pipeline.inputAssemblerPrimitives },
                { "vertexShaderInvocations",
                    pipeline.vertexShaderInvocations },
                { "pixelShaderInvocations",
                    pipeline.pixelShaderInvocations },
                { "hullShaderInvocations",
                    pipeline.hullShaderInvocations },
                { "domainShaderInvocations",
                    pipeline.domainShaderInvocations },
                { "geometryShaderInvocations",
                    pipeline.geometryShaderInvocations },
                { "computeShaderInvocations",
                    pipeline.computeShaderInvocations },
            } },
            { "physics", {
                { "colliderCount2D", physics.colliderCount2D },
                { "colliderCount3D", physics.colliderCount3D },
                { "candidatePairCount2D",
                    physics.candidatePairCount2D },
                { "candidatePairCount3D",
                    physics.candidatePairCount3D },
                { "narrowPhaseTestCount2D",
                    physics.narrowPhaseTestCount2D },
                { "narrowPhaseTestCount3D",
                    physics.narrowPhaseTestCount3D },
                { "activeContactCount",
                    physics.activeContactCount },
                { "fixedStepsLastFrame",
                    m_scene.PhysicsFixedStepsLastFrame() },
            } },
            { "visibility", {
                { "rendererCount", visibility.rendererCount },
                { "visibleRendererCount",
                    visibility.visibleRendererCount },
                { "frustumCulledCount",
                    visibility.frustumCulledCount },
                { "occlusionCulledCount",
                    visibility.occlusionCulledCount },
                { "lodCulledCount", visibility.lodCulledCount },
                { "automaticLodRendererCount",
                    visibility.automaticLodRendererCount },
                { "automaticLodTrianglesSaved",
                    visibility.automaticLodTrianglesSaved },
                { "meshInstanceBatchCount",
                    visibility.meshInstanceBatchCount },
                { "meshInstancedRendererCount",
                    visibility.meshInstancedRendererCount },
                { "modelInstanceBatchCount",
                    visibility.modelInstanceBatchCount },
                { "modelInstancedRendererCount",
                    visibility.modelInstancedRendererCount },
                { "spatialNodeCount",
                    visibility.spatialNodeCount },
                { "spatialNodeTestCount",
                    visibility.spatialNodeTestCount },
                { "spatialIndexReused",
                    visibility.spatialIndexReused },
            } },
        };

        auto stateValues = nlohmann::json::object();
        auto values = m_scene.Scenes().State().Snapshot();
        std::ranges::sort(
            values,
            [](const auto& left, const auto& right)
            {
                return left.first < right.first;
            });
        for (const auto& [key, value] : values)
        {
            std::visit(
                [&stateValues, &key](const auto& item)
                {
                    stateValues[key] = item;
                },
                value);
        }
        runtimeState["gameState"] = std::move(stateValues);

        auto objects = nlohmann::json::array();
        for (const auto& object : m_scene.GameObjects())
        {
            if (object == nullptr)
            {
                continue;
            }
            const auto& transform = object->GetTransform();
            const auto euler = transform.EulerAngles();
            auto components = nlohmann::json::array();
            for (const auto& component : object->Components())
            {
                if (component == nullptr)
                {
                    continue;
                }
                components.push_back({
                    { "type", std::string(component->TypeName()) },
                    { "enabled", component->IsEnabled() },
                    { "activeAndEnabled",
                        component->IsActiveAndEnabled() },
                });
            }
            objects.push_back({
                { "id", object->Id() },
                { "name", object->Name() },
                { "tag", object->Tag() },
                { "enabled", object->IsEnabled() },
                { "activeInHierarchy",
                    object->IsActiveInHierarchy() },
                { "parentId",
                    object->Parent() == nullptr
                        ? 0
                        : object->Parent()->Id() },
                { "sourceScene", object->SourceScene() },
                { "position", {
                    transform.position.x,
                    transform.position.y,
                    transform.position.z,
                } },
                { "rotationEulerRadians", {
                    euler.x,
                    euler.y,
                    euler.z,
                } },
                { "rotationQuaternion", {
                    transform.rotationQuaternion.x,
                    transform.rotationQuaternion.y,
                    transform.rotationQuaternion.z,
                    transform.rotationQuaternion.w,
                } },
                { "scale", {
                    transform.scale.x,
                    transform.scale.y,
                    transform.scale.z,
                } },
                { "components", std::move(components) },
            });
        }
        runtimeState["objects"] = std::move(objects);

        auto loadedScenes = nlohmann::json::array();
        for (const auto& loaded : m_scene.AdditiveScenes())
        {
            loadedScenes.push_back({
                { "handle", loaded.handle },
                { "path", PathToUtf8(loaded.path) },
                { "name", loaded.name },
                { "rootCount", loaded.rootCount },
            });
        }
        runtimeState["additiveScenes"] = std::move(loadedScenes);

        auto actions = nlohmann::json::array();
        for (const auto& action : m_graphics.Input().Actions())
        {
            auto bindings = nlohmann::json::array();
            for (const auto& binding : action.bindings)
            {
                bindings.push_back({
                    { "control",
                        std::string(
                            InputControlName(binding.control)) },
                    { "scale", binding.scale },
                });
            }
            actions.push_back({
                { "name", action.name },
                { "bindings", std::move(bindings) },
                { "value",
                    m_graphics.Input().Value(action.name) },
                { "down",
                    m_graphics.Input().IsDown(action.name) },
                { "pressed",
                    m_graphics.Input().WasPressed(action.name) },
                { "released",
                    m_graphics.Input().WasReleased(action.name) },
            });
        }
        runtimeState["input"] = std::move(actions);

        const auto profileFrames = Profiler::Instance().Snapshot();
        if (!profileFrames.empty())
        {
            const auto& profile = profileFrames.back();
            auto samples = nlohmann::json::array();
            for (const auto& sample : profile.samples)
            {
                samples.push_back({
                    { "name", sample.name },
                    { "milliseconds", sample.milliseconds },
                    { "calls", sample.callCount },
                });
            }
            runtimeState["profiler"] = {
                { "enabled", Profiler::Instance().IsEnabled() },
                { "frameIndex", profile.index },
                { "milliseconds", profile.milliseconds },
                { "samples", std::move(samples) },
            };
        }
        else
        {
            runtimeState["profiler"] = {
                { "enabled", Profiler::Instance().IsEnabled() },
                { "frameIndex", 0 },
                { "milliseconds", 0.0 },
                { "samples", nlohmann::json::array() },
            };
        }

        auto logs = nlohmann::json::array();
        const auto logEntries = Logger::Instance().Snapshot();
        const auto firstLog = logEntries.size() > 64
            ? logEntries.end() - 64
            : logEntries.begin();
        for (auto iterator = firstLog;
            iterator != logEntries.end();
            ++iterator)
        {
            if (iterator->level == LogLevel::Info)
            {
                continue;
            }
            logs.push_back({
                { "sequence", iterator->sequence },
                { "level",
                    std::string(LogLevelName(iterator->level)) },
                { "message", iterator->message },
                { "gameObjectId", iterator->gameObjectId },
            });
        }
        runtimeState["logs"] = std::move(logs);
        return runtimeState;
    }

    void EditorLayer::PollRemoteCommands()
    {
        const auto commandPath =
            m_screenshotRequest.remoteDirectory
            / L"command.json";
        std::ifstream input(
            commandPath,
            std::ios::binary);
        if (!input)
        {
            return;
        }
        nlohmann::json document =
            nlohmann::json::parse(
                input,
                nullptr,
                false);
        // 書き込み途中のファイルを読むと壊れたJSONになります。
        // 失敗したら黙って次のフレームで読み直します。
        if (document.is_discarded()
            || !document.is_object())
        {
            return;
        }
        const auto sequence =
            document.value<std::uint64_t>("seq", 0);
        if (sequence == 0
            || sequence == m_remoteLastSequence)
        {
            return;
        }
        m_remoteLastSequence = sequence;
        m_remoteReportSequence = sequence;
        m_remoteReportPending = true;
        m_remoteReportError.clear();

        auto& io = ImGui::GetIO();
        const auto commands =
            document.value(
                "commands",
                nlohmann::json::array());
        for (const auto& command : commands)
        {
            const std::string type =
                command.value("type", std::string{});
            if (type == "play")
            {
                if (m_playing)
                {
                    m_remoteReportError =
                        "play requested while the game is already playing.";
                }
                else
                {
                    StartPlaying();
                }
            }
            else if (type == "pause")
            {
                if (!m_playing)
                {
                    m_remoteReportError =
                        "pause requires the game to be playing.";
                }
                else
                {
                    SetPaused(true);
                }
            }
            else if (type == "resume")
            {
                if (!m_playing)
                {
                    m_remoteReportError =
                        "resume requires the game to be playing.";
                }
                else
                {
                    SetPaused(false);
                }
            }
            else if (type == "step")
            {
                if (!m_playing || !m_paused)
                {
                    m_remoteReportError =
                        "step requires a paused game.";
                }
                else
                {
                    RequestSimulationStep();
                }
            }
            else if (type == "runtime"
                || type == "observe")
            {
                // 描画後にstate.jsonへ現在のゲーム状態を含めます。
                m_remoteRuntimePending = true;
            }
            else if (type == "timescale")
            {
                if (!command.contains("value")
                    || !command.at("value").is_number())
                {
                    m_remoteReportError =
                        "timescale requires a numeric value.";
                }
                else
                {
                    const float value =
                        command.at("value").get<float>();
                    if (!std::isfinite(value))
                    {
                        m_remoteReportError =
                            "timescale must be finite.";
                    }
                    else
                    {
                        Time::SetTimeScale(value);
                    }
                }
            }
            else if (type == "input")
            {
                if (!m_playing)
                {
                    m_remoteReportError =
                        "input requires the game to be playing.";
                    continue;
                }
                if (!command.contains("value")
                    || !command.at("value").is_number())
                {
                    m_remoteReportError =
                        "input requires a numeric value.";
                    continue;
                }
                const float value =
                    command.at("value").get<float>();
                if (!std::isfinite(value))
                {
                    m_remoteReportError =
                        "input value must be finite.";
                    continue;
                }
                InputControl control{};
                float controlValue = value;
                bool resolved = false;
                if (command.contains("control")
                    && command.at("control").is_string())
                {
                    try
                    {
                        control = InputControlFromName(
                            command.at("control")
                                .get<std::string>());
                        resolved = true;
                    }
                    catch (const std::exception&)
                    {
                        m_remoteReportError =
                            "unknown input control: "
                            + command.at("control")
                                .get<std::string>();
                    }
                }
                else if (command.contains("action")
                    && command.at("action").is_string())
                {
                    const auto actionName =
                        command.at("action").get<std::string>();
                    const auto action = std::find_if(
                        m_graphics.Input().Actions().begin(),
                        m_graphics.Input().Actions().end(),
                        [&actionName](const auto& candidate)
                        {
                            return candidate.name == actionName;
                        });
                    if (action == m_graphics.Input().Actions().end())
                    {
                        m_remoteReportError =
                            "unknown input action: " + actionName;
                    }
                    else
                    {
                        const auto binding = std::find_if(
                            action->bindings.begin(),
                            action->bindings.end(),
                            [](const auto& candidate)
                            {
                                return std::abs(candidate.scale)
                                    > 1.0e-6f;
                            });
                        if (binding == action->bindings.end())
                        {
                            m_remoteReportError =
                                "input action has no usable binding: "
                                + actionName;
                        }
                        else
                        {
                            control = binding->control;
                            controlValue = std::clamp(
                                value / binding->scale,
                                -1.0f,
                                1.0f);
                            resolved = true;
                        }
                    }
                }
                else
                {
                    m_remoteReportError =
                        "input requires control or action.";
                }
                if (!resolved)
                {
                    continue;
                }
                if (!m_remoteInputSnapshot.has_value())
                {
                    m_remoteInputSnapshot.emplace();
                }
                m_remoteInputSnapshot->Set(
                    control,
                    std::clamp(controlValue, -1.0f, 1.0f));
                const auto frames = std::clamp(
                    command.value("frames", 1u),
                    1u,
                    600u);
                m_remoteInputFrames = std::max(
                    m_remoteInputFrames,
                    frames);
            }
            else if (type == "move" || type == "click")
            {
                const float x =
                    command.value("x", 0.0f);
                const float y =
                    command.value("y", 0.0f);
                InjectMousePosition(x, y);
                if (type == "click")
                {
                    const int button =
                        command.value("button", 0);
                    io.AddMouseButtonEvent(button, true);
                    io.AddMouseButtonEvent(button, false);
                    if (command.value("double", false))
                    {
                        io.AddMouseButtonEvent(
                            button,
                            true);
                        io.AddMouseButtonEvent(
                            button,
                            false);
                    }
                }
            }
            else if (type == "click-label")
            {
                // ラベル指定のクリック。座標を画像から読む必要が
                // 無いので、AIの操作がレイアウト変更に強くなります。
                // 直前の完成フレームの記録から探します。
                const std::string label =
                    command.value("label", std::string{});
                const std::string window =
                    command.value("window", std::string{});
                const auto items = UiRecorder::Snapshot();
                const UiRecorder::Item* match = nullptr;
                for (const auto& item : items)
                {
                    if (!window.empty()
                        && item.window.find(window)
                            == std::string::npos)
                    {
                        continue;
                    }
                    if (item.label == label)
                    {
                        match = &item;
                        break;
                    }
                    // 完全一致が無ければ部分一致を候補に。
                    if (match == nullptr
                        && item.label.find(label)
                            != std::string::npos)
                    {
                        match = &item;
                    }
                }
                if (match != nullptr)
                {
                    const float x =
                        match->x + match->width * 0.5f;
                    const float y =
                        match->y + match->height * 0.5f;
                    InjectMousePosition(x, y);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                }
                else
                {
                    m_remoteReportError =
                        "label not found: " + label;
                }
            }
            else if (type == "dump")
            {
                // 可視ウィジェットの一覧をstate.jsonへ返します。
                // "all": true でラベル無しのウィジェット
                // （Transformの各軸など）も矩形付きで含めます。
                m_remoteDumpPending = true;
                m_remoteDumpAll =
                    command.value("all", false);
            }
            else if (type == "set-value")
            {
                // 値を設定します。対象はラベル指定（label/window）
                // または座標指定（x/y。ラベルを報告しない
                // Transformの各軸などに使う）。
                // チェックボックス: 現在値と違うときだけクリック。
                // テキスト/数値入力: Ctrl+クリック（Drag/Sliderは
                // これで入力モードになる）→全選択→入力→Enter、を
                // 複数フレームのマクロで実行します。
                if (!command.contains("value"))
                {
                    m_remoteReportError =
                        "set-value requires a value.";
                    continue;
                }
                float x{};
                float y{};
                const UiRecorder::Item* match = nullptr;
                if (command.contains("x")
                    && command.contains("y"))
                {
                    x = command.value("x", 0.0f);
                    y = command.value("y", 0.0f);
                }
                else
                {
                    const std::string label =
                        command.value(
                            "label",
                            std::string{});
                    const std::string window =
                        command.value(
                            "window",
                            std::string{});
                    const auto items =
                        UiRecorder::Snapshot();
                    for (const auto& item : items)
                    {
                        if (!window.empty()
                            && item.window.find(window)
                                == std::string::npos)
                        {
                            continue;
                        }
                        if (item.label == label)
                        {
                            match = &item;
                            break;
                        }
                        if (match == nullptr
                            && item.label.find(label)
                                != std::string::npos)
                        {
                            match = &item;
                        }
                    }
                    if (match == nullptr)
                    {
                        m_remoteReportError =
                            "label not found: " + label;
                        continue;
                    }
                    x = match->x + match->width * 0.5f;
                    y = match->y + match->height * 0.5f;
                }
                const auto& value = command.at("value");
                if (value.is_boolean())
                {
                    // ImGuiItemStatusFlags_Checked (1<<23)。
                    // 座標指定では現在値が分からないので、
                    // ラベル指定のときだけ差分判定します。
                    if (match != nullptr)
                    {
                        const bool checked =
                            (match->statusFlags
                                & (1u << 23)) != 0;
                        if (checked == value.get<bool>())
                        {
                            continue;
                        }
                    }
                    InjectMousePosition(x, y);
                    io.AddMouseButtonEvent(0, true);
                    io.AddMouseButtonEvent(0, false);
                    continue;
                }
                const std::string text =
                    value.is_string()
                        ? value.get<std::string>()
                        : value.dump();
                m_remoteMacroFrame = 0;
                m_remoteMacro = {
                    { 0, [this, x, y]
                        {
                            InjectMousePosition(x, y);
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiMod_Ctrl, true);
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    0, true);
                        } },
                    { 2, []
                        {
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    0, false);
                        } },
                    { 4, []
                        {
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiMod_Ctrl, false);
                        } },
                    // 全選択（InputTextを直接クリックした場合、
                    // カーソル位置に文字が挿入されるのを防ぐ）。
                    { 6, []
                        {
                            auto& inputOutput =
                                ImGui::GetIO();
                            inputOutput.AddKeyEvent(
                                ImGuiMod_Ctrl, true);
                            inputOutput.AddKeyEvent(
                                ImGuiKey_A, true);
                        } },
                    { 8, []
                        {
                            auto& inputOutput =
                                ImGui::GetIO();
                            inputOutput.AddKeyEvent(
                                ImGuiKey_A, false);
                            inputOutput.AddKeyEvent(
                                ImGuiMod_Ctrl, false);
                        } },
                    { 10, [text]
                        {
                            ImGui::GetIO()
                                .AddInputCharactersUTF8(
                                    text.c_str());
                        } },
                    { 12, []
                        {
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiKey_Enter, true);
                        } },
                    { 14, []
                        {
                            ImGui::GetIO().AddKeyEvent(
                                ImGuiKey_Enter, false);
                        } },
                };
            }
            else if (type == "drag")
            {
                // 始点から終点まで数フレームかけて引っ張ります。
                // ギズモ・スライダー・ドッキングの移動用です。
                const float fromX =
                    command.value("x", 0.0f);
                const float fromY =
                    command.value("y", 0.0f);
                const float toX =
                    command.value("toX", fromX);
                const float toY =
                    command.value("toY", fromY);
                const int button =
                    command.value("button", 0);
                const std::uint32_t moveFrames =
                    std::clamp(
                        command.value("frames", 10u),
                        2u,
                        120u);
                m_remoteMacroFrame = 0;
                m_remoteMacro.clear();
                m_remoteMacro.push_back(
                    { 0, [this, fromX, fromY, button]
                        {
                            InjectMousePosition(
                                fromX, fromY);
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    button, true);
                        } });
                for (std::uint32_t step = 1;
                    step <= moveFrames;
                    ++step)
                {
                    const float ratio =
                        static_cast<float>(step)
                        / static_cast<float>(moveFrames);
                    const float x =
                        fromX + (toX - fromX) * ratio;
                    const float y =
                        fromY + (toY - fromY) * ratio;
                    // 押した直後の1フレームを空けてから動かします
                    // （同フレームだとドラッグ開始と認識されない
                    // ウィジェットがあるため）。
                    m_remoteMacro.push_back(
                        { step + 1, [this, x, y]
                            {
                                InjectMousePosition(x, y);
                            } });
                }
                m_remoteMacro.push_back(
                    { moveFrames + 3, [button]
                        {
                            ImGui::GetIO()
                                .AddMouseButtonEvent(
                                    button, false);
                        } });
            }
            else if (type == "wheel")
            {
                // 指定位置でマウスホイールを回します。deltaYは
                // ノッチ数（正で上＝スクロールアップ）。
                const float x =
                    command.value("x", 0.0f);
                const float y =
                    command.value("y", 0.0f);
                InjectMousePosition(x, y);
                io.AddMouseWheelEvent(
                    command.value("deltaX", 0.0f),
                    command.value("deltaY", 0.0f));
            }
            else if (type == "text")
            {
                io.AddInputCharactersUTF8(
                    command.value(
                        "value",
                        std::string{}).c_str());
            }
            else if (type == "key")
            {
                const std::string name =
                    command.value(
                        "value",
                        std::string{});
                ImGuiKey key = ImGuiKey_None;
                if (name == "enter") { key = ImGuiKey_Enter; }
                else if (name == "tab") { key = ImGuiKey_Tab; }
                else if (name == "escape") { key = ImGuiKey_Escape; }
                else if (name == "backspace") { key = ImGuiKey_Backspace; }
                else if (name == "delete") { key = ImGuiKey_Delete; }
                else if (name == "space") { key = ImGuiKey_Space; }
                else if (name == "up") { key = ImGuiKey_UpArrow; }
                else if (name == "down") { key = ImGuiKey_DownArrow; }
                else if (name == "left") { key = ImGuiKey_LeftArrow; }
                else if (name == "right") { key = ImGuiKey_RightArrow; }
                if (key != ImGuiKey_None)
                {
                    io.AddKeyEvent(key, true);
                    io.AddKeyEvent(key, false);
                }
                else
                {
                    m_remoteReportError =
                        "unknown key: " + name;
                }
            }
            else if (type == "screenshot")
            {
                // 撮影はこのフレームの描画が終わってから
                // （Renderの末尾）。ファイル名はseqごとに変えます。
                // ホスト側で古い内容を取得しないよう、固定名を避けます。
                m_remotePendingShot =
                    m_screenshotRequest.remoteDirectory
                    / (L"screenshot-"
                        + std::to_wstring(sequence)
                        + L".png");
            }
            else if (type == "quit")
            {
                WriteRemoteState();
                PostQuitMessage(0);
            }
            else
            {
                m_remoteReportError =
                    "unknown command: " + type;
            }
        }
    }

    void EditorLayer::WriteRemoteState()
    {
        if (!m_remoteReportPending)
        {
            return;
        }
        m_remoteReportPending = false;
        nlohmann::json state{
            { "seq", m_remoteReportSequence },
            { "ok", m_remoteReportError.empty() },
        };
        if (!m_remoteReportError.empty())
        {
            state["error"] = m_remoteReportError;
        }
        if (!m_remotePendingShot.empty())
        {
            state["screenshot"] =
                PathToUtf8(m_remotePendingShot);
        }
        if (m_remoteDumpPending)
        {
            m_remoteDumpPending = false;
            auto items = nlohmann::json::array();
            for (const auto& item :
                UiRecorder::Snapshot(m_remoteDumpAll))
            {
                items.push_back({
                    { "window", item.window },
                    { "label", item.label },
                    { "x", item.x },
                    { "y", item.y },
                    { "w", item.width },
                    { "h", item.height },
                    { "flags", item.statusFlags },
                });
            }
            state["items"] = std::move(items);
        }
        if (m_remoteRuntimePending)
        {
            m_remoteRuntimePending = false;
            state["runtime"] = BuildRemoteRuntimeState();
        }
        std::ofstream output(
            m_screenshotRequest.remoteDirectory
                / L"state.json",
            std::ios::trunc);
        output << state.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace);
    }

    void EditorLayer::ApplyScreenshotIntent()
    {
        std::string show = m_screenshotRequest.show;
        if (show == "export-windows" || show == "export-web")
        {
            OpenGameExportDialog();
            m_gameExportDialog->SelectTarget(show == "export-web"
                ? GameExportTarget::Web : GameExportTarget::Windows);
            return;
        }
        if (show.empty())
        {
            return;
        }
        // 末尾の「:bottom」は「対象を末尾までスクロールして撮る」
        // 指定です。設定の物理タブの衝突マトリクスや、Inspectorの
        // 下の方のコンポーネントは、これが無いと画面外になります。
        constexpr std::string_view bottomSuffix{
            ":bottom" };
        if (show.size() > bottomSuffix.size()
            && show.ends_with(bottomSuffix))
        {
            m_screenshotScrollToBottom = true;
            show.resize(
                show.size() - bottomSuffix.size());
        }
        constexpr std::string_view settingsPrefix{
            "project-settings:" };
        constexpr std::string_view inspectorPrefix{
            "inspector:" };
        if (show.starts_with(settingsPrefix))
        {
            // カテゴリー名はDrawProjectSettingsDialogと同じ順にします。
            // ASCIIだけを扱う自動化スクリプト向けに別名も受け付けます。
            constexpr std::array<const char*, 7>
                categories{
                    "ゲーム",
                    "グラフィック",
                    "ビューポート設定",
                    "物理",
                    "タグ",
                    "入力",
                    "スクリプト"
                };
            constexpr std::array<const char*, 7>
                aliases{
                    "game",
                    "graphics",
                    "viewport",
                    "physics",
                    "tags",
                    "input",
                    "scripts"
                };
            const std::string category =
                show.substr(settingsPrefix.size());
            for (std::size_t index = 0;
                index < categories.size();
                ++index)
            {
                if (category == categories[index]
                    || category == aliases[index])
                {
                    m_projectSettingsCategory =
                        static_cast<int>(index);
                    break;
                }
            }
            OpenProjectSettingsDialog();
            return;
        }
        if (show.starts_with(inspectorPrefix))
        {
            const std::string name =
                show.substr(inspectorPrefix.size());
            if (const auto* target =
                    m_scene.FindGameObjectByName(name))
            {
                m_selectedObjectId = target->Id();
            }
            else
            {
                Logger::Instance().Warning(
                    "スクリーンショット対象のGameObjectが"
                    "見つかりません: "
                    + name);
            }
            return;
        }
        Logger::Instance().Warning(
            "スクリーンショットの--showを解釈できません: "
            + show);
    }

    void EditorLayer::CaptureScreenshotAndQuit()
    {
        nlohmann::json report{
            { "ok", false },
            { "command", "editor-screenshot" },
            { "show", m_screenshotRequest.show },
            { "image",
                PathToUtf8(
                    m_screenshotRequest.imagePath) },
        };
        try
        {
            std::uint32_t width{};
            std::uint32_t height{};
            const auto pixels =
                m_graphics.CaptureBackBuffer(
                    width,
                    height);
            SavePng(
                m_screenshotRequest.imagePath,
                width,
                height,
                pixels);
            report["ok"] = true;
            report["width"] = width;
            report["height"] = height;
        }
        catch (const std::exception& exception)
        {
            report["error"] = exception.what();
        }
        if (!m_screenshotRequest.reportPath.empty())
        {
            std::ofstream output(
                m_screenshotRequest.reportPath,
                std::ios::trunc);
            output << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::
                    replace);
        }
        // 撮ったら終了します。ConfirmCloseを通さないのは、
        // スクリーンショットモードはシーンを編集しないので
        // 「未保存の変更」の警告が原理的に不要なためです。
        PostQuitMessage(report["ok"].get<bool>() ? 0 : 1);
    }

    void EditorLayer::Draw()
    {
        UpdateGameModuleBuild();
        UpdateExternalSceneFile();
        UpdateExternalProjectSettings();
        UpdateProjectMenus();
        UpdateScriptAutoBuild();
        const bool editorLocked =
            m_gameModuleBuildProcess != nullptr;
        if (editorLocked)
        {
            auto& inputOutput = ImGui::GetIO();
            inputOutput.ClearInputKeys();
            inputOutput.ClearInputMouse();
        }

        m_graphics.Input().SetPointerOverride(
            InputPointerState{});
        if (!m_playing)
        {
            const float deltaTime =
                std::min(
                    ImGui::GetIO().DeltaTime,
                    0.05f);
            for (const auto& object :
                m_scene.GameObjects())
            {
                if (object->IsEnabled())
                {
                    if (auto* particles =
                        object->GetComponent<
                            ParticleSystemComponent>();
                        particles != nullptr
                        && particles->IsEnabled())
                    {
                        particles->UpdatePreview(
                            deltaTime);
                    }
                    // 編集モードでもUIレイアウトを反映します。
                    if (auto* layoutGroup =
                        object->GetComponent<
                            UILayoutGroupComponent>();
                        layoutGroup != nullptr
                        && layoutGroup->IsEnabled())
                    {
                        layoutGroup->ApplyLayout();
                    }
                }
            }
        }

        ImGui::BeginDisabled(editorLocked);
        DrawToolbar();
        DrawDockSpace();
        DrawConsole();
        DrawPerformancePanel();
        DrawProjectPanels();
        DrawPersistencePanel();
        DrawHierarchy();
        DrawAssetBrowser();
        DrawViewport();
        DrawInspector();
        DrawTilePalette();
        DrawPackagesPanel();
        DrawPackageBuildDialog();
        DrawAnimationTimeline();
        DrawAnimatorControllerGraph();
        DrawProjectSettingsDialog();
        DrawGameExportDialog();
        ImGui::EndDisabled();

        DrawGameModuleBuildOverlay();
    }

    void EditorLayer::DrawGameModuleBuildOverlay()
    {
        if (m_gameModuleBuildProcess == nullptr)
        {
            return;
        }

        const ImGuiViewport* viewport =
            ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowFocus();

        constexpr ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoNav;
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{});
        ImGui::PushStyleColor(
            ImGuiCol_WindowBg,
            ImVec4{ 0.01f, 0.015f, 0.025f, 0.76f });
        ImGui::Begin(
            "##GameModuleBuildOverlay",
            nullptr,
            overlayFlags);

        const float cardWidth = std::clamp(
            viewport->Size.x - 40.0f,
            320.0f,
            480.0f);
        constexpr float cardHeight = 224.0f;
        ImGui::SetCursorPos(ImVec2{
            std::max(
                (viewport->Size.x - cardWidth) * 0.5f,
                0.0f),
            std::max(
                (viewport->Size.y - cardHeight) * 0.5f,
                0.0f)
        });

        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildRounding,
            10.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildBorderSize,
            1.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{ 24.0f, 20.0f });
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImVec4{ 0.075f, 0.09f, 0.13f, 1.0f });
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImVec4{ 0.25f, 0.58f, 0.95f, 0.9f });
        ImGui::BeginChild(
            "##GameModuleBuildCard",
            ImVec2{ cardWidth, cardHeight },
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar);

        const auto centeredText =
            [cardWidth](const char* text)
            {
                const float textWidth =
                    ImGui::CalcTextSize(text).x;
                ImGui::SetCursorPosX(std::max(
                    (cardWidth - textWidth) * 0.5f,
                    0.0f));
                ImGui::TextUnformatted(text);
            };

        centeredText("C++ Scriptをビルドしています");
        ImGui::Dummy(ImVec2{ 0.0f, 10.0f });

        constexpr int spinnerDotCount = 12;
        constexpr float spinnerRadius = 18.0f;
        const double animationTime = ImGui::GetTime();
        const int spinnerPhase = static_cast<int>(
            animationTime * 12.0)
            % spinnerDotCount;
        const ImVec2 spinnerCenter{
            ImGui::GetWindowPos().x + cardWidth * 0.5f,
            ImGui::GetCursorScreenPos().y + spinnerRadius
        };
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (int index = 0;
            index < spinnerDotCount;
            ++index)
        {
            const float angle =
                (static_cast<float>(index)
                    / static_cast<float>(spinnerDotCount))
                * DirectX::XM_2PI;
            const int distanceFromHead =
                (spinnerPhase - index + spinnerDotCount)
                % spinnerDotCount;
            const float brightness =
                1.0f
                - static_cast<float>(distanceFromHead)
                    / static_cast<float>(spinnerDotCount);
            drawList->AddCircleFilled(
                ImVec2{
                    spinnerCenter.x
                        + std::cos(angle) * spinnerRadius,
                    spinnerCenter.y
                        + std::sin(angle) * spinnerRadius
                },
                2.5f + brightness * 1.5f,
                ImGui::GetColorU32(ImVec4{
                    0.25f,
                    0.62f,
                    1.0f,
                    0.2f + brightness * 0.8f
                }));
        }
        ImGui::Dummy(ImVec2{
            0.0f,
            spinnerRadius * 2.0f + 10.0f
        });

        centeredText(
            m_pendingScriptAttachments.empty()
                ? "完了後にGame Moduleを自動で再読み込みします。"
                : "完了後に自動で読み込み、GameObjectへアタッチします。");
        ImGui::Dummy(ImVec2{ 0.0f, 7.0f });

        const char* lockMessage =
            "処理が完了するまでエディターは操作できません。";
        const float lockMessageWidth =
            ImGui::CalcTextSize(lockMessage).x;
        ImGui::SetCursorPosX(std::max(
            (cardWidth - lockMessageWidth) * 0.5f,
            0.0f));
        ImGui::TextDisabled("%s", lockMessage);

        const double elapsedSeconds = std::max(
            animationTime - m_gameModuleBuildStartedAt,
            0.0);
        const std::string elapsedText =
            "経過時間: "
            + std::to_string(
                static_cast<int>(elapsedSeconds))
            + " 秒";
        const float elapsedTextWidth =
            ImGui::CalcTextSize(elapsedText.c_str()).x;
        ImGui::SetCursorPosX(std::max(
            (cardWidth - elapsedTextWidth) * 0.5f,
            0.0f));
        ImGui::TextDisabled(
            "%s",
            elapsedText.c_str());

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void EditorLayer::RenderSceneViews()
    {
        constexpr float sceneClearColor[]{ 0.055f, 0.070f, 0.095f, 1.0f };
        constexpr float gameClearColor[]{ 0.025f, 0.035f, 0.055f, 1.0f };

        // 描画先テクスチャを持つCameraを先に描きます。
        m_scene.RenderTargetTextures();

        if (m_activeViewport == ViewportMode::Scene
            && m_sceneRenderTarget.IsValid())
        {
            m_graphics.SetUIViewportSize(
                m_sceneRenderTarget.Width(),
                m_sceneRenderTarget.Height());
            m_sceneRenderTarget.Bind(m_graphics.Context());
            m_sceneRenderTarget.Clear(m_graphics.Context(), sceneClearColor);
            m_scene.RenderWithMatrices(
                SceneViewMatrix(),
                SceneProjectionMatrix(),
                false,
                m_colliderDebugVisible,
                &m_sceneRenderTarget);
            // 並びはRunPostProcessが持っています。
            RunPostProcess(
                m_graphics,
                m_sceneRenderTarget,
                m_scene.PostProcessFrameData());
            // UIはトーンマッピングとFXAAの後に描き、元画像の色と輪郭を保つ。
            m_scene.Render2D();
            // エディターの補助表示はUIより手前に保つ。
            m_graphics.Gpu().BeginSection(
                "エディター補助表示");
            m_sceneRenderTarget.Bind(
                m_graphics.Context());
            if (m_gridVisible)
            {
                if (m_scene2DMode)
                {
                    m_graphics.Debug().DrawGridXY(
                        m_gridSpacing,
                        m_gridExtent,
                        SceneViewMatrix(),
                        SceneProjectionMatrix());
                }
                else
                {
                    m_graphics.Debug().DrawGridXZ(
                        m_gridSpacing,
                        m_gridExtent,
                        SceneViewMatrix(),
                        SceneProjectionMatrix());
                }
            }
            if (m_cameraGizmosVisible)
            {
                DrawCameraGizmos();
            }
            if (m_lightGizmosVisible)
            {
                DrawLightGizmos();
            }
            // 選択枠は「今どれを触っているか」の表示なので、
            // デバッグ線のトグルとは独立に常に出します。
            DrawSelectionHighlight();
            // 完成画像を表示用へ確定します（ポスト処理のswap回数に
            // よらず、ImGuiには常に最終結果を見せるため）。
            m_sceneRenderTarget.CopyToDisplay(
                m_graphics.Context());
            m_graphics.Gpu().EndSection();

            const auto* selected =
                m_scene.FindGameObject(m_selectedObjectId);
            const auto* selectedCamera = selected != nullptr
                ? selected->GetComponent<CameraComponent>()
                : nullptr;
            if (selectedCamera != nullptr
                && m_cameraPreviewRenderTarget.IsValid())
            {
                m_graphics.SetUIViewportSize(
                    m_cameraPreviewRenderTarget.Width(),
                    m_cameraPreviewRenderTarget.Height());
                m_cameraPreviewRenderTarget.Bind(
                    m_graphics.Context());
                m_cameraPreviewRenderTarget.Clear(
                    m_graphics.Context(),
                    gameClearColor);
                m_scene.RenderWithMatrices(
                    selectedCamera->ViewMatrix(),
                    selectedCamera->ProjectionMatrix(
                        m_cameraPreviewRenderTarget.AspectRatio()),
                    false,
                    false,
                    &m_cameraPreviewRenderTarget);

                RunPostProcess(
                    m_graphics,
                    m_cameraPreviewRenderTarget,
                    m_scene.PostProcessFrameData());
                m_scene.Render2D();
                m_cameraPreviewRenderTarget.CopyToDisplay(
                    m_graphics.Context());
            }
        }
        else if (m_activeViewport == ViewportMode::Game
            && m_gameRenderTarget.IsValid())
        {
            m_graphics.SetUIViewportSize(
                m_gameRenderTarget.Width(),
                m_gameRenderTarget.Height());
            m_gameRenderTarget.Bind(m_graphics.Context());
            m_gameRenderTarget.Clear(m_graphics.Context(), gameClearColor);
            m_scene.RenderMainCamera(
                m_gameRenderTarget.AspectRatio(),
                false,
                &m_gameRenderTarget);
            // 並びはRunPostProcessが持っています。
            RunPostProcess(
                m_graphics,
                m_gameRenderTarget,
                m_scene.PostProcessFrameData());
            m_scene.Render2D();

            const auto& scenes =
                m_scene.Scenes();
            if (m_playing
                && scenes.IsLoading())
            {
                // ローディング表示もUIと同様にポストエフェクト後へ重ねる。
                m_graphics.DrawLoadingScreen(
                    scenes.LoadProgress(),
                    scenes.LoadingScreen(),
                    m_gameRenderTarget.Width(),
                    m_gameRenderTarget.Height());
            }
            m_gameRenderTarget.CopyToDisplay(
                m_graphics.Context());
        }
    }

    void EditorLayer::Render()
    {
        ImGui::Render();
        // エディターのUI自体もGPUを使います。ビューポートの絵と
        // 分けて出さないと、GPU合計との差がどこから来たのか
        // 判断できません（パネルの枚数で普通に数ms動きます）。
        m_graphics.Gpu().BeginSection("エディターUI");
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        m_graphics.Gpu().EndSection();

        // スクリーンショットモード: UIがバックバッファへ描かれた
        // この時点（Presentの前）で撮ります。
        if (!m_screenshotRequest.imagePath.empty()
            && m_screenshotFrame
                >= m_screenshotRequest.captureFrame)
        {
            CaptureScreenshotAndQuit();
            // 二重撮影よけ（PostQuitMessageの後も数フレーム
            // 回ることがあるため）。
            m_screenshotRequest.imagePath.clear();
        }

        // リモート操作の撮影依頼も同じ時点（Presentの前）で撮ります。
        if (!m_remotePendingShot.empty())
        {
            try
            {
                std::uint32_t width{};
                std::uint32_t height{};
                const auto pixels =
                    m_graphics.CaptureBackBuffer(
                        width,
                        height);
                SavePng(
                    m_remotePendingShot,
                    width,
                    height,
                    pixels);
            }
            catch (const std::exception& exception)
            {
                m_remoteReportError = exception.what();
            }
            WriteRemoteState();
            m_remotePendingShot.clear();
        }
        else if (m_remoteReportPending
            && m_remoteMacro.empty())
        {
            // 撮影を伴わないコマンド（クリック等）は、実行した
            // フレームの描画が終わった時点で完了とみなします。
            // マクロ（set-value / drag）は全ステップが終わるまで
            // 完了を報告しません（ホストが早読みしないように）。
            WriteRemoteState();
        }
    }

    // これがtrueの間、ApplicationはActionのキーボード入力を丸ごと
    // 切ります（Input().Update(!WantsKeyboard())）。ImGuiのキー処理は
    // 別経路なので、ここでfalseを返してもエディターの操作性は変わりません。
    //
    // WantCaptureKeyboardはキーボードナビゲーション中も立ち続けるため、
    // テキスト入力中かキーボード操作中の場合だけ入力を遮断します。
    bool EditorLayer::WantsKeyboard() const noexcept
    {
        const auto& inputOutput = ImGui::GetIO();
        // 名前や数値を打ち込んでいる最中は、絶対にゲームへ渡しません
        // （オブジェクト名に"w"と打つたびに主人公が動くと困ります）。
        if (inputOutput.WantTextInput)
        {
            return true;
        }
        // 再生中はゲームがキーボードの主役です。
        if (m_playing)
        {
            return false;
        }
        return inputOutput.WantCaptureKeyboard;
    }

    void EditorLayer::ToggleFullscreen()
    {
        if (!m_fullscreen)
        {
            m_windowedPlacement.length =
                sizeof(WINDOWPLACEMENT);
            if (GetWindowPlacement(
                    m_window,
                    &m_windowedPlacement) == FALSE)
            {
                SetStatus(
                    "フルスクリーンへ切り替えられませんでした",
                    true);
                return;
            }

            MONITORINFO monitorInfo{
                sizeof(MONITORINFO)
            };
            const HMONITOR monitor = MonitorFromWindow(
                m_window,
                MONITOR_DEFAULTTONEAREST);
            if (monitor == nullptr
                || GetMonitorInfoW(
                    monitor,
                    &monitorInfo) == FALSE)
            {
                SetStatus(
                    "表示先のモニターを取得できませんでした",
                    true);
                return;
            }

            m_windowedStyle = GetWindowLongPtrW(
                m_window,
                GWL_STYLE);
            m_windowedExtendedStyle = GetWindowLongPtrW(
                m_window,
                GWL_EXSTYLE);

            SetWindowLongPtrW(
                m_window,
                GWL_STYLE,
                m_windowedStyle
                    & ~static_cast<LONG_PTR>(
                        WS_OVERLAPPEDWINDOW));
            SetWindowLongPtrW(
                m_window,
                GWL_EXSTYLE,
                m_windowedExtendedStyle
                    & ~static_cast<LONG_PTR>(
                        WS_EX_WINDOWEDGE
                        | WS_EX_CLIENTEDGE));

            const RECT& monitorBounds =
                monitorInfo.rcMonitor;
            if (SetWindowPos(
                    m_window,
                    HWND_TOP,
                    monitorBounds.left,
                    monitorBounds.top,
                    monitorBounds.right
                        - monitorBounds.left,
                    monitorBounds.bottom
                        - monitorBounds.top,
                    SWP_FRAMECHANGED
                        | SWP_NOOWNERZORDER) == FALSE)
            {
                SetWindowLongPtrW(
                    m_window,
                    GWL_STYLE,
                    m_windowedStyle);
                SetWindowLongPtrW(
                    m_window,
                    GWL_EXSTYLE,
                    m_windowedExtendedStyle);
                SetStatus(
                    "フルスクリーンへ切り替えられませんでした",
                    true);
                return;
            }

            m_fullscreen = true;
            SetStatus("フルスクリーンに切り替えました");
            return;
        }

        SetWindowLongPtrW(
            m_window,
            GWL_STYLE,
            m_windowedStyle);
        SetWindowLongPtrW(
            m_window,
            GWL_EXSTYLE,
            m_windowedExtendedStyle);
        SetWindowPos(
            m_window,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED
                | SWP_NOMOVE
                | SWP_NOSIZE
                | SWP_NOZORDER
                | SWP_NOOWNERZORDER);
        m_windowedPlacement.length =
            sizeof(WINDOWPLACEMENT);
        SetWindowPlacement(
            m_window,
            &m_windowedPlacement);

        m_fullscreen = false;
        SetStatus("ウィンドウ表示に戻しました");
    }

    void EditorLayer::UpdateProjectMenus()
    {
        const double now = ImGui::GetTime();
        if (now - m_lastProjectMenuScanAt < 2.0)
        {
            return;
        }
        m_lastProjectMenuScanAt = now;

        const auto manifestPath = ProjectSettingsPath().parent_path()
            / L"editor-menu.json";
        std::error_code existsError;
        const bool exists = std::filesystem::is_regular_file(
            manifestPath,
            existsError);
        if (!exists)
        {
            if (m_projectMenuManifestSeen)
            {
                m_projectMenus.clear();
                m_projectPanels.clear();
                m_projectMenuManifestHash = 0;
                m_projectMenuManifestSeen = false;
                SetStatus("プロジェクト専用メニューを解除しました");
            }
            return;
        }

        std::ifstream input(manifestPath, std::ios::binary);
        if (!input)
        {
            SetStatus(
                "プロジェクト専用メニューを読み込めません: "
                    + PathToUtf8(manifestPath),
                true);
            return;
        }
        const std::string source{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        const std::uint64_t sourceHash = HashProjectMenuManifest(source);
        if (m_projectMenuManifestSeen
            && sourceHash == m_projectMenuManifestHash)
        {
            return;
        }
        m_projectMenuManifestSeen = true;
        m_projectMenuManifestHash = sourceHash;

        try
        {
            const auto document = nlohmann::json::parse(source);
            if (!document.is_object()
                || document.value("format", std::string{})
                    != "LamaPonEditorMenu"
                || document.value("version", 0) != 1)
            {
                throw std::runtime_error(
                    "format=LamaPonEditorMenu / version=1 が必要です");
            }
            const auto& items = document.at("items");
            if (!items.is_array() || items.size() > 128u)
            {
                throw std::runtime_error(
                    "items は128件以内の配列にしてください");
            }

            std::vector<ProjectMenuNode> menus;
            std::vector<ProjectPanelDefinition> panels;
            constexpr std::array<std::string_view, 8> reservedRoots{
                "ファイル", "編集", "シーン", "GameObject",
                "アセット", "ウィンドウ", "拡張機能", "ヘルプ"
            };
            for (const auto& item : items)
            {
                if (!item.is_object())
                {
                    throw std::runtime_error(
                        "items の各要素はオブジェクトにしてください");
                }
                const auto path = SplitProjectMenuPath(
                    item.at("path").get<std::string>());
                if (std::ranges::find(reservedRoots, path.front())
                    != reservedRoots.end())
                {
                    throw std::runtime_error(
                        "組み込みメニュー名は先頭に使えません: "
                        + path.front());
                }

                std::optional<ProjectMenuCommand> action;
                std::optional<std::size_t> panelIndex;
                if (item.contains("panel"))
                {
                    const auto& value = item.at("panel");
                    if (!value.is_object())
                    {
                        throw std::runtime_error("panel はオブジェクトにしてください");
                    }
                    ProjectPanelDefinition panel;
                    const auto type = value.value("type", std::string{});
                    if (type == "bgm-loop")
                    {
                        panel.kind = ProjectPanelKind::BgmLoop;
                    }
                    else
                    {
                        throw std::runtime_error("未対応のpanel.typeです: " + type);
                    }
                    panel.title = value.value("title", path.back());
                    panel.dataPath = PathFromUtf8(
                        value.at("data").get<std::string>());
                    if (value.contains("saveCommand"))
                    {
                        panel.saveCommand.command = value.at("saveCommand")
                            .get<std::string>();
                        panel.saveCommand.arguments = value.value(
                            "saveArguments", std::vector<std::string>{});
                        panel.saveCommand.workingDirectory = PathFromUtf8(
                            value.value("workingDirectory", std::string{ "." }));
                    }
                    panelIndex = panels.size();
                    panels.push_back(std::move(panel));
                }
                else
                {
                    ProjectMenuCommand command;
                    command.command = item.at("command").get<std::string>();
                    if (command.command.empty())
                    {
                        throw std::runtime_error(
                            "command は空にできません: "
                            + item.at("path").get<std::string>());
                    }
                    if (item.contains("arguments"))
                    {
                        command.arguments = item.at("arguments")
                            .get<std::vector<std::string>>();
                    }
                    command.workingDirectory = PathFromUtf8(
                        item.value("workingDirectory", std::string{ "." }));
                    command.enabledWhilePlaying = item.value(
                        "enabledWhilePlaying", false);
                    action = std::move(command);
                }

                auto* siblings = &menus;
                ProjectMenuNode* node = nullptr;
                for (const auto& segment : path)
                {
                    auto existing = std::ranges::find(
                        *siblings,
                        segment,
                        &ProjectMenuNode::label);
                    if (existing == siblings->end())
                    {
                        siblings->push_back(ProjectMenuNode{ segment });
                        existing = std::prev(siblings->end());
                    }
                    node = &*existing;
                    siblings = &node->children;
                }
                if (node == nullptr || node->action.has_value()
                    || node->panelIndex.has_value())
                {
                    throw std::runtime_error(
                        "同じメニューパスが重複しています: "
                        + item.at("path").get<std::string>());
                }
                node->action = std::move(action);
                node->panelIndex = panelIndex;
            }
            m_projectMenus = std::move(menus);
            m_projectPanels = std::move(panels);
            SetStatus(
                "プロジェクト専用メニューを読み込みました（"
                + std::to_string(items.size())
                + "件）");
        }
        catch (const std::exception& exception)
        {
            m_projectMenus.clear();
            m_projectPanels.clear();
            SetStatus(
                std::string{ "editor-menu.json を読み込めません: " }
                    + exception.what(),
                true);
        }
    }

    void EditorLayer::LaunchProjectMenuCommand(
        const ProjectMenuCommand& command)
    {
        const auto projectRoot = ProjectSettingsPath()
            .parent_path()
            .parent_path();
        std::filesystem::path executable = PathFromUtf8(command.command);
        if (!executable.is_absolute()
            && (command.command.find('/') != std::string::npos
                || command.command.find('\\') != std::string::npos))
        {
            executable = projectRoot / executable;
        }

        std::filesystem::path workingDirectory = command.workingDirectory;
        if (workingDirectory.empty())
        {
            workingDirectory = projectRoot;
        }
        else if (!workingDirectory.is_absolute())
        {
            workingDirectory = projectRoot / workingDirectory;
        }
        workingDirectory = workingDirectory.lexically_normal();

        std::wstring parameters;
        for (const auto& argument : command.arguments)
        {
            if (!parameters.empty())
            {
                parameters.push_back(L' ');
            }
            parameters += QuoteWindowsArgument(Utf8ToWide(argument));
        }
        const HINSTANCE result = ShellExecuteW(
            m_window,
            L"open",
            executable.c_str(),
            parameters.empty() ? nullptr : parameters.c_str(),
            workingDirectory.c_str(),
            SW_SHOWNORMAL);
        if (reinterpret_cast<std::intptr_t>(result) <= 32)
        {
            SetStatus(
                "プロジェクトツールを起動できません: "
                    + command.command,
                true);
            return;
        }
        SetStatus(
            "プロジェクトツールを起動しました: "
                + command.command);
    }

    void EditorLayer::DrawProjectMenuNode(
        ProjectMenuNode& node,
        const std::string_view idPath)
    {
        const std::string id = std::string{ idPath }
            + "/" + node.label;
        const std::string itemLabel = node.label
            + "##ProjectMenu/" + id;
        const bool enabled = !m_playing
            || (node.action.has_value()
                && node.action->enabledWhilePlaying);
        if (node.children.empty())
        {
            if ((node.action.has_value() || node.panelIndex.has_value())
                && ImGui::MenuItem(
                    itemLabel.c_str(),
                    nullptr,
                    false,
                    enabled))
            {
                if (node.panelIndex.has_value())
                {
                    m_projectPanels.at(*node.panelIndex).open = true;
                }
                else
                {
                    LaunchProjectMenuCommand(*node.action);
                }
            }
            return;
        }

        if (ImGui::BeginMenu(itemLabel.c_str()))
        {
            if (node.action.has_value() || node.panelIndex.has_value())
            {
                const std::string openLabel = "開く##ProjectMenuOpen/" + id;
                if (ImGui::MenuItem(
                    openLabel.c_str(),
                    nullptr,
                    false,
                    enabled))
                {
                    if (node.panelIndex.has_value())
                    {
                        m_projectPanels.at(*node.panelIndex).open = true;
                    }
                    else
                    {
                        LaunchProjectMenuCommand(*node.action);
                    }
                }
                ImGui::Separator();
            }
            for (auto& child : node.children)
            {
                DrawProjectMenuNode(child, id);
            }
            ImGui::EndMenu();
        }
    }

    void EditorLayer::DrawProjectMenus()
    {
        for (auto& menu : m_projectMenus)
        {
            DrawProjectMenuNode(menu, "root");
        }
    }

    void EditorLayer::DrawProjectPanels()
    {
        // 再生を始めたらBGMの試聴は必ず止めます。パネル自体はPlay中
        // 描かれないので、ここで止めないと鳴りっぱなしになります。
        if (m_playing && m_bgmPanel)
        {
            m_bgmPanel->StopPreview();
        }
        for (std::size_t index = 0; index < m_projectPanels.size(); ++index)
        {
            auto& panel = m_projectPanels[index];
            if (!panel.open)
            {
                // 閉じられたBGMパネルの試聴も残さない。
                if (m_bgmPanel
                    && m_bgmPanel->Matches(
                        ProjectSettingsPath().parent_path().parent_path()
                            / panel.dataPath))
                {
                    m_bgmPanel->StopPreview();
                }
                continue;
            }
            if (m_playing)
            {
                continue;
            }
            DrawProjectBgmPanel(index, panel);
        }
    }

    void EditorLayer::DrawProjectBgmPanel(
        const std::size_t, ProjectPanelDefinition& panel)
    {
        const auto root = ProjectSettingsPath().parent_path().parent_path();
        const auto catalog = root / panel.dataPath;
        if (!m_bgmPanel || !m_bgmPanel->Matches(catalog))
        {
            m_bgmPanel = std::make_unique<BgmLoopPanel>(
                m_graphics.Audio(), m_graphics.Assets(), root, catalog,
                [this](std::string message, const bool error)
                { SetStatus(std::move(message), error); });
        }
        m_bgmPanel->Draw(panel.title, panel.open, [&]
        {
            if (!panel.saveCommand.command.empty())
            {
                LaunchProjectMenuCommand(panel.saveCommand);
            }
        });
    }

    void EditorLayer::DrawToolbar()
    {
        const auto& inputOutput = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2{ 0.0f, 0.0f });
        ImGui::SetNextWindowSize(ImVec2{ inputOutput.DisplaySize.x, ToolbarHeight });

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_MenuBar;

        ImGui::Begin("##LamaPonToolbar", nullptr, flags);

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("ファイル"))
            {
                if (ImGui::MenuItem(
                    "新規シーン",
                    "Ctrl+N",
                    false,
                    !m_playing))
                {
                    NewScene();
                }
                if (ImGui::MenuItem(
                    "シーンを開く...",
                    "Ctrl+O",
                    false,
                    !m_playing))
                {
                    OpenScene();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "シーンを保存",
                    "Ctrl+S",
                    false,
                    !m_playing))
                {
                    SaveScene();
                }
                if (ImGui::MenuItem(
                    "名前を付けて保存...",
                    "Ctrl+Shift+S",
                    false,
                    !m_playing))
                {
                    SaveSceneAs();
                }
                if (ImGui::MenuItem(
                    "シーンを再読み込み",
                    "Ctrl+R",
                    false,
                    !m_playing
                        && !m_scenePath.empty()))
                {
                    ReloadScene();
                }
                if (ImGui::MenuItem(
                    "選択をPrefabとして保存...",
                    nullptr,
                    false,
                    !m_playing
                        && m_scene.FindGameObject(
                            m_selectedObjectId) != nullptr))
                {
                    SaveSelectedAsPrefab();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "プロジェクト設定...",
                    nullptr,
                    false,
                    !m_playing))
                {
                    OpenProjectSettingsDialog();
                }
                if (ImGui::MenuItem(
                    "ゲームをエクスポート...",
                    nullptr,
                    false,
                    !m_playing
                        && !m_scenePath.empty()))
                {
                    OpenGameExportDialog();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("終了", "Alt+F4"))
                {
                    PostMessageW(
                        m_window,
                        WM_CLOSE,
                        0,
                        0);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("編集"))
            {
                if (ImGui::MenuItem(
                    "元に戻す",
                    "Ctrl+Z",
                    false,
                    !m_playing && CanUndo()))
                {
                    Undo();
                }
                if (ImGui::MenuItem(
                    "やり直す",
                    "Ctrl+Y",
                    false,
                    !m_playing && CanRedo()))
                {
                    Redo();
                }
                ImGui::Separator();
                const bool hasSelection =
                    m_scene.FindGameObject(
                        m_selectedObjectId) != nullptr;
                if (ImGui::MenuItem(
                    "切り取り",
                    "Ctrl+X",
                    false,
                    !m_playing && hasSelection))
                {
                    CutSelectedGameObject();
                }
                if (ImGui::MenuItem(
                    "コピー",
                    "Ctrl+C",
                    false,
                    !m_playing && hasSelection))
                {
                    CopySelectedGameObject();
                }
                if (ImGui::MenuItem(
                    "貼り付け",
                    "Ctrl+V",
                    false,
                    !m_playing
                        && !m_clipboardSceneJson.empty()))
                {
                    PasteGameObject();
                }
                if (ImGui::MenuItem(
                    "複製",
                    "Ctrl+D",
                    false,
                    !m_playing && hasSelection))
                {
                    DuplicateSelectedGameObject();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("シーン"))
            {
                ImGui::MenuItem(
                    "環境設定...",
                    nullptr,
                    &m_sceneEnvironmentOpen);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("GameObject"))
            {
                const bool hasSelection =
                    m_scene.FindGameObject(
                        m_selectedObjectId) != nullptr;
                if (ImGui::MenuItem(
                    "空のルートを作成",
                    nullptr,
                    false,
                    !m_playing))
                {
                    CreateRootGameObject();
                }
                if (ImGui::MenuItem(
                    "空の子を作成",
                    nullptr,
                    false,
                    !m_playing && hasSelection))
                {
                    CreateChildGameObject();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("UI"))
                {
                    if (ImGui::MenuItem(
                        "Canvas",
                        nullptr,
                        false,
                        !m_playing))
                    {
                        CreateUICanvasGameObject();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "選択オブジェクトを削除",
                    "Delete",
                    false,
                    !m_playing && hasSelection))
                {
                    DeleteSelectedGameObject();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("アセット"))
            {
                if (ImGui::MenuItem(
                    "ファイルをインポート...",
                    nullptr,
                    false,
                    !m_playing
                        && m_gameModuleBuildProcess == nullptr))
                {
                    OpenImportAssetsDialog();
                }
                if (ImGui::MenuItem("アセットを更新", "F5"))
                {
                    RefreshAssets();
                }
                if (ImGui::MenuItem(
                    "選択項目をエクスプローラーで表示",
                    nullptr,
                    false,
                    !m_selectedAsset.empty()))
                {
                    OpenAssetInExplorer(
                        m_selectedAsset,
                        true);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("ウィンドウ"))
            {
                if (ImGui::MenuItem(
                        "フルスクリーン",
                        "F11",
                        m_fullscreen))
                {
                    ToggleFullscreen();
                }
                ImGui::Separator();
                ImGui::MenuItem(
                    "コンソール",
                    nullptr,
                    &m_consolePanelOpen);
                ImGui::MenuItem(
                    "パフォーマンス",
                    nullptr,
                    &m_performancePanelOpen);
                ImGui::MenuItem(
                    "セーブデータ",
                    nullptr,
                    &m_persistencePanelOpen);
                ImGui::MenuItem(
                    "アセット",
                    nullptr,
                    &m_assetBrowserPanelOpen);
                ImGui::MenuItem(
                    "タイルパレット",
                    nullptr,
                    &m_tilePalettePanelOpen);
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "レイアウトを保存"))
                {
                    ImGui::SaveIniSettingsToDisk(
                        m_imguiIniPath.c_str());
                    SaveEditorSettings();
                    SetStatus(
                        "ウィンドウレイアウトを保存しました");
                }
                if (ImGui::MenuItem(
                    "標準レイアウトに戻す"))
                {
                    m_resetDockLayout = true;
                    SetStatus(
                        "標準レイアウトへ戻しました");
                }
                ImGui::EndMenu();
            }

            DrawProjectMenus();

            // パッケージはパネルの表示切り替えではなく機能追加の
            // 入口なので、独立したメニューにします。
            if (ImGui::BeginMenu("拡張機能"))
            {
                if (ImGui::MenuItem(
                    "パッケージを探す...",
                    nullptr,
                    &m_packagesPanelOpen))
                {
                    if (m_packagesPanelOpen)
                    {
                        SetStatus(
                            "公式パッケージの一覧を取得しています");
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "Zipから読み込む...",
                    nullptr,
                    false,
                    !m_playing))
                {
                    ImportPackageFromZipDialog();
                }
                if (ImGui::MenuItem(
                    "パッケージを作成...",
                    nullptr,
                    false,
                    !m_playing))
                {
                    OpenPackageBuildDialog();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                    "インストール先を開く"))
                {
                    const auto packagesRoot =
                        m_graphics.Assets().AssetRoot()
                        / L"packages";
                    std::error_code createError;
                    std::filesystem::create_directories(
                        packagesRoot,
                        createError);
                    ShellExecuteW(
                        m_window,
                        L"open",
                        packagesRoot.c_str(),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("ヘルプ"))
            {
                if (ImGui::MenuItem("マニュアル"))
                {
                    ShellExecuteW(
                        m_window,
                        L"open",
                        L"https://lamapon-wiki.lamapon.workers.dev",
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        const float rowY = ImGui::GetCursorPosY();
        const float windowWidth =
            ImGui::GetWindowWidth();

        ImGui::SetCursorPos(
            ImVec2{ 8.0f, rowY + 3.0f });
        const ImVec4 statusColor = m_statusIsError
            ? ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f }
            : ImVec4{ 0.35f, 0.85f, 0.55f, 1.0f };
        const std::string sceneLabel =
            m_scenePath.empty()
                ? "無題のシーン"
                : PathToUtf8(m_scenePath.filename());
        // セーフモード中は、C++スクリプトが動いていないことを
        // 常に分かるようにします（Play中の不可解な挙動を防ぐため）。
        const std::string safeModeLabel =
            "セーフモード：C++スクリプトは読み込まれていません";
        const std::string& leftText = m_safeMode
            ? safeModeLabel
            : (m_statusMessage.empty()
                ? sceneLabel
                : m_statusMessage);
        const auto windowPosition =
            ImGui::GetWindowPos();
        ImGui::PushClipRect(
            ImVec2{
                windowPosition.x + 8.0f,
                windowPosition.y + rowY
            },
            ImVec2{
                windowPosition.x
                    + windowWidth * 0.5f - 52.0f,
                windowPosition.y
                    + ToolbarHeight
            },
            true);
        if (m_safeMode)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.75f, 0.30f, 1.0f },
                "%s",
                leftText.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(
                "通常モードで開き直す"))
            {
                // ここで未保存を確認し、確定したらウィンドウを破棄
                // します（WM_CLOSE経由だと確認が二重になるため）。
                // 実際の再起動は、プロジェクトのロックが解放された
                // プロセス終了後に行います。
                if (ConfirmClose())
                {
                    s_normalModeRestartRequested = true;
                    DestroyWindow(m_window);
                }
            }
        }
        else if (m_statusMessage.empty())
        {
            ImGui::TextDisabled(
                "%s",
                leftText.c_str());
        }
        else
        {
            ImGui::TextColored(
                statusColor,
                "%s",
                leftText.c_str());
        }
        ImGui::PopClipRect();

        // 再生中は「停止／一時停止（再開）／次のフレーム」の3つを並べます
        // ボタンの数で幅が変わるので、まとめて
        // 中央へ寄せます。
        constexpr float playButtonWidth = 72.0f;
        constexpr float pauseButtonWidth = 96.0f;
        constexpr float stepButtonWidth = 112.0f;
        const float spacing =
            ImGui::GetStyle().ItemSpacing.x;
        const float groupWidth = m_playing
            ? playButtonWidth
                + pauseButtonWidth
                + stepButtonWidth
                + spacing * 2.0f
            : playButtonWidth;
        ImGui::SetCursorPos(
            ImVec2{
                (windowWidth - groupWidth) * 0.5f,
                rowY
            });
        if (!m_playing)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{ 0.12f, 0.35f, 0.62f, 1.0f });
            if (ImGui::Button(
                "再生",
                ImVec2{ playButtonWidth, 0.0f }))
            {
                StartPlaying();
            }
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4{ 0.65f, 0.18f, 0.16f, 1.0f });
            if (ImGui::Button(
                "停止",
                ImVec2{ playButtonWidth, 0.0f }))
            {
                StopPlaying();
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                m_paused
                    ? ImVec4{ 0.20f, 0.48f, 0.26f, 1.0f }
                    : ImVec4{ 0.48f, 0.40f, 0.12f, 1.0f });
            if (ImGui::Button(
                m_paused ? "再開" : "一時停止",
                ImVec2{ pauseButtonWidth, 0.0f }))
            {
                SetPaused(!m_paused);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "ゲームの更新だけを止めます。描画は続くので"
                    "Scene Viewで自由に見回せます。");
            }

            ImGui::SameLine();
            // ステップは一時停止中だけ意味があります。
            ImGui::BeginDisabled(!m_paused);
            if (ImGui::Button(
                "次のフレーム",
                ImVec2{ stepButtonWidth, 0.0f }))
            {
                RequestSimulationStep();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    m_paused
                        ? "1フレームだけ進めます。"
                        : "一時停止中に使えます。");
            }
        }

        const std::string statistics =
            "FPS: "
            + std::to_string(
                static_cast<int>(
                    std::lround(
                        m_graphics.FrameStats()
                            .framesPerSecond)))
            + "  オブジェクト: "
            + std::to_string(
                m_scene.GameObjects().size())
            + "  画像: "
            + std::to_string(
                m_graphics.Assets().CachedTextureCount())
            + "  モデル: "
            + std::to_string(
                m_graphics.Assets().CachedModelCount())
            + "  文字: "
            + std::to_string(
                m_graphics.Assets().CachedTextCount());
        const float statisticsWidth =
            ImGui::CalcTextSize(
                statistics.c_str()).x;
        ImGui::SetCursorPos(
            ImVec2{
                std::max(
                    windowWidth - statisticsWidth - 12.0f,
                    windowWidth * 0.5f + 52.0f),
                rowY + 3.0f
            });
        ImGui::TextDisabled(
            "%s",
            statistics.c_str());

        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyCtrl
            && !inputOutput.KeyShift
            && ImGui::IsKeyPressed(
                ImGuiKey_N,
                false))
        {
            NewScene();
        }
        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyCtrl
            && !inputOutput.KeyShift
            && ImGui::IsKeyPressed(
                ImGuiKey_O,
                false))
        {
            OpenScene();
        }
        if (!inputOutput.WantTextInput
            && !m_playing
            && inputOutput.KeyCtrl
            && !inputOutput.KeyShift
            && ImGui::IsKeyPressed(
                ImGuiKey_R,
                false))
        {
            ReloadScene();
        }
        if (!inputOutput.WantTextInput
            && ImGui::IsKeyPressed(
                ImGuiKey_F5,
                false))
        {
            RefreshAssets();
        }
        if (!inputOutput.WantTextInput
            && ImGui::IsKeyPressed(
                ImGuiKey_F11,
                false))
        {
            ToggleFullscreen();
        }

        if (inputOutput.KeyCtrl
            && inputOutput.KeyShift
            && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveSceneAs();
        }
        else if (inputOutput.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            SaveScene();
        }

        if (!m_playing && inputOutput.KeyCtrl && !inputOutput.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
            {
                if (inputOutput.KeyShift)
                {
                    Redo();
                }
                else
                {
                    Undo();
                }
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            {
                Redo();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            {
                CopySelectedGameObject();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            {
                CutSelectedGameObject();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_V, false))
            {
                PasteGameObject();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_D, false))
            {
                DuplicateSelectedGameObject();
            }
        }
        if (!m_playing
            && !inputOutput.WantTextInput
            && ImGui::IsKeyPressed(
                ImGuiKey_Delete,
                false))
        {
            DeleteSelectedGameObject();
        }

        ImGui::End();
    }

    void EditorLayer::DrawDockSpace()
    {
        const ImGuiViewport* viewport =
            ImGui::GetMainViewport();
        const ImVec2 dockPosition{
            viewport->Pos.x,
            viewport->Pos.y + ToolbarHeight
        };
        const ImVec2 dockSize{
            viewport->Size.x,
            std::max(
                viewport->Size.y - ToolbarHeight,
                1.0f)
        };

        ImGui::SetNextWindowPos(dockPosition);
        ImGui::SetNextWindowSize(dockSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        constexpr ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoNavFocus
            | ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowRounding,
            0.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowBorderSize,
            0.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{ 0.0f, 0.0f });
        ImGui::Begin(
            "##LamaPonDockSpaceHost",
            nullptr,
            hostFlags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockspaceId =
            ImGui::GetID("LamaPonDockSpace");
        if (m_resetDockLayout
            || ImGui::DockBuilderGetNode(
                dockspaceId) == nullptr)
        {
            ImGui::DockBuilderRemoveNode(
                dockspaceId);
            ImGui::DockBuilderAddNode(
                dockspaceId,
                ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodePos(
                dockspaceId,
                dockPosition);
            ImGui::DockBuilderSetNodeSize(
                dockspaceId,
                dockSize);

            ImGuiID centerId = dockspaceId;
            ImGuiID leftId{};
            ImGuiID rightId{};
            ImGui::DockBuilderSplitNode(
                centerId,
                ImGuiDir_Left,
                0.23f,
                &leftId,
                &centerId);
            ImGui::DockBuilderSplitNode(
                centerId,
                ImGuiDir_Right,
                0.25f,
                &rightId,
                &centerId);

            ImGuiID hierarchyId = leftId;
            ImGuiID assetId{};
            ImGui::DockBuilderSplitNode(
                hierarchyId,
                ImGuiDir_Down,
                0.46f,
                &assetId,
                &hierarchyId);

            ImGuiID consoleId{};
            ImGui::DockBuilderSplitNode(
                centerId,
                ImGuiDir_Down,
                0.28f,
                &consoleId,
                &centerId);

            ImGui::DockBuilderDockWindow(
                "ヒエラルキー",
                hierarchyId);
            ImGui::DockBuilderDockWindow(
                "アセット",
                assetId);
            ImGui::DockBuilderDockWindow(
                "タイルパレット",
                assetId);
            ImGui::DockBuilderDockWindow(
                "ビューポート",
                centerId);
            ImGui::DockBuilderDockWindow(
                "コンソール",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "セーブデータ",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "パフォーマンス",
                consoleId);
            ImGui::DockBuilderDockWindow(
                "インスペクター",
                rightId);
            ImGui::DockBuilderFinish(
                dockspaceId);
            m_resetDockLayout = false;
        }

        ImGui::DockSpace(
            dockspaceId,
            ImVec2{ 0.0f, 0.0f });
        ImGui::End();
    }

    void EditorLayer::DrawConsole()
    {
        if (!m_consolePanelOpen)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 720.0f, 230.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                "コンソール",
                &m_consolePanelOpen,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        auto& logger = Logger::Instance();
        if (!m_consolePaused)
        {
            m_consoleEntries =
                logger.Snapshot();
        }

        if (ImGui::Button("クリア"))
        {
            logger.Clear();
            m_consoleEntries.clear();
            m_consoleLastSequence = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(
                m_consolePaused
                    ? "再開"
                    : "一時停止"))
        {
            m_consolePaused =
                !m_consolePaused;
            if (!m_consolePaused)
            {
                m_consoleEntries =
                    logger.Snapshot();
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox(
            "自動スクロール",
            &m_consoleAutoScroll);

        std::size_t infoCount{};
        std::size_t warningCount{};
        std::size_t errorCount{};
        for (const auto& entry :
            m_consoleEntries)
        {
            switch (entry.level)
            {
            case LogLevel::Warning:
                ++warningCount;
                break;
            case LogLevel::Error:
                ++errorCount;
                break;
            default:
                ++infoCount;
                break;
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox(
            ("Info "
                + std::to_string(infoCount)
                + "##ConsoleInfo").c_str(),
            &m_consoleShowInfo);
        ImGui::SameLine();
        ImGui::Checkbox(
            ("Warning "
                + std::to_string(warningCount)
                + "##ConsoleWarning").c_str(),
            &m_consoleShowWarning);
        ImGui::SameLine();
        ImGui::Checkbox(
            ("Error "
                + std::to_string(errorCount)
                + "##ConsoleError").c_str(),
            &m_consoleShowError);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##ConsoleFilter",
            "メッセージまたはソースを検索",
            m_consoleFilter.data(),
            m_consoleFilter.size());

        const std::string filter =
            Lowercase(
                std::string{
                    m_consoleFilter.data() });
        const std::uint64_t newestSequence =
            m_consoleEntries.empty()
                ? 0
                : m_consoleEntries.back().
                    sequence;
        const bool hasNewEntries =
            newestSequence
                != m_consoleLastSequence;

        ImGui::BeginChild(
            "ConsoleEntries",
            ImVec2{ 0.0f, 0.0f },
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry :
            m_consoleEntries)
        {
            if ((entry.level
                        == LogLevel::Info
                    && !m_consoleShowInfo)
                || (entry.level
                        == LogLevel::Warning
                    && !m_consoleShowWarning)
                || (entry.level
                        == LogLevel::Error
                    && !m_consoleShowError))
            {
                continue;
            }

            const std::string sourceName =
                entry.sourceFile.empty()
                    ? std::string{}
                    : PathToUtf8(
                        std::filesystem::path(
                            entry.sourceFile)
                            .filename());
            if (!filter.empty())
            {
                const std::string searchable =
                    Lowercase(
                        entry.message
                        + " "
                        + sourceName);
                if (searchable.find(filter)
                    == std::string::npos)
                {
                    continue;
                }
            }

            const auto milliseconds =
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        entry.timestamp.
                            time_since_epoch())
                    % 1000;
            const std::time_t rawTime =
                std::chrono::system_clock::
                    to_time_t(
                        entry.timestamp);
            std::tm localTime{};
            localtime_s(
                &localTime,
                &rawTime);
            std::array<char, 32>
                timeBuffer{};
            std::snprintf(
                timeBuffer.data(),
                timeBuffer.size(),
                "%02d:%02d:%02d.%03lld",
                localTime.tm_hour,
                localTime.tm_min,
                localTime.tm_sec,
                static_cast<long long>(
                    milliseconds.count()));

            const char* levelText =
                entry.level
                    == LogLevel::Warning
                    ? "WARN"
                    : entry.level
                        == LogLevel::Error
                        ? "ERROR"
                        : "INFO";
            const ImVec4 color =
                entry.level
                    == LogLevel::Warning
                    ? ImVec4{
                        1.0f, 0.78f,
                        0.24f, 1.0f }
                    : entry.level
                        == LogLevel::Error
                        ? ImVec4{
                            1.0f, 0.34f,
                            0.32f, 1.0f }
                        : ImVec4{
                            0.76f, 0.84f,
                            0.92f, 1.0f };
            std::string display =
                std::string{ "[" }
                + timeBuffer.data()
                + "] ["
                + levelText
                + "] "
                + entry.message;
            if (entry.gameObjectId != 0)
            {
                display +=
                    "  [GameObject "
                    + std::to_string(
                        entry.gameObjectId)
                    + "]";
            }
            if (!sourceName.empty())
            {
                display +=
                    "  ("
                    + sourceName
                    + ":"
                    + std::to_string(
                        entry.sourceLine)
                    + ")";
            }

            ImGui::PushID(
                static_cast<int>(
                    entry.sequence
                    & 0x7fffffff));
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                color);
            ImGui::Selectable(
                display.c_str(),
                false,
                ImGuiSelectableFlags_AllowDoubleClick);
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered()
                && ImGui::
                    IsMouseDoubleClicked(
                        ImGuiMouseButton_Left)
                && entry.gameObjectId != 0
                && m_scene.FindGameObject(
                    entry.gameObjectId)
                    != nullptr)
            {
                m_selectedObjectId =
                    entry.gameObjectId;
            }
            if (ImGui::
                BeginPopupContextItem(
                    "ConsoleEntryMenu"))
            {
                if (ImGui::MenuItem(
                        "メッセージをコピー"))
                {
                    ImGui::SetClipboardText(
                        entry.message.c_str());
                }
                if (ImGui::MenuItem(
                        "行全体をコピー"))
                {
                    ImGui::SetClipboardText(
                        display.c_str());
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()
                && !entry.sourceFile.empty())
            {
                ImGui::SetTooltip(
                    "%s:%u",
                    entry.sourceFile.c_str(),
                    entry.sourceLine);
            }
            ImGui::PopID();
        }

        if (m_consoleAutoScroll
            && hasNewEntries
            && !m_consolePaused)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        m_consoleLastSequence =
            newestSequence;
        ImGui::End();
    }

    void EditorLayer::DrawPerformancePanel()
    {
        if (!m_performancePanelOpen)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 560.0f, 300.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                "パフォーマンス",
                &m_performancePanelOpen,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        const auto& frame =
            m_graphics.FrameStats();
        m_performanceFrameTimes[
            m_performanceSampleIndex] =
                frame.frameTimeMilliseconds;
        m_performanceCpuTimes[
            m_performanceSampleIndex] =
                frame.cpuTimeMilliseconds;
        m_performanceSampleIndex =
            (m_performanceSampleIndex + 1)
            % m_performanceFrameTimes.size();

        const auto& settings =
            m_graphics.Settings();
        const float frameBudget =
            settings.targetFrameRate > 0
            ? 1000.0f
                / static_cast<float>(
                    settings.targetFrameRate)
            : 16.6667f;
        const ImVec4 frameColor =
            frame.frameTimeMilliseconds
                <= frameBudget
            ? ImVec4{
                0.35f,
                0.85f,
                0.55f,
                1.0f }
            : ImVec4{
                1.0f,
                0.45f,
                0.25f,
                1.0f };
        ImGui::TextColored(
            frameColor,
            "%.1f FPS",
            frame.framesPerSecond);
        ImGui::SameLine();
        ImGui::Text(
            "  Frame %.2f ms  CPU+Present %.2f ms",
            frame.frameTimeMilliseconds,
            frame.cpuTimeMilliseconds);
        ImGui::Text(
            "VSync: %s  FPS上限: %s",
            settings.vSyncEnabled
                ? "ON"
                : "OFF",
            settings.targetFrameRate == 0
                ? "無制限"
                : std::to_string(
                    settings.targetFrameRate)
                    .c_str());
        // 「FPS上限を上げたのに数字が動かない」の理由が出るのはここ
        // だけです。どちらの場合も、実際の上限は設定値ではなく
        // モニターのリフレッシュレートになります。
        if (settings.vSyncEnabled)
        {
            ImGui::TextDisabled(
                "VSyncが有効なので、実際の上限はモニターの"
                "リフレッシュレートです");
        }
        else if (!m_graphics.TearingAllowed())
        {
            ImGui::TextDisabled(
                "この環境はティアリング許可に対応していないため、"
                "実際の上限はモニターのリフレッシュレートです");
        }

        const float graphMaximum =
            std::max(
                33.3333f,
                *std::max_element(
                    m_performanceFrameTimes.begin(),
                    m_performanceFrameTimes.end())
                    * 1.1f);
        ImGui::PlotLines(
            "Frame ms",
            m_performanceFrameTimes.data(),
            static_cast<int>(
                m_performanceFrameTimes.size()),
            static_cast<int>(
                m_performanceSampleIndex),
            nullptr,
            0.0f,
            graphMaximum,
            ImVec2{ 0.0f, 54.0f });
        ImGui::PlotLines(
            "CPU+Present ms",
            m_performanceCpuTimes.data(),
            static_cast<int>(
                m_performanceCpuTimes.size()),
            static_cast<int>(
                m_performanceSampleIndex),
            nullptr,
            0.0f,
            graphMaximum,
            ImVec2{ 0.0f, 54.0f });

        ImGui::SeparatorText("GPU");
        const auto& gpu = m_graphics.Gpu();
        if (!gpu.IsSupported())
        {
            ImGui::TextDisabled(
                "この環境ではGPU計測を利用できません。");
        }
        else
        {
            // 値は数フレーム前の確定した計測結果です。
            ImGui::Text(
                "GPU合計 %.2f ms",
                gpu.LatestFrameMilliseconds());
            const auto& pipeline =
                gpu.LatestPipelineStatistics();
            if (pipeline.valid)
            {
                ImGui::Text(
                    "GPU workload: IA %llu primitives / %llu vertices",
                    static_cast<unsigned long long>(
                        pipeline.inputAssemblerPrimitives),
                    static_cast<unsigned long long>(
                        pipeline.inputAssemblerVertices));
                ImGui::Text(
                    "Shader calls: VS %llu  PS %llu  CS %llu",
                    static_cast<unsigned long long>(
                        pipeline.vertexShaderInvocations),
                    static_cast<unsigned long long>(
                        pipeline.pixelShaderInvocations),
                    static_cast<unsigned long long>(
                        pipeline.computeShaderInvocations));
            }
            // 区間は入れ子にできるので、深さでインデントし、
            // 合計は最上位（depth==0）だけを足します。内側も
            // 足すと二重に数えてGPU合計を超えます。
            float topLevelTotal = 0.0f;
            for (const auto& section :
                gpu.LatestSections())
            {
                if (section.depth == 0)
                {
                    topLevelTotal += section.milliseconds;
                }
                ImGui::Text(
                    "%*s%s: %.2f ms",
                    static_cast<int>(
                        2 + section.depth * 2),
                    "",
                    section.name.c_str(),
                    section.milliseconds);
            }
            // 区間で囲われていないGPU作業には、Present待ちや
            // 計測対象外の描画が含まれます。値が大きい場合は
            // 計測区間の追加が必要です。
            const float unmeasured =
                gpu.LatestFrameMilliseconds()
                - topLevelTotal;
            if (unmeasured > 0.01f)
            {
                ImGui::TextDisabled(
                    "  その他（未計測）: %.2f ms",
                    unmeasured);
            }
        }

        ImGui::SeparatorText("メモリ");
        const auto& memory = m_graphics.MemoryStats();
        constexpr double bytesPerMiB = 1024.0 * 1024.0;
        ImGui::Text(
            "Process RAM: working %.1f MiB  private %.1f MiB",
            static_cast<double>(memory.processWorkingSetBytes)
                / bytesPerMiB,
            static_cast<double>(memory.processPrivateBytes)
                / bytesPerMiB);
        ImGui::Text(
            "System RAM: %.1f / %.1f MiB",
            static_cast<double>(memory.systemPhysicalUsedBytes)
                / bytesPerMiB,
            static_cast<double>(memory.systemPhysicalTotalBytes)
                / bytesPerMiB);
        if (memory.videoMemoryBudgetAvailable)
        {
            ImGui::Text(
                "VRAM local: %.1f / %.1f MiB  dedicated: %.1f MiB",
                static_cast<double>(
                    memory.localVideoMemoryUsageBytes) / bytesPerMiB,
                static_cast<double>(
                    memory.localVideoMemoryBudgetBytes) / bytesPerMiB,
                static_cast<double>(
                    memory.dedicatedVideoMemoryBytes) / bytesPerMiB);
            if (memory.nonLocalVideoMemoryBudgetBytes > 0)
            {
                ImGui::Text(
                    "VRAM non-local: %.1f / %.1f MiB  shared: %.1f MiB",
                    static_cast<double>(
                        memory.nonLocalVideoMemoryUsageBytes)
                        / bytesPerMiB,
                    static_cast<double>(
                        memory.nonLocalVideoMemoryBudgetBytes)
                        / bytesPerMiB,
                    static_cast<double>(
                        memory.sharedSystemMemoryBytes)
                        / bytesPerMiB);
            }
        }
        else
        {
            ImGui::TextDisabled(
                "このアダプターではDXGIのVRAM予算を取得できません。");
        }

        ImGui::SeparatorText("固定物理");
        const std::size_t interpolatedBodies =
            static_cast<std::size_t>(
                std::ranges::count_if(
                    m_scene.GameObjects(),
                    [](const auto& object)
                    {
                        const auto* body =
                            object->template GetComponent<
                                RigidbodyComponent>();
                        return body != nullptr
                            && body->IsEnabled()
                            && body->Interpolates();
                    }));
        ImGui::Text(
            "Rate: 60 Hz  Steps: %zu  Interpolation α: %.2f  Bodies: %zu",
            m_scene.PhysicsFixedStepsLastFrame(),
            m_scene.PhysicsInterpolationAlpha(),
            interpolatedBodies);
        const auto& physics =
            m_scene.PhysicsStats();
        ImGui::Text(
            "Collider 2D/3D: %zu / %zu  Candidate: %zu  Narrow: %zu  Contact: %zu",
            physics.colliderCount2D,
            physics.colliderCount3D,
            physics.candidatePairCount2D
                + physics.candidatePairCount3D,
            physics.narrowPhaseTestCount2D
                + physics.narrowPhaseTestCount3D,
            physics.activeContactCount);

        const auto& visibility =
            m_scene.VisibilityStats();
        ImGui::SeparatorText("描画");
        // どちらの経路で描いているかは絵からは判別できないので、
        // 設定が効いているかの確認用に出します。
        ImGui::Text(
            "描画方式: %s",
            settings.renderingPath
                == RenderingPath::ForwardPlus
                ? "Forward+（クラスタライトカリング）"
                : "Forward（ライト上限まで）");
        ImGui::Text(
            "Renderer: %zu  Visible: %zu  Frustum: %zu  Occlusion: %zu  LOD: %zu",
            visibility.rendererCount,
            visibility.visibleRendererCount,
            visibility.frustumCulledCount,
            visibility.occlusionCulledCount,
            visibility.lodCulledCount);
        ImGui::Text(
            "BVH: %zu nodes / %zu tests  Cache: %s",
            visibility.spatialNodeCount,
            visibility.spatialNodeTestCount,
            visibility.spatialIndexReused
                ? "reuse"
                : "rebuild");
        ImGui::Text(
            "Auto LOD: %zu renderers  Saved: %llu triangles",
            visibility.automaticLodRendererCount,
            static_cast<unsigned long long>(
                visibility.automaticLodTrianglesSaved));
        ImGui::Text(
            "Instancing mesh/model: %zu/%zu batches  %zu/%zu renderers",
            visibility.meshInstanceBatchCount,
            visibility.modelInstanceBatchCount,
            visibility.meshInstancedRendererCount,
            visibility.modelInstancedRendererCount);
        ImGui::Text(
            "Asset upload: texture queue %zu  model queue %zu  model %.2f MiB/frame",
            m_graphics.Assets().PendingTextureUploadCount(),
            m_graphics.Assets().PendingModelUploadCount(),
            static_cast<double>(
                m_graphics.Assets().ModelUploadBytesLastFrame())
                / (1024.0 * 1024.0));

        ImGui::SeparatorText("CPUプロファイラー");
        auto& profiler = Profiler::Instance();
        bool profilerEnabled = profiler.IsEnabled();
        if (ImGui::Checkbox(
                "フレーム計測を有効化",
                &profilerEnabled))
        {
            profiler.SetEnabled(profilerEnabled);
        }
        ImGui::SameLine();
        if (ImGui::Button("JSONを書き出す"))
        {
            const auto profilePath =
                m_graphics.Assets().AssetRoot().
                    parent_path()
                / ".lamapon"
                / "profile.json";
            if (profiler.WriteJson(profilePath))
            {
                SetStatus(
                    "プロファイルを書き出しました: "
                    + PathToUtf8(profilePath));
            }
            else
            {
                SetStatus(
                    "プロファイルを書き出せませんでした",
                    true);
            }
        }

        const auto profileFrames = profiler.Snapshot();
        if (!profileFrames.empty()
            && ImGui::BeginTable(
                "ProfilerSamples",
                3,
                ImGuiTableFlags_Borders
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("区間");
            ImGui::TableSetupColumn(
                "ms",
                ImGuiTableColumnFlags_WidthFixed,
                90.0f);
            ImGui::TableSetupColumn(
                "呼出",
                ImGuiTableColumnFlags_WidthFixed,
                60.0f);
            ImGui::TableHeadersRow();
            for (const auto& sample :
                profileFrames.back().samples)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(
                    sample.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", sample.milliseconds);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", sample.callCount);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void EditorLayer::DrawPersistencePanel()
    {
        if (!m_persistencePanelOpen)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 720.0f, 260.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                "セーブデータ",
                &m_persistencePanelOpen,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped(
            "保存先: %s",
            PathToUtf8(
                m_playerPrefs.FilePath().
                    parent_path()).c_str());
        if (ImGui::Button("PlayerPrefsを保存"))
        {
            try
            {
                m_playerPrefs.Save();
                SetStatus(
                    "PlayerPrefsを保存しました");
            }
            catch (const std::exception& exception)
            {
                SetStatus(exception.what(), true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            try
            {
                m_playerPrefs.Load();
                SetStatus(
                    "PlayerPrefsを再読み込みしました");
            }
            catch (const std::exception& exception)
            {
                SetStatus(exception.what(), true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存フォルダーを開く"))
        {
            std::filesystem::create_directories(
                m_playerPrefs.FilePath().
                    parent_path());
            ShellExecuteW(
                m_window,
                L"open",
                m_playerPrefs.FilePath().
                    parent_path().c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            m_playerPrefs.IsDirty()
                ? "未保存の変更あり"
                : "保存済み");

        if (ImGui::BeginTable(
                "PlayerPrefsTable",
                4,
                ImGuiTableFlags_Borders
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("キー");
            ImGui::TableSetupColumn("型");
            ImGui::TableSetupColumn("値");
            ImGui::TableSetupColumn(
                "操作",
                ImGuiTableColumnFlags_WidthFixed,
                64.0f);
            ImGui::TableHeadersRow();
            for (const auto& key :
                m_playerPrefs.Keys())
            {
                ImGui::PushID(key.c_str());
                const auto type =
                    m_playerPrefs.TypeOf(key);
                std::string typeName;
                std::string value;
                switch (type)
                {
                case PlayerPrefType::Integer:
                    typeName = "整数";
                    value = std::to_string(
                        m_playerPrefs.GetInteger(key));
                    break;
                case PlayerPrefType::Number:
                    typeName = "小数";
                    value = std::to_string(
                        m_playerPrefs.GetNumber(key));
                    break;
                case PlayerPrefType::Boolean:
                    typeName = "真偽値";
                    value = m_playerPrefs.GetBoolean(key)
                        ? "true"
                        : "false";
                    break;
                case PlayerPrefType::String:
                    typeName = "文字列";
                    value =
                        m_playerPrefs.GetString(key);
                    break;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(key.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(typeName.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(value.c_str());
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("削除"))
                {
                    m_playerPrefs.DeleteKey(key);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::InputText(
            "キー##PlayerPref",
            m_playerPrefKeyBuffer.data(),
            m_playerPrefKeyBuffer.size());
        constexpr const char* types[]{
            "整数",
            "小数",
            "真偽値",
            "文字列"
        };
        ImGui::Combo(
            "型##PlayerPref",
            &m_playerPrefType,
            types,
            static_cast<int>(std::size(types)));
        if (m_playerPrefType == 2)
        {
            ImGui::Checkbox(
                "値##PlayerPrefBoolean",
                &m_playerPrefBoolean);
        }
        else
        {
            ImGui::InputText(
                "値##PlayerPref",
                m_playerPrefValueBuffer.data(),
                m_playerPrefValueBuffer.size());
        }
        if (ImGui::Button("追加／更新"))
        {
            try
            {
                const std::string key(
                    m_playerPrefKeyBuffer.data());
                const std::string value(
                    m_playerPrefValueBuffer.data());
                switch (m_playerPrefType)
                {
                case 0:
                    m_playerPrefs.SetInteger(
                        key,
                        std::stoll(value));
                    break;
                case 1:
                    m_playerPrefs.SetNumber(
                        key,
                        std::stod(value));
                    break;
                case 2:
                    m_playerPrefs.SetBoolean(
                        key,
                        m_playerPrefBoolean);
                    break;
                default:
                    m_playerPrefs.SetString(
                        key,
                        value);
                    break;
                }
                SetStatus(
                    "PlayerPrefsを更新しました");
            }
            catch (const std::exception& exception)
            {
                SetStatus(exception.what(), true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("すべて削除"))
        {
            ImGui::OpenPopup(
                "DeleteAllPlayerPrefs");
        }
        if (ImGui::BeginPopupModal(
                "DeleteAllPlayerPrefs",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(
                "すべてのPlayerPrefsを削除しますか？");
            if (ImGui::Button("削除する"))
            {
                m_playerPrefs.DeleteAll();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("キャンセル"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SeparatorText("JSONセーブスロット");
        ImGui::BeginChild(
            "SaveSlotList",
            ImVec2{ 190.0f, 150.0f },
            ImGuiChildFlags_Borders);
        for (const auto& slot :
            m_saveData.ListSlots())
        {
            if (ImGui::Selectable(
                    slot.c_str(),
                    slot == m_selectedSaveSlot))
            {
                try
                {
                    m_selectedSaveSlot = slot;
                    strncpy_s(
                        m_saveSlotBuffer.data(),
                        m_saveSlotBuffer.size(),
                        slot.c_str(),
                        _TRUNCATE);
                    const auto json =
                        m_saveData.LoadJson(slot);
                    strncpy_s(
                        m_saveJsonBuffer.data(),
                        m_saveJsonBuffer.size(),
                        json
                            ? json->c_str()
                            : "{}",
                        _TRUNCATE);
                }
                catch (const std::exception& exception)
                {
                    SetStatus(exception.what(), true);
                }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::InputText(
            "スロット名",
            m_saveSlotBuffer.data(),
            m_saveSlotBuffer.size());
        ImGui::InputTextMultiline(
            "JSON",
            m_saveJsonBuffer.data(),
            m_saveJsonBuffer.size(),
            ImVec2{ -1.0f, 92.0f });
        if (ImGui::Button("スロットを保存"))
        {
            try
            {
                m_saveData.SaveJson(
                    m_saveSlotBuffer.data(),
                    m_saveJsonBuffer.data());
                m_selectedSaveSlot =
                    m_saveSlotBuffer.data();
                SetStatus(
                    "セーブスロットを保存しました");
            }
            catch (const std::exception& exception)
            {
                SetStatus(exception.what(), true);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(
            m_selectedSaveSlot.empty());
        if (ImGui::Button("スロットを削除"))
        {
            try
            {
                m_saveData.DeleteSlot(
                    m_selectedSaveSlot);
                m_selectedSaveSlot.clear();
                m_saveSlotBuffer = {};
                m_saveJsonBuffer = {
                    '{', '}', '\0'
                };
                SetStatus(
                    "セーブスロットを削除しました");
            }
            catch (const std::exception& exception)
            {
                SetStatus(exception.what(), true);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();

        ImGui::End();
    }

    void EditorLayer::DrawHierarchy()
    {
        ImGui::SetNextWindowSize(
            ImVec2{ HierarchyWidth, 420.0f },
            ImGuiCond_FirstUseEver);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("ヒエラルキー", nullptr, flags);

        m_hierarchyContextAction =
            HierarchyContextAction::None;

        // 名前で絞り込みます（空なら全表示）。
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##HierarchyFilter",
            "名前で検索",
            m_hierarchyFilter.data(),
            m_hierarchyFilter.size());
        const std::string hierarchyFilter =
            m_hierarchyFilter.data();
        const int selectionCount =
            static_cast<int>(SelectedObjects().size());
        if (selectionCount > 1)
        {
            ImGui::TextDisabled(
                "%d個を選択中（Ctrl+クリックで増減）",
                selectionCount);
        }

        ImGui::Selectable(
            "シーンルート（ここへドロップ）",
            m_selectedObjectId == 0);
        if (ImGui::IsItemClicked())
        {
            SelectObject(0, false);
        }
        DrawHierarchyRootContextMenu();
        if (!m_playing && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(GameObjectPayload))
            {
                GameObjectId draggedId{};
                std::memcpy(&draggedId, payload->Data, sizeof(draggedId));
                if (auto* dragged = m_scene.FindGameObject(draggedId);
                    dragged != nullptr && dragged->Parent() != nullptr)
                {
                    m_pendingHierarchyParentChange = {
                        dragged->Id(),
                        {},
                        true
                    };
                }
            }
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                // 空白へドロップされたアセットは、ファイル名の
                // GameObjectを新規作成して割り当てます。
                const auto asset = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (!IsCppScriptAsset(asset)
                    && !IsSceneAsset(asset)
                    && !IsPrefabAsset(asset))
                {
                    auto& created = m_scene.CreateGameObject(
                        PathToUtf8(asset.stem()));
                    if (!ApplyDroppedAsset(created, asset))
                    {
                        static_cast<void>(
                            m_scene.DestroyGameObject(
                                created));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (hierarchyFilter.empty())
        {
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                if (gameObject->Parent() == nullptr
                    && gameObject->SourceScene()
                        == Scene::PrimarySceneHandle())
                {
                    DrawHierarchyNode(*gameObject);
                }
            }
            // 追加読み込みしたシーンは、由来ごとにまとめます。
            DrawAdditiveSceneNodes();
        }
        else
        {
            // 検索中は階層をたたまず、一致したものを平らに出します。
            const auto lowered =
                [](std::string value)
                {
                    std::ranges::transform(
                        value,
                        value.begin(),
                        [](const unsigned char character)
                        {
                            return static_cast<char>(
                                std::tolower(character));
                        });
                    return value;
                };
            const auto needle = lowered(hierarchyFilter);
            bool matched = false;
            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                if (lowered(gameObject->Name()).find(needle)
                    == std::string::npos)
                {
                    continue;
                }
                matched = true;
                ImGui::PushID(
                    static_cast<int>(gameObject->Id()));
                if (ImGui::Selectable(
                    gameObject->Name().c_str(),
                    IsObjectSelected(gameObject->Id())))
                {
                    SelectObject(
                        gameObject->Id(),
                        ImGui::GetIO().KeyCtrl);
                }
                if (ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(
                        ImGuiMouseButton_Left))
                {
                    SelectObject(gameObject->Id(), false);
                    FocusSelection();
                }
                ImGui::PopID();
            }
            if (!matched)
            {
                ImGui::TextDisabled(
                    "一致するGameObjectがありません");
            }
        }

        if (ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(
                ImGuiMouseButton_Right)
            && !ImGui::IsAnyItemHovered())
        {
            m_selectedObjectId = 0;
        }
        if (ImGui::BeginPopupContextWindow(
            "##HierarchyBackgroundContext",
            ImGuiPopupFlags_MouseButtonRight
                | ImGuiPopupFlags_NoOpenOverItems))
        {
            ImGui::BeginDisabled(m_playing);
            if (ImGui::MenuItem("空のルートを作成"))
            {
                m_selectedObjectId = 0;
                m_hierarchyContextAction =
                    HierarchyContextAction::CreateRoot;
            }
            if (ImGui::MenuItem(
                "貼り付け",
                "Ctrl+V",
                false,
                !m_clipboardSceneJson.empty()))
            {
                m_selectedObjectId = 0;
                m_hierarchyContextAction =
                    HierarchyContextAction::Paste;
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        if (ImGui::IsWindowHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered())
        {
            m_selectedObjectId = 0;
        }

        ExecuteHierarchyContextAction();
        ExecutePendingHierarchyParentChange();
        ExecutePendingHierarchyReorder();
        ImGui::End();
    }

    void EditorLayer::ExecutePendingHierarchyParentChange()
    {
        if (!m_pendingHierarchyParentChange.requested)
        {
            return;
        }
        const auto request = m_pendingHierarchyParentChange;
        m_pendingHierarchyParentChange = {};

        auto* const moved =
            m_scene.FindGameObject(request.moved);
        if (moved == nullptr)
        {
            return;
        }

        GameObject* parent = nullptr;
        if (request.parent != 0)
        {
            parent = m_scene.FindGameObject(request.parent);
            if (parent == nullptr)
            {
                SetStatus("親GameObjectが見つかりません", true);
                return;
            }
        }
        if (moved->Parent() == parent)
        {
            return;
        }

        try
        {
            moved->SetParent(parent);
            RecordHistory();
            SetStatus(
                parent != nullptr
                    ? "親子関係を変更しました"
                    : "シーンルートへ移動しました");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::ExecutePendingHierarchyReorder()
    {
        if (!m_pendingHierarchyReorder.requested)
        {
            return;
        }
        const auto request = m_pendingHierarchyReorder;
        m_pendingHierarchyReorder = {};

        auto* const moved =
            m_scene.FindGameObject(request.moved);
        auto* const reference =
            m_scene.FindGameObject(request.reference);
        if (moved == nullptr || reference == nullptr)
        {
            return;
        }

        try
        {
            if (request.reparentToReferenceLevel)
            {
                // 階層をまたぐ移動。基準と同じ親へ移してから並べます。
                moved->SetParent(reference->Parent());
            }
            if (m_scene.ReorderGameObject(
                    *moved,
                    *reference,
                    request.insertAfter))
            {
                RecordHistory();
                SetStatus("並び順を変更しました");
            }
            else
            {
                SetStatus(
                    "そこへは並び替えできません",
                    true);
            }
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::DrawAdditiveSceneNodes()
    {
        if (m_scene.AdditiveScenes().empty())
        {
            return;
        }

        // 破棄はGameObjectの走査が終わってから実行します。
        SceneHandle unloadRequest =
            Scene::PrimarySceneHandle();
        for (const auto& additiveScene :
            m_scene.AdditiveScenes())
        {
            ImGui::PushID(
                static_cast<int>(
                    additiveScene.handle));
            const std::string label =
                "[追加] " + additiveScene.name;
            const bool open = ImGui::TreeNodeEx(
                label.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen
                | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "%s",
                    PathToUtf8(
                        additiveScene.path).c_str());
            }
            if (ImGui::BeginPopupContextItem(
                "##AdditiveSceneContext"))
            {
                if (ImGui::MenuItem(
                    "この追加シーンを破棄",
                    nullptr,
                    false,
                    !m_playing))
                {
                    unloadRequest =
                        additiveScene.handle;
                }
                ImGui::EndPopup();
            }
            if (open)
            {
                for (const auto& gameObject :
                    m_scene.GameObjects())
                {
                    if (gameObject->Parent() == nullptr
                        && gameObject->SourceScene()
                            == additiveScene.handle)
                    {
                        DrawHierarchyNode(*gameObject);
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (unloadRequest
            != Scene::PrimarySceneHandle())
        {
            // 追加シーンはUndo履歴（主シーンのスナップショット）に
            // 含まれないため、履歴は記録しません。
            if (m_scene.UnloadScene(unloadRequest))
            {
                if (m_selectedObjectId != 0
                    && m_scene.FindGameObject(
                        m_selectedObjectId) == nullptr)
                {
                    m_selectedObjectId = 0;
                }
                SetStatus("追加シーンを破棄しました");
            }
        }
    }

    void EditorLayer::DrawHierarchyNode(GameObject& gameObject)
    {
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow
            | ImGuiTreeNodeFlags_SpanAvailWidth;

        const bool hasChildren = !gameObject.Children().empty();
        if (!hasChildren)
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (IsObjectSelected(gameObject.Id()))
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        const auto nodeId = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(gameObject.Id()));
        const bool open = ImGui::TreeNodeEx(
            nodeId,
            flags,
            "%s%s%s",
            gameObject.IsPersistent()
                ? "[維持] "
                : "",
            gameObject.IsPrefabInstanceRoot()
                ? "[Prefab] "
                : "",
            gameObject.Name().c_str());

        // 行の矩形はここで取っておきます。この下にはコンテキスト
        // メニューの送出があり、ImGuiの「直前の項目」がそちらへ
        // 移っている可能性があるためです（ドロップ位置の判定に使う）。
        const ImVec2 nodeRectMinimum = ImGui::GetItemRectMin();
        const ImVec2 nodeRectMaximum = ImGui::GetItemRectMax();

        if (ImGui::IsItemClicked())
        {
            // Ctrl+クリックで選択に追加／解除します。
            SelectObject(
                gameObject.Id(),
                ImGui::GetIO().KeyCtrl);
        }
        if (ImGui::IsItemHovered()
            && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            // ダブルクリックでそのGameObjectを単独選択し、
            // Scene Viewのカメラをフォーカスします
            // （「シーンルート」項目はDrawHierarchy側の別処理で
            // 描画しているため、ここには含まれません）。
            SelectObject(gameObject.Id(), false);
            FocusSelection();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            // 右クリックは、選択済みなら選択を保ったまま
            // メニューを開きます（まとめて操作するため）。
            if (!IsObjectSelected(gameObject.Id()))
            {
                SelectObject(gameObject.Id(), false);
            }
            else
            {
                m_selectedObjectId = gameObject.Id();
                m_selectedAsset.clear();
            }
        }

        if (ImGui::BeginPopupContextItem())
        {
            ImGui::BeginDisabled(m_playing);
            if (ImGui::MenuItem("空の子を作成"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::CreateChild;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("切り取り", "Ctrl+X"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Cut;
            }
            if (ImGui::MenuItem("コピー", "Ctrl+C"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Copy;
            }
            if (ImGui::MenuItem(
                "貼り付け",
                "Ctrl+V",
                false,
                !m_clipboardSceneJson.empty()))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Paste;
            }
            if (ImGui::MenuItem("複製", "Ctrl+D"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Duplicate;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                "Prefabとして保存..."))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::SaveAsPrefab;
            }
            if (ImGui::MenuItem("削除", "Delete"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::Delete;
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        if (!m_playing && ImGui::BeginDragDropSource())
        {
            const GameObjectId id = gameObject.Id();
            ImGui::SetDragDropPayload(GameObjectPayload, &id, sizeof(id));
            ImGui::TextUnformatted(gameObject.Name().c_str());
            ImGui::EndDragDropSource();
        }

        if (!m_playing && ImGui::BeginDragDropTarget())
        {
            // 「子にする」のか「間に入れる」のかは、行のどこへ落としたかで
            // 決めます（上端・下端＝並び替え、真ん中＝子にする）。
            // 行の位置に応じて、並び替えまたは子への移動を行います。
            const ImVec2 itemMinimum = nodeRectMinimum;
            const ImVec2 itemMaximum = nodeRectMaximum;
            const float itemHeight = std::max(
                itemMaximum.y - itemMinimum.y,
                1.0f);
            const float positionRatio = std::clamp(
                (ImGui::GetMousePos().y - itemMinimum.y)
                    / itemHeight,
                0.0f,
                1.0f);
            // 上下30%を並び替え帯にします。行が薄いので、これ以上
            // 狭いと狙えません。
            constexpr float reorderEdgeRatio = 0.3f;
            const bool insertBefore =
                positionRatio < reorderEdgeRatio;
            const bool insertAfter =
                positionRatio > 1.0f - reorderEdgeRatio;
            const bool reordering = insertBefore || insertAfter;

            // ImGuiの既定の枠を止めて自分で描きます。既定の枠は
            // 行全体を囲むので、間に入れるつもりでも「子にします」
            // に見えてしまうためです。
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(
                        GameObjectPayload,
                        ImGuiDragDropFlags_AcceptBeforeDelivery
                            | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                auto* const drawList =
                    ImGui::GetWindowDrawList();
                const auto highlight = ImGui::GetColorU32(
                    ImGuiCol_DragDropTarget);
                if (reordering)
                {
                    // 挿入位置を示す横線と、左端の丸印。線だけだと
                    // 行の境界と紛れるので、印を付けて「ここへ挟む」
                    // と分かるようにします。
                    const float lineY = insertBefore
                        ? itemMinimum.y
                        : itemMaximum.y;
                    drawList->AddLine(
                        ImVec2{ itemMinimum.x, lineY },
                        ImVec2{ itemMaximum.x, lineY },
                        highlight,
                        3.0f);
                    drawList->AddCircleFilled(
                        ImVec2{ itemMinimum.x + 3.0f, lineY },
                        4.0f,
                        highlight);
                }
                else
                {
                    // 子にする場合は行を囲みます。
                    drawList->AddRect(
                        itemMinimum,
                        itemMaximum,
                        highlight,
                        2.0f,
                        0,
                        2.0f);
                }

                if (payload->IsDelivery())
                {
                    GameObjectId draggedId{};
                    std::memcpy(&draggedId, payload->Data, sizeof(draggedId));
                    auto* dragged = m_scene.FindGameObject(draggedId);

                    if (dragged != nullptr
                        && dragged != &gameObject)
                    {
                        try
                        {
                            if (reordering)
                            {
                                // ここで並べ替えると、ヒエラルキーを
                                // 走査しているfor文の対象
                                // （Scene::GameObjects()）を反復中に
                                // 壊します。要求だけ溜めて、走査後に
                                // ExecutePendingHierarchyReorderで
                                // 適用します。
                                m_pendingHierarchyReorder = {
                                    dragged->Id(),
                                    gameObject.Id(),
                                    insertAfter,
                                    dragged->Parent()
                                        != gameObject.Parent(),
                                    true
                                };
                            }
                            else if (dragged->Parent()
                                != &gameObject)
                            {
                                // 描画中にChildren()を変更すると、現在の
                                // ツリー走査とImGuiのTreeNode/TreePopの
                                // 対応を壊すため、走査後に適用します。
                                m_pendingHierarchyParentChange = {
                                    dragged->Id(),
                                    gameObject.Id(),
                                    true
                                };
                            }
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                }
            }
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                const auto asset = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (IsCppScriptAsset(asset))
                {
                    QueueCppScriptAttachment(
                        gameObject,
                        asset);
                }
                else
                {
                    static_cast<void>(
                        ApplyDroppedAsset(
                            gameObject,
                            asset));
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (open && hasChildren)
        {
            for (auto* child : gameObject.Children())
            {
                DrawHierarchyNode(*child);
            }
            ImGui::TreePop();
        }
    }

    void EditorLayer::DrawHierarchyRootContextMenu()
    {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            m_selectedObjectId = 0;
        }
        if (!ImGui::BeginPopupContextItem(
            "##HierarchyRootContext"))
        {
            return;
        }

        ImGui::BeginDisabled(m_playing);
        if (ImGui::MenuItem("空のルートを作成"))
        {
            m_hierarchyContextAction =
                HierarchyContextAction::CreateRoot;
        }
        if (ImGui::BeginMenu("UI"))
        {
            if (ImGui::MenuItem("Canvas"))
            {
                m_hierarchyContextAction =
                    HierarchyContextAction::CreateUICanvas;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(
            "貼り付け",
            "Ctrl+V",
            false,
            !m_clipboardSceneJson.empty()))
        {
            m_hierarchyContextAction =
                HierarchyContextAction::Paste;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    void EditorLayer::ExecuteHierarchyContextAction()
    {
        switch (m_hierarchyContextAction)
        {
        case HierarchyContextAction::CreateRoot:
            CreateRootGameObject();
            break;
        case HierarchyContextAction::CreateChild:
            CreateChildGameObject();
            break;
        case HierarchyContextAction::CreateUICanvas:
            CreateUICanvasGameObject();
            break;
        case HierarchyContextAction::Cut:
            CutSelectedGameObject();
            break;
        case HierarchyContextAction::Copy:
            CopySelectedGameObject();
            break;
        case HierarchyContextAction::Paste:
            PasteGameObject();
            break;
        case HierarchyContextAction::Duplicate:
            DuplicateSelectedGameObject();
            break;
        case HierarchyContextAction::SaveAsPrefab:
            SaveSelectedAsPrefab();
            break;
        case HierarchyContextAction::Delete:
            DeleteSelectedGameObject();
            break;
        case HierarchyContextAction::None:
        default:
            break;
        }
        m_hierarchyContextAction =
            HierarchyContextAction::None;
    }

    void EditorLayer::CreateRootGameObject()
    {
        auto& gameObject = m_scene.CreateGameObject("GameObject");
        m_selectedObjectId = gameObject.Id();
        RecordHistory();
        SetStatus("ルートGameObjectを作成しました");
    }

    void EditorLayer::CreateChildGameObject()
    {
        auto* parent = m_scene.FindGameObject(m_selectedObjectId);
        if (parent == nullptr)
        {
            return;
        }

        auto& gameObject = m_scene.CreateGameObject("GameObject");
        gameObject.SetParent(parent);
        m_selectedObjectId = gameObject.Id();
        RecordHistory();
        SetStatus("子GameObjectを作成しました");
    }

    void EditorLayer::CreateUICanvasGameObject()
    {
        auto& gameObject = m_scene.CreateGameObject("Canvas");
        gameObject.AddComponent<UICanvasComponent>();
        m_selectedObjectId = gameObject.Id();
        RecordHistory();
        SetStatus("UI Canvasを作成しました");
    }

    void EditorLayer::DeleteSelectedGameObject()
    {
        const auto selection = SelectedObjects();
        if (selection.empty())
        {
            return;
        }

        // 親を消すと子も消えるため、まだ生きているものだけを
        // IDで引き直しながら削除します。
        std::vector<GameObjectId> ids;
        ids.reserve(selection.size());
        for (const auto* object : selection)
        {
            ids.push_back(object->Id());
        }
        std::size_t deleted = 0;
        for (const auto id : ids)
        {
            if (auto* object = m_scene.FindGameObject(id))
            {
                m_scene.DestroyGameObject(*object);
                ++deleted;
            }
        }

        m_selectedObjectId = 0;
        ClearMultiSelection();
        RecordHistory();
        SetStatus(
            deleted > 1
                ? std::to_string(deleted)
                    + "個のGameObject階層を削除しました"
                : std::string{
                    "GameObject階層を削除しました" });
    }

    void EditorLayer::DuplicateSelectedGameObject()
    {
        const auto selection = SelectedObjects();
        if (selection.empty() || m_playing)
        {
            return;
        }

        try
        {
            // 複製した側を新しい選択にします。
            std::vector<GameObjectId> duplicates;
            duplicates.reserve(selection.size());
            for (auto* object : selection)
            {
                auto& duplicate =
                    m_scene.DuplicateGameObject(
                        *object,
                        object->Parent());
                duplicates.push_back(duplicate.Id());
            }

            ClearMultiSelection();
            m_selectedObjectId = duplicates.front();
            m_additionalSelection.assign(
                duplicates.begin() + 1,
                duplicates.end());
            RecordHistory();
            SetStatus(
                duplicates.size() > 1
                    ? std::to_string(duplicates.size())
                        + "個のGameObject階層を複製しました"
                    : std::string{
                        "GameObject階層を複製しました" });
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::CopySelectedGameObject()
    {
        const auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr)
        {
            return;
        }

        try
        {
            m_clipboardSceneJson = m_scene.SerializeToJson();
            m_clipboardObjectId = selected->Id();
            SetStatus("GameObject階層をコピーしました");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::CutSelectedGameObject()
    {
        auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr || m_playing)
        {
            return;
        }

        try
        {
            m_clipboardSceneJson = m_scene.SerializeToJson();
            m_clipboardObjectId = selected->Id();
            m_scene.DestroyGameObject(*selected);
            m_selectedObjectId = 0;
            RecordHistory();
            SetStatus("GameObject階層を切り取りました");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::PasteGameObject()
    {
        if (m_clipboardSceneJson.empty() || m_playing)
        {
            return;
        }

        try
        {
            Scene clipboardScene(m_graphics);
            clipboardScene.LoadFromJson(m_clipboardSceneJson);
            const auto* clipboardObject =
                clipboardScene.FindGameObject(m_clipboardObjectId);
            if (clipboardObject == nullptr)
            {
                throw std::runtime_error("The copied GameObject is no longer available.");
            }

            auto* currentSelection = m_scene.FindGameObject(m_selectedObjectId);
            auto* targetParent = currentSelection != nullptr
                ? currentSelection->Parent()
                : nullptr;
            auto& pasted = m_scene.DuplicateGameObject(
                *clipboardObject,
                targetParent);
            m_selectedObjectId = pasted.Id();
            RecordHistory();
            SetStatus("GameObject階層を貼り付けました");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::ReimportSelectedAsset()
    {
        if (m_selectedAsset.empty()
            || m_playing)
        {
            return;
        }

        try
        {
            static_cast<void>(
                m_graphics.Assets().Database().Refresh(
                    true));
            m_graphics.Assets().Clear();
            if (IsAudioAsset(m_selectedAsset))
            {
                m_graphics.Audio().Clear();
            }

            for (const auto& gameObject :
                m_scene.GameObjects())
            {
                if (auto* sprite =
                        gameObject->GetComponent<
                            SpriteRendererComponent>();
                    sprite != nullptr
                    && IsSameAssetReference(
                        sprite->TexturePath(),
                        m_selectedAsset))
                {
                    sprite->SetTexturePath(
                        sprite->TexturePath());
                }
                if (auto* audio =
                        gameObject->GetComponent<
                            AudioSourceComponent>();
                    audio != nullptr
                    && IsSameAssetReference(
                        audio->AudioPath(),
                        m_selectedAsset))
                {
                    audio->SetAudioPath(
                        audio->AudioPath());
                }
                if (auto* animator =
                        gameObject->GetComponent<
                            TransformAnimatorComponent>();
                    animator != nullptr)
                {
                    if (IsSameAssetReference(
                            animator->ControllerPath(),
                            m_selectedAsset))
                    {
                        animator->ReloadController();
                    }
                    else if (IsSameAssetReference(
                            animator->ClipPath(),
                            m_selectedAsset))
                    {
                        animator->ReloadClip();
                    }
                }
                if (auto* model =
                        gameObject->GetComponent<
                            ModelRendererComponent>();
                    model != nullptr)
                {
                    if (IsSameAssetReference(
                            model->AnimationControllerPath(),
                            m_selectedAsset))
                    {
                        model->ReloadAnimationController();
                    }
                    if (IsSameAssetReference(
                            model->ModelPath(),
                            m_selectedAsset))
                    {
                        model->SetModelPath(
                            model->ModelPath());
                    }
                    if (IsSameAssetReference(
                            model->AlbedoTexturePath(),
                            m_selectedAsset))
                    {
                        model->SetAlbedoTexturePath(
                            model->AlbedoTexturePath());
                    }
                    if (IsSameAssetReference(
                            model->NormalTexturePath(),
                            m_selectedAsset))
                    {
                        model->SetNormalTexturePath(
                            model->NormalTexturePath());
                    }
                    if (IsSameAssetReference(
                            model->MaterialAssetPath(),
                            m_selectedAsset))
                    {
                        model->SetMaterialAssetPath(
                            model->MaterialAssetPath());
                    }
                    if (IsSameAssetReference(
                            model->ShaderPath(),
                            m_selectedAsset))
                    {
                        model->ReloadShader();
                    }
                }
                if (auto* mesh =
                        gameObject->GetComponent<
                            MeshRendererComponent>();
                    mesh != nullptr)
                {
                    if (IsSameAssetReference(
                            mesh->AlbedoTexturePath(),
                            m_selectedAsset))
                    {
                        mesh->SetAlbedoTexturePath(
                            mesh->AlbedoTexturePath());
                    }
                    if (IsSameAssetReference(
                            mesh->NormalTexturePath(),
                            m_selectedAsset))
                    {
                        mesh->SetNormalTexturePath(
                            mesh->NormalTexturePath());
                    }
                    if (IsSameAssetReference(
                            mesh->MaterialAssetPath(),
                            m_selectedAsset))
                    {
                        mesh->SetMaterialAssetPath(
                            mesh->MaterialAssetPath());
                    }
                    if (IsSameAssetReference(
                            mesh->ShaderPath(),
                            m_selectedAsset))
                    {
                        mesh->ReloadShader();
                    }
                }
            }
            RefreshAssets();
            SetStatus(
                "再インポートしました: "
                + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{
                    "再インポートに失敗しました: "
                } + exception.what(),
                true);
        }
    }

    void EditorLayer::RefreshAssets(
        const bool reuseExistingDatabase)
    {
        m_assetFiles.clear();
        m_assetDirectories.clear();

        const auto& assetRoot = m_graphics.Assets().AssetRoot();
        if (!std::filesystem::exists(assetRoot))
        {
            m_assetDirectory.clear();
            m_selectedAsset.clear();
            return;
        }

        auto& database = m_graphics.Assets().Database();
        if (!reuseExistingDatabase || !database.HasRefreshed())
        {
            static_cast<void>(database.Refresh(true));
        }
        m_assetFiles.reserve(
            database.Assets().size());
        m_dataAssetTypeByPath.clear();
        for (const auto& asset :
            m_graphics.Assets().Database().Assets())
        {
            m_assetFiles.push_back(asset.path);
            // データアセットは中身の"type"で種類が決まるため、
            // 走査のついでに読み出して表にしておきます。参照欄の
            // 絞り込みと、アセット一覧の表示に使います。
            if (IsDataAsset(asset.path))
            {
                std::string typeName;
                try
                {
                    const auto bytes =
                        m_graphics.Assets().ReadFileBytes(
                            asset.path);
                    if (!bytes.empty())
                    {
                        typeName = DataAsset::FromJson(
                            std::string_view(
                                reinterpret_cast<const char*>(
                                    bytes.data()),
                                bytes.size()))
                            .TypeName();
                    }
                }
                catch (const std::exception&)
                {
                    // 壊れたファイルは「型なし」として扱います。
                    // 一覧の走査を止める理由にはしません。
                }
                m_dataAssetTypeByPath.insert_or_assign(
                    Lowercase(PathToUtf8(asset.path)),
                    std::move(typeName));
            }
        }

        std::error_code error;
        const auto options =
            std::filesystem::directory_options::skip_permission_denied;

        for (std::filesystem::recursive_directory_iterator iterator{
                assetRoot,
                options,
                error
            };
            iterator != std::filesystem::recursive_directory_iterator{};
            iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }

            if (iterator->is_directory(error) && !error)
            {
                const auto directory =
                    iterator->path().lexically_relative(assetRoot);
                if (!directory.empty())
                {
                    m_assetDirectories.push_back(directory);
                }
            }
            error.clear();
        }

        std::ranges::sort(m_assetFiles);
        std::ranges::sort(m_assetDirectories);
        const auto uniqueDirectories = std::ranges::unique(m_assetDirectories);
        m_assetDirectories.erase(
            uniqueDirectories.begin(),
            uniqueDirectories.end());

        if (!m_selectedAsset.empty()
            && std::ranges::find(m_assetFiles, m_selectedAsset) == m_assetFiles.end())
        {
            m_selectedAsset.clear();
        }
        if (!m_assetDirectory.empty()
            && std::ranges::find(
                m_assetDirectories,
                m_assetDirectory) == m_assetDirectories.end())
        {
            m_assetDirectory.clear();
        }
    }

    void EditorLayer::OpenSelectedAsset()
    {
        if (!IsSceneAsset(m_selectedAsset) || m_playing)
        {
            return;
        }

        try
        {
            const auto scenePath = m_graphics.Assets().ResolvePath(m_selectedAsset);
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.LoadFromFile(scenePath);
            m_scenePath = scenePath;
            m_selectedObjectId = 0;
            ResetHistory();
            MarkSceneSaved();
            SetStatus("シーンを開きました: " + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::InstantiateSelectedPrefab()
    {
        if (!IsPrefabAsset(m_selectedAsset) || m_playing)
        {
            return;
        }

        try
        {
            auto* parent = m_scene.FindGameObject(m_selectedObjectId);
            auto& instance = m_scene.InstantiatePrefab(
                m_selectedAsset,
                parent);
            m_selectedObjectId = instance.Id();
            RecordHistory();
            SetStatus(
                "Prefabを配置しました: "
                + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::ApplySelectedPrefab()
    {
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return;
        }

        try
        {
            m_scene.ApplyPrefabInstance(*prefabRoot);
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RefreshAssets();
            SetStatus(
                "Prefabへ変更を反映しました: "
                + PathToUtf8(
                    prefabRoot->PrefabAssetPath()));
        }
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            SetStatus(exception.what(), true);
        }
    }

    bool EditorLayer::RevertSelectedPrefab()
    {
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return false;
        }

        try
        {
            const auto assetPath =
                prefabRoot->PrefabAssetPath();
            auto& replacement =
                m_scene.RevertPrefabInstance(
                    *prefabRoot);
            m_selectedObjectId =
                replacement.Id();
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RecordHistory();
            SetStatus(
                "Prefabの変更を元へ戻しました: "
                + PathToUtf8(assetPath));
            return true;
        }
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            SetStatus(exception.what(), true);
            return false;
        }
    }

    void EditorLayer::ApplySelectedPrefabOverride(
        const std::string_view path)
    {
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return;
        }

        try
        {
            const std::string pathCopy{ path };
            m_scene.ApplyPrefabOverride(
                *prefabRoot,
                pathCopy);
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RefreshAssets();
            SetStatus(
                "Prefabの項目をApplyしました: "
                + FormatPrefabOverridePath(
                    pathCopy));
        }
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            SetStatus(exception.what(), true);
        }
    }

    bool EditorLayer::RevertSelectedPrefabOverride(
        const std::string_view path)
    {
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        auto* prefabRoot = selected != nullptr
            ? m_scene.FindPrefabInstanceRoot(*selected)
            : nullptr;
        if (prefabRoot == nullptr || m_playing)
        {
            return false;
        }

        try
        {
            const std::string pathCopy{ path };
            auto& replacement =
                m_scene.RevertPrefabOverride(
                    *prefabRoot,
                    pathCopy);
            m_selectedObjectId =
                replacement.Id();
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            RecordHistory();
            SetStatus(
                "Prefabの項目をRevertしました: "
                + FormatPrefabOverridePath(
                    pathCopy));
            return true;
        }
        catch (const std::exception& exception)
        {
            m_prefabStatusRootId = 0;
            m_prefabOverrides.clear();
            SetStatus(exception.what(), true);
            return false;
        }
    }

    void EditorLayer::AssignSelectedTexture()
    {
        auto* gameObject = m_scene.FindGameObject(m_selectedObjectId);
        auto* sprite = gameObject != nullptr
            ? gameObject->GetComponent<SpriteRendererComponent>()
            : nullptr;
        auto* mesh = gameObject != nullptr
            ? gameObject->GetComponent<MeshRendererComponent>()
            : nullptr;
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<ModelRendererComponent>()
            : nullptr;
        auto* tilemap = gameObject != nullptr
            ? gameObject->GetComponent<
                TilemapComponent>()
            : nullptr;
        auto* particles = gameObject != nullptr
            ? gameObject->GetComponent<
                ParticleSystemComponent>()
            : nullptr;
        auto* uiButton = gameObject != nullptr
            ? gameObject->GetComponent<
                UIButtonComponent>()
            : nullptr;

        if ((sprite == nullptr
                && mesh == nullptr
                && model == nullptr
                && tilemap == nullptr
                && particles == nullptr
                && uiButton == nullptr)
            || !IsTextureAsset(m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            if (sprite != nullptr)
            {
                sprite->SetTexturePath(m_selectedAsset);
            }
            else if (tilemap != nullptr)
            {
                tilemap->SetTexturePath(
                    m_selectedAsset);
            }
            else if (particles != nullptr)
            {
                particles->SetTexturePath(
                    m_selectedAsset);
            }
            else if (uiButton != nullptr)
            {
                uiButton->SetTexturePath(
                    m_selectedAsset);
            }
            else
            {
                if (mesh != nullptr)
                {
                    mesh->SetAlbedoTexturePath(
                        m_selectedAsset);
                }
                else
                {
                    model->SetMaterialOverrideEnabled(true);
                    model->SetAlbedoTexturePath(
                        m_selectedAsset);
                }
            }
            RecordHistory();
            const char* target = sprite != nullptr
                ? "スプライト画像"
                : tilemap != nullptr
                    ? "Tilemapのタイルシート"
                : particles != nullptr
                    ? "パーティクル画像"
                : uiButton != nullptr
                    ? "UI Button画像"
                : mesh != nullptr
                    ? "アルベド画像"
                    : "モデルのアルベド画像";
            SetStatus(
                std::string{ target }
                + "を割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    bool EditorLayer::ApplyDroppedAsset(
        GameObject& gameObject,
        const std::filesystem::path& asset)
    {
        if (m_playing || asset.empty())
        {
            return false;
        }

        try
        {
            std::string message;
            if (IsModelAsset(asset))
            {
                auto* model = gameObject.GetComponent<
                    ModelRendererComponent>();
                auto& renderer = model != nullptr
                    ? *model
                    : gameObject.AddComponent<
                        ModelRendererComponent>();
                renderer.SetModelPath(asset);
                message = "モデルを割り当てました: ";
            }
            else if (IsTextureAsset(asset))
            {
                // 相手の構成から用途を推測します（Tilemapなら
                // タイルシート、3D描画ならアルベド、それ以外は
                // スプライト）。
                if (auto* tilemap = gameObject.GetComponent<
                    TilemapComponent>())
                {
                    tilemap->SetTexturePath(asset);
                    message = "タイルシートを割り当てました: ";
                }
                else if (auto* button = gameObject.GetComponent<
                    UIButtonComponent>())
                {
                    button->SetTexturePath(asset);
                    message = "ボタン画像を割り当てました: ";
                }
                else if (auto* particles =
                    gameObject.GetComponent<
                        ParticleSystemComponent>())
                {
                    particles->SetTexturePath(asset);
                    message = "パーティクル画像を割り当てました: ";
                }
                else if (auto* mesh = gameObject.GetComponent<
                    MeshRendererComponent>())
                {
                    mesh->SetAlbedoTexturePath(asset);
                    message = "アルベドを割り当てました: ";
                }
                else if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetAlbedoTexturePath(asset);
                    message = "アルベドを割り当てました: ";
                }
                else
                {
                    auto* sprite = gameObject.GetComponent<
                        SpriteRendererComponent>();
                    auto& renderer = sprite != nullptr
                        ? *sprite
                        : gameObject.AddComponent<
                            SpriteRendererComponent>();
                    renderer.SetTexturePath(asset);
                    message = "スプライトを割り当てました: ";
                }
            }
            else if (IsMaterialAsset(asset))
            {
                if (auto* mesh = gameObject.GetComponent<
                    MeshRendererComponent>())
                {
                    mesh->SetMaterialAssetPath(asset);
                }
                else if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetMaterialAssetPath(asset);
                }
                else
                {
                    gameObject.AddComponent<
                        MeshRendererComponent>()
                        .SetMaterialAssetPath(asset);
                }
                message = "Materialを割り当てました: ";
            }
            else if (IsShaderErrorPlaceholder(asset))
            {
                // 壊れている印に使うShaderは、どの経路からも
                // 割り当てさせません。黙って無視すると「ドロップ
                // したのに効かない」になるので理由を出します。
                SetStatus(
                    "このShaderはエンジンが「壊れている印」に使うため、"
                    "割り当てられません",
                    true);
                return false;
            }
            else if (IsShaderAsset(asset))
            {
                if (auto* mesh = gameObject.GetComponent<
                    MeshRendererComponent>())
                {
                    mesh->SetShaderPath(asset);
                }
                else if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetShaderPath(asset);
                }
                else if (auto* sprite = gameObject.GetComponent<
                    SpriteRendererComponent>())
                {
                    sprite->SetShaderPath(asset);
                }
                else if (auto* particles =
                    gameObject.GetComponent<
                        ParticleSystemComponent>())
                {
                    particles->SetShaderPath(asset);
                }
                else
                {
                    return false;
                }
                message = "Shaderを割り当てました: ";
            }
            else if (IsAudioAsset(asset))
            {
                auto* audio = gameObject.GetComponent<
                    AudioSourceComponent>();
                auto& source = audio != nullptr
                    ? *audio
                    : gameObject.AddComponent<
                        AudioSourceComponent>();
                source.SetAudioPath(asset);
                message = "オーディオを割り当てました: ";
            }
            else if (IsAnimationAsset(asset))
            {
                auto* animator = gameObject.GetComponent<
                    TransformAnimatorComponent>();
                auto& target = animator != nullptr
                    ? *animator
                    : gameObject.AddComponent<
                        TransformAnimatorComponent>();
                target.SetClipPath(asset);
                message = "Animation Clipを割り当てました: ";
            }
            else if (IsAnimatorControllerAsset(asset))
            {
                // モデルがあればスケルタル側、無ければTransform側。
                if (auto* model = gameObject.GetComponent<
                    ModelRendererComponent>())
                {
                    model->SetAnimationControllerPath(asset);
                }
                else
                {
                    auto* animator = gameObject.GetComponent<
                        TransformAnimatorComponent>();
                    auto& target = animator != nullptr
                        ? *animator
                        : gameObject.AddComponent<
                            TransformAnimatorComponent>();
                    target.SetControllerPath(asset);
                }
                message = "Animator Controllerを割り当てました: ";
            }
            else
            {
                return false;
            }

            m_selectedObjectId = gameObject.Id();
            RecordHistory();
            SetStatus(message + PathToUtf8(asset));
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
            return false;
        }
    }

    bool EditorLayer::IsObjectSelected(
        const GameObjectId id) const noexcept
    {
        if (id == 0)
        {
            return false;
        }
        return m_selectedObjectId == id
            || std::ranges::find(
                m_additionalSelection,
                id) != m_additionalSelection.end();
    }

    void EditorLayer::SelectObject(
        const GameObjectId id,
        const bool additive)
    {
        m_selectedAsset.clear();
        if (!additive)
        {
            m_additionalSelection.clear();
            m_selectedObjectId = id;
            return;
        }
        if (id == 0)
        {
            return;
        }
        if (m_selectedObjectId == 0)
        {
            m_selectedObjectId = id;
            return;
        }
        if (m_selectedObjectId == id)
        {
            // 主選択をCtrl+クリックしたら選択から外し、
            // 残りの先頭を新しい主選択にします。
            if (m_additionalSelection.empty())
            {
                m_selectedObjectId = 0;
                return;
            }
            m_selectedObjectId =
                m_additionalSelection.front();
            m_additionalSelection.erase(
                m_additionalSelection.begin());
            return;
        }
        if (const auto found = std::ranges::find(
                m_additionalSelection,
                id);
            found != m_additionalSelection.end())
        {
            m_additionalSelection.erase(found);
            return;
        }
        m_additionalSelection.push_back(id);
    }

    void EditorLayer::ClearMultiSelection()
    {
        m_additionalSelection.clear();
    }

    std::vector<GameObject*>
        EditorLayer::SelectedObjects() const
    {
        std::vector<GameObject*> objects;
        if (auto* primary =
            m_scene.FindGameObject(m_selectedObjectId))
        {
            objects.push_back(primary);
        }
        for (const auto id : m_additionalSelection)
        {
            if (auto* object = m_scene.FindGameObject(id))
            {
                objects.push_back(object);
            }
        }
        return objects;
    }

    void EditorLayer::SelectPrefabInstances(
        const std::filesystem::path& prefabAsset)
    {
        std::vector<GameObjectId> found;
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (gameObject->IsPrefabInstanceRoot()
                && IsSameAssetReference(
                    gameObject->PrefabAssetPath(),
                    prefabAsset))
            {
                found.push_back(gameObject->Id());
            }
        }

        if (found.empty())
        {
            SetStatus(
                "このPrefabのインスタンスは"
                "シーン内にありません: "
                + PathToUtf8(prefabAsset.filename()));
            return;
        }

        ClearMultiSelection();
        m_selectedObjectId = found.front();
        m_additionalSelection.assign(
            found.begin() + 1,
            found.end());
        m_selectedAsset.clear();
        SetStatus(
            std::to_string(found.size())
            + "個のインスタンスを選択しました: "
            + PathToUtf8(prefabAsset.filename()));
    }

    void EditorLayer::AssignSelectedModel()
    {
        auto* gameObject = m_scene.FindGameObject(m_selectedObjectId);
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<ModelRendererComponent>()
            : nullptr;

        if (model == nullptr || !IsModelAsset(m_selectedAsset) || m_playing)
        {
            return;
        }

        try
        {
            model->SetModelPath(m_selectedAsset);
            RecordHistory();
            SetStatus("モデルを割り当てました: " + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::AssignSelectedMaterial()
    {
        auto* gameObject =
            m_scene.FindGameObject(m_selectedObjectId);
        auto* mesh = gameObject != nullptr
            ? gameObject->GetComponent<MeshRendererComponent>()
            : nullptr;
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<ModelRendererComponent>()
            : nullptr;

        if ((mesh == nullptr && model == nullptr)
            || !IsMaterialAsset(m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            if (mesh != nullptr)
            {
                mesh->SetMaterialAssetPath(m_selectedAsset);
            }
            else
            {
                model->SetMaterialAssetPath(m_selectedAsset);
            }
            RecordHistory();
            SetStatus(
                "Lit Materialを割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::AssignSelectedAnimation()
    {
        auto* gameObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        auto* animator = gameObject != nullptr
            ? gameObject->GetComponent<
                TransformAnimatorComponent>()
            : nullptr;
        if (animator == nullptr
            || !IsAnimationAsset(m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            static_cast<void>(
                m_graphics.Assets().
                    LoadAnimationClip(
                        m_selectedAsset));
            animator->SetClipPath(
                m_selectedAsset);
            RecordHistory();
            SetStatus(
                "Animation Clipを割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::AssignSelectedAnimatorController()
    {
        auto* gameObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        auto* animator = gameObject != nullptr
            ? gameObject->GetComponent<
                TransformAnimatorComponent>()
            : nullptr;
        auto* model = gameObject != nullptr
            ? gameObject->GetComponent<
                ModelRendererComponent>()
            : nullptr;
        if ((animator == nullptr && model == nullptr)
            || !IsAnimatorControllerAsset(
                m_selectedAsset)
            || m_playing)
        {
            return;
        }

        try
        {
            static_cast<void>(
                m_graphics.Assets().
                    LoadAnimatorController(
                        m_selectedAsset));
            if (animator != nullptr)
            {
                animator->SetControllerPath(
                    m_selectedAsset);
            }
            else
            {
                model->SetAnimationControllerPath(
                    m_selectedAsset);
            }
            RecordHistory();
            SetStatus(
                "Animator Controllerを割り当てました: "
                + PathToUtf8(m_selectedAsset));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::ReloadSharedMaterial(
        const std::filesystem::path& materialAsset)
    {
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (auto* mesh =
                gameObject->GetComponent<MeshRendererComponent>();
                mesh != nullptr
                && IsSameAssetReference(
                    mesh->MaterialAssetPath(),
                    materialAsset))
            {
                mesh->ReloadMaterialAsset();
            }
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>();
                model != nullptr
                && IsSameAssetReference(
                    model->MaterialAssetPath(),
                    materialAsset))
            {
                model->ReloadMaterialAsset();
            }
        }
    }

    void EditorLayer::ReloadSharedModel(
        const std::filesystem::path& modelAsset)
    {
        for (const auto& gameObject : m_scene.GameObjects())
        {
            if (auto* model =
                gameObject->GetComponent<ModelRendererComponent>();
                model != nullptr
                && IsSameAssetReference(
                    model->ModelPath(),
                    modelAsset))
            {
                // ReloadModel()はprivateなので、同じパスを渡して
                // 強制的に読み直させます（SetModelPathはパスの
                // 異同を見ずに常に再読み込みします）。
                model->SetModelPath(model->ModelPath());
            }
        }
    }

    void EditorLayer::NewScene()
    {
        if (m_playing)
        {
            return;
        }

        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.Clear();

            auto& cameraObject = m_scene.CreateGameObject("メインカメラ");
            cameraObject.GetTransform().position = { 0.0f, 1.6f, 7.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -0.12f, 0.0f, 0.0f });
            auto& camera = cameraObject.AddComponent<CameraComponent>();
            m_scene.SetMainCamera(camera);

            auto& lightObject =
                m_scene.CreateGameObject("太陽光");
            lightObject.GetTransform().SetEulerAngles({
                DirectX::XMConvertToRadians(-45.0f),
                DirectX::XMConvertToRadians(-35.0f),
                0.0f
            });
            lightObject.AddComponent<
                DirectionalLightComponent>();

            m_scenePath.clear();
            m_scene.Scenes().
                CancelPending();
            m_scene.Scenes().
                SetCurrentScenePath({});
            m_selectedObjectId = cameraObject.Id();
            m_playSnapshot.clear();
            ResetHistory();
            MarkSceneSaved();
            SetStatus("新しいシーンを作成しました");
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::SaveScene()
    {
        if (m_playing)
        {
            return;
        }

        if (m_scenePath.empty())
        {
            SaveSceneAs();
            return;
        }

        try
        {
            m_scene.SaveToFile(m_scenePath);
            MarkSceneSaved();
            m_scene.Scenes().
                SetCurrentScenePath(
                    m_scenePath);
            RefreshAssets();
            SetStatus("保存しました: " + PathToUtf8(m_scenePath.filename()));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::OpenScene()
    {
        if (m_playing)
        {
            return;
        }

        std::array<wchar_t, 32768> filename{};
        const auto sceneDirectory =
            m_graphics.Assets().AssetRoot()
            / L"scenes";
        const std::wstring initialDirectory =
            sceneDirectory.wstring();

        constexpr wchar_t filter[] =
            L"LamaPon シーン (*.scene.json)\0*.scene.json\0"
            L"JSON (*.json)\0*.json\0\0";

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile =
            static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir =
            initialDirectory.c_str();
        dialog.lpstrTitle = L"シーンを開く";
        dialog.Flags =
            OFN_FILEMUSTEXIST
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "シーンを開くダイアログを"
                    "表示できませんでした",
                    true);
            }
            return;
        }

        try
        {
            const std::filesystem::path source{
                filename.data()
            };
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.LoadFromFile(source);
            m_scenePath = source;
            m_selectedObjectId = 0;
            m_playSnapshot.clear();
            RefreshAssets();
            ResetHistory();
            MarkSceneSaved();
            SetStatus(
                "シーンを開きました: "
                + PathToUtf8(
                    m_scenePath.filename()));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::SaveSceneAs()
    {
        if (m_playing)
        {
            return;
        }

        std::array<wchar_t, 32768> filename{};
        const std::wstring suggestedName = m_scenePath.empty()
            ? L"NewScene.scene.json"
            : m_scenePath.filename().wstring();
        wcscpy_s(filename.data(), filename.size(), suggestedName.c_str());

        const auto sceneDirectory =
            m_graphics.Assets().AssetRoot() / L"scenes";
        std::error_code directoryError;
        std::filesystem::create_directories(sceneDirectory, directoryError);
        const std::wstring initialDirectory = sceneDirectory.wstring();

        constexpr wchar_t filter[] =
            L"LamaPon シーン (*.scene.json)\0*.scene.json\0"
            L"JSON (*.json)\0*.json\0\0";

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir = initialDirectory.c_str();
        dialog.lpstrTitle = L"シーンに名前を付けて保存";
        dialog.lpstrDefExt = L"scene.json";
        dialog.Flags =
            OFN_OVERWRITEPROMPT
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus("保存ダイアログを開けませんでした", true);
            }
            return;
        }

        try
        {
            std::filesystem::path destination{ filename.data() };
            if (!HasSceneExtension(destination))
            {
                destination.replace_extension(L".scene.json");
            }

            m_scene.SaveToFile(destination);
            m_scenePath = std::move(destination);
            MarkSceneSaved();
            m_scene.Scenes().
                SetCurrentScenePath(
                    m_scenePath);
            RefreshAssets();
            SetStatus(
                "名前を付けて保存しました: "
                + PathToUtf8(m_scenePath.filename()));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::SaveSelectedAsPrefab()
    {
        auto* selected =
            m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr || m_playing)
        {
            return;
        }

        const std::wstring baseName = SuggestedPrefabFileStem(selected->Name());

        std::array<wchar_t, 32768> filename{};
        const std::wstring suggestedName =
            baseName + L".prefab.json";
        wcscpy_s(
            filename.data(),
            filename.size(),
            suggestedName.c_str());

        const auto assetRoot =
            std::filesystem::absolute(
                m_graphics.Assets().AssetRoot()).lexically_normal();
        const auto prefabDirectory = assetRoot / L"prefabs";
        std::error_code directoryError;
        std::filesystem::create_directories(
            prefabDirectory,
            directoryError);
        if (directoryError)
        {
            SetStatus(
                "Prefabフォルダーを作成できませんでした",
                true);
            return;
        }
        const std::wstring initialDirectory =
            prefabDirectory.wstring();

        constexpr wchar_t filter[] =
            L"LamaPon Prefab (*.prefab.json)\0*.prefab.json\0"
            L"JSON (*.json)\0*.json\0\0";

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir = initialDirectory.c_str();
        dialog.lpstrTitle = L"選択をPrefabとして保存";
        dialog.lpstrDefExt = L"prefab.json";
        dialog.Flags =
            OFN_OVERWRITEPROMPT
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "Prefab保存ダイアログを開けませんでした",
                    true);
            }
            return;
        }

        try
        {
            std::filesystem::path destination{ filename.data() };
            if (!HasPrefabExtension(destination))
            {
                destination.replace_extension(L".prefab.json");
            }
            destination =
                std::filesystem::absolute(destination).lexically_normal();
            if (!IsPathWithin(assetRoot, destination))
            {
                throw std::runtime_error(
                    "Prefabはプロジェクトのassetsフォルダー内へ保存してください。");
            }

            m_scene.SavePrefab(*selected, destination);
            const auto relativePath =
                destination.lexically_relative(assetRoot);
            selected->SetPrefabAssetPath(
                relativePath);
            RecordHistory();
            RefreshAssets();
            m_selectedAsset = relativePath;
            m_assetDirectory = relativePath.parent_path();
            SetStatus(
                "Prefabを保存しました: "
                + PathToUtf8(relativePath));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::MarkSceneSaved()
    {
        try
        {
            m_savedSceneSnapshot = m_scene.SerializeToJson();
        }
        catch (const std::exception&)
        {
            m_savedSceneSnapshot.clear();
        }

        if (!m_scenePath.empty())
        {
            std::error_code error;
            const auto writeTime =
                std::filesystem::last_write_time(
                    m_scenePath,
                    error);
            if (!error)
            {
                m_lastSeenSceneWriteTime = writeTime;
                m_sceneWriteTimeInitialized = true;
                m_externalSceneChangeNotified = false;
            }
        }
    }

    void EditorLayer::UpdateExternalSceneFile()
    {
        if (m_scenePath.empty()
            || m_playing
            || m_gameModuleBuildProcess != nullptr)
        {
            return;
        }

        const double now = ImGui::GetTime();
        if (now - m_lastSceneScanAt < 0.5)
        {
            return;
        }
        m_lastSceneScanAt = now;

        std::error_code error;
        const auto writeTime =
            std::filesystem::last_write_time(
                m_scenePath,
                error);
        if (error)
        {
            // 外部ツールが一時ファイルを置き換えている途中は、
            // 次のスキャンで再試行できるように基準時刻を保持します。
            return;
        }

        if (!m_sceneWriteTimeInitialized)
        {
            m_lastSeenSceneWriteTime = writeTime;
            m_sceneWriteTimeInitialized = true;
            return;
        }
        if (writeTime <= m_lastSeenSceneWriteTime)
        {
            return;
        }

        if (HasUnsavedSceneChanges())
        {
            if (!m_externalSceneChangeNotified)
            {
                SetStatus(
                    "外部エディターでシーンが変更されました。保存するか手動で再読み込みしてください。",
                    true);
                m_externalSceneChangeNotified = true;
            }
            return;
        }

        m_externalSceneChangeNotified = false;
        ReloadScene();
    }

    bool EditorLayer::HasUnsavedSceneChanges() const
    {
        try
        {
            // Play中は実行時の状態ではなく、Play開始前の編集状態
            // （停止時に復元されるスナップショット）と比較します。
            const std::string current = m_playing
                ? m_playSnapshot
                : m_scene.SerializeToJson();
            return current != m_savedSceneSnapshot;
        }
        catch (const std::exception&)
        {
            // 判定できないときは安全側（警告を出す）に倒します。
            return true;
        }
    }

    bool EditorLayer::ConfirmClose()
    {
        if (!HasUnsavedSceneChanges())
        {
            return true;
        }

        if (m_scenePath.empty())
        {
            // 保存先が未確定の新規シーン。ここからWin32の
            // 保存ダイアログへは進めないため、破棄の確認だけ行います。
            return MessageBoxW(
                m_window,
                L"保存されていないシーンの変更があります。\n"
                L"閉じると編集内容は失われます。閉じますか？",
                L"LamaPon Editor",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                == IDYES;
        }

        const int choice = MessageBoxW(
            m_window,
            L"シーンに保存していない変更があります。\n\n"
            L"[はい] 保存して閉じる\n"
            L"[いいえ] 保存せずに閉じる\n"
            L"[キャンセル] 閉じるのをやめる",
            L"LamaPon Editor",
            MB_YESNOCANCEL | MB_ICONWARNING);
        if (choice == IDCANCEL || choice == 0)
        {
            return false;
        }
        if (choice == IDYES)
        {
            try
            {
                // Play中なら編集状態へ戻してから保存します。
                if (m_playing)
                {
                    StopPlaying();
                }
                m_scene.SaveToFile(m_scenePath);
                MarkSceneSaved();
            }
            catch (const std::exception& exception)
            {
                MessageBoxW(
                    m_window,
                    (L"シーンを保存できませんでした:\n"
                        + Utf8ToWide(exception.what())).c_str(),
                    L"LamaPon Editor",
                    MB_OK | MB_ICONERROR);
                return false;
            }
        }
        return true;
    }

    void EditorLayer::ReloadScene()
    {
        if (m_scenePath.empty())
        {
            return;
        }

        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_scene.LoadFromFile(m_scenePath);
            if (m_scene.FindGameObject(m_selectedObjectId) == nullptr)
            {
                m_selectedObjectId = 0;
            }
            m_playing = false;
            ResetHistory();
            MarkSceneSaved();
            SetStatus(
                "再読み込みしました: "
                + PathToUtf8(m_scenePath.filename()));
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

}
