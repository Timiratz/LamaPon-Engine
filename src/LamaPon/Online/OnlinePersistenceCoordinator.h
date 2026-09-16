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
        OnlinePersistenceRecoveryState state{
            OnlinePersistenceRecoveryState::None
        };
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

    void SetOnlinePersistenceRecoveryTestFailPoint(
        OnlinePersistenceRecoveryTestFailPoint failPoint) noexcept;

    // PrepareAccountでI/Oと割当をすべて終え、CommitPreparedの
    // noexcept切り替えまで運ぶmove-only transactionです。
    class PreparedOnlineAccount final
    {
    public:
        // 定義は.cppだけに置く内部transaction payloadです。
        struct State;

        PreparedOnlineAccount() noexcept;
        ~PreparedOnlineAccount();

        PreparedOnlineAccount(PreparedOnlineAccount&&) noexcept;
        PreparedOnlineAccount& operator=(
            PreparedOnlineAccount&&) noexcept;

        PreparedOnlineAccount(const PreparedOnlineAccount&) = delete;
        PreparedOnlineAccount& operator=(
            const PreparedOnlineAccount&) = delete;

        [[nodiscard]] bool IsValid() const noexcept;

    private:
        explicit PreparedOnlineAccount(std::unique_ptr<State> state) noexcept;

        friend class OnlinePersistenceCoordinator;
        std::unique_ptr<State> m_state;
    };

    // OnlineServicesとApplicationの間で、公開PlayerPrefs/SaveDataStore
    // objectを差し替えずにguest/accountの保存状態を切り替えます。
    // 全メソッドは構築したmain threadからだけ呼ぶ契約です。
    class OnlinePersistenceCoordinator final
    {
    public:
        OnlinePersistenceCoordinator(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::filesystem::path trustedUserDataDirectory);
        ~OnlinePersistenceCoordinator();

        OnlinePersistenceCoordinator(
            const OnlinePersistenceCoordinator&) = delete;
        OnlinePersistenceCoordinator& operator=(
            const OnlinePersistenceCoordinator&) = delete;

        // OnlineServices::Configureが新しいclientを完全に準備した後、
        // public configurationをpublishする前に呼びます。失敗時は
        // 以前のnamespaceを維持します。
        void ConfigureNamespace(
            std::string gameId,
            std::string environmentId,
            std::string normalizedBackendBaseUrl,
            bool allowInsecureLoopback,
            std::shared_ptr<const CloudSaveClient> cloudSaveClient = {});
        void DisableNamespace() noexcept;

        [[nodiscard]] bool IsNamespaceEnabled() const noexcept;
        [[nodiscard]] bool IsAccountActive() const noexcept;

        // guest dirty dataのdurable Save、account PlayerPrefsと全SaveDataの
        // strict検証、journal回復を完了します。どこかがUnavailable/
        // Corruptならthrowし、active guestと公開sessionは変更しません。
        [[nodiscard]] PreparedOnlineAccount PrepareAccount(
            std::string_view playerId,
            std::string accessToken = {});

        // preparedのepoch/ownerが現在値と一致するときだけ切り替えます。
        // 成功後もPlayerPrefs/SaveDataStore自体のaddressは変わりません。
        [[nodiscard]] bool CommitPrepared(
            PreparedOnlineAccount&& prepared) noexcept;

        // account dirty PlayerPrefsのSaveをまず試し、成功/失敗に関係なく
        // 同じ呼出し内でguestへ戻ります。失敗したaccount pimplは
        // EndFrameの再試行用にmemory quarantineします。
        [[nodiscard]] OnlinePersistenceDetachResult
            DetachToGuest() noexcept;
        void EndFrame() noexcept;

        [[nodiscard]] bool HasQuarantinedAccount() const noexcept;
        [[nodiscard]] bool HasPendingRecovery() const noexcept;
        [[nodiscard]] OnlinePersistenceRecoveryState
            RecoveryState() const noexcept;
        [[nodiscard]] OnlinePersistenceRecoverySnapshot
            RecoveryStatus() noexcept;
        [[nodiscard]] const std::filesystem::path&
            RecoverySidecarPath() const noexcept;

        // load failure中のdirty snapshotは元fileへ自動上書きしません。
        // 明示restoreだけがstrict検証済みsidecarを元accountへ適用し、
        // discardはsnapshotを明示的に破棄します。
        [[nodiscard]] bool RestorePendingRecovery() noexcept;
        [[nodiscard]] bool DiscardPendingRecovery() noexcept;
        [[nodiscard]] OnlinePersistenceRecoveryOperationResult
            RestorePendingRecovery(std::uint64_t expectedRevision) noexcept;
        [[nodiscard]] OnlinePersistenceRecoveryOperationResult
            DiscardPendingRecovery(std::uint64_t expectedRevision) noexcept;
        [[nodiscard]] std::uint64_t ProfileEpoch() const noexcept;
        [[nodiscard]] std::string_view
            ActiveAccountStorageKey() const noexcept;
        [[nodiscard]] CloudSaveJournal* Journal() noexcept;

        // OnlineServicesだけがtoken rotation/401 edgeを接続します。
        void UpdateCloudSaveAccessToken(std::string accessToken);
        [[nodiscard]] bool ConsumeCloudSaveUnauthorizedSignal() noexcept;
        [[nodiscard]] bool ConsumeCloudSaveHealthySignal() noexcept;
        [[nodiscard]] CloudSaveSynchronizer* Synchronizer() noexcept;

        // Stage 7Cがlocal reconcileを起動するためのedge signalです。
        // observer callback失敗もtrueとして返し、disk再走査を促します。
        [[nodiscard]] bool ConsumeLocalCommitSignal() noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_implementation;
    };

    // Applicationだけが使うOnlineServices private bridgeです。
    class OnlinePersistenceAccess final
    {
    public:
        static void Attach(
            OnlineServices& services,
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::filesystem::path trustedUserDataDirectory);
        [[nodiscard]] static OnlinePersistenceDetachResult Detach(
            OnlineServices& services) noexcept;
        static void EndFrame(OnlineServices& services) noexcept;

        [[nodiscard]] static OnlinePersistenceCoordinator*
            Coordinator(OnlineServices& services) noexcept;
    };
}
