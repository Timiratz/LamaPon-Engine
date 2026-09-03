#include "LamaPon/Editor/EditorLayer.h"

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
#include "LamaPon/Graphics/SkeletalModel.h"
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
#include <Model.h>
#include <Effects.h>

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
#include <iomanip>
#include <iterator>
#include <limits>
#include <ranges>
#include <regex>
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
        ClearProjectStageMapPreview();
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
        // リモート操作: 入力の注入はImGui::NewFrame()の**前**に
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
                // （Renderの末尾）。ファイル名はseqで変えます——
                // 固定名はホスト側のfile pullが古い内容を返すため。
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
            // カテゴリー名はDrawProjectSettingsDialogの並びと
            // 同じです。増やしたらこちらも揃えること。
            // ASCII別名も受けます——VMへ送る自動化スクリプトは
            // ASCII限定のため、日本語を引数に書けません。
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
            "処理が完了するまでエディタは操作できません。";
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

        const bool stagePreviewOwnsViewport =
            m_projectStageEditor.previewVisible;
        // 描画先テクスチャを持つCameraを先に描いておきます。ただし
        // ステージエディタが前面なら、その出力も通常Viewportも見えない
        // ため更新せず、隠れたカメラ描画へGPU時間を使いません。
        if (!stagePreviewOwnsViewport)
        {
            m_scene.RenderTargetTextures();
        }

        // ステージエディタの3D表示は簡易図ではなく、現在のシーンを
        // 専用ターゲットへもう一度描きます。Play中なら実際に走行中の
        // ステージモデルと生成済みコライダーがそのまま確認できます。
        // 道路の編集線・頂点・START/GOALは同じ行列でImGui側から重ねます。
        if (m_projectStageEditor.previewVisible
            && m_projectStageEditor.loaded
            && m_projectStagePreview.IsValid())
        {
            // 停止中のステージとマップは静的なので、カメラや表示設定が
            // 変わったときだけ描きます。再生中は指定周期で更新し、重い
            // マップを常時二重描画してエディター全体を止めません。
            const double now = Time::UnscaledTimeSinceStartup();
            const double interval = 1.0 / static_cast<double>(std::clamp(
                m_projectStageEditor.previewFrameRate, 10, 60));
            const bool intervalElapsed =
                now - m_projectStageEditor.previewLastRenderAt >= interval;
            const bool invalidated =
                m_projectStageEditor.previewNeedsRender
                && (!m_projectStageEditor.previewHasRendered
                    || intervalElapsed);
            const bool liveRefresh = m_playing
                && !m_paused
                && intervalElapsed;
            const bool updatePreview =
                invalidated || liveRefresh;
            if (updatePreview)
            {
                m_graphics.SetUIViewportSize(
                    m_projectStagePreview.Width(),
                    m_projectStagePreview.Height());
                m_projectStagePreview.Bind(m_graphics.Context());
                m_projectStagePreview.Clear(
                    m_graphics.Context(), sceneClearColor);
                auto* const mapRoot = m_scene.FindGameObject(
                    m_projectStageEditor.mapPreviewRootId);
                const bool renderEditorMap = mapRoot != nullptr
                    && m_projectStageEditor.mapModelVisible
                    && !m_playing;
                if (renderEditorMap)
                {
                    mapRoot->SetEnabled(true);
                }
                try
                {
                    m_scene.RenderWithMatrices(
                        ProjectStageViewMatrix(),
                        ProjectStageProjectionMatrix(),
                        false,
                        m_projectStageEditor.showColliders,
                        &m_projectStagePreview);
                }
                catch (...)
                {
                    if (renderEditorMap) mapRoot->SetEnabled(false);
                    throw;
                }
                if (renderEditorMap)
                {
                    // 通常のScene/Game描画や物理更新へプレビュー用
                    // モデルが残らないよう、描画直後に戻します。
                    mapRoot->SetEnabled(false);
                }
                RunPostProcess(
                    m_graphics,
                    m_projectStagePreview,
                    m_scene.PostProcessFrameData());
                m_projectStagePreview.CopyToDisplay(m_graphics.Context());
                m_projectStageEditor.previewLastRenderAt = now;
                m_projectStageEditor.previewHasRendered = true;
                m_projectStageEditor.previewNeedsRender = false;
            }
        }

        // ステージエディタはViewportと同じDockの前面タブです。前面に
        // ある間は背後のScene/Game Viewを描いても見えないため止め、
        // 3Dシーンの二重描画を避けます。
        if (!stagePreviewOwnsViewport
            && m_activeViewport == ViewportMode::Scene
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
        else if (!stagePreviewOwnsViewport
            && m_activeViewport == ViewportMode::Game
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

    // これがtrueの間、Applicationは**Actionのキーボード入力を丸ごと
    // 切ります**（`Input().Update(!WantsKeyboard())`）。ImGuiのキー処理は
    // 別経路なので、ここでfalseを返してもエディターの操作性は変わりません。
    //
    // 以前は`WantCaptureKeyboard`をそのまま返していました。エディターは
    // `ImGuiConfigFlags_NavEnableKeyboard`（キーボードでのUI操作）を
    // 有効にしているため、ナビが働いている間これが立ち続け、**再生中に
    // `WasPressed("Jump")`が永久にfalse**になっていました
    // （Spaceでジャンプできない、という形で実際に踏みました）。
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
                    if (type == "stage-road")
                    {
                        panel.kind = ProjectPanelKind::StageRoad;
                    }
                    else if (type == "vehicle-parameters")
                    {
                        panel.kind = ProjectPanelKind::VehicleParameters;
                    }
                    else if (type == "bgm-loop")
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
            m_projectStageEditor.panelIndex = static_cast<std::size_t>(-1);
            m_projectVehicleEditor.panelIndex = static_cast<std::size_t>(-1);
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
        m_projectStageEditor.previewVisible = false;
        // ステージパネルはPlay中も3Dデバッグ表示として残します。
        // 編集・保存は各パネル側でロックし、実行中アセットとの競合を
        // 防ぎます。その他のプロジェクト専用編集パネルは従来どおり
        // Play中に休止します。
        // 再生を始めたらBGMの試聴は必ず止めます。パネル自体はPlay中
        // 描かれないので、ここで止めないと鳴りっぱなしになります。
        if (m_playing && m_projectBgmEditor.preview)
        {
            StopProjectBgmPreview();
        }
        for (std::size_t index = 0; index < m_projectPanels.size(); ++index)
        {
            auto& panel = m_projectPanels[index];
            if (!panel.open)
            {
                // 閉じられたBGMパネルの試聴も残さない。
                if (panel.kind == ProjectPanelKind::BgmLoop
                    && m_projectBgmEditor.panelIndex == index
                    && m_projectBgmEditor.preview)
                {
                    StopProjectBgmPreview();
                }
                continue;
            }
            if (panel.kind == ProjectPanelKind::StageRoad)
            {
                DrawProjectStagePanel(index, panel);
            }
            else if (m_playing)
            {
                continue;
            }
            else if (panel.kind == ProjectPanelKind::BgmLoop)
            {
                DrawProjectBgmPanel(index, panel);
            }
            else
            {
                DrawProjectVehiclePanel(index, panel);
            }
        }
    }

    DirectX::XMMATRIX EditorLayer::ProjectStageViewMatrix() const noexcept
    {
        using namespace DirectX;
        const auto& state = m_projectStageEditor;
        if (state.baseSections.empty())
        {
            return XMMatrixIdentity();
        }

        XMFLOAT3 minimum = state.baseSections.front().center;
        XMFLOAT3 maximum = minimum;
        for (std::size_t index = 0; index < state.baseSections.size(); ++index)
        {
            const auto& section = state.baseSections[index];
            const float dx = section.right.x - section.left.x;
            const float dz = section.right.z - section.left.z;
            const float length = std::max(std::hypot(dx, dz), 0.001f);
            const float leftDistance = state.widths[index] * 0.5f
                + state.edgeBiases[index];
            const float rightDistance = state.widths[index] * 0.5f
                - state.edgeBiases[index];
            const XMFLOAT3 points[]{
                { section.center.x - dx / length * leftDistance,
                    section.center.y + state.heights[index],
                    section.center.z - dz / length * leftDistance },
                { section.center.x,
                    section.center.y + state.heights[index],
                    section.center.z },
                { section.center.x + dx / length * rightDistance,
                    section.center.y + state.heights[index],
                    section.center.z + dz / length * rightDistance }
            };
            for (const auto& point : points)
            {
                minimum.x = std::min(minimum.x, point.x);
                minimum.y = std::min(minimum.y, point.y);
                minimum.z = std::min(minimum.z, point.z);
                maximum.x = std::max(maximum.x, point.x);
                maximum.y = std::max(maximum.y, point.y);
                maximum.z = std::max(maximum.z, point.z);
            }
        }

        XMVECTOR target = XMVectorSet(
            (minimum.x + maximum.x) * 0.5f,
            (minimum.y + maximum.y) * 0.5f,
            (minimum.z + maximum.z) * 0.5f,
            1.0f);
        const float yaw = XMConvertToRadians(state.orbitYawDegrees);
        const float pitch = XMConvertToRadians(state.orbitPitchDegrees);
        const XMVECTOR offset = XMVector3Normalize(XMVectorSet(
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::cos(yaw) * std::cos(pitch),
            0.0f));
        const XMVECTOR forward = XMVectorNegate(offset);
        const XMVECTOR right = XMVector3Normalize(
            XMVector3Cross(forward, g_XMIdentityR1));
        const XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, forward));
        const float zoom = std::max(state.zoom, 0.001f);
        target = XMVectorMultiplyAdd(
            XMVectorReplicate(-state.pan.x / zoom), right, target);
        target = XMVectorMultiplyAdd(
            XMVectorReplicate(state.pan.y / zoom), up, target);
        const float extent = std::max({ maximum.x - minimum.x,
            maximum.y - minimum.y, maximum.z - minimum.z, 10.0f });
        const XMVECTOR eye = XMVectorMultiplyAdd(
            XMVectorReplicate(extent * 2.0f + 100.0f), offset, target);
        return XMMatrixLookAtRH(eye, target, up);
    }

    DirectX::XMMATRIX
        EditorLayer::ProjectStageProjectionMatrix() const noexcept
    {
        const auto& state = m_projectStageEditor;
        const float zoom = std::max(state.zoom, 0.001f);
        const float width = std::max(state.previewSize.x, 64.0f) / zoom;
        const float height = std::max(state.previewSize.y, 64.0f) / zoom;
        return DirectX::XMMatrixOrthographicRH(
            width, height, 0.1f, 100000.0f);
    }

    void EditorLayer::ClearProjectStageMapPreview()
    {
        auto& state = m_projectStageEditor;
        const auto rootId = state.mapPreviewRootId;
        state.mapPreviewRootId = 0;
        state.mapPreviewModelCount = 0;
        state.mapPreviewError.clear();
        if (auto* root = m_scene.FindGameObject(rootId);
            root != nullptr)
        {
            static_cast<void>(m_scene.DestroyGameObject(*root));
        }
    }

    void EditorLayer::BuildProjectStageMapPreview(
        const nlohmann::json& profile)
    {
        ClearProjectStageMapPreview();
        const auto definitionIt = profile.find("map_model");
        if (definitionIt == profile.end()
            || !definitionIt->is_object())
        {
            return;
        }

        auto& state = m_projectStageEditor;
        auto& root = m_scene.CreateEditorPreviewGameObject(
            "Stage Editor Map Preview");
        state.mapPreviewRootId = root.Id();
        std::string warning;
        std::size_t loadedCount = 0;
        try
        {
            const auto& definition = *definitionIt;
            std::vector<std::string> modelPaths;
            if (definition.contains("path")
                && definition.at("path").is_string())
            {
                modelPaths.push_back(
                    definition.at("path").get<std::string>());
            }
            else if (definition.contains("prefix")
                && definition.at("prefix").is_string())
            {
                const auto prefix = definition.at("prefix").get<std::string>();
                const auto extension = definition.value(
                    "extension", std::string{".fbx"});
                const auto count = std::clamp<std::size_t>(
                    definition.value("count", std::size_t{ 0 }),
                    0,
                    64);
                modelPaths.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                {
                    std::ostringstream suffix;
                    suffix << std::setw(3) << std::setfill('0') << index;
                    modelPaths.push_back(prefix + suffix.str() + extension);
                }
            }
            if (modelPaths.empty())
            {
                throw std::runtime_error(
                    "map_model に path または prefix/count がありません");
            }

            const auto shaderPath = PathFromUtf8(definition.value(
                "shader", std::string{}));
            const auto assetRoot = m_graphics.Assets().AssetRoot();
            for (std::size_t index = 0; index < modelPaths.size(); ++index)
            {
                const auto modelPath = PathFromUtf8(modelPaths[index]);
                if (!std::filesystem::exists(assetRoot / modelPath))
                {
                    if (!warning.empty()) warning += ", ";
                    warning += modelPaths[index];
                    continue;
                }

                auto& model = m_scene.CreateEditorPreviewGameObject(
                    "Stage Map Model " + std::to_string(index));
                model.SetParent(&root);
                auto& renderer =
                    model.AddComponent<ModelRendererComponent>(modelPath);
                renderer.SetMaterialOverrideEnabled(false);
                renderer.SetUseLegacyShading(false);
                if (!shaderPath.empty())
                {
                    renderer.SetShaderPath(shaderPath);
                }

                // モデルのインポートとGPUリソース作成をプロファイル
                // 読み込み時に済ませ、最初のプレビュー更新へ処理落ちを
                // 持ち越しません。親rootはこの時点では有効です。
                model.Update(m_graphics, 0.0f);
                static_cast<void>(model.HasPreRender3DPass(m_graphics));
                ++loadedCount;
            }

            if (loadedCount == 0)
            {
                throw std::runtime_error(
                    "3Dマップモデルを読み込めませんでした");
            }
            root.SetEnabled(false);
            state.mapPreviewModelCount = loadedCount;
            state.mapPreviewError = warning;
        }
        catch (const std::exception& exception)
        {
            const std::string error = exception.what();
            ClearProjectStageMapPreview();
            state.mapPreviewError = error;
        }
    }

    void EditorLayer::RebuildProjectStageMapPreview()
    {
        auto& state = m_projectStageEditor;
        ClearProjectStageMapPreview();
        if (!state.loaded
            || !state.profiles
            || !state.profiles->contains("stages"))
        {
            return;
        }
        const auto& profiles = state.profiles->at("stages");
        if (state.profileIndex < profiles.size())
        {
            BuildProjectStageMapPreview(
                profiles.at(state.profileIndex));
        }
    }

    bool EditorLayer::LoadProjectStageProfile(const std::size_t profileIndex)
    {
        try
        {
            auto& state = m_projectStageEditor;
            if (!state.profiles || !state.profiles->contains("stages"))
            {
                throw std::runtime_error("stages がありません");
            }
            const auto& profiles = state.profiles->at("stages");
            if (profileIndex >= profiles.size())
            {
                throw std::runtime_error("ステージ番号が範囲外です");
            }
            const auto& profile = profiles.at(profileIndex);
            const auto projectRoot = ProjectSettingsPath().parent_path().parent_path();
            const auto basePath = projectRoot
                / PathFromUtf8(profile.at("base").get<std::string>());
            const auto overridePath = projectRoot
                / PathFromUtf8(profile.at("overrides").get<std::string>());

            std::ifstream baseInput(basePath, std::ios::binary);
            if (!baseInput)
            {
                throw std::runtime_error("基準道路を開けません: " + PathToUtf8(basePath));
            }
            const std::string text{
                std::istreambuf_iterator<char>{ baseInput },
                std::istreambuf_iterator<char>{}
            };
            const std::string marker =
                "inline constexpr StageRoadSection RoadSections[] = {";
            const auto markerAt = text.find(marker);
            const auto endAt = markerAt == std::string::npos
                ? std::string::npos : text.find("};", markerAt + marker.size());
            if (markerAt == std::string::npos || endAt == std::string::npos)
            {
                throw std::runtime_error("RoadSections配列を解析できません");
            }
            const std::string block = text.substr(
                markerAt + marker.size(), endAt - markerAt - marker.size());
            const std::regex rowPattern{
                R"(\{\s*\{\s*([^{}]+?)\s*\}\s*,\s*\{\s*([^{}]+?)\s*\}\s*,\s*\{\s*([^{}]+?)\s*\}\s*\})" };
            const std::regex numberPattern{
                R"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?f?)" };
            const auto parsePoint = [&](const std::string& source)
            {
                DirectX::XMFLOAT3 point{};
                float* values[]{ &point.x, &point.y, &point.z };
                std::size_t valueIndex = 0;
                for (std::sregex_iterator it(source.begin(), source.end(), numberPattern), end;
                    it != end && valueIndex < 3; ++it, ++valueIndex)
                {
                    auto number = it->str();
                    if (!number.empty() && number.back() == 'f') number.pop_back();
                    *values[valueIndex] = std::stof(number);
                }
                if (valueIndex != 3) throw std::runtime_error("道路座標が不正です");
                return point;
            };
            std::vector<ProjectRoadSection> sections;
            for (std::sregex_iterator it(block.begin(), block.end(), rowPattern), end;
                it != end; ++it)
            {
                ProjectRoadSection section;
                section.left = parsePoint((*it)[1].str());
                section.center = parsePoint((*it)[2].str());
                section.right = parsePoint((*it)[3].str());
                section.baseWidth = std::hypot(
                    section.right.x - section.left.x,
                    section.right.z - section.left.z);
                section.baseLeftWidth = std::hypot(
                    section.center.x - section.left.x,
                    section.center.z - section.left.z);
                section.baseRightWidth = std::hypot(
                    section.right.x - section.center.x,
                    section.right.z - section.center.z);
                sections.push_back(section);
            }
            if (sections.size() < 2)
            {
                throw std::runtime_error("道路断面が2件未満です");
            }

            std::ifstream overrideInput(overridePath, std::ios::binary);
            if (!overrideInput)
            {
                throw std::runtime_error("当たり判定JSONを開けません: " + PathToUtf8(overridePath));
            }
            auto overrideDocument = std::make_unique<nlohmann::json>();
            overrideInput >> *overrideDocument;
            std::vector<float> widths;
            std::vector<float> edgeBiases;
            std::vector<float> heights(sections.size(), 0.0f);
            widths.reserve(sections.size());
            edgeBiases.reserve(sections.size());
            for (const auto& section : sections)
            {
                widths.push_back(section.baseWidth);
                edgeBiases.push_back(
                    section.baseLeftWidth - section.baseWidth * 0.5f);
            }
            for (const auto& record : overrideDocument->value(
                "sections", nlohmann::json::array()))
            {
                const auto index = record.at("index").get<std::size_t>();
                if (index >= sections.size()) continue;
                const auto& left = record.at("left");
                const auto& right = record.at("right");
                const auto& center = record.at("center");
                widths[index] = std::hypot(
                    right.at(0).get<float>() - left.at(0).get<float>(),
                    right.at(2).get<float>() - left.at(2).get<float>());
                const float leftWidth = std::hypot(
                    center.at(0).get<float>() - left.at(0).get<float>(),
                    center.at(2).get<float>() - left.at(2).get<float>());
                edgeBiases[index] = leftWidth - widths[index] * 0.5f;
                heights[index] = center.at(1).get<float>() - sections[index].center.y;
            }
            state.profileIndex = profileIndex;
            state.baseSections = std::move(sections);
            state.widths = std::move(widths);
            state.edgeBiases = std::move(edgeBiases);
            state.heights = std::move(heights);
            state.overrideDocument = std::move(overrideDocument);
            state.selectedSection = 0;
            state.selectionStart = 0;
            state.selectionEnd = 0;
            const float sampleSpacing = state.overrideDocument->at("source")
                .value("sample_spacing_m", 1.0f);
            const int lastSection =
                static_cast<int>(state.baseSections.size()) - 1;
            state.startSection = std::clamp(
                state.overrideDocument->value("start_section",
                    static_cast<int>(std::lround(18.0f
                        / std::max(sampleSpacing, 0.01f)))),
                0, lastSection);
            state.goalSection = std::clamp(
                state.overrideDocument->value("goal_section",
                    lastSection - static_cast<int>(std::lround(45.0f
                        / std::max(sampleSpacing, 0.01f)))),
                0, lastSection);
            state.editLeftWidth = state.widths.front() * 0.5f
                + state.edgeBiases.front();
            state.editRightWidth = state.widths.front() * 0.5f
                - state.edgeBiases.front();
            state.editHeight = state.heights.front();
            const auto& reference = profile.value(
                "reference", nlohmann::json::object());
            state.referenceVisible = reference.value("visible", true);
            state.referenceFlipHorizontal = reference.value("flip_horizontal", false);
            state.referenceFlipVertical = reference.value("flip_vertical", false);
            state.referenceOpacity = reference.value("opacity", 0.34f);
            state.referenceScale = reference.value("scale", 1.0f);
            state.referenceRotationDegrees = reference.value("rotation_degrees", 0.0f);
            const auto offset = reference.value("offset", nlohmann::json::array({0.0f, 0.0f}));
            state.referenceOffset = {
                offset.size() > 0 ? offset.at(0).get<float>() : 0.0f,
                offset.size() > 1 ? offset.at(1).get<float>() : 0.0f };
            state.referenceImagePath = PathFromUtf8(
                reference.value("image", std::string{}));
            state.referenceTexture.reset();
            if (!state.referenceImagePath.empty())
            {
                state.referenceTexture = m_graphics.Assets().LoadTexture(
                    state.referenceImagePath);
            }
            state.undo.clear();
            state.redo.clear();
            state.dirty = false;
            state.loaded = true;
            state.fitView = true;
            state.previewHasRendered = false;
            state.previewNeedsRender = true;
            state.previewLastRenderAt = 0.0;
            BuildProjectStageMapPreview(profile);
            SetStatus("ステージを読み込みました: "
                + profile.value("label", std::string{}));
            return true;
        }
        catch (const std::exception& exception)
        {
            m_projectStageEditor.loaded = false;
            SetStatus(std::string{ "ステージエディタを読み込めません: " }
                + exception.what(), true);
            return false;
        }
    }

    bool EditorLayer::SaveProjectStageProfile(ProjectPanelDefinition& panel)
    {
        try
        {
            auto& state = m_projectStageEditor;
            const auto& profile = state.profiles->at("stages").at(state.profileIndex);
            const auto projectRoot = ProjectSettingsPath().parent_path().parent_path();
            const auto path = projectRoot
                / PathFromUtf8(profile.at("overrides").get<std::string>());
            auto document = *state.overrideDocument;
            document["revision"] = document.value("revision", 0) + 1;
            auto records = nlohmann::json::array();
            for (std::size_t index = 0; index < state.baseSections.size(); ++index)
            {
                const auto& base = state.baseSections[index];
                const float width = state.widths[index];
                const float leftWidth = width * 0.5f + state.edgeBiases[index];
                const float rightWidth = width * 0.5f - state.edgeBiases[index];
                const float height = state.heights[index];
                if (std::abs(width - base.baseWidth) < 0.0005f
                    && std::abs(leftWidth - base.baseLeftWidth) < 0.0005f
                    && std::abs(height) < 0.0005f) continue;
                const float dx = base.right.x - base.left.x;
                const float dz = base.right.z - base.left.z;
                const float length = std::max(std::hypot(dx, dz), 0.001f);
                const float rx = dx / length;
                const float rz = dz / length;
                records.push_back({
                    { "index", index },
                    { "left", { base.center.x - rx * leftWidth, base.left.y + height,
                        base.center.z - rz * leftWidth } },
                    { "center", { base.center.x, base.center.y + height,
                        base.center.z } },
                    { "right", { base.center.x + rx * rightWidth, base.right.y + height,
                        base.center.z + rz * rightWidth } }
                });
            }
            document["sections"] = std::move(records);
            document["start_section"] = state.startSection;
            document["goal_section"] = state.goalSection;
            document["constraints"]["symmetric_width"] = false;
            document["constraints"]["independent_edges"] = true;
            std::filesystem::create_directories(path.parent_path());
            const auto temporary = path.wstring() + L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) throw std::runtime_error("一時ファイルを作成できません");
                output << document.dump(2) << '\n';
            }
            if (std::filesystem::exists(path))
            {
                CopyFileW(path.c_str(), (path.wstring() + L".bak").c_str(), FALSE);
            }
            if (!MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(temporary.c_str());
                throw std::runtime_error("当たり判定JSONを置き換えられません");
            }
            *state.overrideDocument = std::move(document);
            auto& profileValue = state.profiles->at("stages").at(state.profileIndex);
            profileValue["reference"] = {
                { "image", PathToUtf8(state.referenceImagePath) },
                { "visible", state.referenceVisible },
                { "flip_horizontal", state.referenceFlipHorizontal },
                { "flip_vertical", state.referenceFlipVertical },
                { "opacity", state.referenceOpacity },
                { "scale", state.referenceScale },
                { "rotation_degrees", state.referenceRotationDegrees },
                { "offset", { state.referenceOffset.x, state.referenceOffset.y } }
            };
            const auto profilePath = projectRoot / panel.dataPath;
            const auto profileTemporary = profilePath.wstring() + L".tmp";
            {
                std::ofstream output(profileTemporary, std::ios::binary | std::ios::trunc);
                if (!output) throw std::runtime_error("参照画像設定を保存できません");
                output << state.profiles->dump(2) << '\n';
            }
            if (!MoveFileExW(profileTemporary.c_str(), profilePath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(profileTemporary.c_str());
                throw std::runtime_error("参照画像設定を置き換えられません");
            }
            state.dirty = false;
            if (!panel.saveCommand.command.empty())
            {
                auto command = panel.saveCommand;
                const auto key = profile.at("key").get<std::string>();
                for (auto& argument : command.arguments)
                {
                    if (argument == "{stage}") argument = key;
                }
                LaunchProjectMenuCommand(command);
            }
            SetStatus("ステージ当たり判定を保存しました");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(std::string{ "ステージを保存できません: " }
                + exception.what(), true);
            return false;
        }
    }

    void EditorLayer::DrawProjectStagePanel(
        const std::size_t panelIndex,
        ProjectPanelDefinition& panel)
    {
        auto& state = m_projectStageEditor;
        if (state.panelIndex != panelIndex)
        {
            ClearProjectStageMapPreview();
            state = ProjectStageEditorState{};
            state.panelIndex = panelIndex;
            try
            {
                const auto projectRoot = ProjectSettingsPath().parent_path().parent_path();
                std::ifstream input(projectRoot / panel.dataPath, std::ios::binary);
                if (!input) throw std::runtime_error("ステージ一覧を開けません");
                state.profiles = std::make_unique<nlohmann::json>();
                input >> *state.profiles;
                LoadProjectStageProfile(0);
            }
            catch (const std::exception& exception)
            {
                SetStatus(std::string{ "ステージエディタを開けません: " }
                    + exception.what(), true);
            }
        }

        ImGui::SetNextWindowSize(ImVec2{ 980.0f, 680.0f }, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(panel.title.c_str(), &panel.open,
            ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        if (!state.profiles || !state.loaded)
        {
            ImGui::TextUnformatted("ステージデータを読み込めませんでした。");
            if (ImGui::Button("再読み込み"))
            {
                state.panelIndex = static_cast<std::size_t>(-1);
            }
            ImGui::End();
            return;
        }

        const auto& profiles = state.profiles->at("stages");
        const bool readOnly = m_playing;
        std::vector<const char*> labels;
        labels.reserve(profiles.size());
        for (const auto& profile : profiles)
            labels.push_back(profile.at("label").get_ref<const std::string&>().c_str());
        int profile = static_cast<int>(state.profileIndex);
        ImGui::BeginDisabled(readOnly);
        ImGui::SetNextItemWidth(230.0f);
        if (ImGui::Combo("ステージ", &profile, labels.data(),
            static_cast<int>(labels.size())))
        {
            LoadProjectStageProfile(static_cast<std::size_t>(profile));
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) SaveProjectStageProfile(panel);
        ImGui::SameLine();
        if (ImGui::Button("再読み込み")) LoadProjectStageProfile(state.profileIndex);
        ImGui::SameLine();
        const bool canUndo = !state.undo.empty();
        ImGui::BeginDisabled(!canUndo);
        if (ImGui::Button("元に戻す"))
        {
            state.redo.push_back({ state.widths, state.edgeBiases, state.heights,
                state.startSection, state.goalSection });
            auto values = std::move(state.undo.back()); state.undo.pop_back();
            state.widths = std::move(values.widths);
            state.edgeBiases = std::move(values.edgeBiases);
            state.heights = std::move(values.heights);
            state.startSection = values.startSection;
            state.goalSection = values.goalSection;
            state.editLeftWidth = state.widths[state.selectedSection] * 0.5f
                + state.edgeBiases[state.selectedSection];
            state.editRightWidth = state.widths[state.selectedSection] * 0.5f
                - state.edgeBiases[state.selectedSection];
            state.editHeight = state.heights[state.selectedSection]; state.dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool canRedo = !state.redo.empty();
        ImGui::BeginDisabled(!canRedo);
        if (ImGui::Button("やり直す"))
        {
            state.undo.push_back({ state.widths, state.edgeBiases, state.heights,
                state.startSection, state.goalSection });
            auto values = std::move(state.redo.back()); state.redo.pop_back();
            state.widths = std::move(values.widths);
            state.edgeBiases = std::move(values.edgeBiases);
            state.heights = std::move(values.heights);
            state.startSection = values.startSection;
            state.goalSection = values.goalSection;
            state.editLeftWidth = state.widths[state.selectedSection] * 0.5f
                + state.edgeBiases[state.selectedSection];
            state.editRightWidth = state.widths[state.selectedSection] * 0.5f
                - state.edgeBiases[state.selectedSection];
            state.editHeight = state.heights[state.selectedSection]; state.dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (readOnly)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.35f,0.78f,1.0f,1.0f},
                "再生中: 3Dデバッグ表示（編集ロック）");
        }
        if (state.dirty)
        {
            ImGui::SameLine(); ImGui::TextColored(ImVec4{1.0f,0.72f,0.2f,1.0f}, "未保存");
        }
        ImGui::Separator();

        ImGui::BeginChild("StageControls", ImVec2{ 285.0f, 0.0f }, true);
        ImGui::TextUnformatted("デバッグ表示");
        if (ImGui::RadioButton("2D", !state.view3D))
        {
            state.view3D = false;
            state.fitView = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("3D", state.view3D))
        {
            state.view3D = true;
            state.fitView = true;
            // 3Dへ戻った最初のフレームは、更新間隔を待たずに描きます。
            state.previewHasRendered = false;
            state.previewNeedsRender = true;
        }
        ImGui::SameLine();
        ImGui::Checkbox("頂点", &state.showVertices);
        if (ImGui::Button("全体を表示", ImVec2{-1.0f, 0.0f}))
            state.fitView = true;
        if (ImGui::Button("選択位置へフォーカス", ImVec2{-1.0f, 0.0f}))
            state.focusSelection = true;
        if (state.view3D)
        {
            if (ImGui::Checkbox("コライダー", &state.showColliders))
            {
                state.previewNeedsRender = true;
            }
            ImGui::Checkbox("マップ", &state.mapOverlayVisible);
            if (ImGui::Checkbox("3Dマップ", &state.mapModelVisible))
            {
                state.previewNeedsRender = true;
            }
            if (m_scene.FindGameObject(state.mapPreviewRootId) != nullptr)
            {
                ImGui::TextDisabled(
                    "3Dモデル: %zu個（実モデル座標）",
                    state.mapPreviewModelCount);
            }
            else if (!state.mapPreviewError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.55f, 0.35f, 1.0f },
                    "3Dモデル: %s",
                    state.mapPreviewError.c_str());
            }
            int previewFrameRate = state.previewFrameRate;
            if (ImGui::SliderInt(
                "プレビューFPS", &previewFrameRate, 10, 60, "%d Hz"))
            {
                state.previewFrameRate = previewFrameRate;
                state.previewNeedsRender = true;
            }
            if (ImGui::SliderFloat(
                "プレビュー解像度", &state.previewRenderScale,
                0.5f, 1.0f, "%.2fx"))
            {
                state.previewHasRendered = false;
                state.previewNeedsRender = true;
            }
            if (ImGui::Button("3D表示を更新", ImVec2{-1.0f, 0.0f}))
            {
                state.previewNeedsRender = true;
            }
            ImGui::TextDisabled(
                "静止中は再描画しません（再生・操作中は設定Hzまで）");
            ImGui::TextDisabled("実モデルと同じ座標（高さ 1.0x）");
            ImGui::TextWrapped(
                "3D操作: 右ドラッグ=回転 / 中ドラッグ=移動 / ホイール=拡大縮小");
        }
        ImGui::Separator();
        ImGui::BeginDisabled(readOnly);
        int sectionIndex = state.selectedSection;
        if (ImGui::SliderInt("断面", &sectionIndex, 0,
            static_cast<int>(state.baseSections.size()) - 1))
        {
            state.selectedSection = sectionIndex;
            state.editLeftWidth = state.widths[sectionIndex] * 0.5f
                + state.edgeBiases[sectionIndex];
            state.editRightWidth = state.widths[sectionIndex] * 0.5f
                - state.edgeBiases[sectionIndex];
            state.editHeight = state.heights[sectionIndex];
        }
        ImGui::Text("全 %zu断面 / 基準幅 %.2fm", state.baseSections.size(),
            state.baseSections[state.selectedSection].baseWidth);
        ImGui::Spacing();
        ImGui::TextUnformatted("道路端（中心線からの距離）");
        const auto pushStageUndo = [&state]()
        {
            state.undo.push_back({ state.widths, state.edgeBiases, state.heights,
                state.startSection, state.goalSection });
            if (state.undo.size() > 64) state.undo.erase(state.undo.begin());
            state.redo.clear();
        };
        const auto commitSelectedSection = [&state]()
        {
            const auto index = static_cast<std::size_t>(state.selectedSection);
            state.widths[index] = state.editLeftWidth + state.editRightWidth;
            state.edgeBiases[index] =
                (state.editLeftWidth - state.editRightWidth) * 0.5f;
            state.heights[index] = state.editHeight;
            state.dirty = true;
        };
        int range[2]{state.selectionStart, state.selectionEnd};
        if (ImGui::DragIntRange2("選択範囲", &range[0], &range[1], 1.0f,
            0, static_cast<int>(state.baseSections.size()) - 1,
            "%d", "%d"))
        {
            state.selectionStart = std::min(range[0], range[1]);
            state.selectionEnd = std::max(range[0], range[1]);
        }
        if (ImGui::Button("現在だけ選択"))
        {
            state.selectionStart = state.selectedSection;
            state.selectionEnd = state.selectedSection;
        }
        ImGui::SameLine();
        if (ImGui::Button("全選択 (Ctrl+A)"))
        {
            state.selectionStart = 0;
            state.selectionEnd = static_cast<int>(state.baseSections.size()) - 1;
        }
        if (ImGui::Button("選択位置をスタートに設定", ImVec2{-1.0f, 0.0f}))
        {
            pushStageUndo();
            state.startSection = state.selectedSection;
            state.dirty = true;
        }
        if (ImGui::Button("選択位置をゴールに設定", ImVec2{-1.0f, 0.0f}))
        {
            pushStageUndo();
            state.goalSection = state.selectedSection;
            state.dirty = true;
        }
        const float spacing = state.overrideDocument->at("source")
            .value("sample_spacing_m", 1.0f);
        ImGui::Text("START #%d (%.1fm) / GOAL #%d (%.1fm)",
            state.startSection, state.startSection * spacing,
            state.goalSection, state.goalSection * spacing);
        ImGui::SetNextItemWidth(-1.0f);
        const bool leftWidthChanged = ImGui::DragFloat(
            "左端##RoadLeftWidth", &state.editLeftWidth,
            0.05f, 0.5f, 20.0f, "%.2f m");
        if (ImGui::IsItemActivated()) pushStageUndo();
        if (leftWidthChanged) commitSelectedSection();
        ImGui::SetNextItemWidth(-1.0f);
        const bool rightWidthChanged = ImGui::DragFloat(
            "右端##RoadRightWidth", &state.editRightWidth,
            0.05f, 0.5f, 20.0f, "%.2f m");
        if (ImGui::IsItemActivated()) pushStageUndo();
        if (rightWidthChanged) commitSelectedSection();
        if (ImGui::Button("左を基準"))
        {
            pushStageUndo();
            state.editLeftWidth = state.baseSections[state.selectedSection].baseLeftWidth;
            commitSelectedSection();
        }
        ImGui::SameLine(); if (ImGui::Button("右を基準"))
        {
            pushStageUndo();
            state.editRightWidth = state.baseSections[state.selectedSection].baseRightWidth;
            commitSelectedSection();
        }
        if (ImGui::Button("両端を基準へ戻す", ImVec2{-1.0f, 0.0f}))
        {
            pushStageUndo();
            state.editLeftWidth = state.baseSections[state.selectedSection].baseLeftWidth;
            state.editRightWidth = state.baseSections[state.selectedSection].baseRightWidth;
            commitSelectedSection();
        }
        ImGui::TextUnformatted("高さ補正");
        ImGui::SetNextItemWidth(-1.0f);
        const bool heightChanged = ImGui::DragFloat(
            "##RoadHeight", &state.editHeight, 0.01f, -5.0f, 5.0f, "%.2f m");
        if (ImGui::IsItemActivated()) pushStageUndo();
        if (heightChanged) commitSelectedSection();
        ImGui::TextUnformatted("周囲へなじませる範囲");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##BrushRadius", &state.brushRadius, 1.0f, 0.0f, 300.0f, "%.0f m");
        if (ImGui::Button("この範囲へ適用", ImVec2{-1.0f, 34.0f}))
        {
            state.undo.push_back({ state.widths, state.edgeBiases, state.heights,
                state.startSection, state.goalSection });
            if (state.undo.size() > 64) state.undo.erase(state.undo.begin());
            state.redo.clear();
            const float brushSpacing = state.overrideDocument->at("source")
                .value("sample_spacing_m", 1.0f);
            const int radius = static_cast<int>(std::ceil(state.brushRadius
                / std::max(brushSpacing, 0.01f)));
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const int target = state.selectedSection + offset;
                if (target < 0 || target >= static_cast<int>(state.widths.size())) continue;
                const float distance = std::abs(offset) * brushSpacing;
                const float ratio = state.brushRadius <= 0.0f ? 0.0f
                    : std::min(1.0f, distance / state.brushRadius);
                const float weight = offset == 0 && state.brushRadius <= 0.0f
                    ? 1.0f : std::pow(1.0f - ratio * ratio, 2.0f);
                const float oldLeft = state.widths[target]*0.5f + state.edgeBiases[target];
                const float oldRight = state.widths[target]*0.5f - state.edgeBiases[target];
                const float newLeft = oldLeft + (state.editLeftWidth-oldLeft)*weight;
                const float newRight = oldRight + (state.editRightWidth-oldRight)*weight;
                state.widths[target] = newLeft + newRight;
                state.edgeBiases[target] = (newLeft-newRight)*0.5f;
                state.heights[target] += (state.editHeight - state.heights[target]) * weight;
            }
            state.dirty = true;
        }
        if (ImGui::Button("選択断面を基準へ戻す", ImVec2{-1.0f, 0.0f}))
        {
            state.undo.push_back({ state.widths, state.edgeBiases, state.heights,
                state.startSection, state.goalSection }); state.redo.clear();
            state.widths[state.selectedSection] = state.baseSections[state.selectedSection].baseWidth;
            state.edgeBiases[state.selectedSection] =
                state.baseSections[state.selectedSection].baseLeftWidth
                - state.baseSections[state.selectedSection].baseWidth*0.5f;
            state.heights[state.selectedSection] = 0.0f;
            state.editLeftWidth = state.baseSections[state.selectedSection].baseLeftWidth;
            state.editRightWidth = state.baseSections[state.selectedSection].baseRightWidth;
            state.editHeight = 0.0f;
            state.dirty = true;
        }
        ImGui::Separator();
        ImGui::TextUnformatted("選択範囲の自動スムージング");
        ImGui::SliderFloat("強さ", &state.smoothStrength, 0.05f, 1.0f, "%.2f");
        ImGui::SliderInt("反復回数", &state.smoothIterations, 1, 12);
        const auto smoothSelection = [&state, &pushStageUndo]()
        {
            const int first = std::clamp(std::min(
                state.selectionStart, state.selectionEnd), 0,
                static_cast<int>(state.widths.size()) - 1);
            const int last = std::clamp(std::max(
                state.selectionStart, state.selectionEnd), 0,
                static_cast<int>(state.widths.size()) - 1);
            if (last - first < 2)
            {
                return;
            }
            pushStageUndo();
            for (int pass = 0; pass < state.smoothIterations; ++pass)
            {
                auto widths = state.widths;
                auto biases = state.edgeBiases;
                auto heights = state.heights;
                for (int index = first + 1; index < last; ++index)
                {
                    const auto blend = [index, &state](
                        const std::vector<float>& values)
                    {
                        const float average = (values[index - 1]
                            + values[index + 1]) * 0.5f;
                        return values[index]
                            + (average - values[index])
                                * state.smoothStrength;
                    };
                    widths[index] = blend(state.widths);
                    biases[index] = blend(state.edgeBiases);
                    heights[index] = blend(state.heights);
                }
                state.widths = std::move(widths);
                state.edgeBiases = std::move(biases);
                state.heights = std::move(heights);
            }
            state.editLeftWidth = state.widths[state.selectedSection] * 0.5f
                + state.edgeBiases[state.selectedSection];
            state.editRightWidth = state.widths[state.selectedSection] * 0.5f
                - state.edgeBiases[state.selectedSection];
            state.editHeight = state.heights[state.selectedSection];
            state.dirty = true;
        };
        if (ImGui::Button("選択範囲を滑らかに (S)", ImVec2{-1.0f, 34.0f}))
            smoothSelection();
        if (!readOnly
            && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_A, false))
            {
                state.selectionStart = 0;
                state.selectionEnd =
                    static_cast<int>(state.baseSections.size()) - 1;
            }
            else if (!ImGui::GetIO().KeyCtrl
                && ImGui::IsKeyPressed(ImGuiKey_S, false))
            {
                smoothSelection();
            }
        }
        ImGui::Separator();
        ImGui::TextWrapped("選択: 左クリック=頂点選択 / Shift+左クリック=範囲選択 / Ctrl+A=全選択 / S=選択範囲を滑らかに / 赤い端点ドラッグ=幅調整");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(readOnly);
        if (ImGui::CollapsingHeader("トレス用の参照画像",
            ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Checkbox("画像を表示", &state.referenceVisible))
                state.dirty = true;
            if (ImGui::Checkbox("左右反転", &state.referenceFlipHorizontal))
                state.dirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("上下反転", &state.referenceFlipVertical))
                state.dirty = true;
            if (ImGui::Button("画像を選択...", ImVec2{-1.0f, 0.0f}))
            {
                std::array<wchar_t, 32768> pathBuffer{};
                OPENFILENAMEW dialog{};
                dialog.lStructSize = sizeof(dialog);
                dialog.hwndOwner = m_window;
                dialog.lpstrFilter = L"画像 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0すべてのファイル\0*.*\0\0";
                dialog.lpstrFile = pathBuffer.data();
                dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
                dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&dialog))
                {
                    state.referenceImagePath = pathBuffer.data();
                    state.referenceTexture = m_graphics.Assets().LoadTexture(
                        state.referenceImagePath);
                    state.referenceVisible = true;
                    state.dirty = true;
                }
            }
            ImGui::TextWrapped("%s", state.referenceImagePath.empty()
                ? "画像なし" : PathToUtf8(state.referenceImagePath).c_str());
            float referenceOpacityPercent = state.referenceOpacity * 100.0f;
            if (ImGui::SliderFloat("透明度", &referenceOpacityPercent,
                5.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_None))
            {
                state.referenceOpacity = referenceOpacityPercent / 100.0f;
                state.dirty = true;
            }
            if (ImGui::DragFloat("画像倍率", &state.referenceScale,
                0.005f, 0.05f, 10.0f, "%.3f x")) state.dirty = true;
            if (ImGui::DragFloat("画像回転", &state.referenceRotationDegrees,
                0.1f, -180.0f, 180.0f, "%.1f deg")) state.dirty = true;
            float offsetValues[]{state.referenceOffset.x, state.referenceOffset.y};
            if (ImGui::DragFloat2("画像位置 X/Z", offsetValues,
                0.5f, -10000.0f, 10000.0f, "%.1f m"))
            {
                state.referenceOffset = {offsetValues[0], offsetValues[1]};
                state.dirty = true;
            }
            if (ImGui::Button("画像合わせをリセット", ImVec2{-1.0f, 0.0f}))
            {
                state.referenceScale = 1.0f;
                state.referenceRotationDegrees = 0.0f;
                state.referenceOffset = {};
                state.referenceFlipHorizontal = false;
                state.referenceFlipVertical = false;
                state.dirty = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("StageCanvas", ImVec2{0.0f, 0.0f}, true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize{
            std::max(ImGui::GetContentRegionAvail().x, 64.0f),
            std::max(ImGui::GetContentRegionAvail().y, 64.0f) };
        state.previewVisible = state.view3D;
        state.previewSize = { canvasSize.x, canvasSize.y };
        if (state.view3D)
        {
            const float renderScale = std::clamp(
                state.previewRenderScale, 0.5f, 1.0f);
            const auto previewWidth = std::max(
                static_cast<std::uint32_t>(std::lround(
                    canvasSize.x * renderScale)), 1u);
            const auto previewHeight = std::max(
                static_cast<std::uint32_t>(std::lround(
                    canvasSize.y * renderScale)), 1u);
            if (previewWidth != m_projectStagePreview.Width()
                || previewHeight != m_projectStagePreview.Height())
            {
                state.previewHasRendered = false;
                state.previewNeedsRender = true;
            }
            m_projectStagePreview.Resize(
                m_graphics.Device(),
                previewWidth,
                previewHeight);
        }
        ImGui::InvisibleButton("##StageRoadCanvas", canvasSize,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
            | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        if (state.view3D && m_projectStagePreview.IsValid())
        {
            draw->AddImage(
                MakeTextureReference(
                    m_projectStagePreview.DisplayShaderResourceView()),
                canvasPosition,
                ImVec2{ canvasPosition.x + canvasSize.x,
                    canvasPosition.y + canvasSize.y });
        }
        else
        {
            draw->AddRectFilled(canvasPosition,
                ImVec2{canvasPosition.x + canvasSize.x,
                    canvasPosition.y + canvasSize.y},
                IM_COL32(18, 22, 28, 255));
        }
        // 3Dビューでは全体の位置関係も同時に確認できるよう、ステージ
        // プロファイルのミニマップを右上へインセット表示します。画像は
        // 2Dトレスと同じ参照テクスチャなので、モデルやコースの座標を
        // 変更せずに全体地図だけを重ねられます。
        if (state.view3D
            && state.mapOverlayVisible
            && state.referenceTexture
            && state.referenceTexture->view)
        {
            const float imageAspect = static_cast<float>(
                std::max(state.referenceTexture->width, 1u))
                / static_cast<float>(
                    std::max(state.referenceTexture->height, 1u));
            const float insetWidth = std::min(
                220.0f,
                std::max(canvasSize.x - 28.0f, 96.0f) * 0.34f);
            const float insetHeight = insetWidth
                / std::max(imageAspect, 0.001f);
            const ImVec2 insetMin{
                canvasPosition.x + canvasSize.x - insetWidth - 14.0f,
                canvasPosition.y + 14.0f };
            const ImVec2 insetMax{
                insetMin.x + insetWidth,
                std::min(
                    insetMin.y + insetHeight,
                    canvasPosition.y + canvasSize.y - 14.0f) };
            draw->AddRectFilled(
                ImVec2{ insetMin.x - 4.0f, insetMin.y - 4.0f },
                ImVec2{ insetMax.x + 4.0f, insetMax.y + 4.0f },
                IM_COL32(5, 9, 14, 220), 4.0f);
            const float uvTop = state.referenceFlipVertical ? 1.0f : 0.0f;
            const float uvBottom = state.referenceFlipVertical ? 0.0f : 1.0f;
            const float uvLeft = state.referenceFlipHorizontal ? 1.0f : 0.0f;
            const float uvRight = state.referenceFlipHorizontal ? 0.0f : 1.0f;
            draw->AddImage(
                MakeTextureReference(state.referenceTexture->view.Get()),
                insetMin,
                insetMax,
                ImVec2{ uvLeft, uvTop },
                ImVec2{ uvRight, uvBottom },
                IM_COL32(255, 255, 255, 230));
            draw->AddRect(
                insetMin,
                insetMax,
                IM_COL32(105, 190, 245, 235),
                3.0f,
                0,
                1.5f);
            draw->AddText(
                ImVec2{ insetMin.x + 6.0f, insetMin.y + 5.0f },
                IM_COL32(255, 255, 255, 245),
                "MAP");
        }
        const auto editedRoadEdge = [&state](
            const std::size_t index, const bool left)
        {
            const auto& section = state.baseSections[index];
            const float dx = section.right.x - section.left.x;
            const float dz = section.right.z - section.left.z;
            const float directionLength = std::max(std::hypot(dx, dz), 0.001f);
            const float distance = left
                ? state.widths[index] * 0.5f + state.edgeBiases[index]
                : state.widths[index] * 0.5f - state.edgeBiases[index];
            const float direction = left ? -1.0f : 1.0f;
            return DirectX::XMFLOAT3{
                section.center.x + direction * dx / directionLength * distance,
                section.center.y + state.heights[index],
                section.center.z + direction * dz / directionLength * distance };
        };
        const auto editedCenter = [&state](const std::size_t index)
        {
            auto point = state.baseSections[index].center;
            point.y += state.heights[index];
            return point;
        };
        float minX = state.baseSections.front().center.x, maxX = minX;
        float minZ = state.baseSections.front().center.z, maxZ = minZ;
        float minY = editedCenter(0).y, maxY = minY;
        for (std::size_t index = 0; index < state.baseSections.size(); ++index)
        {
            const auto center = editedCenter(index);
            const auto left = editedRoadEdge(index, true);
            const auto right = editedRoadEdge(index, false);
            minX = std::min({minX, left.x, center.x, right.x});
            maxX = std::max({maxX, left.x, center.x, right.x});
            minY = std::min({minY, left.y, center.y, right.y});
            maxY = std::max({maxY, left.y, center.y, right.y});
            minZ = std::min({minZ, left.z, center.z, right.z});
            maxZ = std::max({maxZ, left.z, center.z, right.z});
        }
        const float midX = (minX + maxX) * 0.5f, midZ = (minZ + maxZ) * 0.5f;
        const float midY = (minY + maxY) * 0.5f;
        const auto projectedPoint = [&](const DirectX::XMFLOAT3& point)
        {
            if (!state.view3D)
                return DirectX::XMFLOAT2{point.x - midX, -(point.z - midZ)};
            const float yaw = DirectX::XMConvertToRadians(state.orbitYawDegrees);
            const float pitch = DirectX::XMConvertToRadians(state.orbitPitchDegrees);
            const float localX = point.x - midX;
            const float localY = (point.y - midY) * state.verticalScale;
            const float localZ = point.z - midZ;
            const float horizontal = localX * std::cos(yaw)
                - localZ * std::sin(yaw);
            const float depth = localX * std::sin(yaw)
                + localZ * std::cos(yaw);
            const float vertical = localY * std::cos(pitch)
                - depth * std::sin(pitch);
            return DirectX::XMFLOAT2{horizontal, -vertical};
        };
        if (state.fitView)
        {
            float projectMinX = std::numeric_limits<float>::max();
            float projectMaxX = std::numeric_limits<float>::lowest();
            float projectMinY = std::numeric_limits<float>::max();
            float projectMaxY = std::numeric_limits<float>::lowest();
            for (std::size_t index = 0; index < state.baseSections.size(); ++index)
            {
                for (const auto point : {editedRoadEdge(index, true),
                    editedCenter(index), editedRoadEdge(index, false)})
                {
                    const auto projected = projectedPoint(point);
                    projectMinX = std::min(projectMinX, projected.x);
                    projectMaxX = std::max(projectMaxX, projected.x);
                    projectMinY = std::min(projectMinY, projected.y);
                    projectMaxY = std::max(projectMaxY, projected.y);
                }
            }
            state.zoom = 0.88f * std::min(
                canvasSize.x / std::max(projectMaxX-projectMinX, 1.0f),
                canvasSize.y / std::max(projectMaxY-projectMinY, 1.0f));
            state.pan = {}; state.fitView = false;
            state.previewNeedsRender = true;
        }
        if (state.focusSelection)
        {
            const auto focus = projectedPoint(editedCenter(
                static_cast<std::size_t>(state.selectedSection)));
            state.zoom = std::clamp(std::max(state.zoom, 8.0f),
                0.005f, 100.0f);
            state.pan = {-focus.x * state.zoom, -focus.y * state.zoom};
            state.focusSelection = false;
            state.previewNeedsRender = true;
        }
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 center{canvasPosition.x+canvasSize.x*0.5f+state.pan.x,
                canvasPosition.y+canvasSize.y*0.5f+state.pan.y};
            const float factor = std::pow(1.18f, ImGui::GetIO().MouseWheel);
            state.zoom = std::clamp(state.zoom * factor, 0.005f, 100.0f);
            state.pan.x = mouse.x - (mouse.x-center.x)*factor
                - (canvasPosition.x+canvasSize.x*0.5f);
            state.pan.y = mouse.y - (mouse.y-center.y)*factor
                - (canvasPosition.y+canvasSize.y*0.5f);
            state.previewNeedsRender = true;
        }
        if (hovered && state.view3D
            && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            state.orbitYawDegrees += ImGui::GetIO().MouseDelta.x * 0.35f;
            state.orbitPitchDegrees = std::clamp(state.orbitPitchDegrees
                - ImGui::GetIO().MouseDelta.y * 0.35f, 5.0f, 85.0f);
            state.previewNeedsRender = true;
        }
        if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)
            || (!state.view3D
                && ImGui::IsMouseDragging(ImGuiMouseButton_Right))))
        {
            state.pan.x += ImGui::GetIO().MouseDelta.x;
            state.pan.y += ImGui::GetIO().MouseDelta.y;
            state.previewNeedsRender = true;
        }
        // ProjectStageViewMatrixはコース全断面から表示範囲を求めます。
        // 頂点ごとに呼ぶと「表示頂点数 × 全断面数」になり、赤城でも
        // 数千万回相当の走査が発生していました。操作反映後の行列を
        // 1フレームに1回だけ作り、全投影で共有します。
        const auto stageViewMatrix = state.view3D
            ? ProjectStageViewMatrix()
            : DirectX::XMMatrixIdentity();
        const auto stageProjectionMatrix = state.view3D
            ? ProjectStageProjectionMatrix()
            : DirectX::XMMatrixIdentity();
        const auto screenPoint = [&](const DirectX::XMFLOAT3& point)
        {
            if (state.view3D)
            {
                DirectX::XMFLOAT3 projected{};
                DirectX::XMStoreFloat3(&projected,
                    DirectX::XMVector3Project(
                        DirectX::XMLoadFloat3(&point),
                        0.0f, 0.0f, canvasSize.x, canvasSize.y,
                        0.0f, 1.0f,
                        stageProjectionMatrix,
                        stageViewMatrix,
                        DirectX::XMMatrixIdentity()));
                return ImVec2{ canvasPosition.x + projected.x,
                    canvasPosition.y + projected.y };
            }
            const auto projected = projectedPoint(point);
            return ImVec2{ canvasPosition.x + canvasSize.x*0.5f + state.pan.x
                    + projected.x*state.zoom,
                canvasPosition.y + canvasSize.y*0.5f + state.pan.y
                    + projected.y*state.zoom };
        };
        if (!state.view3D && state.referenceVisible && state.referenceTexture
            && state.referenceTexture->view)
        {
            const float roadWidth = std::max(maxX-minX, 1.0f);
            const float roadHeight = std::max(maxZ-minZ, 1.0f);
            const float imageAspect = static_cast<float>(
                std::max(state.referenceTexture->width, 1u))
                / static_cast<float>(std::max(state.referenceTexture->height, 1u));
            float imageWorldWidth = roadWidth;
            float imageWorldHeight = imageWorldWidth / std::max(imageAspect, 0.001f);
            if (imageWorldHeight < roadHeight)
            {
                imageWorldHeight = roadHeight;
                imageWorldWidth = imageWorldHeight * imageAspect;
            }
            const ImVec2 imageCenter{
                canvasPosition.x + canvasSize.x*0.5f + state.pan.x
                    + state.referenceOffset.x*state.zoom,
                canvasPosition.y + canvasSize.y*0.5f + state.pan.y
                    - state.referenceOffset.y*state.zoom };
            const float halfWidth = imageWorldWidth*state.referenceScale*state.zoom*0.5f;
            const float halfHeight = imageWorldHeight*state.referenceScale*state.zoom*0.5f;
            const float radians = -DirectX::XMConvertToRadians(
                state.referenceRotationDegrees);
            const float cosine = std::cos(radians), sine = std::sin(radians);
            const auto corner = [&](const float x, const float y)
            {
                return ImVec2{imageCenter.x + x*cosine-y*sine,
                    imageCenter.y + x*sine+y*cosine};
            };
            const float uvLeft = state.referenceFlipHorizontal ? 1.0f : 0.0f;
            const float uvRight = state.referenceFlipHorizontal ? 0.0f : 1.0f;
            const float uvTop = state.referenceFlipVertical ? 1.0f : 0.0f;
            const float uvBottom = state.referenceFlipVertical ? 0.0f : 1.0f;
            draw->AddImageQuad(
                MakeTextureReference(state.referenceTexture->view.Get()),
                corner(-halfWidth,-halfHeight), corner(halfWidth,-halfHeight),
                corner(halfWidth,halfHeight), corner(-halfWidth,halfHeight),
                ImVec2{uvLeft,uvTop}, ImVec2{uvRight,uvTop},
                ImVec2{uvRight,uvBottom}, ImVec2{uvLeft,uvBottom},
                IM_COL32(255,255,255,static_cast<int>(
                    std::clamp(state.referenceOpacity,0.0f,1.0f)*255.0f)));
        }
        // 3DではXMVector3ProjectとImGuiプリミティブがCPU負荷になるため、
        // 全体表示時の線密度を抑えます。編集値そのものは間引きません。
        const std::size_t roadBudget = state.view3D ? 700 : 2200;
        const std::size_t step = std::max<std::size_t>(
            1, state.baseSections.size() / roadBudget);
        for (std::size_t index = step; index < state.baseSections.size(); index += step)
        {
            const auto& previous = state.baseSections[index-step];
            const auto& current = state.baseSections[index];
            const auto previousLeft = editedRoadEdge(index-step, true);
            const auto previousRight = editedRoadEdge(index-step, false);
            const auto currentLeft = editedRoadEdge(index, true);
            const auto currentRight = editedRoadEdge(index, false);
            if (state.view3D)
            {
                const ImVec2 previousLeftScreen = screenPoint(previousLeft);
                const ImVec2 previousRightScreen = screenPoint(previousRight);
                const ImVec2 currentLeftScreen = screenPoint(currentLeft);
                const ImVec2 currentRightScreen = screenPoint(currentRight);
                const ImVec2 previousCenterScreen = screenPoint(
                    editedCenter(index-step));
                const ImVec2 currentCenterScreen = screenPoint(
                    editedCenter(index));
                draw->AddQuadFilled(previousLeftScreen,
                    previousRightScreen, currentRightScreen,
                    currentLeftScreen, IM_COL32(42,91,72,105));
                draw->AddLine(currentLeftScreen, currentRightScreen,
                    IM_COL32(75,125,155,100), 1.0f);
                draw->AddLine(previousLeftScreen, currentLeftScreen,
                    IM_COL32(70,145,215,210), 1.4f);
                draw->AddLine(previousRightScreen, currentRightScreen,
                    IM_COL32(70,145,215,210), 1.4f);
                draw->AddLine(previousCenterScreen, currentCenterScreen,
                    IM_COL32(185,195,205,160), 1.0f);
            }
            else
            {
                draw->AddLine(screenPoint(previous.left), screenPoint(current.left),
                    IM_COL32(95,105,115,90), 1.0f);
                draw->AddLine(screenPoint(previous.right), screenPoint(current.right),
                    IM_COL32(95,105,115,90), 1.0f);
                draw->AddLine(screenPoint(previousLeft),
                    screenPoint(currentLeft),
                    IM_COL32(70,145,215,210), 1.4f);
                draw->AddLine(screenPoint(previousRight),
                    screenPoint(currentRight),
                    IM_COL32(70,145,215,210), 1.4f);
                draw->AddLine(screenPoint(editedCenter(index-step)),
                    screenPoint(editedCenter(index)),
                    IM_COL32(185,195,205,160), 1.0f);
            }
        }
        if (state.showVertices)
        {
            const std::size_t vertexBudget = state.view3D ? 450 : 1500;
            const std::size_t vertexStep = std::max<std::size_t>(
                1, state.baseSections.size() / vertexBudget);
            for (std::size_t index = 0; index < state.baseSections.size();
                index += vertexStep)
            {
                const bool selectedRange = static_cast<int>(index)
                        >= std::min(state.selectionStart, state.selectionEnd)
                    && static_cast<int>(index)
                        <= std::max(state.selectionStart, state.selectionEnd);
                draw->AddCircleFilled(screenPoint(editedCenter(index)),
                    selectedRange ? 3.0f : 2.0f,
                    selectedRange ? IM_COL32(255,205,70,235)
                        : IM_COL32(130,205,255,185));
            }
        }
        const auto drawCourseMarker = [&](const int index, const char* label,
            const ImU32 color)
        {
            const ImVec2 point = screenPoint(editedCenter(
                static_cast<std::size_t>(index)));
            draw->AddCircleFilled(point, 8.0f, color);
            draw->AddCircle(point, 11.0f, IM_COL32(255,255,255,230), 0, 2.0f);
            draw->AddText(ImVec2{point.x + 13.0f, point.y - 9.0f}, color, label);
        };
        drawCourseMarker(state.startSection, "START", IM_COL32(75,235,120,255));
        drawCourseMarker(state.goalSection, "GOAL", IM_COL32(255,95,90,255));
        const auto editedLeft = editedRoadEdge(state.selectedSection, true);
        const auto editedRight = editedRoadEdge(state.selectedSection, false);
        const auto selectedCenter = editedCenter(state.selectedSection);
        const ImVec2 leftHandle = screenPoint(editedLeft);
        const ImVec2 rightHandle = screenPoint(editedRight);
        draw->AddLine(leftHandle, rightHandle, IM_COL32(255,190,45,255), 3.0f);
        draw->AddCircleFilled(leftHandle, 6.0f, IM_COL32(245,80,70,255));
        draw->AddCircleFilled(rightHandle, 6.0f, IM_COL32(245,80,70,255));
        draw->AddCircleFilled(screenPoint(selectedCenter), 5.0f,
            IM_COL32(255,235,115,255));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const auto distanceSquared = [&](const ImVec2 point)
            {
                return (point.x-mouse.x)*(point.x-mouse.x)
                    + (point.y-mouse.y)*(point.y-mouse.y);
            };
            state.draggingRoadEdge = !readOnly
                && distanceSquared(leftHandle) <= 144.0f
                ? -1 : (!readOnly && distanceSquared(rightHandle) <= 144.0f
                    ? 1 : 0);
            if (state.draggingRoadEdge != 0)
            {
                state.undo.push_back({ state.widths, state.edgeBiases, state.heights,
                    state.startSection, state.goalSection });
                if (state.undo.size() > 64) state.undo.erase(state.undo.begin());
                state.redo.clear();
            }
            float nearest = 18.0f*18.0f; int nearestIndex = -1;
            for (std::size_t index = 0;
                state.draggingRoadEdge == 0 && index < state.baseSections.size(); ++index)
            {
                const auto point = screenPoint(editedCenter(index));
                const float distance = (point.x-mouse.x)*(point.x-mouse.x)
                    + (point.y-mouse.y)*(point.y-mouse.y);
                if (distance < nearest) { nearest = distance; nearestIndex = static_cast<int>(index); }
            }
            if (nearestIndex >= 0)
            {
                state.selectedSection = nearestIndex;
                if (ImGui::GetIO().KeyShift)
                {
                    state.selectionStart = std::min(
                        state.selectionStart, nearestIndex);
                    state.selectionEnd = std::max(
                        state.selectionEnd, nearestIndex);
                }
                else
                {
                    state.selectionStart = nearestIndex;
                    state.selectionEnd = nearestIndex;
                }
                state.editLeftWidth = state.widths[nearestIndex]*0.5f
                    + state.edgeBiases[nearestIndex];
                state.editRightWidth = state.widths[nearestIndex]*0.5f
                    - state.edgeBiases[nearestIndex];
                state.editHeight = state.heights[nearestIndex];
            }
        }
        if (!readOnly && state.draggingRoadEdge != 0
            && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 center = screenPoint(selectedCenter);
            const float edgeDistance = std::clamp(
                std::hypot(mouse.x-center.x, mouse.y-center.y)
                    / std::max(state.zoom, 0.001f), 0.5f, 20.0f);
            if (state.draggingRoadEdge < 0) state.editLeftWidth = edgeDistance;
            else state.editRightWidth = edgeDistance;
            state.widths[state.selectedSection] =
                state.editLeftWidth + state.editRightWidth;
            state.edgeBiases[state.selectedSection] =
                (state.editLeftWidth-state.editRightWidth)*0.5f;
            state.dirty = true;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            state.draggingRoadEdge = 0;
        ImGui::EndChild();
        ImGui::End();
    }

    bool EditorLayer::LoadProjectVehicleData(ProjectPanelDefinition& panel)
    {
        try
        {
            const auto path = ProjectSettingsPath().parent_path().parent_path()
                / panel.dataPath;
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("車両パラメーターJSONを開けません");
            auto document = std::make_unique<nlohmann::json>(); input >> *document;
            if (!document->contains("vehicles") || !document->at("vehicles").is_array())
                throw std::runtime_error("vehicles配列がありません");
            m_projectVehicleEditor.document = std::move(document);
            m_projectVehicleEditor.selectedVehicle = 0;
            m_projectVehicleEditor.previewVehicle = -1;
            m_projectVehicleEditor.previewModel.reset();
            m_projectVehicleEditor.loaded = true;
            m_projectVehicleEditor.dirty = false;
            SetStatus("車両パラメーターを読み込みました");
            return true;
        }
        catch (const std::exception& exception)
        {
            m_projectVehicleEditor.loaded = false;
            SetStatus(std::string{"カーエディタを読み込めません: "}+exception.what(), true);
            return false;
        }
    }

    bool EditorLayer::SaveProjectVehicleData(ProjectPanelDefinition& panel)
    {
        try
        {
            const auto path = ProjectSettingsPath().parent_path().parent_path()
                / panel.dataPath;
            const auto temporary = path.wstring() + L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) throw std::runtime_error("一時ファイルを作成できません");
                output << m_projectVehicleEditor.document->dump(2) << '\n';
            }
            if (std::filesystem::exists(path))
                CopyFileW(path.c_str(), (path.wstring()+L".bak").c_str(), FALSE);
            if (!MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(temporary.c_str());
                throw std::runtime_error("車両JSONを置き換えられません");
            }
            m_projectVehicleEditor.dirty = false;
            if (!panel.saveCommand.command.empty()) LaunchProjectMenuCommand(panel.saveCommand);
            SetStatus("車両パラメーターを保存しました");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(std::string{"車両パラメーターを保存できません: "}+exception.what(), true);
            return false;
        }
    }

    bool EditorLayer::LoadProjectBgmData(ProjectPanelDefinition& panel)
    {
        try
        {
            const auto path = ProjectSettingsPath().parent_path().parent_path()
                / panel.dataPath;
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error("BGMカタログJSONを開けません");
            }
            auto document = std::make_unique<nlohmann::json>();
            input >> *document;
            if (!document->contains("tracks")
                || !document->at("tracks").is_array()
                || document->at("tracks").empty())
            {
                throw std::runtime_error("tracks配列がありません");
            }
            StopProjectBgmPreview();
            m_projectBgmEditor.document = std::move(document);
            m_projectBgmEditor.selectedTrack = 0;
            m_projectBgmEditor.waveformTrack = -1;
            m_projectBgmEditor.waveformPeaks.clear();
            m_projectBgmEditor.totalFrames = 0;
            m_projectBgmEditor.loaded = true;
            m_projectBgmEditor.dirty = false;
            SetStatus("BGMカタログを読み込みました");
            return true;
        }
        catch (const std::exception& exception)
        {
            m_projectBgmEditor.loaded = false;
            SetStatus(
                std::string{ "BGMループエディタを読み込めません: " }
                + exception.what(),
                true);
            return false;
        }
    }

    bool EditorLayer::SaveProjectBgmData(ProjectPanelDefinition& panel)
    {
        try
        {
            const auto path = ProjectSettingsPath().parent_path().parent_path()
                / panel.dataPath;
            const auto temporary = path.wstring() + L".tmp";
            {
                std::ofstream output(
                    temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw std::runtime_error("一時ファイルを作成できません");
                }
                output << m_projectBgmEditor.document->dump(2) << '\n';
            }
            if (std::filesystem::exists(path))
            {
                CopyFileW(
                    path.c_str(), (path.wstring() + L".bak").c_str(), FALSE);
            }
            if (!MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(temporary.c_str());
                throw std::runtime_error("BGMカタログを置き換えられません");
            }
            m_projectBgmEditor.dirty = false;
            if (!panel.saveCommand.command.empty())
            {
                LaunchProjectMenuCommand(panel.saveCommand);
            }
            SetStatus("BGMカタログを保存しました");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "BGMカタログを保存できません: " }
                + exception.what(),
                true);
            return false;
        }
    }

    void EditorLayer::BuildProjectBgmWaveform()
    {
        auto& state = m_projectBgmEditor;
        state.waveformTrack = state.selectedTrack;
        state.waveformPeaks.clear();
        state.totalFrames = 0;
        if (!state.loaded || !state.document)
        {
            return;
        }
        const auto& tracks = state.document->at("tracks");
        if (state.selectedTrack < 0
            || state.selectedTrack >= static_cast<int>(tracks.size()))
        {
            return;
        }
        try
        {
            const auto root =
                ProjectSettingsPath().parent_path().parent_path();
            const auto asset = root / PathFromUtf8(
                tracks.at(static_cast<std::size_t>(state.selectedTrack))
                    .value("asset", std::string{}));
            // 波形専用のstreamで読みます。試聴中のstreamで読むと
            // その場の再生が途切れます。
            auto probe = m_graphics.Audio().CreateStream(
                m_graphics.Assets(), asset);
            constexpr std::size_t buckets = 1200;
            state.waveformPeaks.assign(buckets, 0.0f);
            probe->ReadPeakEnvelope(state.waveformPeaks.data(), buckets);
            state.totalFrames = probe->TotalFrames();
            state.sampleRate = probe->SampleRate() > 0
                ? probe->SampleRate()
                : 44100;
        }
        catch (const std::exception& exception)
        {
            state.waveformPeaks.clear();
            SetStatus(
                std::string{ "波形を作れません: " } + exception.what(), true);
        }
    }

    void EditorLayer::StartProjectBgmPreview(
        const std::uint64_t fromFrame, const bool usePreviewRange)
    {
        auto& state = m_projectBgmEditor;
        if (!state.loaded || !state.document)
        {
            return;
        }
        auto& tracks = state.document->at("tracks");
        if (state.selectedTrack < 0
            || state.selectedTrack >= static_cast<int>(tracks.size()))
        {
            return;
        }
        try
        {
            if (!state.preview || state.previewTrack != state.selectedTrack)
            {
                const auto root =
                    ProjectSettingsPath().parent_path().parent_path();
                const auto asset = root / PathFromUtf8(
                    tracks.at(static_cast<std::size_t>(state.selectedTrack))
                        .value("asset", std::string{}));
                state.preview = m_graphics.Audio().CreateStream(
                    m_graphics.Assets(), asset);
                state.preview->SetLevelMeterEnabled(true);
                state.previewTrack = state.selectedTrack;
            }
            auto& track =
                tracks.at(static_cast<std::size_t>(state.selectedTrack));
            // 編集中の値をそのまま入れるので、保存しなくても継ぎ目を
            // 聞いて確かめられます。
            state.preview->SetLoop(true);
            if (usePreviewRange)
            {
                // ゲームの選曲画面と同じ鳴り方（試聴範囲を繰り返す）。
                state.preview->SetLoopRegionFrames(
                    track.value(
                        "preview_start_frame", std::uint64_t{ 0 }),
                    track.value("preview_end_frame", std::uint64_t{ 0 }),
                    0);
            }
            else
            {
                state.preview->SetLoopRegionFrames(
                    track.value("loop_start_frame", std::uint64_t{ 0 }),
                    track.value("loop_end_frame", std::uint64_t{ 0 }),
                    track.value(
                        "loop_crossfade_frames", std::uint64_t{ 0 }));
            }
            state.preview->SetStartFrame(fromFrame);
            state.preview->SetVolume(state.previewVolume);
            state.preview->Play();
            state.lastStartFrame = fromFrame;
            state.lastUsedPreviewRange = usePreviewRange;
            state.hasLastStart = true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "BGMを再生できません: " } + exception.what(),
                true);
        }
    }

    void EditorLayer::StopProjectBgmPreview()
    {
        auto& state = m_projectBgmEditor;
        if (state.preview)
        {
            state.preview->Stop();
        }
        state.preview.reset();
        state.previewTrack = -1;
    }

    void EditorLayer::DrawProjectBgmPanel(
        const std::size_t panelIndex, ProjectPanelDefinition& panel)
    {
        auto& state = m_projectBgmEditor;
        if (state.panelIndex != panelIndex)
        {
            StopProjectBgmPreview();
            state = ProjectBgmEditorState{};
            state.panelIndex = panelIndex;
            LoadProjectBgmData(panel);
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 940.0f, 620.0f }, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                panel.title.c_str(), &panel.open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        if (!state.loaded || !state.document)
        {
            ImGui::TextUnformatted("BGMカタログを読み込めませんでした。");
            if (ImGui::Button("再読み込み"))
            {
                LoadProjectBgmData(panel);
            }
            ImGui::End();
            return;
        }

        auto& tracks = state.document->at("tracks");
        std::vector<const char*> labels;
        labels.reserve(tracks.size());
        for (auto& entry : tracks)
        {
            labels.push_back(
                entry.at("name").get_ref<std::string&>().c_str());
        }
        const int previousTrack = state.selectedTrack;
        ImGui::SetNextItemWidth(320.0f);
        ImGui::Combo(
            "曲", &state.selectedTrack, labels.data(),
            static_cast<int>(labels.size()));
        if (state.selectedTrack != previousTrack)
        {
            StopProjectBgmPreview();
            // 別の曲の位置を引きずらない。
            state.hasLastStart = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("保存"))
        {
            SaveProjectBgmData(panel);
        }
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            // documentごと差し替わるので、この後のtracks参照は無効に
            // なります。この行より先はこのフレームでは描きません。
            LoadProjectBgmData(panel);
            ImGui::End();
            return;
        }
        if (state.dirty)
        {
            ImGui::SameLine();
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.72f, 0.2f, 1.0f }, "未保存");
        }
        ImGui::Separator();

        if (state.waveformTrack != state.selectedTrack)
        {
            BuildProjectBgmWaveform();
        }

        auto& track =
            tracks.at(static_cast<std::size_t>(state.selectedTrack));
        const auto rate = static_cast<double>(
            state.sampleRate > 0 ? state.sampleRate : 44100);
        const std::uint64_t totalFrames = state.totalFrames;
        auto loopStart = track.value("loop_start_frame", std::uint64_t{ 0 });
        auto loopEnd = track.value("loop_end_frame", std::uint64_t{ 0 });
        auto crossfade =
            track.value("loop_crossfade_frames", std::uint64_t{ 0 });
        auto introStart =
            track.value("intro_start_frame", std::uint64_t{ 0 });
        auto previewStart =
            track.value("preview_start_frame", std::uint64_t{ 0 });
        auto previewEnd =
            track.value("preview_end_frame", std::uint64_t{ 0 });

        auto clampFrame = [&](const std::uint64_t frame)
        {
            return totalFrames > 0 ? std::min(frame, totalFrames) : frame;
        };
        auto writeFrame = [&](const char* key, const std::uint64_t frame)
        {
            track[key] = clampFrame(frame);
            // 秒はカタログの読み物であると同時に検証にも使うので、
            // frameを変えたら必ず一緒に書き換えます。
            const std::string_view name{ key };
            if (name == "preview_start_frame"
                || name == "preview_end_frame"
                || name == "intro_start_frame")
            {
                track[std::string{ name.substr(0, name.size() - 5) }
                    + "seconds"] =
                    std::round(
                        static_cast<double>(clampFrame(frame)) / rate * 1000.0)
                    / 1000.0;
            }
            state.dirty = true;
        };

        // ---- 波形 ----
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 area{
            std::max(ImGui::GetContentRegionAvail().x, 240.0f), 170.0f };
        ImGui::InvisibleButton("##BgmWaveform", area);
        const bool waveformHovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(
            origin,
            ImVec2{ origin.x + area.x, origin.y + area.y },
            IM_COL32(13, 18, 15, 255));
        auto frameToX = [&](const std::uint64_t frame)
        {
            const double ratio = totalFrames > 0
                ? static_cast<double>(frame) / static_cast<double>(totalFrames)
                : 0.0;
            return origin.x
                + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) * area.x;
        };
        auto xToFrame = [&](const float x)
        {
            const double ratio = std::clamp(
                static_cast<double>(x - origin.x)
                    / static_cast<double>(area.x),
                0.0, 1.0);
            return static_cast<std::uint64_t>(
                ratio * static_cast<double>(totalFrames));
        };
        if (loopEnd > loopStart)
        {
            draw->AddRectFilled(
                ImVec2{ frameToX(loopStart), origin.y },
                ImVec2{ frameToX(loopEnd), origin.y + area.y },
                IM_COL32(38, 92, 50, 96));
        }
        if (introStart > 0)
        {
            // 序盤より前はレースでは鳴りません。暗く落として
            // 「ここは飛ばす」と一目で分かるようにします。
            draw->AddRectFilled(
                origin,
                ImVec2{ frameToX(introStart), origin.y + area.y },
                IM_COL32(0, 0, 0, 150));
        }
        if (previewEnd > previewStart)
        {
            // 試聴範囲は上端の帯で示します。ループ範囲と重なっても
            // どちらがどちらか分かるようにするためです。
            draw->AddRectFilled(
                ImVec2{ frameToX(previewStart), origin.y },
                ImVec2{ frameToX(previewEnd), origin.y + 16.0f },
                IM_COL32(150, 120, 30, 130));
        }
        const float center = origin.y + area.y * 0.5f;
        if (!state.waveformPeaks.empty())
        {
            const auto buckets = state.waveformPeaks.size();
            for (int x = 0; x < static_cast<int>(area.x); ++x)
            {
                const auto bucket = std::min(
                    buckets - 1,
                    static_cast<std::size_t>(
                        static_cast<double>(x) / area.x
                        * static_cast<double>(buckets)));
                const float peak =
                    state.waveformPeaks[bucket] * area.y * 0.46f;
                draw->AddLine(
                    ImVec2{ origin.x + static_cast<float>(x), center - peak },
                    ImVec2{ origin.x + static_cast<float>(x), center + peak },
                    IM_COL32(92, 200, 120, 190));
            }
        }
        else
        {
            draw->AddText(
                ImVec2{ origin.x + 10.0f, center - 8.0f },
                IM_COL32(160, 170, 165, 255),
                "波形を読み込んでいます…");
        }
        auto drawMarker = [&](const std::uint64_t frame,
                              const ImU32 color,
                              const char* label)
        {
            const float x = frameToX(frame);
            draw->AddLine(
                ImVec2{ x, origin.y },
                ImVec2{ x, origin.y + area.y },
                color, 2.0f);
            draw->AddText(ImVec2{ x + 4.0f, origin.y + 3.0f }, color, label);
        };
        drawMarker(introStart, IM_COL32(120, 200, 255, 255), "序盤");
        drawMarker(previewStart, IM_COL32(255, 214, 80, 255), "試聴始");
        drawMarker(previewEnd, IM_COL32(255, 180, 60, 255), "試聴終");
        drawMarker(loopStart, IM_COL32(120, 255, 140, 255), "LOOP IN");
        drawMarker(loopEnd, IM_COL32(255, 120, 120, 255), "LOOP OUT");
        const bool playing = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (playing)
        {
            const float x = frameToX(state.preview->PlaybackFrame());
            draw->AddLine(
                ImVec2{ x, origin.y },
                ImVec2{ x, origin.y + area.y },
                IM_COL32(255, 255, 255, 220), 1.5f);
        }
        if (waveformHovered && totalFrames > 0)
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                StartProjectBgmPreview(xToFrame(ImGui::GetIO().MousePos.x));
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                state.contextFrame = xToFrame(ImGui::GetIO().MousePos.x);
                ImGui::OpenPopup("##BgmWaveformMenu");
            }
        }
        if (ImGui::BeginPopup("##BgmWaveformMenu"))
        {
            ImGui::Text(
                "%.3f 秒",
                static_cast<double>(state.contextFrame) / rate);
            ImGui::Separator();
            if (ImGui::MenuItem("ここを序盤の開始にする"))
            {
                writeFrame("intro_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここをLOOP INにする"))
            {
                writeFrame("loop_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここをLOOP OUTにする"))
            {
                writeFrame("loop_end_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここを試聴の開始にする"))
            {
                writeFrame("preview_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここを試聴の終わりにする"))
            {
                writeFrame("preview_end_frame", state.contextFrame);
            }
            ImGui::EndPopup();
        }
        ImGui::TextDisabled(
            "左クリック: その位置から試聴 / 右クリック: この位置を"
            "序盤・LOOP IN・LOOP OUT・試聴の開始/終わりに設定");

        // ---- 数値 ----
        auto secondsField = [&](const char* label,
                                const char* key,
                                const std::uint64_t frame)
        {
            double seconds = static_cast<double>(frame) / rate;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputDouble(label, &seconds, 0.001, 0.5, "%.3f 秒"))
            {
                writeFrame(
                    key,
                    static_cast<std::uint64_t>(
                        std::max(seconds, 0.0) * rate + 0.5));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%llu frame",
                static_cast<unsigned long long>(frame));
        };
        secondsField(
            "序盤の開始（レースで鳴り始める位置）",
            "intro_start_frame", introStart);
        secondsField("ループ開始 (LOOP IN)", "loop_start_frame", loopStart);
        secondsField("ループ終了 (LOOP OUT)", "loop_end_frame", loopEnd);
        secondsField(
            "試聴の開始（セレクト画面）", "preview_start_frame", previewStart);
        secondsField(
            "試聴の終わり（ここで頭へ戻る）", "preview_end_frame", previewEnd);
        {
            double milliseconds =
                static_cast<double>(crossfade) / rate * 1000.0;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputDouble(
                    "継ぎ目クロスフェード", &milliseconds, 1.0, 10.0,
                    "%.0f ms"))
            {
                writeFrame(
                    "loop_crossfade_frames",
                    static_cast<std::uint64_t>(
                        std::max(milliseconds, 0.0) / 1000.0 * rate + 0.5));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%llu frame",
                static_cast<unsigned long long>(crossfade));
        }
        if (previewEnd <= previewStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "試聴の終わりは開始より後ろにしてください。");
        }
        else
        {
            ImGui::TextDisabled(
                "試聴の長さ %.1f 秒",
                static_cast<double>(previewEnd - previewStart) / rate);
        }
        if (introStart >= loopEnd && loopEnd > 0)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "序盤の開始は LOOP OUT より前にしてください。");
        }
        if (loopEnd <= loopStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "LOOP OUT は LOOP IN より後ろにしてください。");
        }
        else if (crossfade * 2 >= loopEnd - loopStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "クロスフェードがループ区間の半分を超えています。");
        }

        // ---- 音量補正 ----
        // 曲ごとのマスタリング差をならす値。音源は作り直さず、
        // 再生音量へ掛ける方式なので、ここは何度でも変えられる。
        if (track.contains("gain_db"))
        {
            auto gain = static_cast<float>(
                track.value("gain_db", 0.0));
            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::SliderFloat(
                    "音量補正", &gain, -12.0f, 9.0f, "%+.2f dB"))
            {
                track["gain_db"] =
                    std::round(static_cast<double>(gain) * 100.0) / 100.0;
                state.dirty = true;
                if (state.preview)
                {
                    // 鳴らしながら動かせるように、その場で反映する。
                    state.preview->SetVolume(std::clamp(
                        state.previewVolume * std::pow(10.0f, gain / 20.0f),
                        0.0f, 1.0f));
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "曲ごとの音量差をならす（音源は書き換えません）");
        }

        // ---- 「おまかせ」で流すコース ----
        // panel.dataのJSONに courses（id と表示名の一覧）があるときだけ
        // 出します。曲ごとに「このコースで流していい」印を付ける欄です。
        if (state.document->contains("courses")
            && state.document->at("courses").is_array()
            && ImGui::CollapsingHeader("おまかせで流すコース"))
        {
            ImGui::TextDisabled(
                "選曲の「おまかせ」でこの曲が候補になるコースです。"
                "1曲も指定が無いコースは全曲から選ばれます。");
            if (!track.contains("courses")
                || !track.at("courses").is_array())
            {
                track["courses"] = nlohmann::json::array();
            }
            auto& assigned = track.at("courses");
            const auto& courses = state.document->at("courses");
            int column = 0;
            for (const auto& course : courses)
            {
                const auto id = course.value("id", std::string{});
                if (id.empty())
                {
                    continue;
                }
                const auto label = course.value("name", id);
                bool enabled = false;
                for (const auto& value : assigned)
                {
                    if (value.is_string()
                        && value.get_ref<const std::string&>() == id)
                    {
                        enabled = true;
                        break;
                    }
                }
                if (column != 0)
                {
                    ImGui::SameLine(static_cast<float>(column) * 220.0f);
                }
                if (ImGui::Checkbox(
                        (label + "##bgmcourse-" + id).c_str(), &enabled))
                {
                    if (enabled)
                    {
                        assigned.push_back(id);
                    }
                    else
                    {
                        for (auto entry = assigned.begin();
                             entry != assigned.end();
                             ++entry)
                        {
                            if (entry->is_string()
                                && entry->get_ref<const std::string&>()
                                    == id)
                            {
                                assigned.erase(entry);
                                break;
                            }
                        }
                    }
                    state.dirty = true;
                }
                column = (column + 1) % 3;
            }
            if (assigned.empty())
            {
                ImGui::TextDisabled(
                    "（この曲はどのコースの「おまかせ」にも出てきません）");
            }
        }

        // ---- 試聴 ----
        ImGui::Separator();
        if (ImGui::Button("▶ 序盤から"))
        {
            // レースとまったく同じ鳴り方（序盤の開始→LOOP OUT→ループ）。
            StartProjectBgmPreview(introStart);
        }
        ImGui::SameLine();
        if (ImGui::Button("▶ 試聴範囲"))
        {
            // ゲームの選曲画面とまったく同じ鳴り方で確かめます。
            StartProjectBgmPreview(previewStart, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("▶ 継ぎ目を聞く"))
        {
            // LOOP OUTの3秒手前から鳴らすと、折り返しがそのまま来ます。
            const auto lead = static_cast<std::uint64_t>(rate * 3.0);
            StartProjectBgmPreview(loopEnd > lead ? loopEnd - lead : 0);
        }
        ImGui::SameLine();
        // 鳴っているかはこの行で取り直す。上のボタンで状態が変わって
        // いることがあるので、フレーム頭の値を使うと表示がずれる。
        const bool transportPlaying = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (transportPlaying)
        {
            if (ImGui::Button("■ 停止"))
            {
                // streamは残したまま止める。次の「▶ 再生」で読み直さず
                // すぐ鳴らせる。パネルを閉じるときだけ完全に捨てる。
                state.preview->Stop();
            }
        }
        else if (ImGui::Button("▶ 再生"))
        {
            // 直前に鳴らした位置から。まだ一度も鳴らしていなければ
            // 試聴範囲を鳴らす（このパネルで一番よく使う聞き方）。
            StartProjectBgmPreview(
                state.hasLastStart ? state.lastStartFrame : previewStart,
                state.hasLastStart ? state.lastUsedPreviewRange : true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat(
                "音量", &state.previewVolume, 0.0f, 1.0f, "%.2f")
            && state.preview)
        {
            state.preview->SetVolume(state.previewVolume);
        }

        // 上で求めたplayingは、この間にあるボタン（「■ 停止」や曲の
        // 切り替え）でstate.previewが捨てられていると嘘になる。
        // 破棄済みのstreamへPlaybackFrameを呼ぶとその場で落ちるので、
        // ここで必ず取り直す。
        const bool stillPlaying = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (stillPlaying)
        {
            const auto position = state.preview->PlaybackFrame();
            const double seconds = static_cast<double>(position) / rate;
            const double length =
                static_cast<double>(totalFrames) / rate;
            ImGui::Text(
                "再生位置 %d:%06.3f / %d:%06.3f    折り返し %llu 回",
                static_cast<int>(seconds) / 60,
                seconds - (static_cast<int>(seconds) / 60) * 60,
                static_cast<int>(length) / 60,
                length - (static_cast<int>(length) / 60) * 60,
                static_cast<unsigned long long>(
                    state.preview->CompletedLoopCount()));

            // エンジンの帯域レベルメーター。ゲーム内のBGMプレートと
            // 同じ値なので、ここで動きを確かめられます。
            std::array<float, AudioStreamVoice::LevelBandCount> bands{};
            state.preview->ReadLevelBands(bands.data(), bands.size());
            const ImVec2 meterOrigin = ImGui::GetCursorScreenPos();
            const float meterHeight = 34.0f;
            const float barWidth = 10.0f;
            ImGui::InvisibleButton(
                "##BgmMeter",
                ImVec2{ barWidth * bands.size() * 1.4f, meterHeight });
            for (std::size_t band = 0; band < bands.size(); ++band)
            {
                const float level = std::clamp(bands[band], 0.0f, 1.0f);
                const float x =
                    meterOrigin.x + static_cast<float>(band) * barWidth * 1.4f;
                const float top =
                    meterOrigin.y + meterHeight * (1.0f - level);
                draw->AddRectFilled(
                    ImVec2{ x, top },
                    ImVec2{ x + barWidth, meterOrigin.y + meterHeight },
                    IM_COL32(
                        static_cast<int>(60 + 160 * level),
                        static_cast<int>(140 + 100 * level),
                        90, 220));
            }
        }
        else
        {
            ImGui::TextDisabled(
                "停止中。波形をクリックするか、上のボタンで鳴らします。");
        }

        ImGui::End();
        if (!panel.open)
        {
            StopProjectBgmPreview();
        }
    }

    void EditorLayer::DrawProjectVehiclePanel(
        const std::size_t panelIndex, ProjectPanelDefinition& panel)
    {
        auto& state = m_projectVehicleEditor;
        if (state.panelIndex != panelIndex)
        {
            state = ProjectVehicleEditorState{};
            state.panelIndex = panelIndex;
            LoadProjectVehicleData(panel);
        }
        ImGui::SetNextWindowSize(ImVec2{760.0f, 680.0f}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(panel.title.c_str(), &panel.open, ImGuiWindowFlags_NoCollapse))
        { ImGui::End(); return; }
        if (!state.loaded || !state.document)
        {
            ImGui::TextUnformatted("車両データを読み込めませんでした。");
            if (ImGui::Button("再読み込み")) LoadProjectVehicleData(panel);
            ImGui::End(); return;
        }
        auto& vehicles = state.document->at("vehicles");
        std::vector<const char*> labels;
        for (auto& vehicle : vehicles)
            labels.push_back(vehicle.at("menu_name").get_ref<std::string&>().c_str());
        ImGui::SetNextItemWidth(300.0f);
        ImGui::Combo("車種", &state.selectedVehicle, labels.data(), static_cast<int>(labels.size()));
        ImGui::SameLine(); if (ImGui::Button("保存")) SaveProjectVehicleData(panel);
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            // LoadProjectVehicleData replaces the JSON document. References such as
            // `vehicles` and its label strings above become invalid immediately, so
            // do not continue drawing with them during this frame.
            LoadProjectVehicleData(panel);
            ImGui::End();
            return;
        }
        if (state.dirty) { ImGui::SameLine(); ImGui::TextColored(
            ImVec4{1.0f,0.72f,0.2f,1.0f}, "未保存"); }
        ImGui::Separator();
        auto& vehicle = vehicles.at(state.selectedVehicle);
        if (state.previewVehicle != state.selectedVehicle)
        {
            state.previewVehicle = state.selectedVehicle;
            state.previewModel.reset();
            try
            {
                const auto modelPath = PathFromUtf8(
                    vehicle.value("model_path", std::string{}));
                if (!modelPath.empty())
                    state.previewModel = m_graphics.Assets().CreateModelInstance(modelPath);
            }
            catch (const std::exception& exception)
            {
                SetStatus(std::string{"車両モデルを読み込めません: "}
                    + exception.what(), true);
            }
        }
        ImGui::Text("ID: %s   駆動方式: %s", vehicle.value("id", "").c_str(),
            vehicle.value("layout", "").c_str());
        auto drag = [&](const char* label, const char* key, float speed,
            float minimum, float maximum, const char* format)
        {
            float value = vehicle.at(key).get<float>();
            if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format))
            { vehicle[key] = value; state.dirty = true; }
        };
        if (ImGui::CollapsingHeader("車体寸法", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drag("全長", "length_m", .01f, .5f, 10.f, "%.3f m");
            drag("全幅", "width_m", .01f, .5f, 5.f, "%.3f m");
            drag("全高", "height_m", .01f, .2f, 5.f, "%.3f m");
            drag("ホイールベース", "wheelbase_m", .01f, .5f, 6.f, "%.3f m");
            drag("前トレッド", "front_track_m", .01f, .2f, 4.f, "%.3f m");
            drag("後トレッド", "rear_track_m", .01f, .2f, 4.f, "%.3f m");
        }
        if (ImGui::CollapsingHeader("エンジン・タイヤ", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drag("アイドル回転数", "idle_rpm", 10.f, 100.f, 5000.f, "%.0f rpm");
            drag("レッドゾーン", "redline_rpm", 25.f, 1000.f, 20000.f, "%.0f rpm");
            drag("前輪半径", "front_wheel_radius_m", .001f, .05f, 1.f, "%.3f m");
            drag("後輪半径", "rear_wheel_radius_m", .001f, .05f, 1.f, "%.3f m");
            drag("後輪幅", "rear_tyre_width_m", .001f, .05f, 1.f, "%.3f m");
        }
        if (ImGui::CollapsingHeader("走行性能", ImGuiTreeNodeFlags_DefaultOpen))
        {
            drag("加速倍率", "acceleration_scale", .005f, .1f, 3.f, "%.3f x");
            drag("制動倍率", "brake_scale", .005f, .1f, 3.f, "%.3f x");
            drag("グリップ旋回倍率", "grip_cornering_scale", .005f, .1f, 3.f, "%.3f x");
            drag("ドリフト旋回倍率", "drift_cornering_scale", .005f, .1f, 3.f, "%.3f x");
        }
        if (ImGui::CollapsingHeader("当たり判定", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto& extents = vehicle.at("collider_half_extents");
            auto& offset = vehicle.at("collider_offset");
            float e[3]{extents[0].get<float>(),extents[1].get<float>(),extents[2].get<float>()};
            float o[3]{offset[0].get<float>(),offset[1].get<float>(),offset[2].get<float>()};
            if (ImGui::DragFloat3("半サイズ X/Y/Z", e, .005f, .02f, 10.f, "%.3f m"))
            { for (int i=0;i<3;++i) extents[i]=e[i]; state.dirty=true; }
            if (ImGui::DragFloat3("中心オフセット X/Y/Z", o, .005f, -10.f, 10.f, "%.3f m"))
            { for (int i=0;i<3;++i) offset[i]=o[i]; state.dirty=true; }
            ImGui::TextWrapped("半サイズは中心から片側までの距離です。全体の大きさは X=%.2fm / Y=%.2fm / Z=%.2fm",
                e[0]*2.f,e[1]*2.f,e[2]*2.f);
            ImGui::TextUnformatted("上面と側面は共通縮尺（同じ1m=同じ画面上の長さ）です。");
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 area{ImGui::GetContentRegionAvail().x, 230.f};
            ImGui::InvisibleButton("##VehicleColliderPreview", area);
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, ImVec2{origin.x+area.x,origin.y+area.y}, IM_COL32(18,22,28,255));
            const float previewWidth = std::max((area.x-12.0f)*0.5f, 64.0f);
            const float previewHeight = std::min(
                std::max(area.y-30.0f, 64.0f), previewWidth/1.6f);
            const float previewAspect = previewWidth / previewHeight;
            float sharedWorldWidth = std::max({
                e[0]*2.4f,
                e[2]*2.4f,
                e[2]*2.4f*previewAspect,
                e[1]*2.4f*previewAspect,
                0.1f });
            float topOverlayScale = previewWidth/sharedWorldWidth;
            float sideOverlayScale = topOverlayScale;
            DirectX::XMFLOAT3 modelCenter{o[0],o[1],o[2]};
            if (state.previewModel && state.previewModel->hasLocalBounds)
            {
                constexpr std::uint32_t previewTargetWidth = 512;
                const auto previewTargetHeight = static_cast<std::uint32_t>(
                    std::max(1.0f, std::round(
                        static_cast<float>(previewTargetWidth)/previewAspect)));
                m_projectVehicleTopPreview.Resize(
                    m_graphics.Device(), previewTargetWidth, previewTargetHeight);
                m_projectVehicleSidePreview.Resize(
                    m_graphics.Device(), previewTargetWidth, previewTargetHeight);
                const auto& bounds = state.previewModel->localBounds;
                const DirectX::XMFLOAT3 center3{
                    (bounds.minimum.x+bounds.maximum.x)*0.5f,
                    (bounds.minimum.y+bounds.maximum.y)*0.5f,
                    (bounds.minimum.z+bounds.maximum.z)*0.5f};
                const float sizeX = std::max(bounds.maximum.x-bounds.minimum.x, 0.1f);
                const float sizeY = std::max(bounds.maximum.y-bounds.minimum.y, 0.1f);
                const float sizeZ = std::max(bounds.maximum.z-bounds.minimum.z, 0.1f);
                modelCenter = center3;
                const float distance = std::max({sizeX,sizeY,sizeZ})*3.0f+1.0f;
                const auto renderWireframe = [&](RenderTarget& target,
                    const DirectX::XMMATRIX& view,
                    const DirectX::XMMATRIX& projection)
                {
                    constexpr float clear[]{0.035f,0.045f,0.06f,1.0f};
                    target.Bind(m_graphics.Context());
                    target.Clear(m_graphics.Context(), clear);
                    const LitMaterial material{
                        DirectX::XMFLOAT4{0.15f,0.82f,1.0f,1.0f}, {}, {}, 0.8f};
                    if (state.previewModel->skeletalModel)
                    {
                        state.previewModel->skeletalModel->Draw(
                            m_graphics.Context(), m_graphics.States(),
                            m_graphics.Lighting(), DirectX::XMMatrixIdentity(),
                            view, projection, nullptr, 0.0f, true, &material);
                    }
                    else if (state.previewModel->model)
                    {
                        state.previewModel->model->UpdateEffects([](DirectX::IEffect* effect)
                        {
                            if (auto* basic = dynamic_cast<DirectX::BasicEffect*>(effect))
                            {
                                basic->SetTextureEnabled(false);
                                basic->SetDiffuseColor(
                                    DirectX::XMVectorSet(0.15f,0.82f,1.0f,1.0f));
                            }
                        });
                        state.previewModel->model->Draw(
                            m_graphics.Context(), m_graphics.States(),
                            DirectX::XMMatrixIdentity(), view, projection, true);
                    }
                    target.CopyToDisplay(m_graphics.Context());
                };
                const auto focus = DirectX::XMLoadFloat3(&center3);
                const float colliderSpanX =
                    2.0f*(std::abs(o[0]-center3.x)+e[0])*1.12f;
                const float colliderSpanY =
                    2.0f*(std::abs(o[1]-center3.y)+e[1])*1.12f;
                const float colliderSpanZ =
                    2.0f*(std::abs(o[2]-center3.z)+e[2])*1.12f;
                sharedWorldWidth = std::max({
                    sizeX*1.25f,
                    sizeZ*1.25f,
                    sizeZ*1.25f*previewAspect,
                    sizeY*1.35f*previewAspect,
                    colliderSpanX,
                    colliderSpanZ,
                    colliderSpanZ*previewAspect,
                    colliderSpanY*previewAspect,
                    0.1f });
                const float sharedWorldHeight = sharedWorldWidth/previewAspect;
                topOverlayScale = previewWidth/sharedWorldWidth;
                sideOverlayScale = topOverlayScale;
                renderWireframe(m_projectVehicleTopPreview,
                    DirectX::XMMatrixLookAtLH(
                    DirectX::XMVectorSet(center3.x,center3.y+distance,center3.z,1.0f),
                        focus, DirectX::XMVectorSet(0,0,1,0)),
                    DirectX::XMMatrixOrthographicLH(
                        sharedWorldWidth,sharedWorldHeight,0.01f,distance*2.0f));
                renderWireframe(m_projectVehicleSidePreview,
                    DirectX::XMMatrixLookAtLH(
                        DirectX::XMVectorSet(center3.x+distance,center3.y,center3.z,1.0f),
                        focus, DirectX::XMVectorSet(0,1,0,0)),
                    DirectX::XMMatrixOrthographicLH(
                        sharedWorldWidth,sharedWorldHeight,0.01f,distance*2.0f));
                draw->AddImage(MakeTextureReference(
                    m_projectVehicleTopPreview.DisplayShaderResourceView()),
                    ImVec2{origin.x,origin.y+25.0f},
                    ImVec2{origin.x+previewWidth,origin.y+25.0f+previewHeight});
                draw->AddImage(MakeTextureReference(
                    m_projectVehicleSidePreview.DisplayShaderResourceView()),
                    ImVec2{origin.x+area.x-previewWidth,origin.y+25.0f},
                    ImVec2{origin.x+area.x,origin.y+25.0f+previewHeight});
            }
            const ImVec2 center{
                origin.x+previewWidth*.5f+(o[0]-modelCenter.x)*topOverlayScale,
                origin.y+25.0f+previewHeight*.5f-(o[2]-modelCenter.z)*topOverlayScale};
            draw->AddRect(ImVec2{center.x-e[0]*topOverlayScale,center.y-e[2]*topOverlayScale},
                ImVec2{center.x+e[0]*topOverlayScale,center.y+e[2]*topOverlayScale}, IM_COL32(255,185,55,255),0,0,2.f);
            draw->AddText(ImVec2{origin.x+8,origin.y+7},IM_COL32_WHITE,"上面 (X/Z)");
            const ImVec2 side{
                origin.x+area.x-previewWidth*.5f+(o[2]-modelCenter.z)*sideOverlayScale,
                origin.y+25.0f+previewHeight*.5f-(o[1]-modelCenter.y)*sideOverlayScale};
            draw->AddRect(ImVec2{side.x-e[2]*sideOverlayScale,side.y-e[1]*sideOverlayScale},
                ImVec2{side.x+e[2]*sideOverlayScale,side.y+e[1]*sideOverlayScale},IM_COL32(255,185,55,255),0,0,2.f);
            draw->AddText(ImVec2{origin.x+area.x*.5f+8,origin.y+7},IM_COL32_WHITE,"側面 (Z/Y)");
            draw->AddText(ImVec2{origin.x+area.x*.5f-95.0f,origin.y+7},
                IM_COL32(90,215,255,255),"水色=実モデル形状");
            draw->AddText(ImVec2{origin.x+area.x-145.0f,origin.y+7},
                IM_COL32(255,185,55,255),"橙=当たり判定");
        }
        ImGui::End();
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
            // 区間で囲われていないGPU作業（Present待ちや、まだ
            // 計測を入れていない描画）。ここが大きいときは、
            // 犯人がまだ計測外にいるという意味です。
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
            ? L"新しいシーン.scene.json"
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

        std::wstring baseName =
            PathFromUtf8(selected->Name()).filename().wstring();
        for (auto& character : baseName)
        {
            if (character < L' '
                || std::wstring_view{ L"<>:\"/\\|?*" }.find(character)
                    != std::wstring_view::npos)
            {
                character = L'_';
            }
        }
        while (!baseName.empty()
            && (baseName.back() == L' ' || baseName.back() == L'.'))
        {
            baseName.pop_back();
        }
        if (baseName.empty())
        {
            baseName = L"NewPrefab";
        }

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
