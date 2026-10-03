#include "LamaPon/Scene/SceneManager.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Scene/EventBus.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/GameObject.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace LamaPon
{
    SceneManager::~SceneManager() noexcept
    {
        CancelPending();
        // 遷移の途中で破棄されても、Musicバスを下げたままにしません。
        ApplyMusicFade(1.0f);
        if (!m_asyncFuture.valid())
        {
            return;
        }
        try
        {
            // アセット管理を借用するワーカーをシーンの資源解放前に終了させます。
            static_cast<void>(m_asyncFuture.get());
        }
        catch (...)
        {
            // 破棄時はワーカーの終了を待ち、結果の例外を外へ送出しません。
        }
    }

    SceneManager::SceneManager(
        Scene& scene,
        GraphicsDevice& graphics) noexcept
        : m_scene(scene)
        , m_graphics(graphics)
    {
    }

    bool SceneManager::RequestLoad(
        std::filesystem::path scenePath)
    {
        if (IsLoading())
        {
            m_lastError =
                "An asynchronous scene load is already in progress.";
            return false;
        }
        if (scenePath.empty())
        {
            m_lastError =
                "Scene path is empty.";
            return false;
        }
        // 切り替えは追加読み込みの要求も無効にします（切り替え時に追加シーンごと破棄されるため）。
        m_pendingRequests.clear();
        m_pendingRequests.push_back(
            PendingRequest{
                std::move(scenePath),
                SceneLoadMode::Replace,
                false
            });
        m_lastError.clear();
        Logger::Instance().Info(
            "Scene load requested: "
            + PathToUtf8(
                m_pendingRequests.front().path));
        return true;
    }

    bool SceneManager::RequestLoadAdditive(
        std::filesystem::path scenePath)
    {
        if (IsLoading())
        {
            m_lastError =
                "An asynchronous scene load is already in progress.";
            return false;
        }
        if (scenePath.empty())
        {
            m_lastError =
                "Scene path is empty.";
            return false;
        }
        // 処理する解決済みシーンパス
        const auto destination =
            Resolve(scenePath).lexically_normal();
        if (m_scene.FindAdditiveScene(destination)
            != Scene::PrimarySceneHandle())
        {
            m_lastError =
                "そのシーンはすでに追加読み込みされています: "
                + PathToUtf8(destination);
            return false;
        }
        m_pendingRequests.push_back(
            PendingRequest{
                std::move(scenePath),
                SceneLoadMode::Additive,
                false
            });
        m_lastError.clear();
        Logger::Instance().Info(
            "追加シーンの読み込みを要求しました: "
            + PathToUtf8(destination));
        return true;
    }

    bool SceneManager::RequestUnload(
        std::filesystem::path scenePath)
    {
        if (scenePath.empty())
        {
            m_lastError =
                "Scene path is empty.";
            return false;
        }
        m_pendingRequests.push_back(
            PendingRequest{
                std::move(scenePath),
                SceneLoadMode::Additive,
                true
            });
        m_lastError.clear();
        return true;
    }

    bool SceneManager::RequestReload()
    {
        if (IsLoading())
        {
            m_lastError =
                "An asynchronous scene load is already in progress.";
            return false;
        }
        if (m_currentScene.empty())
        {
            m_lastError =
                "There is no current scene to reload.";
            return false;
        }
        m_pendingRequests.clear();
        m_pendingRequests.push_back(
            PendingRequest{
                m_currentScene,
                SceneLoadMode::Replace,
                false
            });
        m_lastError.clear();
        Logger::Instance().Info(
            "Scene reload requested: "
            + PathToUtf8(
                m_currentScene));
        return true;
    }

    bool SceneManager::RequestLoadAsync(
        std::filesystem::path scenePath)
    {
        return RequestLoadAsync(
            std::move(scenePath),
            m_defaultTransition);
    }

    bool SceneManager::RequestLoadAsync(
        std::filesystem::path scenePath,
        const SceneTransitionSettings& transition)
    {
        // 開始成功後だけ遷移を始め、借用中の既定設定もコピーして保持します。
        // 処理に使う遷移設定
        const auto settings = transition;
        if (!BeginAsyncLoad(
                std::move(scenePath),
                SceneLoadMode::Replace))
        {
            return false;
        }
        StartLoadTransition(settings, m_asyncDestination);
        return true;
    }

    bool SceneManager::RequestLoad(
        std::filesystem::path scenePath,
        const SceneTransitionSettings& transition)
    {
        // 処理に使う遷移設定
        const auto settings = transition;
        // 処理する解決済みシーンパス
        const auto destination =
            Resolve(scenePath).lexically_normal();
        if (!RequestLoad(std::move(scenePath)))
        {
            return false;
        }
        StartLoadTransition(settings, destination);
        return true;
    }

    bool SceneManager::RequestReload(
        const SceneTransitionSettings& transition)
    {
        // 処理に使う遷移設定
        const auto settings = transition;
        if (!RequestReload())
        {
            return false;
        }
        StartLoadTransition(settings, m_currentScene);
        return true;
    }

    bool SceneManager::RequestLoadAdditiveAsync(
        std::filesystem::path scenePath)
    {
        // 処理する解決済みシーンパス
        const auto destination =
            Resolve(scenePath).lexically_normal();
        if (m_scene.FindAdditiveScene(destination)
            != Scene::PrimarySceneHandle())
        {
            m_lastError =
                "そのシーンはすでに追加読み込みされています: "
                + PathToUtf8(destination);
            return false;
        }
        return BeginAsyncLoad(
            std::move(scenePath),
            SceneLoadMode::Additive);
    }

    bool SceneManager::BeginAsyncLoad(
        std::filesystem::path scenePath,
        const SceneLoadMode mode)
    {
        if (scenePath.empty())
        {
            m_lastError =
                "Scene path is empty.";
            return false;
        }
        if (IsLoading()
            || !m_pendingRequests.empty()
            || (m_asyncFuture.valid()
                && m_asyncFuture.wait_for(
                    std::chrono::seconds(0))
                    != std::future_status::ready))
        {
            m_lastError =
                "A scene load is already in progress.";
            return false;
        }

        m_asyncMode = mode;
        m_asyncDestination =
            Resolve(scenePath).lexically_normal();
        m_asyncStagedJson.reset();
        m_asyncShared =
            std::make_shared<AsyncLoadShared>();
        m_asyncShared->state.store(
            SceneLoadState::Queued);
        m_asyncShared->progress.store(0.02f);
        {
            // 共有する状態文の排他ロック
            std::scoped_lock lock(
                m_asyncShared->statusMutex);
            m_asyncShared->status =
                "Queued";
        }
        m_asyncRequestTime =
            std::chrono::steady_clock::now();
        m_displayedProgress = 0.0f;
        m_loadingScreenTime = 0.0f;
        m_lastError.clear();
        m_prefetchedAssetCount = 0;
        m_prefetchedAssetBytes = 0;
        m_prefetchFailureCount = 0;

        // 処理する解決済みシーンパス
        const auto destination =
            m_asyncDestination;
        // ワーカーと共有する読込状態
        const auto shared = m_asyncShared;
        // 借用するアセット管理
        AssetManager& assets = m_graphics.Assets();
        assets.ClearPrefetchedFiles();
        // シーンを検証して参照アセットを先読みし、有効化は呼び出し側へ残します。
        m_asyncFuture = std::async(
            std::launch::async,
            [destination, shared, &assets]()
                -> AsyncLoadResult
            {
                // 取消を確認して共有状態を更新します(state: 読込段階, progress: 読込進捗, status: 状態文)。
                const auto setStatus =
                    [&shared](
                        const SceneLoadState state,
                        const float progress,
                        std::string status)
                    {
                        if (shared->
                            cancelRequested.load())
                        {
                            shared->state.store(
                                SceneLoadState::Cancelled);
                            shared->progress.store(
                                0.0f);
                            return;
                        }
                        shared->state.store(state);
                        shared->progress.store(progress);
                        if (shared->
                            cancelRequested.load())
                        {
                            shared->state.store(
                                SceneLoadState::Cancelled);
                            shared->progress.store(
                                0.0f);
                            return;
                        }
                        // 共有する状態文の排他ロック
                        std::scoped_lock lock(
                            shared->statusMutex);
                        shared->status =
                            std::move(status);
                    };
                // 非同期読み込みの結果
                AsyncLoadResult result;
                try
                {
                    if (shared->cancelRequested.load())
                    {
                        result.cancelled = true;
                        return result;
                    }

                    setStatus(
                        SceneLoadState::Reading,
                        0.08f,
                        "Reading scene file");
                    if (!assets.FileExists(destination))
                    {
                        throw std::runtime_error(
                            "Could not open scene for reading: "
                            + LamaPon::PathToUtf8(destination));
                    }
                    // 読み込んだシーンファイルのバイト
                    const auto bytes =
                        assets.ReadFileBytes(destination);
                    result.json.assign(
                        bytes.begin(),
                        bytes.end());
                    shared->progress.store(0.7f);

                    if (shared->cancelRequested.load())
                    {
                        result.cancelled = true;
                        return result;
                    }
                    setStatus(
                        SceneLoadState::Parsing,
                        0.75f,
                        "Validating scene JSON");
                    // 形式更新して検証するJSON
                    auto document =
                        nlohmann::json::parse(
                            result.json);
                    static_cast<void>(
                        MigrateSerializedDocument(
                            document,
                            SerializedDocumentKind::Scene));
                    if (!document.contains(
                            "objects")
                        || !document.at(
                            "objects").is_array())
                    {
                        throw std::runtime_error(
                            "Unsupported or malformed LamaPon scene.");
                    }
                    result.json = document.dump();
                    if (shared->cancelRequested.load())
                    {
                        result.cancelled = true;
                        return result;
                    }

                    // 先読みする参照アセットのパス
                    const auto assetPaths =
                        CollectSerializedAssetPaths(
                            document);
                    setStatus(
                        SceneLoadState::Preloading,
                        0.8f,
                        "Preloading "
                            + std::to_string(
                                assetPaths.size())
                            + " asset files");
                    // 取消要求時にfalseを返します(completed: 完了件数, total: 総件数)。
                    // アセットの先読み結果
                    const auto prefetch =
                        assets.PrefetchFiles(
                            assetPaths,
                            [&shared](
                                const std::size_t completed,
                                const std::size_t total)
                            {
                                if (shared->
                                    cancelRequested.load())
                                {
                                    return false;
                                }
                                // 先読みファイルの完了割合
                                const float fraction =
                                    total == 0
                                        ? 1.0f
                                        : static_cast<float>(
                                            completed)
                                            / static_cast<float>(
                                                total);
                                shared->progress.store(
                                    0.8f
                                    + fraction * 0.14f);
                                return true;
                            });
                    result.prefetchedAssets =
                        prefetch.loadedFiles
                        + prefetch.cachedFiles;
                    result.prefetchedBytes =
                        prefetch.loadedBytes;
                    result.prefetchFailures =
                        prefetch.failedFiles;
                    if (prefetch.cancelled)
                    {
                        result.cancelled = true;
                        return result;
                    }
                    setStatus(
                        SceneLoadState::ReadyToActivate,
                        0.94f,
                        "Ready to activate");
                }
                // 記録する読み込み失敗の例外
                catch (const std::exception& exception)
                {
                    result.error =
                        exception.what();
                }
                return result;
            });
        Logger::Instance().Info(
            mode == SceneLoadMode::Additive
                ? "追加シーンの非同期読み込みを要求しました: "
                    + PathToUtf8(m_asyncDestination)
                : "Asynchronous scene load requested: "
                    + PathToUtf8(m_asyncDestination));
        return true;
    }

    bool SceneManager::RequestReloadAsync()
    {
        return RequestReloadAsync(m_defaultTransition);
    }

    bool SceneManager::RequestReloadAsync(
        const SceneTransitionSettings& transition)
    {
        if (m_currentScene.empty())
        {
            m_lastError =
                "There is no current scene to reload.";
            return false;
        }
        return RequestLoadAsync(m_currentScene, transition);
    }

    void SceneManager::CancelPending() noexcept
    {
        m_pendingRequests.clear();
        m_asyncStagedJson.reset();
        // 読み込みを待っていた遷移は、今の覆い具合から開き直します。
        FinishTransitionLoad(true);
        if (m_asyncShared
            && IsLoading())
        {
            m_asyncShared->
                cancelRequested.store(true);
            m_asyncShared->state.store(
                SceneLoadState::Cancelled);
            m_asyncShared->progress.store(0.0f);
            try
            {
                // 共有する状態文の排他ロック
                std::scoped_lock lock(
                    m_asyncShared->statusMutex);
                m_asyncShared->status =
                    "Cancelled";
            }
            catch (...)
            {
            }
        }
    }

    bool SceneManager::IsLoading() const noexcept
    {
        switch (LoadState())
        {
        case SceneLoadState::Queued:
        case SceneLoadState::Reading:
        case SceneLoadState::Parsing:
        case SceneLoadState::Preloading:
        case SceneLoadState::ReadyToActivate:
        case SceneLoadState::Activating:
            return true;
        default:
            return false;
        }
    }

    float SceneManager::LoadProgress() const noexcept
    {
        return m_asyncShared
            ? std::clamp(
                m_asyncShared->progress.load(),
                0.0f,
                1.0f)
            : 0.0f;
    }

    SceneLoadState
        SceneManager::LoadState() const noexcept
    {
        return m_asyncShared
            ? m_asyncShared->state.load()
            : SceneLoadState::Idle;
    }

    std::string SceneManager::LoadStatus() const
    {
        if (!m_asyncShared)
        {
            return {};
        }
        // 共有する状態文の排他ロック
        std::scoped_lock lock(
            m_asyncShared->statusMutex);
        return m_asyncShared->status;
    }

    void SceneManager::SetMinimumLoadingScreenDuration(
        const float seconds) noexcept
    {
        m_minimumLoadingScreenDuration =
            std::clamp(seconds, 0.0f, 10.0f);
    }

    const std::filesystem::path&
        SceneManager::PendingScenePath() const noexcept
    {
        // 保留要求がない場合の空パス
        static const std::filesystem::path
            empty;
        if (!m_pendingRequests.empty())
        {
            return m_pendingRequests.front().path;
        }
        return IsLoading()
            ? m_asyncDestination
            : empty;
    }

    void SceneManager::SetCurrentScenePath(
        std::filesystem::path path)
    {
        m_currentScene =
            Resolve(path).lexically_normal();
    }

    bool SceneManager::ProcessPending()
    {
        // 遷移はtimeScaleの影響を受けない実時間で進めます（timeScaleを0にした一時停止メニューからの移動でも演出を止めないため）。
        AdvanceTransition(Time::UnscaledDeltaTime());

        if (m_asyncFuture.valid()
            && m_asyncFuture.wait_for(
                std::chrono::seconds(0))
                == std::future_status::ready)
        {
            // 非同期読み込みの結果
            auto result =
                m_asyncFuture.get();
            m_prefetchedAssetCount =
                result.prefetchedAssets;
            m_prefetchedAssetBytes =
                result.prefetchedBytes;
            m_prefetchFailureCount =
                result.prefetchFailures;
            if (result.cancelled
                || (m_asyncShared
                    && m_asyncShared->
                        cancelRequested.load()))
            {
                if (m_asyncShared)
                {
                    m_asyncShared->state.store(
                        SceneLoadState::Cancelled);
                    m_asyncShared->progress.store(
                        0.0f);
                }
                m_graphics.Assets().
                    ClearPrefetchedFiles();
                FinishTransitionLoad(true);
            }
            else if (!result.error.empty())
            {
                m_lastError =
                    std::move(result.error);
                if (m_asyncShared)
                {
                    m_asyncShared->state.store(
                        SceneLoadState::Failed);
                    // 共有する状態文の排他ロック
                    std::scoped_lock lock(
                        m_asyncShared->statusMutex);
                    m_asyncShared->status =
                        m_lastError;
                }
                Logger::Instance().Error(
                    "Asynchronous scene load failed: "
                    + PathToUtf8(
                        m_asyncDestination)
                    + " | "
                    + m_lastError);
                m_graphics.Assets().
                    ClearPrefetchedFiles();
                // 失敗しても元のシーンは残っているので、覆いを開いて元の画面へ戻します。
                FinishTransitionLoad(true);
            }
            else
            {
                m_asyncStagedJson =
                    std::move(result.json);
                if (m_prefetchFailureCount > 0)
                {
                    Logger::Instance().Warning(
                        "Scene prefetch could not read "
                        + std::to_string(
                            m_prefetchFailureCount)
                        + " optional asset files; "
                          "activation will resolve them normally.");
                }
            }
        }

        // 保持対象を引き継いで切り替え、失敗時に復元を試みます(destination: 移動先パス, loadScene: シーンを読み込む処理)。
        const auto activate =
            [this](
                const std::filesystem::path&
                    destination,
                const auto& loadScene)
            {
                // 復元に使う変更前のシーンJSON
                std::string rollbackScene;
                // シーンをまたいで保持する対象
                Scene::PersistentTransfer
                    persistentObjects;
                try
                {
                    rollbackScene =
                        m_scene.SerializeToJson();
                    persistentObjects =
                        m_scene.ExtractPersistentObjects();
                    loadScene();
                    m_scene.MergePersistentObjects(
                        std::move(
                            persistentObjects));
                    m_currentScene =
                        destination;
                    m_lastError.clear();
                    ++m_loadRevision;
                    return true;
                }
                // 記録する切り替え失敗の例外
                catch (const std::exception&
                    exception)
                {
                    m_lastError =
                        exception.what();
                    if (!rollbackScene.empty())
                    {
                        try
                        {
                            m_scene.LoadFromJson(
                                rollbackScene);
                            m_scene.
                                MergePersistentObjects(
                                    std::move(
                                        persistentObjects));
                        }
                        // 復元にも失敗した場合の例外
                        catch (const std::exception&
                            rollbackException)
                        {
                            m_lastError +=
                                " | Scene rollback failed: ";
                            m_lastError +=
                                rollbackException.what();
                        }
                    }
                    Logger::Instance().Error(
                        "Scene load failed: "
                        + PathToUtf8(
                            destination)
                        + " | "
                        + m_lastError);
                    return false;
                }
            };

        if (m_asyncStagedJson)
        {
            // 読み込み要求からの経過秒数
            const auto elapsed =
                std::chrono::duration<float>(
                    std::chrono::steady_clock::now()
                    - m_asyncRequestTime).count();
            if (elapsed
                < m_minimumLoadingScreenDuration)
            {
                return false;
            }
            // 遷移演出が旧シーンを覆い終えるまで有効化を待ちます（読み込み自体は覆っている間に済ませています）。
            if (TransitionBlocksActivation())
            {
                return false;
            }
            if (m_asyncShared)
            {
                m_asyncShared->state.store(
                    SceneLoadState::Activating);
                m_asyncShared->progress.store(
                    0.97f);
                // 共有する状態文の排他ロック
                std::scoped_lock lock(
                    m_asyncShared->statusMutex);
                m_asyncShared->status =
                    "Activating scene";
            }
            // 有効化する検証済みJSON
            auto staged =
                std::move(*m_asyncStagedJson);
            m_asyncStagedJson.reset();
            // 処理する解決済みシーンパス
            const auto destination =
                m_asyncDestination;
            // 追加読み込みの失敗はMergeFromJsonが追加途中の対象を取り消します。
            // 切り替え時は検証済みJSONをシーンへ適用します。
            // シーンの有効化に成功した状態
            const bool loaded =
                m_asyncMode == SceneLoadMode::Additive
                    ? MergeStagedScene(
                        destination,
                        staged)
                    : activate(
                        destination,
                        [this, &staged]()
                        {
                            m_scene.LoadFromJson(
                                staged);
                        });
            if (m_asyncShared)
            {
                m_asyncShared->state.store(
                    loaded
                        ? SceneLoadState::Succeeded
                        : SceneLoadState::Failed);
                m_asyncShared->progress.store(
                    loaded ? 1.0f : 0.0f);
                // 共有する状態文の排他ロック
                std::scoped_lock lock(
                    m_asyncShared->statusMutex);
                m_asyncShared->status =
                    loaded
                        ? "Scene loaded"
                        : m_lastError;
            }
            if (m_asyncMode == SceneLoadMode::Replace)
            {
                FinishTransitionLoad(false);
            }
            if (loaded)
            {
                Logger::Instance().Info(
                    m_asyncMode
                        == SceneLoadMode::Additive
                        ? "追加シーンを非同期で読み込みました: "
                            + PathToUtf8(destination)
                        : "Scene loaded asynchronously: "
                            + PathToUtf8(destination));
            }
            return loaded;
        }

        if (m_pendingRequests.empty())
        {
            return false;
        }
        // 覆いが終わるまでは後続の追加要求も順番を維持して待機します。
        if (TransitionBlocksActivation())
        {
            return false;
        }

        // 1フレームで複数の要求（ステージ2枚の追加など）を順番に処理します。
        // 今回処理する保留要求の一覧
        auto requests =
            std::move(m_pendingRequests);
        m_pendingRequests.clear();
        // いずれかの要求が成功した状態
        bool processed = false;
        // 順番に処理するシーン要求
        for (auto& request : requests)
        {
            // 処理する解決済みシーンパス
            const auto destination =
                Resolve(request.path).
                    lexically_normal();
            if (request.unload)
            {
                if (m_scene.UnloadScene(destination))
                {
                    ++m_loadRevision;
                    m_lastError.clear();
                    processed = true;
                }
                else
                {
                    m_lastError =
                        "追加シーンが見つかりません: "
                        + PathToUtf8(destination);
                    Logger::Instance().Warning(
                        m_lastError);
                }
                continue;
            }
            if (request.mode
                == SceneLoadMode::Additive)
            {
                processed =
                    MergeScene(destination)
                    || processed;
                continue;
            }
            // 指定ファイルから主シーンを読み込みます。
            processed = activate(
                destination,
                [this, &destination]()
                {
                    m_scene.LoadFromFile(
                        destination);
                })
                || processed;
            // 成功・失敗のどちらでも、覆いは通常どおり開きます（失敗時はロールバックした元のシーンが見えます）。
            FinishTransitionLoad(false);
        }
        return processed;
    }

    bool SceneManager::MergeScene(
        const std::filesystem::path& destination)
    {
        try
        {
            static_cast<void>(
                m_scene.MergeFromFile(destination));
            m_lastError.clear();
            ++m_loadRevision;
            return true;
        }
        // 記録する標準例外
        catch (const std::exception& exception)
        {
            m_lastError = exception.what();
            Logger::Instance().Error(
                "追加シーンの読み込みに失敗しました: "
                + PathToUtf8(destination)
                + " | "
                + m_lastError);
            return false;
        }
    }

    bool SceneManager::MergeStagedScene(
        const std::filesystem::path& destination,
        const std::string& json)
    {
        try
        {
            static_cast<void>(
                m_scene.MergeFromJson(
                    json,
                    destination));
            m_lastError.clear();
            ++m_loadRevision;
            return true;
        }
        // 記録する標準例外
        catch (const std::exception& exception)
        {
            m_lastError = exception.what();
            Logger::Instance().Error(
                "追加シーンの読み込みに失敗しました: "
                + PathToUtf8(destination)
                + " | "
                + m_lastError);
            return false;
        }
    }

    void SceneManager::SetDefaultTransition(
        const SceneTransitionSettings& settings)
    {
        m_defaultTransition =
            SanitizeSceneTransition(settings);
    }

    bool SceneManager::PlayTransition(
        const SceneTransitionSettings& transition)
    {
        if (m_transitionAwaitsLoad || IsLoading())
        {
            m_lastError =
                "A scene load with a transition is already in progress.";
            return false;
        }
        StartTransition(transition, {}, false);
        return true;
    }

    void SceneManager::ResetTransition() noexcept
    {
        m_transition.Reset();
        m_transitionAwaitsLoad = false;
        m_transitionTarget.clear();
        m_loadingScreenAlpha = 0.0f;
        m_loadingScreenTime = 0.0f;
        ApplyMusicFade(1.0f);
    }

    void SceneManager::StartLoadTransition(
        const SceneTransitionSettings& transition,
        const std::filesystem::path& destination)
    {
        StartTransition(
            transition,
            PathToUtf8(destination),
            true);
    }

    void SceneManager::StartTransition(
        const SceneTransitionSettings& transition,
        std::string target,
        const bool awaitsLoad)
    {
        m_transition.Start(transition);
        m_transitionTarget = std::move(target);
        m_transitionAwaitsLoad = awaitsLoad;
        PublishTransitionEvent(SceneTransitionStartedEvent);
    }

    void SceneManager::FinishTransitionLoad(
        const bool revealImmediately) noexcept
    {
        if (!m_transitionAwaitsLoad)
        {
            return;
        }
        m_transitionAwaitsLoad = false;
        if (revealImmediately)
        {
            m_transition.Reveal();
        }
    }

    bool SceneManager::TransitionBlocksActivation() const noexcept
    {
        return m_transitionAwaitsLoad
            && !m_transition.IsFullyCovered();
    }

    bool SceneManager::IsInputBlocked() const noexcept
    {
        return m_transition.IsActive()
            && m_transition.Settings().blockInput;
    }

    void SceneManager::AdvanceTransition(
        const float unscaledDeltaSeconds)
    {
        // 有効化で重いフレームがあっても、開く演出が一瞬で終わらないよう1回に進める時間を抑えます。
        // 上限を設けた実時間の経過秒数
        const float delta = std::clamp(
            std::isfinite(unscaledDeltaSeconds)
                ? unscaledDeltaSeconds
                : 0.0f,
            0.0f,
            1.0f / 30.0f);
        // 非同期読み込みが進行中の状態
        const bool loading = IsLoading();

        // 表示が追従する読み込み進捗
        const float targetProgress = loading
            ? LoadProgress()
            : LoadState() == SceneLoadState::Succeeded
                ? 1.0f
                : m_displayedProgress;
        if (!m_loadingScreen.smoothProgress)
        {
            m_displayedProgress = targetProgress;
        }
        else
        {
            // 進捗を滑らかに追従する補間率
            const float blend = 1.0f - std::exp(-delta * 10.0f);
            m_displayedProgress = std::max(
                m_displayedProgress,
                m_displayedProgress
                    + (targetProgress - m_displayedProgress) * blend);
        }
        m_loadingScreenTime = loading || m_loadingScreenAlpha > 0.0f
            ? m_loadingScreenTime + delta
            : 0.0f;

        if (!m_transition.IsActive())
        {
            m_loadingScreenAlpha = 0.0f;
            ApplyMusicFade(1.0f);
            return;
        }

        // 処理に使う遷移設定
        const auto& settings = m_transition.Settings();
        // 時間のある遷移は覆いを使い、即時遷移は標準読み込み画面を使います。
        // 時間のある覆い演出の適用状態
        const bool covering = !IsInstantSceneTransition(settings);
        if (covering)
        {
            // 覆い終えた後も読み込みが続く間だけ標準画面を重ねます。
            // 標準読み込み画面の表示条件
            const bool wantsLoadingScreen =
                m_transition.IsFullyCovered()
                && loading
                && settings.showLoadingScreen
                && m_loadingScreen.enabled;
            // 標準画面のフェード所要秒数
            const float fade = m_loadingScreen.fadeDuration;
            // 今回の画面不透明度の変更量
            const float step = fade > 0.0f ? delta / fade : 1.0f;
            m_loadingScreenAlpha = wantsLoadingScreen
                ? std::min(m_loadingScreenAlpha + step, 1.0f)
                : std::max(m_loadingScreenAlpha - step, 0.0f);
        }
        else
        {
            m_loadingScreenAlpha = 0.0f;
        }

        // 読み込み画面を消し終えてから開き始めます。
        // 新シーンを開き始められる状態
        const bool ready =
            !m_transitionAwaitsLoad
            && (!covering || m_loadingScreenAlpha <= 0.0f);
        // 時間軸で今回発生した通知
        const auto events = m_transition.Advance(delta, ready);
        // 覆いに応じて音楽を減衰する状態
        const bool fadesMusic = covering && settings.fadeMusic;
        ApplyMusicFade(
            fadesMusic && m_transition.IsActive()
                ? 1.0f - m_transition.Coverage()
                : 1.0f);

        if (events.covered)
        {
            PublishTransitionEvent(SceneTransitionCoveredEvent);
        }
        if (events.finished)
        {
            PublishTransitionEvent(SceneTransitionFinishedEvent);
        }
        if (events.finished && !m_transition.IsActive())
        {
            m_transitionTarget.clear();
        }
    }

    SceneTransitionFrame SceneManager::TransitionFrame() const
    {
        // 描画用の遷移と標準画面の状態
        SceneTransitionFrame frame;
        frame.settings = m_transition.Settings();
        frame.phase = m_transition.Phase();
        frame.coverage = m_transition.Coverage();
        frame.loadingProgress = m_loadingScreen.smoothProgress
            ? m_displayedProgress
            : LoadProgress();
        frame.loadingScreenTime = m_loadingScreenTime;
        // 時間のある覆い演出の適用状態
        const bool covering =
            m_transition.IsActive()
            && !IsInstantSceneTransition(frame.settings);
        if (covering)
        {
            frame.loadingScreenAlpha = m_loadingScreenAlpha;
        }
        else
        {
            // 覆いの無い読み込みは、従来どおり読み込み中だけ即座に読み込み画面を表示します。
            frame.legacyLoadingScreen = true;
            frame.loadingScreenAlpha =
                IsLoading() && m_loadingScreen.enabled
                    ? 1.0f
                    : 0.0f;
        }
        return frame;
    }

    void SceneManager::PublishTransitionEvent(
        const std::string_view eventName)
    {
        // 移動先を含む遷移イベント引数
        EventArgs eventArgs;
        eventArgs.text = m_transitionTarget;
        try
        {
            m_scene.Events().Publish(eventName, eventArgs);
        }
        // 記録する標準例外
        catch (const std::exception& exception)
        {
            // 受信側の例外で遷移の進行を止めないよう、ここで記録します。
            Logger::Instance().Error(
                "シーン遷移イベントの処理中に例外が発生しました: "
                + std::string(eventName)
                + " | "
                + exception.what());
        }
    }

    void SceneManager::ApplyMusicFade(const float gain) noexcept
    {
        // 0〜1に制限した音楽減衰係数
        const float clamped = std::clamp(gain, 0.0f, 1.0f);
        if (std::abs(clamped - m_appliedMusicFade) < 1.0e-4f)
        {
            return;
        }
        // 音声が使えない環境（未初期化など）で毎フレーム例外を投げ直さないよう、失敗しても適用済みとして扱います。
        m_appliedMusicFade = clamped;
        try
        {
            m_graphics.Audio().SetBusFade(
                AudioBus::Music,
                clamped);
        }
        catch (...)
        {
        }
    }

    std::filesystem::path SceneManager::Resolve(
        const std::filesystem::path& path) const
    {
        if (path.empty() || path.is_absolute())
        {
            return path;
        }
        // 借用するアセット管理
        if (const auto* assets =
            m_graphics.TryAssets())
        {
            return assets->ResolvePath(path);
        }
        return path;
    }
}
