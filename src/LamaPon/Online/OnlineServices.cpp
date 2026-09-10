#include "LamaPon/Online/OnlineServices.h"

#include "LamaPon/Online/DiscordAuth.h"
#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Online/WindowsOnlinePlatform.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
    void EraseSecret(std::string& value) noexcept
    {
        // clear()だけではcapacity内に認証情報が残るため、破棄前に
        // volatile書き込みで使用中の領域を消します。
        volatile char* bytes = value.empty()
            ? nullptr
            : value.data();
        for (std::size_t index = 0;
            index < value.size();
            ++index)
        {
            bytes[index] = 0;
        }
        value.clear();
    }

    void EraseSession(
        LamaPon::Detail::OnlineSession& session) noexcept
    {
        EraseSecret(session.accessToken);
        EraseSecret(session.refreshToken);
        session.expiresInSeconds = 0;
        session.player = {};
    }

    void EraseTransaction(
        LamaPon::Detail::DiscordLoginTransaction& transaction) noexcept
    {
        EraseSecret(transaction.pollToken);
        transaction.transactionId.clear();
        transaction.authorizationUrl.clear();
        transaction.expiresInSeconds = 0;
        transaction.pollIntervalSeconds = 1;
    }

    bool IsKnownErrorCode(const std::string_view code) noexcept
    {
        return code == "network_error"
            || code == "service_error"
            || code == "invalid_response"
            || code == "invalid_transaction"
            || code == "login_denied"
            || code == "login_expired"
            || code == "request_failed"
            || code == "credential_store_unavailable"
            || code == "credential_store_corrupt"
            || code == "credential_save_failed"
            || code == "credential_delete_failed"
            || code == "stored_session_invalid"
            || code == "browser_launch_failed";
    }

    std::string PublicErrorCode(const std::string_view code)
    {
        return IsKnownErrorCode(code)
            ? std::string(code)
            : std::string("service_error");
    }

    std::string PublicErrorMessage(const std::string_view code)
    {
        if (code == "network_error")
        {
            return "オンラインサービスへ接続できませんでした。";
        }
        if (code == "invalid_response")
        {
            return "オンラインサービスから不正な応答を受信しました。";
        }
        if (code == "invalid_transaction")
        {
            return "ログイン要求が無効です。もう一度お試しください。";
        }
        if (code == "login_denied")
        {
            return "Discordログインがキャンセルされました。";
        }
        if (code == "login_expired")
        {
            return "Discordログインの有効時間が切れました。";
        }
        if (code == "request_failed")
        {
            return "オンライン認証の通信を完了できませんでした。";
        }
        if (code == "credential_store_unavailable")
        {
            return "保存済みのログイン情報を読み込めませんでした。";
        }
        if (code == "credential_store_corrupt")
        {
            return "保存済みのログイン情報が壊れていたため破棄しました。";
        }
        if (code == "credential_save_failed")
        {
            return "ログイン状態をこの端末へ保存できませんでした。";
        }
        if (code == "credential_delete_failed")
        {
            return "この端末のログイン情報を削除できませんでした。";
        }
        if (code == "stored_session_invalid")
        {
            return "保存済みのログイン情報は期限切れまたは無効です。";
        }
        if (code == "browser_launch_failed")
        {
            return "ブラウザーを開けませんでした。表示されたURLを手動で開いてください。";
        }
        return "オンライン認証に失敗しました。";
    }

    bool InvalidRefreshTokenError(
        const std::string_view code) noexcept
    {
        return code == "invalid_refresh_token"
            || code == "refresh_token_expired"
            || code == "session_expired"
            || code == "session_not_found"
            || code == "unauthorized";
    }

    struct SecretText final
    {
        explicit SecretText(const std::string_view value)
            : text(value)
        {
        }

        ~SecretText()
        {
            EraseSecret(text);
        }

        SecretText(const SecretText&) = delete;
        SecretText& operator=(const SecretText&) = delete;

        std::string text;
    };
}

namespace LamaPon
{
    struct OnlineServices::Implementation final
    {
        enum class TaskKind
        {
            StartLogin,
            PollLogin,
            RestoreSession,
            RefreshSession,
            Logout
        };

        struct AsyncResult final
        {
            Detail::DiscordLoginStartResult loginStart;
            Detail::DiscordLoginPollResult loginPoll;
            Detail::OnlineSessionResult sessionRefresh;
            bool logoutSucceeded{};
        };

        struct AsyncMailbox final
        {
            TaskKind kind{};
            std::uint64_t generation{};
            std::mutex mutex;
            AsyncResult result;
            bool completed{};
            bool failed{};

            ~AsyncMailbox()
            {
                // OnlineServicesが先に破棄された場合も、workerが最後の
                // shared_ptrを解放した時点で結果内のtokenを消します。
                EraseTransaction(result.loginStart.transaction);
                EraseSession(result.loginPoll.session);
                EraseSession(result.sessionRefresh.session);
            }
        };

        explicit Implementation(
            Detail::OnlineServicesTestAccess::HttpSender sender = {},
            std::unique_ptr<Detail::IRefreshTokenStore> store = {},
            std::unique_ptr<Detail::IAuthorizationLauncher> launcher = {},
            const bool useWindowsDefaults = true)
            : senderOverride(std::move(sender))
            , refreshTokenStore(std::move(store))
            , authorizationLauncher(std::move(launcher))
            , useWindowsPlatformDefaults(useWindowsDefaults)
        {
        }

        ~Implementation()
        {
            AdvanceGeneration();
            // workerはmailboxと認証clientだけを所有します。ここでは
            // joinせず参照を手放すため、未完了HTTP通信を待ちません。
            inFlight.reset();
            ClearLoginTransaction();
            ClearSession();
            EraseSecret(pendingLogoutAccessToken);
        }

        void AdvanceGeneration() noexcept
        {
            ++generation;
            if (generation == 0)
            {
                ++generation;
            }
        }

        [[nodiscard]] bool IsBusy() const noexcept
        {
            return state == OnlineAccountState::StartingSignIn
                || state
                    == OnlineAccountState::WaitingForAuthorization
                || state
                    == OnlineAccountState::PollingAuthorization
                || state == OnlineAccountState::RestoringSession
                || state == OnlineAccountState::RefreshingSession
                || state == OnlineAccountState::SigningOut;
        }

        void ClearError() noexcept
        {
            errorCode.clear();
            errorMessage.clear();
        }

        void SetError(
            const std::string_view code,
            const bool signInFailure)
        {
            const auto publicCode = PublicErrorCode(code);
            errorCode = publicCode;
            errorMessage = PublicErrorMessage(publicCode);
            if (signInFailure)
            {
                AdvanceGeneration();
                ClearLoginTransaction();
                ClearSession();
                player = {};
                state = OnlineAccountState::Error;
            }
        }

        void ClearLoginTransaction() noexcept
        {
            EraseTransaction(loginTransaction);
            authorizationUrl.clear();
            loginRemainingSeconds = 0.0f;
            pollRemainingSeconds = 0.0f;
            browserLaunchAttempted = false;
            browserLaunchFailed = false;
        }

        void ClearSession() noexcept
        {
            EraseSession(session);
            sessionRemainingSeconds = 0.0f;
            sessionRefreshLeadSeconds = 0.0f;
            sessionRefreshRetrySeconds = 0.0f;
        }

        static void EraseAsyncResult(AsyncResult& result) noexcept
        {
            EraseTransaction(result.loginStart.transaction);
            EraseSession(result.loginPoll.session);
            EraseSession(result.sessionRefresh.session);
        }

        void SetFixedError(const std::string_view code)
        {
            const auto publicCode = PublicErrorCode(code);
            errorCode = publicCode;
            errorMessage = PublicErrorMessage(publicCode);
        }

        [[nodiscard]] bool SaveRefreshToken(
            const std::string_view refreshToken) noexcept
        {
            if (!refreshTokenStore)
            {
                return true;
            }
            try
            {
                return static_cast<bool>(
                    refreshTokenStore->Save(refreshToken));
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] bool DeleteRefreshToken() noexcept
        {
            if (!refreshTokenStore)
            {
                return true;
            }
            try
            {
                return static_cast<bool>(refreshTokenStore->Delete());
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] bool ReplacePendingLogoutAccessToken(
            Detail::OnlineSession& completedSession)
        {
            if (completedSession.accessToken.empty())
            {
                EraseSession(completedSession);
                return false;
            }

            // swap後に完了結果側へ移った旧tokenもEraseSessionで消し、
            // 新tokenだけを次のlogout workerへ引き渡します。
            pendingLogoutAccessToken.swap(
                completedSession.accessToken);
            EraseSession(completedSession);
            return true;
        }

        template<class Work>
        [[nodiscard]] bool Launch(
            const TaskKind kind,
            Work&& work)
        {
            if (inFlight)
            {
                return false;
            }

            auto mailbox = std::make_shared<AsyncMailbox>();
            mailbox->kind = kind;
            mailbox->generation = generation;
            inFlight = mailbox;
            try
            {
                std::thread worker(
                    [mailbox,
                        work = std::forward<Work>(work)]() mutable noexcept
                    {
                        AsyncResult result;
                        bool failed{};
                        try
                        {
                            result = work();
                        }
                        catch (...)
                        {
                            failed = true;
                        }

                        try
                        {
                            std::scoped_lock lock(mailbox->mutex);
                            mailbox->result = std::move(result);
                            mailbox->failed = failed;
                            mailbox->completed = true;
                        }
                        catch (...)
                        {
                            // 認証情報を含む可能性がある例外本文は保存
                            // しません。通常到達しない割当失敗時も、可能
                            // なら失敗完了だけを通知します。
                            try
                            {
                                std::scoped_lock lock(mailbox->mutex);
                                mailbox->failed = true;
                                mailbox->completed = true;
                            }
                            catch (...)
                            {
                            }
                        }
                        EraseAsyncResult(result);
                    });
                worker.detach();
                return true;
            }
            catch (...)
            {
                inFlight.reset();
                return false;
            }
        }

        [[nodiscard]] bool LaunchLoginStart()
        {
            const auto authClient = client;
            return Launch(
                TaskKind::StartLogin,
                [authClient]
                {
                    AsyncResult result;
                    result.loginStart = authClient->BeginLogin();
                    return result;
                });
        }

        [[nodiscard]] bool LaunchLoginPoll()
        {
            const auto authClient = client;
            const auto transactionId =
                loginTransaction.transactionId;
            const auto pollToken =
                std::make_shared<SecretText>(
                    loginTransaction.pollToken);
            return Launch(
                TaskKind::PollLogin,
                [authClient,
                    transactionId,
                    pollToken]
                {
                    AsyncResult result;
                    result.loginPoll = authClient->PollLogin(
                        transactionId,
                        pollToken->text);
                    return result;
                });
        }

        [[nodiscard]] bool LaunchSessionRefresh(
            std::string& refreshToken,
            const TaskKind kind)
        {
            if (kind != TaskKind::RestoreSession
                && kind != TaskKind::RefreshSession)
            {
                EraseSecret(refreshToken);
                return false;
            }
            const auto authClient = client;
            const auto secret = std::make_shared<SecretText>(
                refreshToken);
            EraseSecret(refreshToken);
            return Launch(
                kind,
                [authClient, secret]
                {
                    AsyncResult result;
                    result.sessionRefresh =
                        authClient->RefreshSession(secret->text);
                    return result;
                });
        }

        [[nodiscard]] bool LaunchLogout(std::string& accessToken)
        {
            const auto authClient = client;
            const auto secret = std::make_shared<SecretText>(
                accessToken);
            EraseSecret(accessToken);
            return Launch(
                TaskKind::Logout,
                [authClient, secret]
                {
                    AsyncResult result;
                    result.logoutSucceeded = authClient->Logout(
                        secret->text);
                    return result;
                });
        }

        [[nodiscard]] bool IsCurrentTask(
            const TaskKind kind,
            const std::uint64_t taskGeneration) const noexcept
        {
            if (taskGeneration != generation)
            {
                return false;
            }
            switch (kind)
            {
            case TaskKind::StartLogin:
                return state == OnlineAccountState::StartingSignIn;
            case TaskKind::PollLogin:
                return state
                    == OnlineAccountState::PollingAuthorization;
            case TaskKind::RestoreSession:
                return state == OnlineAccountState::RestoringSession;
            case TaskKind::RefreshSession:
                return state == OnlineAccountState::RefreshingSession;
            case TaskKind::Logout:
                return state == OnlineAccountState::SigningOut;
            }
            return false;
        }

        void TryOpenAuthorizationUrl()
        {
            if (browserLaunchAttempted
                || !openAuthorizationBrowser
                || !authorizationLauncher
                || authorizationUrl.empty())
            {
                return;
            }

            browserLaunchAttempted = true;
            bool launched{};
            try
            {
                launched = static_cast<bool>(
                    authorizationLauncher->Launch(
                        authorizationUrl,
                        allowInsecureLoopback));
            }
            catch (...)
            {
                launched = false;
            }
            browserLaunchFailed = !launched;
            if (browserLaunchFailed)
            {
                SetFixedError("browser_launch_failed");
            }
        }

        void AdoptSession(Detail::OnlineSession& nextSession)
        {
            const bool stored = SaveRefreshToken(
                nextSession.refreshToken);
            ClearSession();
            session.accessToken.swap(nextSession.accessToken);
            session.refreshToken.swap(nextSession.refreshToken);
            session.expiresInSeconds = std::exchange(
                nextSession.expiresInSeconds,
                0);
            session.player = std::move(nextSession.player);
            nextSession.player = {};
            player.playerId = session.player.playerId;
            player.displayName = session.player.displayName;
            player.avatarUrl = session.player.avatarUrl;
            player.linkedProvider = session.player.linkedProvider;
            sessionRemainingSeconds = static_cast<float>(
                session.expiresInSeconds);
            sessionRefreshLeadSeconds = std::clamp(
                sessionRemainingSeconds * 0.2f,
                5.0f,
                60.0f);
            sessionRefreshRetrySeconds = 0.0f;
            ClearLoginTransaction();
            state = OnlineAccountState::SignedIn;
            ClearError();
            if (!stored)
            {
                SetFixedError("credential_save_failed");
            }
        }

        void RestoreStoredSession()
        {
            if (!client || !refreshTokenStore)
            {
                return;
            }

            Detail::RefreshTokenLoadResult loaded;
            try
            {
                loaded = refreshTokenStore->Load();
            }
            catch (...)
            {
                loaded.status =
                    Detail::RefreshTokenLoadStatus::Unavailable;
            }

            std::string refreshToken;
            refreshToken.swap(loaded.refreshToken);
            EraseSecret(loaded.refreshToken);
            switch (loaded.status)
            {
            case Detail::RefreshTokenLoadStatus::NotFound:
                EraseSecret(refreshToken);
                return;

            case Detail::RefreshTokenLoadStatus::Unavailable:
                EraseSecret(refreshToken);
                SetFixedError("credential_store_unavailable");
                return;

            case Detail::RefreshTokenLoadStatus::Corrupt:
                EraseSecret(refreshToken);
                if (DeleteRefreshToken())
                {
                    SetFixedError("credential_store_corrupt");
                }
                else
                {
                    SetFixedError("credential_delete_failed");
                }
                return;

            case Detail::RefreshTokenLoadStatus::Loaded:
                break;
            }

            if (refreshToken.empty())
            {
                if (DeleteRefreshToken())
                {
                    SetFixedError("credential_store_corrupt");
                }
                else
                {
                    SetFixedError("credential_delete_failed");
                }
                return;
            }

            state = OnlineAccountState::RestoringSession;
            if (!LaunchSessionRefresh(
                    refreshToken,
                    TaskKind::RestoreSession))
            {
                EraseSecret(refreshToken);
                state = OnlineAccountState::Error;
                SetFixedError("request_failed");
            }
        }

        void CompleteLoginStart(
            Detail::DiscordLoginStartResult& result)
        {
            if (!result.Succeeded())
            {
                SetError(result.errorCode, true);
                return;
            }

            ClearLoginTransaction();
            loginTransaction = std::move(result.transaction);
            authorizationUrl =
                std::move(loginTransaction.authorizationUrl);
            loginRemainingSeconds = static_cast<float>(
                loginTransaction.expiresInSeconds);
            pollRemainingSeconds = static_cast<float>(
                loginTransaction.pollIntervalSeconds);
            state = OnlineAccountState::WaitingForAuthorization;
            ClearError();
            TryOpenAuthorizationUrl();
        }

        void CompleteLoginPoll(
            Detail::DiscordLoginPollResult& result)
        {
            switch (result.status)
            {
            case Detail::DiscordLoginPollStatus::Pending:
                pollRemainingSeconds = static_cast<float>(
                    result.retryAfterSeconds);
                state = OnlineAccountState::WaitingForAuthorization;
                if (!browserLaunchFailed)
                {
                    ClearError();
                }
                return;

            case Detail::DiscordLoginPollStatus::Authorized:
                AdoptSession(result.session);
                return;

            case Detail::DiscordLoginPollStatus::Denied:
                SetError("login_denied", true);
                return;

            case Detail::DiscordLoginPollStatus::Expired:
                SetError("login_expired", true);
                return;

            case Detail::DiscordLoginPollStatus::Failed:
                SetError(result.errorCode, true);
                return;
            }
        }

        void CompleteSessionRefresh(
            Detail::OnlineSessionResult& result,
            const bool restoring)
        {
            if (result.Succeeded())
            {
                AdoptSession(result.session);
                return;
            }

            if (InvalidRefreshTokenError(result.errorCode))
            {
                const bool deleted = DeleteRefreshToken();
                ClearSession();
                player = {};
                state = restoring
                    ? OnlineAccountState::SignedOut
                    : OnlineAccountState::Error;
                SetFixedError(
                    deleted
                        ? "stored_session_invalid"
                        : "credential_delete_failed");
                return;
            }

            if (restoring)
            {
                // 一時的な通信失敗で端末のrefresh tokenを
                // 削除しません。次回起動で再試行できます。
                state = OnlineAccountState::Error;
                SetError(result.errorCode, false);
                return;
            }

            if (sessionRemainingSeconds > 0.0f)
            {
                // access tokenがまだ有効な間は現在のセッションを
                // 保持し、5秒後にrefreshを再試行します。
                state = OnlineAccountState::SignedIn;
                sessionRefreshRetrySeconds = std::min(
                    5.0f,
                    sessionRemainingSeconds);
                SetError(result.errorCode, false);
                return;
            }

            // セッションは失効していますが、ネットワーク
            // 障害で保存済みtokenを消すと復旧できなくなります。
            ClearSession();
            player = {};
            state = OnlineAccountState::Error;
            SetError(result.errorCode, false);
        }

        void CompleteLogout(const bool succeeded)
        {
            EraseSecret(pendingLogoutAccessToken);
            awaitingCancelledTask = false;
            const bool returnToExpiredLoginError =
                std::exchange(cancelledPollExpired, false);
            const bool deleteFailed =
                std::exchange(localRefreshTokenDeleteFailed, false);
            state = returnToExpiredLoginError
                ? OnlineAccountState::Error
                : OnlineAccountState::SignedOut;
            if (deleteFailed)
            {
                SetFixedError("credential_delete_failed");
            }
            else if (succeeded)
            {
                if (!returnToExpiredLoginError)
                {
                    ClearError();
                }
            }
            else
            {
                errorCode = "logout_failed";
                errorMessage =
                    "サーバー上のセッションを失効できませんでした。"
                    "端末上ではサインアウト済みです。";
            }
        }

        [[nodiscard]] bool CompleteCancelledTask(
            const TaskKind kind,
            const bool failed,
            AsyncResult& result)
        {
            if (!awaitingCancelledTask
                || kind != cancelledTaskKind)
            {
                return false;
            }
            awaitingCancelledTask = false;

            if (kind == TaskKind::RestoreSession)
            {
                if (!failed
                    && result.sessionRefresh.Succeeded()
                    && ReplacePendingLogoutAccessToken(
                        result.sessionRefresh.session))
                {
                    // Update末尾のTickPendingLogoutが、復元で新規発行
                    // されたaccess tokenを使って失効要求を開始します。
                    return true;
                }

                // 復元失敗ならサーバー側に新しいセッションはなく、
                // ローカル削除だけでサインアウト完了です。
                CompleteLogout(true);
                return true;
            }

            if (kind == TaskKind::RefreshSession)
            {
                if (!failed && result.sessionRefresh.Succeeded())
                {
                    // rotation後は旧access tokenが無効な可能性があるため、
                    // 完了結果の新tokenを優先します。
                    (void)ReplacePendingLogoutAccessToken(
                        result.sessionRefresh.session);
                }
                // refresh失敗時はSignOut時に退避した旧tokenを維持し、
                // 成功時は上で置き換えた新tokenを使います。
                if (pendingLogoutAccessToken.empty())
                {
                    CompleteLogout(false);
                }
                return true;
            }

            if (kind == TaskKind::PollLogin)
            {
                if (!failed
                    && result.loginPoll.status
                        == Detail::DiscordLoginPollStatus::Authorized)
                {
                    // Cancelと認証完了が競合した場合も、受け取った
                    // セッションを公開せず直ちにサーバーで失効します。
                    state = OnlineAccountState::SigningOut;
                    if (ReplacePendingLogoutAccessToken(
                            result.loginPoll.session))
                    {
                        return true;
                    }
                    CompleteLogout(false);
                    return true;
                }

                if (cancelledPollExpired)
                {
                    // 期限切れの公開状態とエラーを維持します。遅延応答が
                    // Authorizedでなければ失効すべきsessionはありません。
                    cancelledPollExpired = false;
                    return true;
                }
                CompleteLogout(true);
                return true;
            }

            return false;
        }

        void CompleteCancelledTaskException(
            const TaskKind kind)
        {
            awaitingCancelledTask = false;
            if (kind == TaskKind::RefreshSession
                && !pendingLogoutAccessToken.empty())
            {
                // 新tokenを回収できなくても、旧tokenの失効は試します。
                return;
            }
            if (kind == TaskKind::PollLogin
                && cancelledPollExpired)
            {
                cancelledPollExpired = false;
                return;
            }
            CompleteLogout(kind == TaskKind::RestoreSession
                || kind == TaskKind::PollLogin);
        }

        void CompleteTask(
            const TaskKind kind,
            AsyncResult& result)
        {
            switch (kind)
            {
            case TaskKind::StartLogin:
                CompleteLoginStart(result.loginStart);
                return;
            case TaskKind::PollLogin:
                CompleteLoginPoll(result.loginPoll);
                return;
            case TaskKind::RestoreSession:
                CompleteSessionRefresh(result.sessionRefresh, true);
                return;
            case TaskKind::RefreshSession:
                CompleteSessionRefresh(result.sessionRefresh, false);
                return;
            case TaskKind::Logout:
                CompleteLogout(result.logoutSucceeded);
                return;
            }
        }

        void CompleteTaskException(const TaskKind kind)
        {
            // exception.what()には注入sender由来のtoken等が含まれる
            // 可能性があるため、公開エラーへ転記しません。
            if (kind == TaskKind::Logout)
            {
                CompleteLogout(false);
                return;
            }
            if (kind == TaskKind::RestoreSession)
            {
                state = OnlineAccountState::Error;
                SetFixedError("request_failed");
                return;
            }
            if (kind == TaskKind::RefreshSession)
            {
                if (sessionRemainingSeconds > 0.0f)
                {
                    state = OnlineAccountState::SignedIn;
                    sessionRefreshRetrySeconds = std::min(
                        5.0f,
                        sessionRemainingSeconds);
                    SetFixedError("request_failed");
                }
                else
                {
                    ClearSession();
                    player = {};
                    state = OnlineAccountState::Error;
                    SetFixedError("request_failed");
                }
                return;
            }
            SetError("request_failed", true);
        }

        [[nodiscard]] bool ReapCompletedTask()
        {
            const auto mailbox = inFlight;
            if (!mailbox)
            {
                return false;
            }

            AsyncResult result;
            TaskKind kind{};
            std::uint64_t taskGeneration{};
            bool failed{};
            {
                std::scoped_lock lock(mailbox->mutex);
                if (!mailbox->completed)
                {
                    return false;
                }
                kind = mailbox->kind;
                taskGeneration = mailbox->generation;
                failed = mailbox->failed;
                result = std::move(mailbox->result);
            }
            inFlight.reset();

            const auto stateBefore = state;
            if (IsCurrentTask(kind, taskGeneration))
            {
                if (failed)
                {
                    CompleteTaskException(kind);
                }
                else
                {
                    try
                    {
                        CompleteTask(kind, result);
                    }
                    catch (...)
                    {
                        CompleteTaskException(kind);
                    }
                }
            }
            else if (taskGeneration != generation)
            {
                try
                {
                    (void)CompleteCancelledTask(
                        kind,
                        failed,
                        result);
                }
                catch (...)
                {
                    CompleteCancelledTaskException(kind);
                }
            }
            EraseAsyncResult(result);
            return state != stateBefore;
        }

        void TickLogin(const float elapsedSeconds)
        {
            const bool loginActive =
                state == OnlineAccountState::WaitingForAuthorization
                || state
                    == OnlineAccountState::PollingAuthorization;
            if (!loginActive)
            {
                return;
            }

            loginRemainingSeconds -= elapsedSeconds;
            if (loginRemainingSeconds <= 0.0f)
            {
                if (state
                        == OnlineAccountState::PollingAuthorization
                    && inFlight)
                {
                    awaitingCancelledTask = true;
                    cancelledTaskKind = TaskKind::PollLogin;
                    cancelledPollExpired = true;
                }
                SetError("login_expired", true);
                return;
            }

            if (state != OnlineAccountState::WaitingForAuthorization)
            {
                return;
            }

            pollRemainingSeconds -= elapsedSeconds;
            if (pollRemainingSeconds > 0.0f)
            {
                return;
            }

            if (!LaunchLoginPoll())
            {
                SetError("request_failed", true);
                return;
            }
            state = OnlineAccountState::PollingAuthorization;
        }

        void TickSession(const float elapsedSeconds)
        {
            if (state == OnlineAccountState::RefreshingSession)
            {
                sessionRemainingSeconds = std::max(
                    0.0f,
                    sessionRemainingSeconds - elapsedSeconds);
                return;
            }
            if (state != OnlineAccountState::SignedIn)
            {
                return;
            }

            sessionRemainingSeconds = std::max(
                0.0f,
                sessionRemainingSeconds - elapsedSeconds);
            sessionRefreshRetrySeconds = std::max(
                0.0f,
                sessionRefreshRetrySeconds - elapsedSeconds);
            if (sessionRefreshRetrySeconds > 0.0f
                || sessionRemainingSeconds
                    > sessionRefreshLeadSeconds)
            {
                return;
            }

            auto refreshToken = session.refreshToken;
            if (refreshToken.empty()
                || !LaunchSessionRefresh(
                    refreshToken,
                    TaskKind::RefreshSession))
            {
                EraseSecret(refreshToken);
                if (sessionRemainingSeconds <= 0.0f)
                {
                    ClearSession();
                    player = {};
                    state = OnlineAccountState::Error;
                }
                SetFixedError("request_failed");
                return;
            }
            state = OnlineAccountState::RefreshingSession;
        }

        void TickPendingLogout()
        {
            if (state != OnlineAccountState::SigningOut
                || inFlight
                || awaitingCancelledTask
                || pendingLogoutAccessToken.empty())
            {
                return;
            }
            if (!LaunchLogout(pendingLogoutAccessToken))
            {
                CompleteLogout(false);
            }
        }

        Detail::OnlineServicesTestAccess::HttpSender senderOverride;
        std::shared_ptr<Detail::DiscordAuthClient> client;
        std::shared_ptr<AsyncMailbox> inFlight;
        std::unique_ptr<Detail::IRefreshTokenStore> refreshTokenStore;
        std::unique_ptr<Detail::IAuthorizationLauncher>
            authorizationLauncher;
        Detail::DiscordLoginTransaction loginTransaction;
        Detail::OnlineSession session;
        OnlinePlayerProfile player;
        std::string pendingLogoutAccessToken;
        std::string authorizationUrl;
        std::string errorCode;
        std::string errorMessage;
        OnlineAccountState state{ OnlineAccountState::Unconfigured };
        std::uint64_t generation{ 1 };
        float loginRemainingSeconds{};
        float pollRemainingSeconds{};
        float sessionRemainingSeconds{};
        float sessionRefreshLeadSeconds{};
        float sessionRefreshRetrySeconds{};
        bool useWindowsPlatformDefaults{ true };
        bool allowInsecureLoopback{};
        bool openAuthorizationBrowser{ true };
        bool browserLaunchAttempted{};
        bool browserLaunchFailed{};
        bool localRefreshTokenDeleteFailed{};
        bool awaitingCancelledTask{};
        bool cancelledPollExpired{};
        TaskKind cancelledTaskKind{ TaskKind::StartLogin };
    };

    OnlineServices::OnlineServices()
        : m_implementation(
            std::make_unique<Implementation>())
    {
    }

    OnlineServices::OnlineServices(
        OnlineServiceConfiguration configuration)
        : OnlineServices()
    {
        Configure(std::move(configuration));
    }

    OnlineServices::OnlineServices(
        std::unique_ptr<Implementation> implementation)
        : m_implementation(std::move(implementation))
    {
        if (!m_implementation)
        {
            throw std::invalid_argument(
                "OnlineServices implementation is required.");
        }
    }

    OnlineServices::~OnlineServices()
    {
        if (ActiveOnlineServices() == this)
        {
            SetActiveOnlineServices(nullptr);
        }
    }

    void OnlineServices::Configure(
        OnlineServiceConfiguration configuration)
    {
        auto& implementation = *m_implementation;
        if (implementation.IsBusy()
            || implementation.inFlight
            || implementation.state == OnlineAccountState::SignedIn)
        {
            throw std::logic_error(
                "Sign out before changing the online service configuration.");
        }

        std::shared_ptr<Detail::DiscordAuthClient> nextClient;
        std::unique_ptr<Detail::IRefreshTokenStore> nextStore;
        std::unique_ptr<Detail::IAuthorizationLauncher> nextLauncher;
        if (!configuration.serviceBaseUrl.empty())
        {
            nextClient = std::make_shared<Detail::DiscordAuthClient>(
                std::move(configuration.serviceBaseUrl),
                configuration.allowInsecureLoopback,
                implementation.senderOverride,
                configuration.gameId,
                configuration.environmentId);
            if (implementation.useWindowsPlatformDefaults)
            {
                if (!configuration.gameId.empty())
                {
                    nextStore = Detail::MakeWindowsRefreshTokenStore(
                        configuration.gameId,
                        configuration.environmentId);
                }
                nextLauncher =
                    Detail::MakeWindowsAuthorizationLauncher();
            }
        }

        implementation.AdvanceGeneration();
        implementation.ClearLoginTransaction();
        implementation.ClearSession();
        EraseSecret(implementation.pendingLogoutAccessToken);
        implementation.player = {};
        implementation.ClearError();
        implementation.localRefreshTokenDeleteFailed = false;
        implementation.awaitingCancelledTask = false;
        implementation.cancelledPollExpired = false;
        implementation.allowInsecureLoopback =
            configuration.allowInsecureLoopback;
        implementation.openAuthorizationBrowser =
            configuration.openAuthorizationBrowser;
        if (implementation.useWindowsPlatformDefaults)
        {
            implementation.refreshTokenStore = std::move(nextStore);
            implementation.authorizationLauncher =
                std::move(nextLauncher);
        }
        implementation.client = std::move(nextClient);
        implementation.state = implementation.client
            ? OnlineAccountState::SignedOut
            : OnlineAccountState::Unconfigured;
        implementation.RestoreStoredSession();
    }

    void OnlineServices::Update(float elapsedSeconds)
    {
        auto& implementation = *m_implementation;
        if (!std::isfinite(elapsedSeconds)
            || elapsedSeconds < 0.0f)
        {
            elapsedSeconds = 0.0f;
        }
        if (implementation.ReapCompletedTask())
        {
            // 完了によって遷移したばかりの状態へ、前状態で経過した
            // elapsedSecondsを同じフレーム中に適用しません。
            return;
        }
        implementation.TickLogin(elapsedSeconds);
        implementation.TickSession(elapsedSeconds);
        implementation.TickPendingLogout();
    }

    bool OnlineServices::BeginDiscordSignIn()
    {
        auto& implementation = *m_implementation;
        if (!implementation.client)
        {
            implementation.errorCode = "not_configured";
            implementation.errorMessage =
                "オンラインサービスが設定されていません。";
            return false;
        }
        if (implementation.IsBusy()
            || implementation.inFlight)
        {
            implementation.errorCode = "operation_in_progress";
            implementation.errorMessage =
                "オンラインアカウント処理が進行中です。";
            return false;
        }
        if (implementation.state == OnlineAccountState::SignedIn)
        {
            implementation.errorCode = "already_signed_in";
            implementation.errorMessage =
                "すでにサインインしています。";
            return false;
        }

        implementation.AdvanceGeneration();
        implementation.ClearLoginTransaction();
        implementation.ClearSession();
        implementation.player = {};
        implementation.ClearError();
        implementation.localRefreshTokenDeleteFailed = false;
        implementation.awaitingCancelledTask = false;
        implementation.cancelledPollExpired = false;
        implementation.state = OnlineAccountState::StartingSignIn;
        if (!implementation.LaunchLoginStart())
        {
            implementation.SetError("request_failed", true);
            return false;
        }
        return true;
    }

    void OnlineServices::CancelDiscordSignIn() noexcept
    {
        auto& implementation = *m_implementation;
        const bool signingIn =
            implementation.state == OnlineAccountState::StartingSignIn
            || implementation.state
                == OnlineAccountState::WaitingForAuthorization
            || implementation.state
                == OnlineAccountState::PollingAuthorization;
        if (!signingIn)
        {
            return;
        }

        if (implementation.state
                == OnlineAccountState::PollingAuthorization
            && implementation.inFlight)
        {
            implementation.awaitingCancelledTask = true;
            implementation.cancelledPollExpired = false;
            implementation.cancelledTaskKind =
                Implementation::TaskKind::PollLogin;
        }

        // WinHTTP要求自体は同期APIのため途中で破棄せず、世代を進めて
        // 完了結果だけを無視します。ゲームループは待ちません。
        implementation.AdvanceGeneration();
        implementation.ClearLoginTransaction();
        implementation.ClearSession();
        implementation.player = {};
        implementation.ClearError();
        implementation.state = implementation.client
            ? OnlineAccountState::SignedOut
            : OnlineAccountState::Unconfigured;
    }

    void OnlineServices::SignOut()
    {
        auto& implementation = *m_implementation;
        if (implementation.state == OnlineAccountState::SigningOut)
        {
            // 内部cleanup中に明示SignOutされた場合も、端末tokenの削除を
            // 必ず試します。前回失敗していればこの呼び出しが再試行です。
            implementation.localRefreshTokenDeleteFailed =
                !implementation.DeleteRefreshToken();
            implementation.cancelledPollExpired = false;
            implementation.ClearError();
            if (implementation.localRefreshTokenDeleteFailed)
            {
                implementation.SetFixedError(
                    "credential_delete_failed");
            }
            return;
        }

        // 通信の完了を待たず、端末の再ログイン情報は
        // この呼び出し中に削除します。
        implementation.localRefreshTokenDeleteFailed =
            !implementation.DeleteRefreshToken();
        if (implementation.awaitingCancelledTask
            && implementation.cancelledTaskKind
                == Implementation::TaskKind::PollLogin)
        {
            // 明示SignOutは期限切れ表示よりローカルSignedOutを優先します。
            implementation.cancelledPollExpired = false;
        }
        if (implementation.state
                == OnlineAccountState::StartingSignIn
            || implementation.state
                == OnlineAccountState::WaitingForAuthorization
            || implementation.state
                == OnlineAccountState::PollingAuthorization)
        {
            CancelDiscordSignIn();
            if (implementation.localRefreshTokenDeleteFailed)
            {
                implementation.SetFixedError(
                    "credential_delete_failed");
            }
            return;
        }
        if (implementation.state == OnlineAccountState::RestoringSession)
        {
            implementation.awaitingCancelledTask = true;
            implementation.cancelledTaskKind =
                Implementation::TaskKind::RestoreSession;
            implementation.AdvanceGeneration();
            implementation.ClearLoginTransaction();
            implementation.ClearSession();
            EraseSecret(implementation.pendingLogoutAccessToken);
            implementation.player = {};
            implementation.state = OnlineAccountState::SigningOut;
            implementation.ClearError();
            return;
        }
        if (implementation.state != OnlineAccountState::SignedIn
            && implementation.state
                != OnlineAccountState::RefreshingSession)
        {
            implementation.AdvanceGeneration();
            implementation.ClearLoginTransaction();
            implementation.ClearSession();
            EraseSecret(implementation.pendingLogoutAccessToken);
            implementation.player = {};
            implementation.ClearError();
            implementation.state = implementation.client
                ? OnlineAccountState::SignedOut
                : OnlineAccountState::Unconfigured;
            if (implementation.localRefreshTokenDeleteFailed)
            {
                implementation.SetFixedError(
                    "credential_delete_failed");
            }
            return;
        }

        const bool refreshInProgress = implementation.state
            == OnlineAccountState::RefreshingSession;
        implementation.awaitingCancelledTask = refreshInProgress;
        if (refreshInProgress)
        {
            implementation.cancelledTaskKind =
                Implementation::TaskKind::RefreshSession;
        }
        implementation.AdvanceGeneration();
        EraseSecret(implementation.pendingLogoutAccessToken);
        implementation.pendingLogoutAccessToken.swap(
            implementation.session.accessToken);
        implementation.ClearSession();
        implementation.ClearLoginTransaction();
        implementation.player = {};
        implementation.ClearError();
        implementation.state = OnlineAccountState::SigningOut;
        if (implementation.pendingLogoutAccessToken.empty()
            && !implementation.awaitingCancelledTask)
        {
            implementation.CompleteLogout(false);
            return;
        }
        implementation.TickPendingLogout();
    }

    OnlineAccountState OnlineServices::State() const noexcept
    {
        return m_implementation->state;
    }

    bool OnlineServices::IsSignedIn() const noexcept
    {
        return m_implementation->state
                == OnlineAccountState::SignedIn
            || m_implementation->state
                == OnlineAccountState::RefreshingSession;
    }

    const OnlinePlayerProfile&
        OnlineServices::Player() const noexcept
    {
        return m_implementation->player;
    }

    const std::string&
        OnlineServices::AuthorizationUrl() const noexcept
    {
        return m_implementation->authorizationUrl;
    }

    const std::string&
        OnlineServices::LastErrorCode() const noexcept
    {
        return m_implementation->errorCode;
    }

    const std::string& OnlineServices::LastError() const noexcept
    {
        return m_implementation->errorMessage;
    }

    namespace Detail
    {
        std::unique_ptr<OnlineServices>
            OnlineServicesTestAccess::Create(
                OnlineServiceConfiguration configuration,
                HttpSender sender,
                std::unique_ptr<IRefreshTokenStore> refreshTokenStore,
                std::unique_ptr<IAuthorizationLauncher>
                    authorizationLauncher)
        {
            if (!sender)
            {
                throw std::invalid_argument(
                    "OnlineServices test sender is required.");
            }
            auto services = std::unique_ptr<OnlineServices>(
                new OnlineServices(
                    std::make_unique<OnlineServices::Implementation>(
                        std::move(sender),
                        std::move(refreshTokenStore),
                        std::move(authorizationLauncher),
                        false)));
            services->Configure(std::move(configuration));
            return services;
        }
    }

    namespace
    {
        OnlineServices* g_activeOnlineServices{};
    }

    OnlineServices* ActiveOnlineServices() noexcept
    {
        return g_activeOnlineServices;
    }

    void SetActiveOnlineServices(
        OnlineServices* services) noexcept
    {
        g_activeOnlineServices = services;
    }
}
