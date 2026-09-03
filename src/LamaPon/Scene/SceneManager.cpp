#include "LamaPon/Scene/SceneManager.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/GameObject.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace LamaPon
{
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
        // 切り替えは追加読み込みの要求も無効にします（切り替え時に
        // 追加シーンごと破棄されるため）。
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
        return BeginAsyncLoad(
            std::move(scenePath),
            SceneLoadMode::Replace);
    }

    bool SceneManager::RequestLoadAdditiveAsync(
        std::filesystem::path scenePath)
    {
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
            std::scoped_lock lock(
                m_asyncShared->statusMutex);
            m_asyncShared->status =
                "Queued";
        }
        m_asyncRequestTime =
            std::chrono::steady_clock::now();
        m_lastError.clear();
        m_prefetchedAssetCount = 0;
        m_prefetchedAssetBytes = 0;
        m_prefetchFailureCount = 0;

        const auto destination =
            m_asyncDestination;
        const auto shared = m_asyncShared;
        AssetManager& assets = m_graphics.Assets();
        assets.ClearPrefetchedFiles();
        m_asyncFuture = std::async(
            std::launch::async,
            [destination, shared, &assets]()
                -> AsyncLoadResult
            {
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
                        std::scoped_lock lock(
                            shared->statusMutex);
                        shared->status =
                            std::move(status);
                    };
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
        if (m_currentScene.empty())
        {
            m_lastError =
                "There is no current scene to reload.";
            return false;
        }
        return RequestLoadAsync(m_currentScene);
    }

    void SceneManager::CancelPending() noexcept
    {
        m_pendingRequests.clear();
        m_asyncStagedJson.reset();
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
        if (m_asyncFuture.valid()
            && m_asyncFuture.wait_for(
                std::chrono::seconds(0))
                == std::future_status::ready)
        {
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
            }
            else if (!result.error.empty())
            {
                m_lastError =
                    std::move(result.error);
                if (m_asyncShared)
                {
                    m_asyncShared->state.store(
                        SceneLoadState::Failed);
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

        const auto activate =
            [this](
                const std::filesystem::path&
                    destination,
                const auto& loadScene)
            {
                std::string rollbackScene;
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
            const auto elapsed =
                std::chrono::duration<float>(
                    std::chrono::steady_clock::now()
                    - m_asyncRequestTime).count();
            if (elapsed
                < m_minimumLoadingScreenDuration)
            {
                return false;
            }
            if (m_asyncShared)
            {
                m_asyncShared->state.store(
                    SceneLoadState::Activating);
                m_asyncShared->progress.store(
                    0.97f);
                std::scoped_lock lock(
                    m_asyncShared->statusMutex);
                m_asyncShared->status =
                    "Activating scene";
            }
            auto staged =
                std::move(*m_asyncStagedJson);
            m_asyncStagedJson.reset();
            const auto destination =
                m_asyncDestination;
            // 追加読み込みは今のシーンを消さないので、退避と
            // ロールバックは不要です（失敗しても足しかけた分は
            // MergeFromJson側で取り消されます）。
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
                std::scoped_lock lock(
                    m_asyncShared->statusMutex);
                m_asyncShared->status =
                    loaded
                        ? "Scene loaded"
                        : m_lastError;
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

        // 1フレームで複数の要求（ステージ2枚の追加など）を
        // 順番に処理します。
        auto requests =
            std::move(m_pendingRequests);
        m_pendingRequests.clear();
        bool processed = false;
        for (auto& request : requests)
        {
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
            processed = activate(
                destination,
                [this, &destination]()
                {
                    m_scene.LoadFromFile(
                        destination);
                })
                || processed;
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

    std::filesystem::path SceneManager::Resolve(
        const std::filesystem::path& path) const
    {
        if (path.empty() || path.is_absolute())
        {
            return path;
        }
        if (const auto* assets =
            m_graphics.TryAssets())
        {
            return assets->ResolvePath(path);
        }
        return path;
    }
}
