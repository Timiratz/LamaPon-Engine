#pragma once

#include "LamaPon/Scene/RuntimeGameState.h"

#include <DirectXMath.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon
{
    enum class SceneLoadState
    {
        Idle,
        Queued,
        Reading,
        Parsing,
        Preloading,
        ReadyToActivate,
        Activating,
        Succeeded,
        Failed,
        Cancelled
    };

    enum class SceneLoadMode
    {
        // 今のシーンを閉じて切り替えます（従来の読み込み）。
        Replace,
        // 今のシーンを残したまま、別シーンを足します。
        Additive
    };

    struct SceneLoadingScreenSettings final
    {
        bool enabled{ true };
        std::string message{ "シーンを読み込んでいます..." };
        DirectX::XMFLOAT4 backgroundColor{
            0.02f, 0.03f, 0.06f, 0.94f
        };
        DirectX::XMFLOAT4 barBackgroundColor{
            0.12f, 0.16f, 0.24f, 1.0f
        };
        DirectX::XMFLOAT4 barFillColor{
            0.12f, 0.48f, 0.9f, 1.0f
        };
        DirectX::XMFLOAT4 textColor{
            0.92f, 0.96f, 1.0f, 1.0f
        };
        bool showPercentage{ true };
    };

    class GraphicsDevice;
    class Scene;

    class SceneManager final
    {
    public:
        SceneManager(
            Scene& scene,
            GraphicsDevice& graphics) noexcept;

        [[nodiscard]] bool RequestLoad(
            std::filesystem::path scenePath);
        [[nodiscard]] bool RequestReload();
        [[nodiscard]] bool RequestLoadAsync(
            std::filesystem::path scenePath);
        [[nodiscard]] bool RequestReloadAsync();
        // 追加読み込み。今のシーンを残したまま別シーンを足します。
        // 常駐UIやステージ分割に使います。破棄はRequestUnloadで、
        // 足したシーンのパスを渡します。
        [[nodiscard]] bool RequestLoadAdditive(
            std::filesystem::path scenePath);
        [[nodiscard]] bool RequestLoadAdditiveAsync(
            std::filesystem::path scenePath);
        [[nodiscard]] bool RequestUnload(
            std::filesystem::path scenePath);
        void CancelPending() noexcept;

        [[nodiscard]] bool HasPendingLoad() const noexcept
        {
            return !m_pendingRequests.empty()
                || IsLoading();
        }
        [[nodiscard]] bool IsLoading() const noexcept;
        [[nodiscard]] float LoadProgress() const noexcept;
        [[nodiscard]] SceneLoadState
            LoadState() const noexcept;
        [[nodiscard]] std::string
            LoadStatus() const;
        [[nodiscard]] const std::filesystem::path&
            CurrentScenePath() const noexcept
        {
            return m_currentScene;
        }
        [[nodiscard]] const std::filesystem::path&
            PendingScenePath() const noexcept;
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }
        [[nodiscard]] std::uint64_t
            LoadRevision() const noexcept
        {
            return m_loadRevision;
        }
        [[nodiscard]] std::size_t
            PrefetchedAssetCount() const noexcept
        {
            return m_prefetchedAssetCount;
        }
        [[nodiscard]] std::size_t
            PrefetchedAssetBytes() const noexcept
        {
            return m_prefetchedAssetBytes;
        }
        [[nodiscard]] std::size_t
            PrefetchFailureCount() const noexcept
        {
            return m_prefetchFailureCount;
        }
        [[nodiscard]] RuntimeGameState&
            State() noexcept
        {
            return m_state;
        }
        [[nodiscard]] SceneLoadingScreenSettings&
            LoadingScreen() noexcept
        {
            return m_loadingScreen;
        }
        [[nodiscard]] const SceneLoadingScreenSettings&
            LoadingScreen() const noexcept
        {
            return m_loadingScreen;
        }
        void SetMinimumLoadingScreenDuration(
            float seconds) noexcept;
        [[nodiscard]] float
            MinimumLoadingScreenDuration() const noexcept
        {
            return m_minimumLoadingScreenDuration;
        }
        [[nodiscard]] const RuntimeGameState&
            State() const noexcept
        {
            return m_state;
        }

        void SetCurrentScenePath(
            std::filesystem::path path);
        // プロジェクト相対パスを、読み込み時と同じ絶対パスへ
        // そろえます（追加シーンの重複判定用）。
        [[nodiscard]] std::filesystem::path
            ResolveScenePath(
                const std::filesystem::path& path) const
        {
            return Resolve(path).lexically_normal();
        }
        [[nodiscard]] bool ProcessPending();

    private:
        struct AsyncLoadShared final
        {
            std::atomic<SceneLoadState>
                state{ SceneLoadState::Idle };
            std::atomic<float> progress{};
            std::atomic<bool> cancelRequested{};
            mutable std::mutex statusMutex;
            std::string status;
        };
        // ProcessPendingで順番に処理する要求です。
        struct PendingRequest final
        {
            std::filesystem::path path;
            SceneLoadMode mode{
                SceneLoadMode::Replace
            };
            bool unload{};
        };
        struct AsyncLoadResult final
        {
            std::string json;
            std::string error;
            std::size_t prefetchedAssets{};
            std::size_t prefetchedBytes{};
            std::size_t prefetchFailures{};
            bool cancelled{};
        };

        [[nodiscard]] std::filesystem::path
            Resolve(
                const std::filesystem::path& path) const;
        // RequestLoadAsyncとRequestLoadAdditiveAsyncの共通処理。
        [[nodiscard]] bool BeginAsyncLoad(
            std::filesystem::path scenePath,
            SceneLoadMode mode);
        // 追加読み込みの実行（同期／非同期の受け皿）。
        bool MergeScene(
            const std::filesystem::path& destination);
        bool MergeStagedScene(
            const std::filesystem::path& destination,
            const std::string& json);

        Scene& m_scene;
        GraphicsDevice& m_graphics;
        std::filesystem::path m_currentScene;
        std::vector<PendingRequest>
            m_pendingRequests;
        std::filesystem::path m_asyncDestination;
        SceneLoadMode m_asyncMode{
            SceneLoadMode::Replace
        };
        std::shared_ptr<AsyncLoadShared>
            m_asyncShared;
        std::future<AsyncLoadResult>
            m_asyncFuture;
        std::optional<std::string>
            m_asyncStagedJson;
        std::chrono::steady_clock::time_point
            m_asyncRequestTime{};
        std::string m_lastError;
        std::uint64_t m_loadRevision{};
        std::size_t m_prefetchedAssetCount{};
        std::size_t m_prefetchedAssetBytes{};
        std::size_t m_prefetchFailureCount{};
        RuntimeGameState m_state;
        SceneLoadingScreenSettings
            m_loadingScreen;
        float m_minimumLoadingScreenDuration{
            0.2f
        };
    };
}
