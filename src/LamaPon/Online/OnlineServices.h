#pragma once

#include "LamaPon/Core/Api.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class OnlineServices;

    struct OnlineServiceConfiguration final
    {
        // Discordのclient_secretをゲームへ設定してはいけません。
        // ここにはLamaPon用バックエンドのベースURLだけを指定します。
        std::string serviceBaseUrl;

        // 配布ビルドではfalseのまま使います。ローカル開発用の
        // 127.0.0.1/localhostバックエンドだけをHTTPで試す設定です。
        bool allowInsecureLoopback{};

        // 名前を変えても同じゲームと識別できる安定IDです。
        // 空の場合はログインできますが、次回起動用の
        // refresh tokenは端末へ保存しません。
        std::string gameId;

        // production/stagingなどの接続先を分離し、異なる環境で
        // 同じrefresh tokenが使われないようにします。
        std::string environmentId{ "production" };

        // ログイン開始後、認証URLを既定ブラウザーで開きます。
        // falseでもAuthorizationUrl()から手動で開けます。
        bool openAuthorizationBrowser{ true };
    };

    enum class OnlineAccountState : std::uint8_t
    {
        Unconfigured,
        SignedOut,
        StartingSignIn,
        WaitingForAuthorization,
        PollingAuthorization,
        SignedIn,
        SigningOut,
        Error,
        // 保存済みrefresh tokenで起動時セッションを復元中。
        RestoringSession,
        // 入力を止めずにセッションの期限を更新中。
        RefreshingSession
    };

    // ゲームへ公開してよいプロフィール情報だけを保持します。
    // Discord IDではなく、バックエンドが発行したplayerIdを
    // セーブデータの所有者として利用します。
    struct OnlinePlayerProfile final
    {
        std::string playerId;
        std::string displayName;
        std::string avatarUrl;
        std::string linkedProvider;
    };

    enum class OnlineCloudSyncState : std::uint8_t
    {
        Unavailable,
        Idle,
        Synchronizing,
        WaitingToRetry,
        Conflict,
        Unauthorized,
        Stopped
    };

    enum class OnlineCloudSyncStopReason : std::uint8_t
    {
        None,
        LocalUnavailable,
        LocalCorrupt,
        RemoteRejected,
        InvalidRemoteResponse,
        JournalFailure,
        InternalFailure
    };

    struct OnlineCloudSyncStatus final
    {
        OnlineCloudSyncState state{ OnlineCloudSyncState::Unavailable };
        OnlineCloudSyncStopReason stopReason{
            OnlineCloudSyncStopReason::None
        };
        // 絶対時刻を公開せず、照会時点からの残り秒だけを返します。
        float retryAfterSeconds{};
        std::size_t conflictCount{};
    };

    enum class OnlineCloudResourceKind : std::uint8_t
    {
        Preferences,
        SaveSlot
    };

    struct OnlineCloudConflict final
    {
        // process memory内だけで有効なCSPRNG IDです。ETag、mutation ID、
        // player ID、保存先pathなどの内部識別子は公開しません。
        std::string id;
        OnlineCloudResourceKind kind{
            OnlineCloudResourceKind::Preferences
        };
        std::string slot;
        bool localDeleted{};
        std::size_t localByteLength{};
        bool remoteDeleted{};
        std::size_t remoteByteLength{};
    };

    enum class OnlineCloudConflictResolution : std::uint8_t
    {
        UseLocal,
        UseRemote
    };

    enum class OnlinePersistenceRecoveryState : std::uint8_t
    {
        None,
        MemorySnapshot,
        DurableSidecar,
        UnavailableSidecar
    };

    struct OnlinePersistenceRecoveryStatus final
    {
        OnlinePersistenceRecoveryState state{
            OnlinePersistenceRecoveryState::None
        };
        // Noneでは0です。復旧対象の状態または観測byteが変わるたびに
        // 非0のrevisionへ進み、明示操作のstale UI入力を拒否します。
        std::uint64_t revision{};
    };

    enum class OnlinePersistenceOperationResult : std::uint8_t
    {
        Succeeded,
        Unavailable,
        Busy,
        Stale,
        Failed
    };

    namespace Detail
    {
        class OnlinePersistenceAccess;
        class OnlineServicesTestAccess;
    }

    // Discordログインの非同期進行とオンラインアカウント状態を
    // ゲームループ上で管理します。すべての公開メソッドは、
    // Applicationを動かす同じスレッドから呼んでください。
    class OnlineServices final
    {
    public:
        LAMAPON_API OnlineServices();
        explicit LAMAPON_API OnlineServices(
            OnlineServiceConfiguration configuration);
        LAMAPON_API ~OnlineServices();

        OnlineServices(const OnlineServices&) = delete;
        OnlineServices& operator=(const OnlineServices&) = delete;
        OnlineServices(OnlineServices&&) = delete;
        OnlineServices& operator=(OnlineServices&&) = delete;

        // 実行中のログインやサインアウトがある場合はlogic_errorです。
        // 保存済みrefresh tokenがあれば非同期復元を開始し、
        // 空URLを指定するとUnconfiguredへ戻ります。
        LAMAPON_API void Configure(
            OnlineServiceConfiguration configuration);

        // Applicationが毎フレーム呼び、完了した通信を反映して次の
        // ポーリングを開始します。通信自体はワーカースレッド上です。
        LAMAPON_API void Update(float elapsedSeconds);

        // 開始要求を受理したときだけtrueです。自動起動が
        // 無効または失敗した場合もAuthorizationUrl()を利用できます。
        [[nodiscard]] LAMAPON_API bool BeginDiscordSignIn();
        LAMAPON_API void CancelDiscordSignIn() noexcept;
        LAMAPON_API void SignOut();

        [[nodiscard]] LAMAPON_API OnlineAccountState
            State() const noexcept;
        [[nodiscard]] LAMAPON_API bool IsSignedIn() const noexcept;
        [[nodiscard]] LAMAPON_API const OnlinePlayerProfile&
            Player() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            AuthorizationUrl() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            LastErrorCode() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            LastError() const noexcept;

        [[nodiscard]] LAMAPON_API OnlineCloudSyncStatus
            CloudSyncStatus() const noexcept;
        [[nodiscard]] LAMAPON_API std::vector<OnlineCloudConflict>
            CloudConflicts() const;
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            RequestCloudSync() noexcept;
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            ResolveCloudConflict(
            std::string_view conflictId,
            OnlineCloudConflictResolution resolution) noexcept;

        [[nodiscard]] LAMAPON_API OnlinePersistenceRecoveryStatus
            PersistenceRecoveryStatus() const noexcept;
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            RestorePersistence(std::uint64_t expectedRevision) noexcept;
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            DiscardPersistence(std::uint64_t expectedRevision) noexcept;

    private:
        struct Implementation;

        explicit OnlineServices(
            std::unique_ptr<Implementation> implementation);

        friend class Detail::OnlineServicesTestAccess;
        friend class Detail::OnlinePersistenceAccess;

        std::unique_ptr<Implementation> m_implementation;
    };

    // Applicationが所有する現在のサービスです。CLIや単体テストでは
    // nullptrを許容します。実体はRuntime DLL側に1つだけ置きます。
    [[nodiscard]] LAMAPON_API OnlineServices*
        ActiveOnlineServices() noexcept;
    LAMAPON_API void SetActiveOnlineServices(
        OnlineServices* services) noexcept;
}
