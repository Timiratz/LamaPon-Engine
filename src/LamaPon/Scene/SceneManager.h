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
        // 読み込み未開始
        Idle,
        // ワーカーの開始待機
        Queued,
        // シーンファイルの読み込み
        Reading,
        // シーンJSONの検証
        Parsing,
        // 参照アセットの先読み
        Preloading,
        // 読み込み済みで有効化待ち
        ReadyToActivate,
        // シーンの有効化中
        Activating,
        // 非同期読み込みの成功
        Succeeded,
        // 非同期読み込みの失敗
        Failed,
        // 非同期読み込みの取り消し
        Cancelled
    };

    enum class SceneLoadMode
    {
        // 今のシーンを閉じて切り替えます（従来の読み込み）。
        Replace,
        // 今のシーンを残したまま、別シーンを足します。
        Additive
    };

    // 遷移イベントのEventArgs::textは移動先のUTF-8パスで、切り替えを伴わない演出では空です。
    // 旧シーンで受け取る遷移開始通知
    inline constexpr std::string_view SceneTransitionStartedEvent =
        "SceneTransition.Started";
    // 有効化直前に全面を覆った通知
    inline constexpr std::string_view SceneTransitionCoveredEvent =
        "SceneTransition.Covered";
    // 新シーンを開き終えた通知
    inline constexpr std::string_view SceneTransitionFinishedEvent =
        "SceneTransition.Finished";

    // 標準画面はDrawLoadingScreenへ渡し、覆いの演出はcoverageを使って呼び出し側で描きます。
    struct SceneTransitionFrame final
    {
        // 適用中の遷移設定
        SceneTransitionSettings settings;
        // 現在の遷移段階
        SceneTransitionPhase phase{ SceneTransitionPhase::Idle };
        // 0〜1の全面を覆う割合
        float coverage{};
        // 標準読み込み画面の不透明度
        float loadingScreenAlpha{};
        // 表示用の読み込み進捗率
        float loadingProgress{};
        // 標準画面を表示した秒数
        float loadingScreenTime{};
        // フェードなしの標準画面指定
        bool legacyLoadingScreen{};
    };

    class GraphicsDevice;
    class Scene;

    class SceneManager final
    {
    public:
        // シーンと描画機器を借用して管理を開始します(scene: 管理するシーン, graphics: 借用する描画機器)。
        SceneManager(
            Scene& scene,
            GraphicsDevice& graphics) noexcept;
        // 保留要求を取り消し、借用資源の破棄前に非同期ワーカーの終了を待って音楽音量を戻します。
        ~SceneManager() noexcept;

        // 他の保留要求を置き換えて切り替えを予約します(scenePath: 読み込むシーンのパス)。
        // 空のパスや非同期読み込み中の要求はfalseで拒否し、ProcessPendingで実行します。
        [[nodiscard]] bool RequestLoad(
            std::filesystem::path scenePath);
        // 他の保留要求を置き換えて主シーンの再読み込みを予約します。
        // 主シーン未設定または非同期読み込み中ならfalseを返します。
        [[nodiscard]] bool RequestReload();
        // 既定の遷移を使って非同期読み込みを開始します(scenePath: シーンのパス)。
        [[nodiscard]] bool RequestLoadAsync(
            std::filesystem::path scenePath);
        // 主シーンを既定の遷移で非同期に再読み込みします。
        [[nodiscard]] bool RequestReloadAsync();
        // 現在のシーンに追加する読み込みを予約します(scenePath: 追加するシーンのパス)。
        // 空のパス・読み込み済み・非同期読み込み中ならfalseを返し、追加読み込みに遷移は使いません。
        [[nodiscard]] bool RequestLoadAdditive(
            std::filesystem::path scenePath);
        // 現在のシーンに追加する非同期読み込みを開始します(scenePath: 追加するシーンのパス)。
        // 読み込み済みの追加シーンや保留要求がある場合はfalseを返します。
        [[nodiscard]] bool RequestLoadAdditiveAsync(
            std::filesystem::path scenePath);
        // 追加シーンの破棄を予約します(scenePath: 破棄する追加シーンのパス)。
        // 空のパスはfalseを返し、該当シーンの有無はProcessPendingで判定します。
        [[nodiscard]] bool RequestUnload(
            std::filesystem::path scenePath);
        // 保留要求を消去し非同期読み込みの取消を要求して、待機中の遷移を開き直します。
        void CancelPending() noexcept;

        // 未処理の要求または進行中の非同期読み込みがあるか返します。
        [[nodiscard]] bool HasPendingLoad() const noexcept
        {
            return !m_pendingRequests.empty()
                || IsLoading();
        }
        // 非同期読み込みが要求受付から有効化までの段階にあるか返します。
        [[nodiscard]] bool IsLoading() const noexcept;
        // 非同期読み込みの進捗を0〜1で返し、未開始なら0を返します。
        [[nodiscard]] float LoadProgress() const noexcept;
        // 非同期読み込みの段階を返し、未開始ならIdleを返します。
        [[nodiscard]] SceneLoadState
            LoadState() const noexcept;
        // 非同期ワーカーが更新した状態文をコピーで返します。
        [[nodiscard]] std::string
            LoadStatus() const;
        // 現在の主シーンの正規化したパスへの参照を返します。
        [[nodiscard]] const std::filesystem::path&
            CurrentScenePath() const noexcept
        {
            return m_currentScene;
        }
        // 最初の保留要求または読み込み中のパスを返し、どちらもなければ空のパスを返します。
        [[nodiscard]] const std::filesystem::path&
            PendingScenePath() const noexcept;
        // 最後に記録したエラー文への参照を返します。
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }
        // 読み込み・追加・破棄が成功するたびに増える改訂番号を返します。
        [[nodiscard]] std::uint64_t
            LoadRevision() const noexcept
        {
            return m_loadRevision;
        }
        // 直近の非同期読み込みで読み込んだ、またはキャッシュ済みのアセット数を返します。
        [[nodiscard]] std::size_t
            PrefetchedAssetCount() const noexcept
        {
            return m_prefetchedAssetCount;
        }
        // 直近の非同期読み込みで新たに先読みしたバイト数を返します。
        [[nodiscard]] std::size_t
            PrefetchedAssetBytes() const noexcept
        {
            return m_prefetchedAssetBytes;
        }
        // 直近の非同期読み込みで先読みに失敗したファイル数を返します。
        [[nodiscard]] std::size_t
            PrefetchFailureCount() const noexcept
        {
            return m_prefetchFailureCount;
        }
        // シーンをまたいで保持する実行時状態への参照を返します。
        [[nodiscard]] RuntimeGameState&
            State() noexcept
        {
            return m_state;
        }
        // 標準の読み込み画面の設定への参照を返します。
        [[nodiscard]] SceneLoadingScreenSettings&
            LoadingScreen() noexcept
        {
            return m_loadingScreen;
        }
        // 標準の読み込み画面の設定への読み取り参照を返します。
        [[nodiscard]] const SceneLoadingScreenSettings&
            LoadingScreen() const noexcept
        {
            return m_loadingScreen;
        }
        // 非同期有効化までの最短待機時間を0〜10秒へ制限して設定します(seconds: 有限な最短秒数)。
        void SetMinimumLoadingScreenDuration(
            float seconds) noexcept;
        // 非同期有効化までの最短待機秒数を返します。
        [[nodiscard]] float
            MinimumLoadingScreenDuration() const noexcept
        {
            return m_minimumLoadingScreenDuration;
        }
        // シーンをまたいで保持する実行時状態への読み取り参照を返します。
        [[nodiscard]] const RuntimeGameState&
            State() const noexcept
        {
            return m_state;
        }

        // 現在の主シーンのパスを解決して記録します(path: 主シーンのパス)。
        void SetCurrentScenePath(
            std::filesystem::path path);
        // 読み込みと同じ基準でパスを解決し正規化します(path: プロジェクト相対または絶対パス)。
        [[nodiscard]] std::filesystem::path
            ResolveScenePath(
                const std::filesystem::path& path) const
        {
            return Resolve(path).lexically_normal();
        }
        // 遷移を進め保留要求と有効化を処理し、読み込み・追加・破棄が成功した場合にtrueを返します。
        [[nodiscard]] bool ProcessPending();

        // 引数なしの非同期要求に使う設定を補正して保存します(settings: 既定の遷移設定)。
        void SetDefaultTransition(
            const SceneTransitionSettings& settings);
        // 非同期要求に使う既定の遷移設定への参照を返します。
        [[nodiscard]] const SceneTransitionSettings&
            DefaultTransition() const noexcept
        {
            return m_defaultTransition;
        }
        // 覆い終えてから切り替える要求を予約します(scenePath: シーンのパス, transition: 遷移設定)。
        [[nodiscard]] bool RequestLoad(
            std::filesystem::path scenePath,
            const SceneTransitionSettings& transition);
        // 主シーンを覆い終えてから再読み込みする要求を予約します(transition: 遷移設定)。
        [[nodiscard]] bool RequestReload(
            const SceneTransitionSettings& transition);
        // 非同期読み込みと遷移を開始します(scenePath: シーンのパス, transition: 遷移設定)。
        // 有効化はProcessPendingが覆いと最短読み込み表示時間の完了後に行います。
        [[nodiscard]] bool RequestLoadAsync(
            std::filesystem::path scenePath,
            const SceneTransitionSettings& transition);
        // 主シーンを指定した遷移で非同期に再読み込みします(transition: 遷移設定)。
        [[nodiscard]] bool RequestReloadAsync(
            const SceneTransitionSettings& transition);
        // シーンを切り替えずに覆いと開きを再生します(transition: 遷移設定)。
        // 非同期読み込み中や遷移が読み込みを待っている間はfalseを返します。
        [[nodiscard]] bool PlayTransition(
            const SceneTransitionSettings& transition);
        // 遷移を打ち切って覆いを消し、音楽音量を戻します。
        void ResetTransition() noexcept;
        // 遷移と標準画面を実時間で進めます(unscaledDeltaSeconds: 実時間の経過秒数)。
        // ProcessPendingが通常は呼び出し、1回の進行は0〜1/30秒に制限します。
        void AdvanceTransition(float unscaledDeltaSeconds);
        // 遷移の時間軸が待機状態以外か返します。
        [[nodiscard]] bool IsTransitioning() const noexcept
        {
            return m_transition.IsActive();
        }
        // 遷移の現在の段階を返します。
        [[nodiscard]] SceneTransitionPhase
            TransitionPhase() const noexcept
        {
            return m_transition.Phase();
        }
        // 曲線適用後の覆い具合を0〜1で返します。
        [[nodiscard]] float TransitionCoverage() const noexcept
        {
            return m_transition.Coverage();
        }
        // 進行中の遷移に適用した設定への参照を返します。
        [[nodiscard]] const SceneTransitionSettings&
            ActiveTransition() const noexcept
        {
            return m_transition.Settings();
        }
        // 入力制限を指定した遷移の進行中か返します。
        [[nodiscard]] bool IsInputBlocked() const noexcept;
        // 覆いと標準読み込み画面の描画に必要な現在の状態を返します。
        [[nodiscard]] SceneTransitionFrame TransitionFrame() const;

    private:
        struct AsyncLoadShared final
        {
            // ワーカーと共有する読込段階
            std::atomic<SceneLoadState>
                state{ SceneLoadState::Idle };
            // ワーカーと共有する読込進捗
            std::atomic<float> progress{};
            // ワーカーへの取り消し要求
            std::atomic<bool> cancelRequested{};
            // 状態文の排他制御
            mutable std::mutex statusMutex;
            // ワーカーが更新する状態文
            std::string status;
        };
        // ProcessPendingで順番に処理する要求です。
        struct PendingRequest final
        {
            // 処理するシーンのパス
            std::filesystem::path path;
            // 切り替えまたは追加の区分
            SceneLoadMode mode{
                SceneLoadMode::Replace
            };
            // 追加シーンの破棄要求
            bool unload{};
        };
        struct AsyncLoadResult final
        {
            // 形式更新と検証済みのJSON
            std::string json;
            // 読み込み失敗の説明
            std::string error;
            // 先読みとキャッシュ済みの件数
            std::size_t prefetchedAssets{};
            // 新たに先読みしたバイト数
            std::size_t prefetchedBytes{};
            // 先読みに失敗した件数
            std::size_t prefetchFailures{};
            // ワーカーが取り消された状態
            bool cancelled{};
        };

        // アセット管理の基準でパスを解決します(path: 解決するパス)。
        // 空・絶対パスやアセット管理の未初期化時は引数をそのまま返します。
        [[nodiscard]] std::filesystem::path
            Resolve(
                const std::filesystem::path& path) const;
        // ファイル読込・検証・アセット先読みをワーカーで開始します(scenePath: シーンのパス, mode: 切り替えか追加か)。
        // 空のパス・保留要求・進行中の読み込みがあればfalseを返します。
        [[nodiscard]] bool BeginAsyncLoad(
            std::filesystem::path scenePath,
            SceneLoadMode mode);
        // シーンを追加して成功時に改訂番号を増やします(destination: 解決済みのシーンパス)。
        bool MergeScene(
            const std::filesystem::path& destination);
        // 読込済みのシーンを追加して成功時に改訂番号を増やします(destination: 生成元パス, json: シーンのJSON)。
        bool MergeStagedScene(
            const std::filesystem::path& destination,
            const std::string& json);
        // 読み込みの有効化を待つ遷移を開始します(transition: 遷移設定, destination: 移動先のパス)。
        void StartLoadTransition(
            const SceneTransitionSettings& transition,
            const std::filesystem::path& destination);
        // 覆いの時間軸を開始し開始イベントを発行します(transition: 遷移設定, target: 移動先のUTF-8パス, awaitsLoad: 有効化を待つ指定)。
        void StartTransition(
            const SceneTransitionSettings& transition,
            std::string target,
            bool awaitsLoad);
        // 遷移の読み込み待ちを解除します(revealImmediately: 現在の覆いから直ちに開く指定)。
        void FinishTransitionLoad(bool revealImmediately) noexcept;
        // 読み込みに紐づく遷移が覆い終えておらず有効化を待つ必要があるか返します。
        [[nodiscard]] bool TransitionBlocksActivation() const noexcept;
        // 移動先を引数にして遷移イベントを発行し受信側の標準例外を記録します(eventName: 発行するイベント名)。
        void PublishTransitionEvent(std::string_view eventName);
        // 音楽バスの音量係数を0〜1へ制限して設定します(gain: 有限な減衰係数)。
        void ApplyMusicFade(float gain) noexcept;

        // 借用する管理対象のシーン
        Scene& m_scene;
        // 借用する描画機器
        GraphicsDevice& m_graphics;
        // 現在の主シーンの解決済みパス
        std::filesystem::path m_currentScene;
        // 順番に処理する読み込み要求
        std::vector<PendingRequest>
            m_pendingRequests;
        // 非同期読み込み先の解決済みパス
        std::filesystem::path m_asyncDestination;
        // 非同期読み込みの切り替え区分
        SceneLoadMode m_asyncMode{
            SceneLoadMode::Replace
        };
        // 非同期ワーカーとの共有状態
        std::shared_ptr<AsyncLoadShared>
            m_asyncShared;
        // 非同期読み込みの結果と終了待ち
        std::future<AsyncLoadResult>
            m_asyncFuture;
        // 有効化を待つ検証済みJSON
        std::optional<std::string>
            m_asyncStagedJson;
        // 非同期読み込みを要求した時刻
        std::chrono::steady_clock::time_point
            m_asyncRequestTime{};
        // 最後に記録したエラー文
        std::string m_lastError;
        // 読み込みや破棄の成功改訂番号
        std::uint64_t m_loadRevision{};
        // 先読みとキャッシュ済みの件数
        std::size_t m_prefetchedAssetCount{};
        // 新たに先読みしたバイト数
        std::size_t m_prefetchedAssetBytes{};
        // 先読みに失敗した件数
        std::size_t m_prefetchFailureCount{};
        // シーンをまたぐ実行時状態
        RuntimeGameState m_state;
        // 標準読み込み画面の設定
        SceneLoadingScreenSettings
            m_loadingScreen;
        // 非同期有効化までの最短秒数
        float m_minimumLoadingScreenDuration{
            0.2f
        };
        // 引数なし非同期要求の遷移設定
        SceneTransitionSettings m_defaultTransition;
        // 覆いと保持と開きの時間軸
        SceneTransitionTimeline m_transition;
        // 遷移先のUTF-8パス
        std::string m_transitionTarget;
        // 遷移に紐づく有効化の待機
        bool m_transitionAwaitsLoad{};
        // 標準画面の表示不透明度
        float m_loadingScreenAlpha{};
        // 滑らかに追従する表示進捗率
        float m_displayedProgress{};
        // 標準画面の表示経過秒数
        float m_loadingScreenTime{};
        // 適用を試みた音楽減衰係数
        float m_appliedMusicFade{ 1.0f };
    };
}
