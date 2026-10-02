#pragma once

#include "LamaPon/Core/Api.h"
#include "LamaPon/Online/DiscordPresence.h"

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
        // Discordのclient_secretを設定せず、LamaPon用バックエンドのURLだけを渡します。
        // LamaPon用バックエンドの基点URL
        std::string serviceBaseUrl;

        // 配布ビルドではfalseとし、ローカル開発でだけHTTPを許可します。
        // 開発用のローカルHTTPを許可するか
        bool allowInsecureLoopback{};

        // 空でも認証できますが、端末のrefresh token保存とクラウド同期は有効になりません。
        // 名前を変えても不変のゲームID
        std::string gameId;

        // 資格情報を分離する環境ID
        std::string environmentId{ "production" };

        // 認証URLをブラウザーで自動起動するか
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

    // バックエンドのplayerIdを保存データの所有者とし、ゲームに公開できるプロフィールだけを保持します。
    struct OnlinePlayerProfile final
    {
        // バックエンドが発行した所有者ID
        std::string playerId;
        // ゲームへ公開する表示名
        std::string displayName;
        // プロフィール画像のHTTPS URL
        std::string avatarUrl;
        // 連携した認証提供元の識別名
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
        // 現在のクラウド同期状態
        OnlineCloudSyncState state{ OnlineCloudSyncState::Unavailable };
        // 同期を停止した理由
        OnlineCloudSyncStopReason stopReason{
            OnlineCloudSyncStopReason::None
        };
        // 照会時点から再試行までの秒数
        float retryAfterSeconds{};
        // 明示解決が必要な競合数
        std::size_t conflictCount{};
    };

    enum class OnlineCloudResourceKind : std::uint8_t
    {
        Preferences,
        SaveSlot
    };

    struct OnlineCloudConflict final
    {
        // ETag・更新ID・所有者ID・内部保存先は公開しません。
        // process内だけで有効な公開用乱数ID
        std::string id;
        // 設定または保存スロットの種別
        OnlineCloudResourceKind kind{
            OnlineCloudResourceKind::Preferences
        };
        // 保存スロット名・設定なら空
        std::string slot;
        // ローカル更新が削除か
        bool localDeleted{};
        // ローカル内容のバイト数
        std::size_t localByteLength{};
        // リモートが削除済みか
        bool remoteDeleted{};
        // リモート内容のバイト数
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
        // 未解決の保存復旧状態
        OnlinePersistenceRecoveryState state{
            OnlinePersistenceRecoveryState::None
        };
        // 対象の状態や観測バイトが変わるたびに0以外へ進め、古いUIからの復旧・破棄を拒否します。
        // 復旧対象の識別版・対象なしは0
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

    // 全公開操作をApplicationと同じスレッドで呼び、非同期認証をゲームループ上で進めます。
    // DiscordPresenceは認証と独立し、ConfigureやSignOutの有無にかかわらず利用できます。
    class OnlineServices final
    {
    public:
        // 未設定のオンラインサービスを構築する。
        LAMAPON_API OnlineServices();
        // 設定を検証して保存済みtokenの復元を始める(configuration: バックエンドと認証の設定)。
        explicit LAMAPON_API OnlineServices(
            OnlineServiceConfiguration configuration);
        // 公開中のサービス参照を解除し非同期結果の後処理へ引き渡す。
        LAMAPON_API ~OnlineServices();

        // サービス状態の複製を禁止する。
        OnlineServices(const OnlineServices&) = delete;
        // サービス状態のコピー代入を禁止する。
        OnlineServices& operator=(const OnlineServices&) = delete;
        // サービス状態の移動を禁止する。
        OnlineServices(OnlineServices&&) = delete;
        // サービス状態の移動代入を禁止する。
        OnlineServices& operator=(OnlineServices&&) = delete;

        // 未進行時に設定を検証し保存済みtokenの非同期復元を始める(configuration: バックエンドと認証の設定)。
        // 認証・サインアウト・有効アカウント・未解決の保存復旧があれば拒否し、空URLは未設定へ戻します。
        LAMAPON_API void Configure(
            OnlineServiceConfiguration configuration);

        // 毎フレーム通信結果を反映し認証・Presenceの進行を更新する(elapsedSeconds: 前フレームからの経過秒数)。
        LAMAPON_API void Update(float elapsedSeconds);

        // Discord認証の開始要求を受理した場合だけtrueを返す。
        [[nodiscard]] LAMAPON_API bool BeginDiscordSignIn();
        // ログイン結果を失効させ、遅れて発行されたセッションも公開せず失効を試す。
        LAMAPON_API void CancelDiscordSignIn() noexcept;
        // 通信を待たずguestへ切り替え端末tokenを削除しサーバーの失効を進める。
        LAMAPON_API void SignOut();

        // 現在のオンラインアカウント状態を返す。
        [[nodiscard]] LAMAPON_API OnlineAccountState
            State() const noexcept;
        // 認証済みまたは有効セッションの更新中かを返す。
        [[nodiscard]] LAMAPON_API bool IsSignedIn() const noexcept;
        // 公開可能なプロフィールを次の状態更新まで借用する。
        [[nodiscard]] LAMAPON_API const OnlinePlayerProfile&
            Player() const noexcept;
        // 自動起動が無効・失敗でも使える認証URLを次の状態更新まで借用する。
        [[nodiscard]] LAMAPON_API const std::string&
            AuthorizationUrl() const noexcept;
        // 秘密値を含まない診断識別子を次の状態更新まで借用する。
        [[nodiscard]] LAMAPON_API const std::string&
            LastErrorCode() const noexcept;
        // 秘密値を含まない表示用の診断を次の状態更新まで借用する。
        [[nodiscard]] LAMAPON_API const std::string&
            LastError() const noexcept;

        // 内部の識別値を含まない同期状態と残り待機秒数を返す。
        [[nodiscard]] LAMAPON_API OnlineCloudSyncStatus
            CloudSyncStatus() const noexcept;
        // 本文・ETag・更新IDを含まない競合一覧を公開用の乱数IDで返す。
        [[nodiscard]] LAMAPON_API std::vector<OnlineCloudConflict>
            CloudConflicts() const;
        // 利用可否と実行中状態を確認し再照合の要求結果を返す。
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            RequestCloudSync() noexcept;
        // 公開用IDを現在の競合と照合して解決する(conflictId: このprocessの競合ID, resolution: ローカル再送またはリモート採用)。
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            ResolveCloudConflict(
            std::string_view conflictId,
            OnlineCloudConflictResolution resolution) noexcept;

        // 保存復旧の状態と対象の識別版を値として返す。
        [[nodiscard]] LAMAPON_API OnlinePersistenceRecoveryStatus
            PersistenceRecoveryStatus() const noexcept;
        // 古いUIの識別版を拒否して隔離中の保存を復旧する(expectedRevision: 照会時の復旧対象の識別版)。
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            RestorePersistence(std::uint64_t expectedRevision) noexcept;
        // 古いUIの識別版を拒否して隔離中の保存を破棄する(expectedRevision: 照会時の復旧対象の識別版)。
        [[nodiscard]] LAMAPON_API OnlinePersistenceOperationResult
            DiscardPersistence(std::uint64_t expectedRevision) noexcept;

        // アカウント認証と独立した表示設定を変更する(configuration: Discordの表示設定)。
        LAMAPON_API void ConfigureDiscordPresence(
            DiscordPresenceConfiguration configuration);
        // 独立したDiscord表示機能をサービスの存続中借用する。
        [[nodiscard]] LAMAPON_API DiscordPresence&
            Presence() noexcept;
        // 独立したDiscord表示機能を読取専用でサービスの存続中借用する。
        [[nodiscard]] LAMAPON_API const DiscordPresence&
            Presence() const noexcept;

    private:
        struct Implementation;

        // 内部テスト用の状態を受け取る(implementation: 非同期認証の所有状態)。
        explicit OnlineServices(
            std::unique_ptr<Implementation> implementation);

        friend class Detail::OnlineServicesTestAccess;
        friend class Detail::OnlinePersistenceAccess;

        // 非同期認証と保存連携の状態の所有先
        std::unique_ptr<Implementation> m_implementation;
    };

    // Applicationが公開するサービスを借用し未設定ならnullを返す。
    [[nodiscard]] LAMAPON_API OnlineServices*
        ActiveOnlineServices() noexcept;
    // Runtime内の現在のサービス参照を設定する(services: 借用・nullで解除)。
    LAMAPON_API void SetActiveOnlineServices(
        OnlineServices* services) noexcept;
}
