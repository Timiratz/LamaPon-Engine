#pragma once

#include "LamaPon/Scene/RuntimeGameState.h"
#include "LamaPon/Scene/SceneTransition.h"

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
#include <string_view>
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

    // SceneLoadingScreenSettingsはSceneTransition.hで定義しています
    // （Project Settingsからも使うため）。

    // シーン遷移の各段階でSceneのEventBusへ発行するイベント名です。
    // EventArgs::textには移動先シーンのパス（UTF-8、シーンを切り替えない
    // 演出では空）が入ります。Script::Onで受信できます。
    //   Started  : 遷移を始めた（旧シーンで受け取れます）
    //   Covered  : 画面を覆い終えた（シーンを有効化する直前）
    //   Finished : 新シーンを見せ終えた（ここから操作を始めると安全です）
    inline constexpr std::string_view SceneTransitionStartedEvent =
        "SceneTransition.Started";
    inline constexpr std::string_view SceneTransitionCoveredEvent =
        "SceneTransition.Covered";
    inline constexpr std::string_view SceneTransitionFinishedEvent =
        "SceneTransition.Finished";

    // 1フレーム分の遷移演出と読み込み画面の描画内容です。
    // GraphicsDevice::DrawSceneTransitionへ渡します。
    struct SceneTransitionFrame final
    {
        SceneTransitionSettings settings;
        SceneTransitionPhase phase{ SceneTransitionPhase::Idle };
        // 0で何も覆わず、1で全面を覆います。
        float coverage{};
        // 読み込み画面を重ねる不透明度（0なら描きません）。
        float loadingScreenAlpha{};
        // 表示用に滑らかにした読み込みの進捗（0～1）。
        float loadingProgress{};
        // 読み込み画面を表示し続けている秒数（スピナー用）。
        float loadingScreenTime{};
        // trueなら従来の読み込み画面と同じく、フェードせずに描きます。
        bool legacyLoadingScreen{};
    };

    class GraphicsDevice;
    class Scene;

    class SceneManager final
    {
    public:
        SceneManager(
            Scene& scene,
            GraphicsDevice& graphics) noexcept;
        ~SceneManager() noexcept;

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

        // シーン遷移演出です。遷移はtimeScaleの影響を受けない実時間で
        // 進み、旧シーンを覆い終えてから新シーンを有効化します。
        // 読み込みは覆っている間に並行して進めます。
        //
        // 引数なしのRequestLoadAsync／RequestReloadAsync（UI Buttonと
        // 起動シーンを含む）はDefaultTransitionを使います。既定はNoneで、
        // 従来どおり読み込み画面だけを表示します。RequestLoad／
        // RequestReloadは、遷移を渡した場合だけ覆い終えるまで待ちます。
        // 追加読み込み（Additive）には遷移を使いません。
        void SetDefaultTransition(
            const SceneTransitionSettings& settings);
        [[nodiscard]] const SceneTransitionSettings&
            DefaultTransition() const noexcept
        {
            return m_defaultTransition;
        }
        [[nodiscard]] bool RequestLoad(
            std::filesystem::path scenePath,
            const SceneTransitionSettings& transition);
        [[nodiscard]] bool RequestReload(
            const SceneTransitionSettings& transition);
        [[nodiscard]] bool RequestLoadAsync(
            std::filesystem::path scenePath,
            const SceneTransitionSettings& transition);
        [[nodiscard]] bool RequestReloadAsync(
            const SceneTransitionSettings& transition);
        // シーンを切り替えずに画面を覆って開きます（部屋の移動や
        // ワープの演出）。覆い終えたときにSceneTransition.Coveredが
        // 届くので、その間にプレイヤーを移動します。シーンの読み込みを
        // 待っている遷移がある間はfalseを返します。
        [[nodiscard]] bool PlayTransition(
            const SceneTransitionSettings& transition);
        // PlayTransitionと同じ見た目を、イベントと音量変更なしで
        // 再生します（エディターのプレビュー用）。
        [[nodiscard]] bool PreviewTransition(
            const SceneTransitionSettings& transition);
        // 演出を打ち切り、覆いと音量を元へ戻します（再生停止時など）。
        void ResetTransition() noexcept;
        // 遷移を実時間で進めます。ProcessPendingが毎回呼ぶため、通常は
        // 呼ぶ必要はありません。Sceneを更新していない間（エディターの
        // 編集中のプレビューなど）に演出だけを進めるときに使います。
        void AdvanceTransition(float unscaledDeltaSeconds);
        [[nodiscard]] bool IsTransitioning() const noexcept
        {
            return m_transition.IsActive();
        }
        [[nodiscard]] SceneTransitionPhase
            TransitionPhase() const noexcept
        {
            return m_transition.Phase();
        }
        [[nodiscard]] float TransitionCoverage() const noexcept
        {
            return m_transition.Coverage();
        }
        [[nodiscard]] const SceneTransitionSettings&
            ActiveTransition() const noexcept
        {
            return m_transition.Settings();
        }
        // blockInputを指定した遷移の途中ならtrueです。UI Buttonは
        // この間クリックを受け付けません。
        [[nodiscard]] bool IsInputBlocked() const noexcept;
        [[nodiscard]] SceneTransitionFrame TransitionFrame() const;

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
        // 読み込みを伴う遷移を始めます（Noneでもイベントは発行します）。
        void StartLoadTransition(
            const SceneTransitionSettings& transition,
            const std::filesystem::path& destination);
        void StartTransition(
            const SceneTransitionSettings& transition,
            std::string target,
            bool awaitsLoad,
            bool preview);
        // 遷移が待っている読み込みが終わった（成功・失敗・キャンセル）
        // ことを記録します。覆い切る前なら、そこから開き直します。
        void FinishTransitionLoad(bool revealImmediately) noexcept;
        // 覆い終える前は、遷移に紐づく読み込みを有効化しません。
        [[nodiscard]] bool TransitionBlocksActivation() const noexcept;
        void PublishTransitionEvent(std::string_view eventName);
        void ApplyMusicFade(float gain) noexcept;

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
        // ここから下はシーン遷移演出の状態です。
        SceneTransitionSettings m_defaultTransition;
        SceneTransitionTimeline m_transition;
        std::string m_transitionTarget;
        // 遷移に紐づく読み込みがまだ有効化されていない間trueです。
        bool m_transitionAwaitsLoad{};
        bool m_transitionIsPreview{};
        float m_loadingScreenAlpha{};
        float m_displayedProgress{};
        float m_loadingScreenTime{};
        float m_appliedMusicFade{ 1.0f };
    };
}
