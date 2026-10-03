#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace LamaPon
{
    class OnlineServices;
    class PlayerPrefs;
    class SaveDataStore;
}

namespace LamaPon::Detail
{
    class CloudSaveJournal;
    class CloudSaveClient;
    class CloudSaveSynchronizer;

    enum class OnlinePersistenceDetachResult : std::uint8_t
    {
        AlreadyGuest,
        SavedAccount,
        QuarantinedAccount
    };

    enum class OnlinePersistenceRecoveryState : std::uint8_t
    {
        None,
        MemorySnapshot,
        DurableSidecar,
        UnavailableSidecar
    };

    struct OnlinePersistenceRecoverySnapshot final
    {
        // 現在の復旧状態
        OnlinePersistenceRecoveryState state{
            OnlinePersistenceRecoveryState::None
        };
        // 操作時に検証する世代・不在は0
        std::uint64_t revision{};
    };

    enum class OnlinePersistenceRecoveryOperationResult : std::uint8_t
    {
        Succeeded,
        Unavailable,
        Busy,
        Stale,
        Failed
    };

    enum class OnlinePersistenceRecoveryTestFailPoint : std::uint8_t
    {
        None,
        AfterSidecarPublishBeforeVerification
    };

    // 次回の復旧処理に一度だけ検証用の失敗を仕込む(failPoint: 再現する失敗段階)。
    void SetOnlinePersistenceRecoveryTestFailPoint(
        OnlinePersistenceRecoveryTestFailPoint failPoint) noexcept;

    // 切替前にI/Oと割当を終え、例外を出さないcommitへ渡す移動専用の準備状態。
    class PreparedOnlineAccount final
    {
    public:
        // cpp内で定義する切替準備の内部状態。
        struct State;

        // 空の切替準備を作る。
        PreparedOnlineAccount() noexcept;
        // 未使用の切替準備とそのロックを解放する。
        ~PreparedOnlineAccount();

        // 切替準備の所有権を移す。
        PreparedOnlineAccount(PreparedOnlineAccount&&) noexcept;
        // 既存の準備を解放して切替準備の所有権を移す。
        PreparedOnlineAccount& operator=(
            PreparedOnlineAccount&&) noexcept;

        // 切替準備の共有を禁止する。
        PreparedOnlineAccount(const PreparedOnlineAccount&) = delete;
        // 切替準備の共有を禁止する。
        PreparedOnlineAccount& operator=(
            const PreparedOnlineAccount&) = delete;

        // 切替準備を保持しているかを返す。
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        // 検証済みの切替状態を所有する(state: 切替準備の所有先)。
        explicit PreparedOnlineAccount(std::unique_ptr<State> state) noexcept;

        friend class OnlinePersistenceCoordinator;
        // 検証済み切替準備の所有先
        std::unique_ptr<State> m_state;
    };

    // 公開PlayerPrefs・SaveDataStoreのアドレスを保ってguestとaccountを切り替え、全操作を構築元のmain threadで行う。
    class OnlinePersistenceCoordinator final
    {
    public:
        // guestの保存先を借用して切替処理を作る(preferences: 存続する設定オブジェクト, saves: 存続する保存領域オブジェクト, trustedUserDataDirectory: 信頼済みUserDataの絶対パス)。
        OnlinePersistenceCoordinator(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::filesystem::path trustedUserDataDirectory);
        // guestへ戻し隔離中の保存を一度再試行して切替状態を解放する。
        ~OnlinePersistenceCoordinator();

        // 保存先切替の状態の共有を禁止する。
        OnlinePersistenceCoordinator(
            const OnlinePersistenceCoordinator&) = delete;
        // 保存先切替の状態の共有を禁止する。
        OnlinePersistenceCoordinator& operator=(
            const OnlinePersistenceCoordinator&) = delete;

        // 認証設定の公開前に保存領域と同期処理を準備し失敗時は旧領域を維持する(gameId: ゲーム固有のID, environmentId: 保存領域を分ける環境ID, normalizedBackendBaseUrl: 正規化済み認証サービスURL, allowInsecureLoopback: loopbackのHTTPを許可するか, cloudSaveClient: 同期用クライアントの共有所有先)。
        void ConfigureNamespace(
            std::string gameId,
            std::string environmentId,
            std::string normalizedBackendBaseUrl,
            bool allowInsecureLoopback,
            std::shared_ptr<const CloudSaveClient> cloudSaveClient = {});
        // 復旧待ちなら状態を保持し、それ以外はguestへ戻して保存領域を無効化する。
        void DisableNamespace() noexcept;

        // アカウント用の保存領域が設定済みかを返す。
        [[nodiscard]] bool IsNamespaceEnabled() const noexcept;
        // アカウントの保存先を使用中かを返す。
        [[nodiscard]] bool IsAccountActive() const noexcept;

        // guestの未保存設定を保存しアカウントの全保存文書とjournalを検証する(playerId: サービスの公開プレイヤーID, accessToken: 同期用token・同期なしなら空)。
        // 不正・取得不能な文書があれば例外を出しguestと公開sessionを維持する。
        [[nodiscard]] PreparedOnlineAccount PrepareAccount(
            std::string_view playerId,
            std::string accessToken = {});

        // 同じ所有者と世代の準備を適用し公開オブジェクトのアドレスを保つ(prepared: 消費する切替準備)。
        [[nodiscard]] bool CommitPrepared(
            PreparedOnlineAccount&& prepared) noexcept;

        // 未保存設定の保存を試して同じ呼出し内でguestへ戻し失敗分を隔離する。
        [[nodiscard]] OnlinePersistenceDetachResult
            DetachToGuest() noexcept;
        // ローカル変更の同期を進め隔離中の保存とcheckpointを再試行する。
        void EndFrame() noexcept;

        // 未保存設定のメモリを隔離して保持中かを返す。
        [[nodiscard]] bool HasQuarantinedAccount() const noexcept;
        // メモリまたは復旧ファイルの解決待ちがあるかを返す。
        [[nodiscard]] bool HasPendingRecovery() const noexcept;
        // 再読せず現在の復旧状態を返す。
        [[nodiscard]] OnlinePersistenceRecoveryState
            RecoveryState() const noexcept;
        // 復旧ファイルの変更を確認して現在の状態と操作世代を返す。
        [[nodiscard]] OnlinePersistenceRecoverySnapshot
            RecoveryStatus() noexcept;
        // 保持中の復旧ファイルのパスを借用し復旧待ちがなければ空を返す。
        [[nodiscard]] const std::filesystem::path&
            RecoverySidecarPath() const noexcept;

        // 現在の世代で保存を再試行し検証済み復旧ファイルを元の設定へ適用する。
        // 読込失敗した未保存設定は元の文書へ自動上書きせず、明示的復元でのみ反映する。
        [[nodiscard]] bool RestorePendingRecovery() noexcept;
        // 現在の世代の復旧状態を明示的に破棄する。
        [[nodiscard]] bool DiscardPendingRecovery() noexcept;
        // 指定世代の復旧だけを適用し変更済みならStaleを返す(expectedRevision: 表示時に取得した非0の世代)。
        [[nodiscard]] OnlinePersistenceRecoveryOperationResult
            RestorePendingRecovery(std::uint64_t expectedRevision) noexcept;
        // 指定世代で観測した復旧だけを破棄し変更済みならStaleを返す(expectedRevision: 表示時に取得した非0の世代)。
        [[nodiscard]] OnlinePersistenceRecoveryOperationResult
            DiscardPendingRecovery(std::uint64_t expectedRevision) noexcept;
        // 保存先の切替を識別する非0の世代を返す。
        [[nodiscard]] std::uint64_t ProfileEpoch() const noexcept;
        // 使用中の保存領域キーを切替まで借用しguestなら空を返す。
        [[nodiscard]] std::string_view
            ActiveAccountStorageKey() const noexcept;
        // 使用中のjournalを切替まで借用し未使用ならnullを返す。
        [[nodiscard]] CloudSaveJournal* Journal() noexcept;

        // OnlineServicesから更新tokenを受け取り旧応答を失効させる(accessToken: 新しい同期用token)。
        void UpdateCloudSaveAccessToken(std::string accessToken);
        // 未通知の同期認証失敗を一度だけ取り出す。
        [[nodiscard]] bool ConsumeCloudSaveUnauthorizedSignal() noexcept;
        // token更新後の検証済み通信成功を一度だけ取り出す。
        [[nodiscard]] bool ConsumeCloudSaveHealthySignal() noexcept;
        // 同期処理を再設定・破棄まで借用し未設定ならnullを返す。
        [[nodiscard]] CloudSaveSynchronizer* Synchronizer() noexcept;

        // ローカル変更または監視失敗を一度だけ取り出し保存領域の再走査を促す。
        [[nodiscard]] bool ConsumeLocalCommitSignal() noexcept;

    private:
        struct Implementation;
        // 保存先切替と復旧状態の所有先
        std::unique_ptr<Implementation> m_implementation;
    };

    // Applicationから保存先切替を接続するOnlineServices内部API。
    class OnlinePersistenceAccess final
    {
    public:
        // 認証開始前に保存先切替を接続する(services: 接続先, preferences: 存続する設定の借用, saves: 存続する保存領域の借用, trustedUserDataDirectory: 信頼済みUserDataの絶対パス)。
        static void Attach(
            OnlineServices& services,
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::filesystem::path trustedUserDataDirectory);
        // 競合IDを失効させてguestへ戻す(services: 切替対象)。
        [[nodiscard]] static OnlinePersistenceDetachResult Detach(
            OnlineServices& services) noexcept;
        // フレーム終端で隔離中の保存を再試行する(services: 再試行対象)。
        static void EndFrame(OnlineServices& services) noexcept;

        // 保存先切替処理を借用し未接続ならnullを返す(services: 参照対象)。
        [[nodiscard]] static OnlinePersistenceCoordinator*
            Coordinator(OnlineServices& services) noexcept;
    };
}
