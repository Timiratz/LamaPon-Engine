#include "LamaPon/Online/OnlineServices.h"

#include "LamaPon/Online/DiscordAuth.h"
#include "LamaPon/Online/CloudSaveClient.h"
#include "LamaPon/Online/CloudSaveSynchronizer.h"
#include "LamaPon/Online/OnlinePersistenceCoordinator.h"
#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Online/WindowsOnlinePlatform.h"
#include "LamaPon/Core/SaveSlotValidation.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
            || code == "browser_launch_failed"
            || code == "persistence_activation_failed"
            || code == "account_identity_changed"
            || code == "account_save_failed"
            || code == "persistence_recovery_required";
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
        if (code == "persistence_activation_failed")
        {
            return "アカウントのセーブデータを安全に読み込めませんでした。";
        }
        if (code == "account_identity_changed")
        {
            return "セッションのアカウント識別が変化したためサインアウトしました。";
        }
        if (code == "account_save_failed")
        {
            return "アカウントのPlayerPrefsを保存できませんでした。";
        }
        if (code == "persistence_recovery_required")
        {
            return "未解決のセーブデータ復旧を先に完了してください。";
        }
        return "オンライン認証に失敗しました。";
    }

    [[nodiscard]] std::uint64_t SteadyMilliseconds() noexcept
    {
        const auto count = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return count > 0 ? static_cast<std::uint64_t>(count) : 0u;
    }

    [[nodiscard]] std::string GenerateOpaqueConflictId()
    {
        std::array<unsigned char, 16> randomBytes{};
        if (BCryptGenRandom(
                nullptr,
                randomBytes.data(),
                static_cast<ULONG>(randomBytes.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        {
            throw std::runtime_error(
                "Opaque conflict identifier generation failed.");
        }
        static constexpr char Hex[] = "0123456789abcdef";
        std::string result(randomBytes.size() * 2u, '0');
        for (std::size_t index = 0; index < randomBytes.size(); ++index)
        {
            result[index * 2u] = Hex[randomBytes[index] >> 4u];
            result[index * 2u + 1u] = Hex[randomBytes[index] & 0x0fu];
        }
        SecureZeroMemory(randomBytes.data(), randomBytes.size());
        return result;
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
            std::shared_ptr<Detail::DiscordAuthClient> cleanupClient;
            std::shared_ptr<Detail::IRefreshTokenStore> usageLeaseStore;
            std::chrono::steady_clock::time_point completedAt{};
            bool completed{};
            bool failed{};
            bool abandoned{};

            [[nodiscard]] static Detail::OnlineSession*
                RevocableSession(
                    const TaskKind taskKind,
                    const bool taskFailed,
                    AsyncResult& taskResult) noexcept
            {
                if (taskFailed)
                {
                    return nullptr;
                }
                if (taskKind == TaskKind::PollLogin
                    && taskResult.loginPoll.status
                        == Detail::DiscordLoginPollStatus::Authorized)
                {
                    return &taskResult.loginPoll.session;
                }
                if ((taskKind == TaskKind::RestoreSession
                        || taskKind == TaskKind::RefreshSession)
                    && taskResult.sessionRefresh.Succeeded())
                {
                    return &taskResult.sessionRefresh.session;
                }
                return nullptr;
            }

            static void CleanupAbandonedResult(
                const TaskKind taskKind,
                const bool taskFailed,
                const std::shared_ptr<Detail::DiscordAuthClient>&
                    authClient,
                AsyncResult& taskResult) noexcept
            {
                auto* const receivedSession = RevocableSession(
                    taskKind,
                    taskFailed,
                    taskResult);
                if (authClient
                    && receivedSession
                    && !receivedSession->accessToken.empty())
                {
                    try
                    {
                        (void)authClient->Logout(
                            receivedSession->accessToken);
                    }
                    catch (...)
                    {
                    }
                }
                EraseTransaction(taskResult.loginStart.transaction);
                EraseSession(taskResult.loginPoll.session);
                EraseSession(taskResult.sessionRefresh.session);
            }

            static void DispatchAbandonedCleanup(
                const TaskKind taskKind,
                const bool taskFailed,
                std::shared_ptr<Detail::DiscordAuthClient> authClient,
                std::shared_ptr<Detail::IRefreshTokenStore> usageLeaseStore,
                AsyncResult taskResult) noexcept
            {
                if (!RevocableSession(
                        taskKind,
                        taskFailed,
                        taskResult))
                {
                    CleanupAbandonedResult(
                        taskKind,
                        taskFailed,
                        authClient,
                        taskResult);
                    return;
                }

                std::shared_ptr<AsyncResult> cleanupResult;
                try
                {
                    cleanupResult = std::make_shared<AsyncResult>(
                        std::move(taskResult));
                    std::thread cleanupWorker(
                        [taskKind,
                            taskFailed,
                            authClient = std::move(authClient),
                            usageLeaseStore = std::move(usageLeaseStore),
                            cleanupResult]() mutable
                            noexcept
                        {
                            (void)usageLeaseStore;
                            CleanupAbandonedResult(
                                taskKind,
                                taskFailed,
                                authClient,
                                *cleanupResult);
                        });
                    cleanupWorker.detach();
                }
                catch (...)
                {
                    if (cleanupResult)
                    {
                        CleanupAbandonedResult(
                            taskKind,
                            true,
                            authClient,
                            *cleanupResult);
                    }
                    else
                    {
                        CleanupAbandonedResult(
                            taskKind,
                            true,
                            authClient,
                            taskResult);
                    }
                }
            }

            void Abandon() noexcept
            {
                AsyncResult completedResult;
                std::shared_ptr<Detail::DiscordAuthClient>
                    completedClient;
                std::shared_ptr<Detail::IRefreshTokenStore>
                    completedUsageLeaseStore;
                bool dispatchCleanup{};
                bool taskFailed{};
                try
                {
                    std::scoped_lock lock(mutex);
                    abandoned = true;
                    if (completed)
                    {
                        completedResult = std::move(result);
                        completedClient = cleanupClient;
                        completedUsageLeaseStore = usageLeaseStore;
                        taskFailed = failed;
                        dispatchCleanup = true;
                    }
                }
                catch (...)
                {
                    return;
                }

                if (dispatchCleanup)
                {
                    DispatchAbandonedCleanup(
                        kind,
                        taskFailed,
                        std::move(completedClient),
                        std::move(completedUsageLeaseStore),
                        std::move(completedResult));
                }
            }

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
            bool credentialDeleteAttempted{};
            if (localRefreshTokenDeleteFailed)
            {
                // SignOut等の同期削除が失敗した後にownerが破棄されても、
                // 残った資格情報を次回起動へ持ち越さないよう再試行します。
                (void)DeleteRefreshTokenTracked();
                credentialDeleteAttempted = true;
            }
            if (inFlight)
            {
                const auto taskKind = inFlight->kind;
                if ((taskKind == TaskKind::RestoreSession
                        || taskKind == TaskKind::RefreshSession)
                    && state != OnlineAccountState::SigningOut
                    && !credentialDeleteAttempted)
                {
                    // refresh token rotationの成否がowner破棄後まで
                    // 確定しないため、旧資格情報を同期的にfail-closedで
                    // 削除します。明示SignOut済みなら二重削除しません。
                    (void)DeleteRefreshTokenTracked();
                }
                inFlight->Abandon();
            }
            inFlight.reset();
            DetachAccountPersistence();
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

        struct CloudConflictBinding final
        {
            std::string opaqueId;
            std::uint64_t profileEpoch{};
            Detail::CloudSaveConflictDescriptor descriptor;
        };

        [[nodiscard]] static bool SameResource(
            const CloudSaveResource& left,
            const CloudSaveResource& right) noexcept
        {
            if (left.kind != right.kind)
            {
                return false;
            }
            return left.kind == CloudSaveResourceKind::Preferences
                ? left.slot.empty() && right.slot.empty()
                : Detail::EquivalentSaveSlotNames(
                    left.slot,
                    right.slot);
        }

        [[nodiscard]] static bool SameConflictIdentity(
            const CloudConflictBinding& binding,
            const std::uint64_t profileEpoch,
            const Detail::CloudSaveConflictDescriptor& descriptor) noexcept
        {
            return binding.profileEpoch == profileEpoch
                && SameResource(
                    binding.descriptor.resource,
                    descriptor.resource)
                && binding.descriptor.expectedMutationId
                    == descriptor.expectedMutationId;
        }

        void ClearCloudConflictRegistry() noexcept
        {
            for (auto& binding : cloudConflictRegistry)
            {
                EraseSecret(binding.descriptor.expectedMutationId);
            }
            cloudConflictRegistry.clear();
        }

        [[nodiscard]] std::vector<OnlineCloudConflict>
            RefreshCloudConflictRegistry()
        {
            if (!persistenceCoordinator
                || !persistenceCoordinator->IsAccountActive())
            {
                ClearCloudConflictRegistry();
                return {};
            }
            auto* const synchronizer =
                persistenceCoordinator->Synchronizer();
            if (!synchronizer || !synchronizer->IsAttached())
            {
                ClearCloudConflictRegistry();
                return {};
            }

            const auto epoch = persistenceCoordinator->ProfileEpoch();
            const auto descriptors = synchronizer->Conflicts();
            std::vector<CloudConflictBinding> nextRegistry;
            std::vector<OnlineCloudConflict> publicConflicts;
            nextRegistry.reserve(descriptors.size());
            publicConflicts.reserve(descriptors.size());
            for (const auto& descriptor : descriptors)
            {
                std::string opaqueId;
                const auto existing = std::find_if(
                    cloudConflictRegistry.begin(),
                    cloudConflictRegistry.end(),
                    [&](const CloudConflictBinding& binding)
                    {
                        return SameConflictIdentity(
                            binding,
                            epoch,
                            descriptor);
                    });
                if (existing != cloudConflictRegistry.end())
                {
                    opaqueId = existing->opaqueId;
                }
                else
                {
                    for (unsigned int attempt = 0; attempt < 16u; ++attempt)
                    {
                        opaqueId = GenerateOpaqueConflictId();
                        const auto collision = [&](const auto& binding)
                        {
                            return binding.opaqueId == opaqueId;
                        };
                        if (std::none_of(
                                cloudConflictRegistry.begin(),
                                cloudConflictRegistry.end(),
                                collision)
                            && std::none_of(
                                nextRegistry.begin(),
                                nextRegistry.end(),
                                collision))
                        {
                            break;
                        }
                        opaqueId.clear();
                    }
                    if (opaqueId.empty())
                    {
                        throw std::runtime_error(
                            "Opaque conflict identifier collision.");
                    }
                }

                const auto publicKind = descriptor.resource.kind
                        == CloudSaveResourceKind::Preferences
                    ? OnlineCloudResourceKind::Preferences
                    : OnlineCloudResourceKind::SaveSlot;
                publicConflicts.push_back({
                    opaqueId,
                    publicKind,
                    descriptor.resource.slot,
                    descriptor.localDeleted,
                    descriptor.localByteLength,
                    descriptor.remoteDeleted,
                    descriptor.remoteByteLength
                });
                nextRegistry.push_back({ opaqueId, epoch, descriptor });
            }
            ClearCloudConflictRegistry();
            cloudConflictRegistry.swap(nextRegistry);
            return publicConflicts;
        }

        [[nodiscard]] CloudConflictBinding* FindConflictBinding(
            const std::string_view opaqueId) noexcept
        {
            const auto found = std::find_if(
                cloudConflictRegistry.begin(),
                cloudConflictRegistry.end(),
                [&](const CloudConflictBinding& binding)
                {
                    return binding.opaqueId == opaqueId;
                });
            return found == cloudConflictRegistry.end()
                ? nullptr
                : &*found;
        }

        void EraseCloudConflictBinding(
            const std::string_view opaqueId) noexcept
        {
            const auto found = std::find_if(
                cloudConflictRegistry.begin(),
                cloudConflictRegistry.end(),
                [&](const CloudConflictBinding& binding)
                {
                    return binding.opaqueId == opaqueId;
                });
            if (found != cloudConflictRegistry.end())
            {
                EraseSecret(found->descriptor.expectedMutationId);
                cloudConflictRegistry.erase(found);
            }
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
            if (!EnsureCredentialUsageLease())
            {
                return false;
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
            if (!EnsureCredentialUsageLease())
            {
                return false;
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

        [[nodiscard]] bool DeleteRefreshTokenTracked() noexcept
        {
            localRefreshTokenDeleteFailed =
                !DeleteRefreshToken();
            return !localRefreshTokenDeleteFailed;
        }

        [[nodiscard]] bool ResolvePendingRefreshTokenDelete() noexcept
        {
            return !localRefreshTokenDeleteFailed
                || DeleteRefreshTokenTracked();
        }

        [[nodiscard]] bool EnsureCredentialUsageLease() noexcept
        {
            if (!refreshTokenStore || credentialUsageLeaseHeld)
            {
                return true;
            }
            try
            {
                const auto result = refreshTokenStore->AcquireUsageLease();
                if (!result.succeeded)
                {
                    return false;
                }
                credentialUsageLeaseHeld = true;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        void ReleaseCredentialUsageLeaseIfSafe() noexcept
        {
            if (!credentialUsageLeaseHeld || !refreshTokenStore
                || inFlight || awaitingCancelledTask
                || !pendingLogoutAccessToken.empty()
                || localRefreshTokenDeleteFailed
                || state == OnlineAccountState::SignedIn
                || state == OnlineAccountState::RefreshingSession
                || state == OnlineAccountState::RestoringSession
                || state == OnlineAccountState::StartingSignIn
                || state == OnlineAccountState::WaitingForAuthorization
                || state == OnlineAccountState::PollingAuthorization
                || state == OnlineAccountState::SigningOut)
            {
                return;
            }
            refreshTokenStore->ReleaseUsageLease();
            credentialUsageLeaseHeld = false;
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
            mailbox->cleanupClient = client;
            if (credentialUsageLeaseHeld)
            {
                // ownerが先に破棄されてもrestore/refresh/logoutと遅延cleanupが
                // 終わるまでstore dtor（lease解放）を遅らせます。
                mailbox->usageLeaseStore = refreshTokenStore;
            }
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
                        const auto completedAt =
                            std::chrono::steady_clock::now();

                        bool cleanupAbandoned{};
                        std::shared_ptr<Detail::DiscordAuthClient>
                            cleanupClient;
                        try
                        {
                            std::scoped_lock lock(mailbox->mutex);
                            if (mailbox->abandoned)
                            {
                                cleanupAbandoned = true;
                                cleanupClient = mailbox->cleanupClient;
                            }
                            else
                            {
                                mailbox->result = std::move(result);
                                mailbox->failed = failed;
                                mailbox->completedAt = completedAt;
                                mailbox->completed = true;
                            }
                        }
                        catch (...)
                        {
                            // 認証情報を含む可能性がある例外本文は保存
                            // しません。通常到達しない割当失敗時も、可能
                            // なら失敗完了だけを通知します。
                            try
                            {
                                std::scoped_lock lock(mailbox->mutex);
                                if (mailbox->abandoned)
                                {
                                    cleanupAbandoned = true;
                                    cleanupClient =
                                        mailbox->cleanupClient;
                                }
                                else
                                {
                                    mailbox->failed = true;
                                    mailbox->completedAt = completedAt;
                                    mailbox->completed = true;
                                }
                            }
                            catch (...)
                            {
                            }
                        }
                        if (cleanupAbandoned)
                        {
                            AsyncMailbox::CleanupAbandonedResult(
                                mailbox->kind,
                                failed,
                                cleanupClient,
                                result);
                        }
                        else
                        {
                            EraseAsyncResult(result);
                        }
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

        [[nodiscard]] OnlinePlayerProfile MakePublicProfile(
            const Detail::OnlinePlayerProfile& source) const
        {
            return {
                source.playerId,
                source.displayName,
                source.avatarUrl,
                source.linkedProvider
            };
        }

        void DetachAccountPersistence() noexcept
        {
            // UIへ渡したopaque IDはsign-out/Detach開始時点で失効します。
            ClearCloudConflictRegistry();
            if (!persistenceCoordinator)
            {
                return;
            }
            const auto result =
                persistenceCoordinator->DetachToGuest();
            accountPersistenceSaveFailed =
                result
                    == Detail::OnlinePersistenceDetachResult::
                        QuarantinedAccount
                || accountPersistenceSaveFailed;
        }

        void ConsumeCloudSaveSignals(const float elapsedSeconds) noexcept
        {
            if (!persistenceCoordinator)
            {
                return;
            }
            if (persistenceCoordinator->ConsumeCloudSaveHealthySignal())
            {
                cloudUnauthorizedRefreshAttempts = 0u;
                cloudUnauthorizedRefreshCooldownSeconds = 0.0f;
                cloudUnauthorizedRefreshQueued = false;
            }
            const bool unauthorized = persistenceCoordinator
                ->ConsumeCloudSaveUnauthorizedSignal();
            if (unauthorized
                && (state == OnlineAccountState::SignedIn
                    || state == OnlineAccountState::RefreshingSession))
            {
                if (cloudUnauthorizedRefreshAttempts == 0u)
                {
                    // 系列の最初だけ即時refreshします。既にproactive refresh中
                    // なら、その要求を系列の1回目として数えます。
                    cloudUnauthorizedRefreshAttempts = 1u;
                    if (state == OnlineAccountState::SignedIn)
                    {
                        cloudRefreshRequested = true;
                        sessionRefreshRetrySeconds = 0.0f;
                    }
                }
                else if (!cloudUnauthorizedRefreshQueued)
                {
                    constexpr float MaximumCooldownSeconds = 300.0f;
                    const auto shift = (std::min)(
                        cloudUnauthorizedRefreshAttempts - 1u,
                        6u);
                    cloudUnauthorizedRefreshCooldownSeconds =
                        (std::min)(
                            MaximumCooldownSeconds,
                            5.0f * static_cast<float>(1u << shift));
                    cloudUnauthorizedRefreshQueued = true;
                }
            }

            if (!cloudUnauthorizedRefreshQueued)
            {
                return;
            }
            cloudUnauthorizedRefreshCooldownSeconds = (std::max)(
                0.0f,
                cloudUnauthorizedRefreshCooldownSeconds - elapsedSeconds);
            if (cloudUnauthorizedRefreshCooldownSeconds <= 0.0f
                && state == OnlineAccountState::SignedIn)
            {
                cloudUnauthorizedRefreshQueued = false;
                cloudRefreshRequested = true;
                sessionRefreshRetrySeconds = 0.0f;
                if (cloudUnauthorizedRefreshAttempts
                    < (std::numeric_limits<std::uint32_t>::max)())
                {
                    ++cloudUnauthorizedRefreshAttempts;
                }
            }
        }

        [[nodiscard]] static float RemainingReceivedSessionSeconds(
            const Detail::OnlineSession& receivedSession,
            const std::chrono::steady_clock::time_point
                completedAt) noexcept
        {
            const auto ageSeconds = std::max(
                0.0,
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now()
                    - completedAt).count());
            return static_cast<float>(std::max(
                0.0,
                static_cast<double>(
                    receivedSession.expiresInSeconds)
                    - ageSeconds));
        }

        [[nodiscard]] bool PublishSession(
            Detail::OnlineSession& nextSession,
            OnlinePlayerProfile nextPlayer,
            const std::chrono::steady_clock::time_point
                completedAt) noexcept
        {
            // HTTP完了後にmain threadが停止・suspendしていた時間もtokenの
            // 有効時間です。公開直前に再計算し、既に失効したsessionは
            // 呼出し側のrevoke経路へ返します。
            const auto remainingSeconds =
                RemainingReceivedSessionSeconds(
                    nextSession,
                    completedAt);
            if (remainingSeconds <= 0.0f)
            {
                return false;
            }
            ClearSession();
            session.accessToken.swap(nextSession.accessToken);
            session.refreshToken.swap(nextSession.refreshToken);
            session.expiresInSeconds = std::exchange(
                nextSession.expiresInSeconds,
                0);
            session.player = std::move(nextSession.player);
            nextSession.player = {};
            player = std::move(nextPlayer);
            sessionRemainingSeconds = remainingSeconds;
            sessionRefreshLeadSeconds = std::clamp(
                sessionRemainingSeconds * 0.2f,
                5.0f,
                60.0f);
            sessionRefreshRetrySeconds = 0.0f;
            ClearLoginTransaction();
            state = OnlineAccountState::SignedIn;
            ClearError();
            return true;
        }

        void RejectReceivedSession(
            Detail::OnlineSession& rejectedSession,
            const std::string_view code,
            const bool deleteStoredCredential)
        {
            AdvanceGeneration();
            DetachAccountPersistence();
            if (deleteStoredCredential)
            {
                static_cast<void>(DeleteRefreshTokenTracked());
            }

            ClearLoginTransaction();
            ClearSession();
            player = {};
            EraseSecret(pendingLogoutAccessToken);
            pendingLogoutAccessToken.swap(
                rejectedSession.accessToken);
            EraseSession(rejectedSession);
            awaitingCancelledTask = false;
            cancelledPollExpired = false;
            preserveErrorAfterLogout = true;
            accountPersistenceSaveFailed = false;
            cloudRefreshRequested = false;
            SetFixedError(code);
            state = OnlineAccountState::SigningOut;
            if (pendingLogoutAccessToken.empty())
            {
                CompleteLogout(false);
                return;
            }
            TickPendingLogout();
        }

        void AdoptInitialSession(
            Detail::OnlineSession& nextSession,
            const bool restoring,
            const std::chrono::steady_clock::time_point
                completedAt)
        {
            if (RemainingReceivedSessionSeconds(
                    nextSession,
                    completedAt) <= 0.0f)
            {
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    restoring);
                return;
            }
            OnlinePlayerProfile nextPlayer;
            try
            {
                nextPlayer = MakePublicProfile(nextSession.player);
                if (persistenceCoordinator
                    && persistenceCoordinator->IsNamespaceEnabled())
                {
                    auto prepared =
                        persistenceCoordinator->PrepareAccount(
                            nextSession.player.playerId,
                            nextSession.accessToken);
                    if (!persistenceCoordinator->CommitPrepared(
                            std::move(prepared)))
                    {
                        throw std::runtime_error(
                            "Prepared persistence transaction became stale.");
                    }
                    ClearCloudConflictRegistry();
                }
            }
            catch (...)
            {
                RejectReceivedSession(
                    nextSession,
                    "persistence_activation_failed",
                    restoring);
                return;
            }

            if (RemainingReceivedSessionSeconds(
                    nextSession,
                    completedAt) <= 0.0f)
            {
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    restoring);
                return;
            }

            // refresh tokenはaccount profileがactiveになった後だけ保存します。
            // 保存の成否が曖昧な場合はguestへ戻し、store削除とsession失効を
            // 行って、次回起動に半端なsessionを残しません。
            if (!SaveRefreshToken(nextSession.refreshToken))
            {
                RejectReceivedSession(
                    nextSession,
                    "credential_save_failed",
                    true);
                return;
            }
            if (!PublishSession(
                    nextSession,
                    std::move(nextPlayer),
                    completedAt))
            {
                // Save成功後に期限切れとなった場合は、今保存した資格情報も
                // rollbackして受領sessionを失効します。
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    true);
                return;
            }
        }

        void AdoptRefreshedSession(
            Detail::OnlineSession& nextSession,
            const std::chrono::steady_clock::time_point
                completedAt)
        {
            if (nextSession.player.playerId != session.player.playerId)
            {
                RejectReceivedSession(
                    nextSession,
                    "account_identity_changed",
                    true);
                return;
            }

            OnlinePlayerProfile nextPlayer;
            try
            {
                nextPlayer = MakePublicProfile(nextSession.player);
            }
            catch (...)
            {
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    true);
                return;
            }
            if (RemainingReceivedSessionSeconds(
                    nextSession,
                    completedAt) <= 0.0f)
            {
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    true);
                return;
            }
            if (!SaveRefreshToken(nextSession.refreshToken))
            {
                RejectReceivedSession(
                    nextSession,
                    "credential_save_failed",
                    true);
                return;
            }
            try
            {
                if (persistenceCoordinator
                    && persistenceCoordinator->Synchronizer()
                    && persistenceCoordinator->IsAccountActive())
                {
                    // 新access tokenをcloud fenceへ先に適用します。ここで
                    // allocation/validationが失敗したsessionは公開しません。
                    persistenceCoordinator->UpdateCloudSaveAccessToken(
                        nextSession.accessToken);
                }
            }
            catch (...)
            {
                RejectReceivedSession(
                    nextSession,
                    "persistence_activation_failed",
                    true);
                return;
            }
            if (!PublishSession(
                    nextSession,
                    std::move(nextPlayer),
                    completedAt))
            {
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    true);
                return;
            }
            cloudRefreshRequested = false;
        }

        void RestoreStoredSession()
        {
            if (!client || !refreshTokenStore)
            {
                return;
            }
            if (!EnsureCredentialUsageLease())
            {
                SetFixedError("credential_store_unavailable");
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
                if (DeleteRefreshTokenTracked())
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
                if (DeleteRefreshTokenTracked())
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
            Detail::DiscordLoginPollResult& result,
            const std::chrono::steady_clock::time_point
                completedAt)
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
                AdoptInitialSession(
                    result.session,
                    false,
                    completedAt);
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
            const bool restoring,
            const std::chrono::steady_clock::time_point
                completedAt)
        {
            if (result.Succeeded())
            {
                if (restoring)
                {
                    AdoptInitialSession(
                        result.session,
                        true,
                        completedAt);
                }
                else
                {
                    AdoptRefreshedSession(
                        result.session,
                        completedAt);
                }
                return;
            }

            if (InvalidRefreshTokenError(result.errorCode))
            {
                const bool deleted = DeleteRefreshTokenTracked();
                if (!restoring)
                {
                    DetachAccountPersistence();
                }
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
            DetachAccountPersistence();
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
                localRefreshTokenDeleteFailed;
            const bool persistenceSaveFailed =
                std::exchange(accountPersistenceSaveFailed, false);
            const bool preserveFailure =
                std::exchange(preserveErrorAfterLogout, false);
            if (preserveFailure)
            {
                // failed adoptionの主原因をlogout成否で上書きしません。
                state = OnlineAccountState::Error;
                return;
            }
            state = returnToExpiredLoginError
                ? OnlineAccountState::Error
                : OnlineAccountState::SignedOut;
            if (deleteFailed)
            {
                SetFixedError("credential_delete_failed");
            }
            else if (persistenceSaveFailed)
            {
                SetFixedError("account_save_failed");
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
            AsyncResult& result,
            const std::chrono::steady_clock::time_point
                completedAt)
        {
            switch (kind)
            {
            case TaskKind::StartLogin:
                CompleteLoginStart(result.loginStart);
                return;
            case TaskKind::PollLogin:
                CompleteLoginPoll(result.loginPoll, completedAt);
                return;
            case TaskKind::RestoreSession:
                CompleteSessionRefresh(
                    result.sessionRefresh,
                    true,
                    completedAt);
                return;
            case TaskKind::RefreshSession:
                CompleteSessionRefresh(
                    result.sessionRefresh,
                    false,
                    completedAt);
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
                    // worker例外はserver応答後のparse/allocation失敗も
                    // 含みrotation成否が曖昧なため、旧資格情報を残しません。
                    static_cast<void>(DeleteRefreshTokenTracked());
                    DetachAccountPersistence();
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
            std::chrono::steady_clock::time_point completedAt{};
            bool failed{};
            {
                std::scoped_lock lock(mailbox->mutex);
                if (!mailbox->completed)
                {
                    return false;
                }
                kind = mailbox->kind;
                taskGeneration = mailbox->generation;
                completedAt = mailbox->completedAt;
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
                        CompleteTask(kind, result, completedAt);
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

        void ExpireRefreshingSession()
        {
            DetachAccountPersistence();
            // refresh中はserver側でrotation済みか判別できないため、
            // 旧refresh tokenを次回起動へ残しません。
            static_cast<void>(DeleteRefreshTokenTracked());
            awaitingCancelledTask = inFlight != nullptr;
            cancelledTaskKind = TaskKind::RefreshSession;
            cancelledPollExpired = false;
            AdvanceGeneration();
            EraseSecret(pendingLogoutAccessToken);
            pendingLogoutAccessToken.swap(session.accessToken);
            ClearSession();
            ClearLoginTransaction();
            player = {};
            preserveErrorAfterLogout = true;
            SetFixedError("request_failed");
            state = OnlineAccountState::SigningOut;
            if (pendingLogoutAccessToken.empty()
                && !awaitingCancelledTask)
            {
                CompleteLogout(false);
                return;
            }
            TickPendingLogout();
        }

        void TickSession(const float elapsedSeconds)
        {
            if (state == OnlineAccountState::RefreshingSession)
            {
                sessionRemainingSeconds = std::max(
                    0.0f,
                    sessionRemainingSeconds - elapsedSeconds);
                if (sessionRemainingSeconds <= 0.0f)
                {
                    ExpireRefreshingSession();
                }
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
                || (!cloudRefreshRequested
                    && sessionRemainingSeconds
                        > sessionRefreshLeadSeconds))
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
                    DetachAccountPersistence();
                    ClearSession();
                    player = {};
                    state = OnlineAccountState::Error;
                }
                else if (cloudRefreshRequested)
                {
                    sessionRefreshRetrySeconds = (std::min)(
                        5.0f,
                        sessionRemainingSeconds);
                }
                SetFixedError("request_failed");
                return;
            }
            state = OnlineAccountState::RefreshingSession;
            if (sessionRemainingSeconds <= 0.0f)
            {
                ExpireRefreshingSession();
            }
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
        std::shared_ptr<const Detail::CloudSaveClient> cloudSaveClient;
        std::shared_ptr<AsyncMailbox> inFlight;
        std::unique_ptr<Detail::OnlinePersistenceCoordinator>
            persistenceCoordinator;
        std::vector<CloudConflictBinding> cloudConflictRegistry;
        std::shared_ptr<Detail::IRefreshTokenStore> refreshTokenStore;
        std::unique_ptr<Detail::IAuthorizationLauncher>
            authorizationLauncher;
        Detail::DiscordLoginTransaction loginTransaction;
        Detail::OnlineSession session;
        OnlinePlayerProfile player;
        std::string pendingLogoutAccessToken;
        std::string authorizationUrl;
        std::string errorCode;
        std::string errorMessage;
        std::string configuredGameId;
        std::string configuredEnvironmentId;
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
        bool accountPersistenceSaveFailed{};
        bool preserveErrorAfterLogout{};
        bool awaitingCancelledTask{};
        bool cancelledPollExpired{};
        bool cloudRefreshRequested{};
        std::uint32_t cloudUnauthorizedRefreshAttempts{};
        float cloudUnauthorizedRefreshCooldownSeconds{};
        bool cloudUnauthorizedRefreshQueued{};
        bool credentialUsageLeaseHeld{};
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
        if (implementation.persistenceCoordinator
            && implementation.persistenceCoordinator
                ->HasPendingRecovery())
        {
            // empty configurationを含め、binding材料や資格情報へ触れる前に
            // fail-closedで拒否します。
            throw std::logic_error(
                "Resolve pending persistence recovery before configuring.");
        }
        if (implementation.IsBusy()
            || implementation.inFlight
            || implementation.state == OnlineAccountState::SignedIn
            || (implementation.persistenceCoordinator
                && implementation.persistenceCoordinator
                    ->IsAccountActive()))
        {
            throw std::logic_error(
                "Sign out before changing the online service configuration.");
        }
        if (!implementation.ResolvePendingRefreshTokenDelete())
        {
            throw std::runtime_error(
                "Pending refresh credential could not be deleted.");
        }
        implementation.ReleaseCredentialUsageLeaseIfSafe();
        if (implementation.credentialUsageLeaseHeld)
        {
            throw std::runtime_error(
                "Credential usage lease could not be released.");
        }

        auto nextGameId = configuration.gameId;
        auto nextEnvironmentId = configuration.environmentId;
        std::shared_ptr<Detail::DiscordAuthClient> nextClient;
        std::shared_ptr<const Detail::CloudSaveClient> nextCloudSaveClient;
        // unique_ptr -> shared_ptr のcontrol block割当までnamespace commit前に
        // 完了し、commit後をnoexcept moveだけにします。
        std::shared_ptr<Detail::IRefreshTokenStore> nextStore;
        std::unique_ptr<Detail::IAuthorizationLauncher> nextLauncher;
        if (!configuration.serviceBaseUrl.empty())
        {
            nextClient = std::make_shared<Detail::DiscordAuthClient>(
                std::move(configuration.serviceBaseUrl),
                configuration.allowInsecureLoopback,
                implementation.senderOverride,
                nextGameId,
                nextEnvironmentId);
            if (!nextGameId.empty())
            {
                // auth/cloud両clientを完全に構築してからCoordinatorと公開
                // configurationを切り替え、片方だけ更新される状態を作りません。
                nextCloudSaveClient =
                    std::make_shared<Detail::CloudSaveClient>(
                        nextClient->ServiceBaseUrl(),
                        nextGameId,
                        nextEnvironmentId,
                        configuration.allowInsecureLoopback,
                        implementation.senderOverride);
            }
            if (implementation.useWindowsPlatformDefaults)
            {
                if (!nextGameId.empty())
                {
                    nextStore = Detail::MakeWindowsRefreshTokenStore(
                        nextGameId,
                        nextEnvironmentId);
                }
                nextLauncher =
                    Detail::MakeWindowsAuthorizationLauncher();
            }
        }
        else
        {
            nextGameId.clear();
            nextEnvironmentId.clear();
        }

        if (implementation.persistenceCoordinator)
        {
            if (nextClient && !nextGameId.empty())
            {
                implementation.persistenceCoordinator->ConfigureNamespace(
                    nextGameId,
                    nextEnvironmentId,
                    nextClient->ServiceBaseUrl(),
                    configuration.allowInsecureLoopback,
                    nextCloudSaveClient);
            }
            else
            {
                implementation.persistenceCoordinator->DisableNamespace();
            }
        }

        // Coordinator namespace commitが成功した後だけ旧profile由来の
        // process-local conflict IDを失効させます。失敗したConfigureでは
        // registryを維持します。
        implementation.ClearCloudConflictRegistry();

        implementation.AdvanceGeneration();
        implementation.ClearLoginTransaction();
        implementation.ClearSession();
        EraseSecret(implementation.pendingLogoutAccessToken);
        implementation.player = {};
        implementation.ClearError();
        implementation.localRefreshTokenDeleteFailed = false;
        implementation.accountPersistenceSaveFailed = false;
        implementation.preserveErrorAfterLogout = false;
        implementation.awaitingCancelledTask = false;
        implementation.cancelledPollExpired = false;
        implementation.cloudRefreshRequested = false;
        implementation.cloudUnauthorizedRefreshAttempts = 0u;
        implementation.cloudUnauthorizedRefreshCooldownSeconds = 0.0f;
        implementation.cloudUnauthorizedRefreshQueued = false;
        implementation.allowInsecureLoopback =
            configuration.allowInsecureLoopback;
        implementation.openAuthorizationBrowser =
            configuration.openAuthorizationBrowser;
        if (implementation.useWindowsPlatformDefaults)
        {
            implementation.refreshTokenStore = std::move(nextStore);
            implementation.credentialUsageLeaseHeld = false;
            implementation.authorizationLauncher =
                std::move(nextLauncher);
        }
        implementation.client = std::move(nextClient);
        implementation.cloudSaveClient = std::move(nextCloudSaveClient);
        implementation.configuredGameId.swap(nextGameId);
        implementation.configuredEnvironmentId.swap(nextEnvironmentId);
        implementation.cloudRefreshRequested = false;
        implementation.cloudUnauthorizedRefreshAttempts = 0u;
        implementation.cloudUnauthorizedRefreshCooldownSeconds = 0.0f;
        implementation.cloudUnauthorizedRefreshQueued = false;
        implementation.state = implementation.client
            ? OnlineAccountState::SignedOut
            : OnlineAccountState::Unconfigured;
        implementation.RestoreStoredSession();
        implementation.ReleaseCredentialUsageLeaseIfSafe();
    }

    void OnlineServices::Update(float elapsedSeconds)
    {
        auto& implementation = *m_implementation;
        if (!std::isfinite(elapsedSeconds)
            || elapsedSeconds < 0.0f)
        {
            elapsedSeconds = 0.0f;
        }
        implementation.ConsumeCloudSaveSignals(elapsedSeconds);
        const bool refreshElapsedApplied =
            implementation.state
                == OnlineAccountState::RefreshingSession;
        if (refreshElapsedApplied)
        {
            // 完了mailboxをreapする前に旧access tokenの残存時間へ
            // このframe分を一度だけ適用します。成功ならPublishSessionが
            // 新期限へ置換し、失敗なら0を見て同じframeでfail-closedに
            // できます。
            implementation.sessionRemainingSeconds = std::max(
                0.0f,
                implementation.sessionRemainingSeconds
                    - elapsedSeconds);
        }
        if (implementation.ReapCompletedTask())
        {
            implementation.ReleaseCredentialUsageLeaseIfSafe();
            // 完了によって遷移したばかりの状態へ、前状態で経過した
            // elapsedSecondsを同じフレーム中に適用しません。
            return;
        }
        implementation.TickLogin(elapsedSeconds);
        implementation.TickSession(
            refreshElapsedApplied ? 0.0f : elapsedSeconds);
        implementation.TickPendingLogout();
        implementation.ReleaseCredentialUsageLeaseIfSafe();
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
        if (implementation.persistenceCoordinator
            && implementation.persistenceCoordinator
                ->HasPendingRecovery())
        {
            // credential lease取得やHTTP worker起動より前に拒否します。
            implementation.SetFixedError(
                "persistence_recovery_required");
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
        if (!implementation.EnsureCredentialUsageLease())
        {
            implementation.SetFixedError(
                "credential_store_unavailable");
            return false;
        }
        if (!implementation.ResolvePendingRefreshTokenDelete())
        {
            implementation.SetFixedError(
                "credential_delete_failed");
            return false;
        }

        implementation.AdvanceGeneration();
        implementation.ClearLoginTransaction();
        implementation.ClearSession();
        implementation.player = {};
        implementation.ClearError();
        implementation.localRefreshTokenDeleteFailed = false;
        implementation.accountPersistenceSaveFailed = false;
        implementation.preserveErrorAfterLogout = false;
        implementation.awaitingCancelledTask = false;
        implementation.cancelledPollExpired = false;
        implementation.cloudRefreshRequested = false;
        implementation.cloudUnauthorizedRefreshAttempts = 0u;
        implementation.cloudUnauthorizedRefreshCooldownSeconds = 0.0f;
        implementation.cloudUnauthorizedRefreshQueued = false;
        implementation.state = OnlineAccountState::StartingSignIn;
        if (!implementation.LaunchLoginStart())
        {
            implementation.SetError("request_failed", true);
            implementation.ReleaseCredentialUsageLeaseIfSafe();
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
        implementation.ReleaseCredentialUsageLeaseIfSafe();
    }

    void OnlineServices::SignOut()
    {
        auto& implementation = *m_implementation;
        implementation.cloudRefreshRequested = false;
        implementation.cloudUnauthorizedRefreshAttempts = 0u;
        implementation.cloudUnauthorizedRefreshCooldownSeconds = 0.0f;
        implementation.cloudUnauthorizedRefreshQueued = false;
        if (implementation.state != OnlineAccountState::SigningOut)
        {
            implementation.AdvanceGeneration();
        }
        implementation.DetachAccountPersistence();
        // failed adoption cleanup中でも、明示操作は最終SignedOutを優先します。
        implementation.preserveErrorAfterLogout = false;
        if (implementation.state == OnlineAccountState::SigningOut)
        {
            // 内部cleanup中に明示SignOutされた場合も、端末tokenの削除を
            // 必ず試します。前回失敗していればこの呼び出しが再試行です。
            static_cast<void>(
                implementation.DeleteRefreshTokenTracked());
            implementation.cancelledPollExpired = false;
            implementation.ClearError();
            if (implementation.localRefreshTokenDeleteFailed)
            {
                implementation.SetFixedError(
                    "credential_delete_failed");
            }
            implementation.ReleaseCredentialUsageLeaseIfSafe();
            return;
        }

        // 通信の完了を待たず、端末の再ログイン情報は
        // この呼び出し中に削除します。
        static_cast<void>(
            implementation.DeleteRefreshTokenTracked());
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
            implementation.ReleaseCredentialUsageLeaseIfSafe();
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
            implementation.ReleaseCredentialUsageLeaseIfSafe();
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
            implementation.ReleaseCredentialUsageLeaseIfSafe();
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
            implementation.ReleaseCredentialUsageLeaseIfSafe();
            return;
        }
        implementation.TickPendingLogout();
        implementation.ReleaseCredentialUsageLeaseIfSafe();
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

    OnlineCloudSyncStatus OnlineServices::CloudSyncStatus() const noexcept
    {
        const auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator
            || !implementation.persistenceCoordinator->IsAccountActive())
        {
            return {};
        }
        auto* const synchronizer =
            implementation.persistenceCoordinator->Synchronizer();
        if (!synchronizer || !synchronizer->IsAttached())
        {
            return {};
        }

        const auto internal = synchronizer->Status();
        OnlineCloudSyncStatus result;
        switch (internal.state)
        {
        case Detail::CloudSaveSynchronizerState::Detached:
            result.state = OnlineCloudSyncState::Unavailable;
            break;
        case Detail::CloudSaveSynchronizerState::Idle:
            result.state = OnlineCloudSyncState::Idle;
            break;
        case Detail::CloudSaveSynchronizerState::Synchronizing:
            result.state = OnlineCloudSyncState::Synchronizing;
            break;
        case Detail::CloudSaveSynchronizerState::BackingOff:
            result.state = OnlineCloudSyncState::WaitingToRetry;
            break;
        case Detail::CloudSaveSynchronizerState::Conflict:
            result.state = OnlineCloudSyncState::Conflict;
            break;
        case Detail::CloudSaveSynchronizerState::Unauthorized:
            result.state = OnlineCloudSyncState::Unauthorized;
            break;
        case Detail::CloudSaveSynchronizerState::Halted:
            result.state = OnlineCloudSyncState::Stopped;
            break;
        }
        switch (internal.stopReason)
        {
        case Detail::CloudSaveSynchronizerStopReason::None:
            result.stopReason = OnlineCloudSyncStopReason::None;
            break;
        case Detail::CloudSaveSynchronizerStopReason::LocalUnavailable:
            result.stopReason = OnlineCloudSyncStopReason::LocalUnavailable;
            break;
        case Detail::CloudSaveSynchronizerStopReason::LocalCorrupt:
            result.stopReason = OnlineCloudSyncStopReason::LocalCorrupt;
            break;
        case Detail::CloudSaveSynchronizerStopReason::RemoteRejected:
            result.stopReason = OnlineCloudSyncStopReason::RemoteRejected;
            break;
        case Detail::CloudSaveSynchronizerStopReason::InvalidRemoteResponse:
            result.stopReason =
                OnlineCloudSyncStopReason::InvalidRemoteResponse;
            break;
        case Detail::CloudSaveSynchronizerStopReason::JournalFailure:
            result.stopReason = OnlineCloudSyncStopReason::JournalFailure;
            break;
        case Detail::CloudSaveSynchronizerStopReason::InternalFailure:
            result.stopReason = OnlineCloudSyncStopReason::InternalFailure;
            break;
        }
        const auto now = SteadyMilliseconds();
        if (internal.retryAtMilliseconds > now)
        {
            const auto remainingMilliseconds =
                internal.retryAtMilliseconds - now;
            result.retryAfterSeconds = static_cast<float>(
                static_cast<double>(remainingMilliseconds) / 1000.0);
        }
        result.conflictCount = internal.conflictCount;
        return result;
    }

    std::vector<OnlineCloudConflict>
        OnlineServices::CloudConflicts() const
    {
        try
        {
            return m_implementation->RefreshCloudConflictRegistry();
        }
        catch (...)
        {
            // path/token/ETag/mutation IDを含み得る内部例外を公開しません。
            throw std::runtime_error(
                "Cloud conflict enumeration failed.");
        }
    }

    OnlinePersistenceOperationResult
        OnlineServices::RequestCloudSync() noexcept
    {
        auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator
            || !implementation.persistenceCoordinator->IsAccountActive())
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        auto* const synchronizer =
            implementation.persistenceCoordinator->Synchronizer();
        if (!synchronizer || !synchronizer->IsAttached())
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        if (implementation.IsBusy() || implementation.inFlight)
        {
            return OnlinePersistenceOperationResult::Busy;
        }
        if (synchronizer->Status().state
            == Detail::CloudSaveSynchronizerState::Halted)
        {
            return OnlinePersistenceOperationResult::Failed;
        }
        try
        {
            synchronizer->RequestReconcile();
            return OnlinePersistenceOperationResult::Succeeded;
        }
        catch (...)
        {
            return OnlinePersistenceOperationResult::Failed;
        }
    }

    OnlinePersistenceOperationResult
        OnlineServices::ResolveCloudConflict(
        const std::string_view conflictId,
        const OnlineCloudConflictResolution resolution) noexcept
    {
        auto& implementation = *m_implementation;
        auto* const binding =
            implementation.FindConflictBinding(conflictId);
        if (!binding
            || !implementation.persistenceCoordinator
            || !implementation.persistenceCoordinator->IsAccountActive())
        {
            return OnlinePersistenceOperationResult::Stale;
        }
        auto* const synchronizer =
            implementation.persistenceCoordinator->Synchronizer();
        if (!synchronizer || !synchronizer->IsAttached())
        {
            return OnlinePersistenceOperationResult::Stale;
        }

        try
        {
            // stale判定をwire/auth Busyより先に行います。
            const auto currentConflicts = synchronizer->Conflicts();
            const auto current = std::find_if(
                currentConflicts.begin(),
                currentConflicts.end(),
                [&](const Detail::CloudSaveConflictDescriptor& descriptor)
                {
                    return Implementation::SameConflictIdentity(
                        *binding,
                        implementation.persistenceCoordinator
                            ->ProfileEpoch(),
                        descriptor);
                });
            if (current == currentConflicts.end())
            {
                implementation.EraseCloudConflictBinding(conflictId);
                return OnlinePersistenceOperationResult::Stale;
            }
            if (implementation.IsBusy()
                || implementation.inFlight
                || synchronizer->HasInFlightRequest())
            {
                return OnlinePersistenceOperationResult::Busy;
            }

            Detail::CloudSaveConflictResolution internalResolution;
            switch (resolution)
            {
            case OnlineCloudConflictResolution::UseLocal:
                internalResolution =
                    Detail::CloudSaveConflictResolution::RetryLocal;
                break;
            case OnlineCloudConflictResolution::UseRemote:
                internalResolution =
                    Detail::CloudSaveConflictResolution::UseRemote;
                break;
            default:
                return OnlinePersistenceOperationResult::Failed;
            }
            // Resolve中のvector変更に備え、参照は呼出し後に使いません。
            const auto resource = binding->descriptor.resource;
            const auto mutationId =
                binding->descriptor.expectedMutationId;
            synchronizer->ResolveConflict(
                resource,
                mutationId,
                internalResolution);
            implementation.EraseCloudConflictBinding(conflictId);
            return OnlinePersistenceOperationResult::Succeeded;
        }
        catch (...)
        {
            return OnlinePersistenceOperationResult::Failed;
        }
    }

    OnlinePersistenceRecoveryStatus
        OnlineServices::PersistenceRecoveryStatus() const noexcept
    {
        const auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator)
        {
            return {};
        }
        const auto internal =
            implementation.persistenceCoordinator->RecoveryStatus();
        OnlinePersistenceRecoveryStatus result;
        result.revision = internal.revision;
        switch (internal.state)
        {
        case Detail::OnlinePersistenceRecoveryState::None:
            result.state = OnlinePersistenceRecoveryState::None;
            result.revision = 0;
            break;
        case Detail::OnlinePersistenceRecoveryState::MemorySnapshot:
            result.state = OnlinePersistenceRecoveryState::MemorySnapshot;
            break;
        case Detail::OnlinePersistenceRecoveryState::DurableSidecar:
            result.state = OnlinePersistenceRecoveryState::DurableSidecar;
            break;
        case Detail::OnlinePersistenceRecoveryState::UnavailableSidecar:
            result.state =
                OnlinePersistenceRecoveryState::UnavailableSidecar;
            break;
        }
        return result;
    }

    namespace
    {
        [[nodiscard]] OnlinePersistenceOperationResult MapRecoveryResult(
            const Detail::OnlinePersistenceRecoveryOperationResult result)
                noexcept
        {
            switch (result)
            {
            case Detail::OnlinePersistenceRecoveryOperationResult::Succeeded:
                return OnlinePersistenceOperationResult::Succeeded;
            case Detail::OnlinePersistenceRecoveryOperationResult::Unavailable:
                return OnlinePersistenceOperationResult::Unavailable;
            case Detail::OnlinePersistenceRecoveryOperationResult::Busy:
                return OnlinePersistenceOperationResult::Busy;
            case Detail::OnlinePersistenceRecoveryOperationResult::Stale:
                return OnlinePersistenceOperationResult::Stale;
            case Detail::OnlinePersistenceRecoveryOperationResult::Failed:
                return OnlinePersistenceOperationResult::Failed;
            }
            return OnlinePersistenceOperationResult::Failed;
        }
    }

    OnlinePersistenceOperationResult OnlineServices::RestorePersistence(
        const std::uint64_t expectedRevision) noexcept
    {
        auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator)
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        const auto status =
            implementation.persistenceCoordinator->RecoveryStatus();
        if (expectedRevision == 0
            || expectedRevision != status.revision)
        {
            return OnlinePersistenceOperationResult::Stale;
        }
        if (implementation.IsBusy() || implementation.inFlight)
        {
            return OnlinePersistenceOperationResult::Busy;
        }
        return MapRecoveryResult(
            implementation.persistenceCoordinator
                ->RestorePendingRecovery(expectedRevision));
    }

    OnlinePersistenceOperationResult OnlineServices::DiscardPersistence(
        const std::uint64_t expectedRevision) noexcept
    {
        auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator)
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        const auto status =
            implementation.persistenceCoordinator->RecoveryStatus();
        if (expectedRevision == 0
            || expectedRevision != status.revision)
        {
            return OnlinePersistenceOperationResult::Stale;
        }
        if (implementation.IsBusy() || implementation.inFlight)
        {
            return OnlinePersistenceOperationResult::Busy;
        }
        return MapRecoveryResult(
            implementation.persistenceCoordinator
                ->DiscardPendingRecovery(expectedRevision));
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

        bool OnlineServicesTestAccess::CurrentTaskCompleted(
            const OnlineServices& services) noexcept
        {
            const auto mailbox = services.m_implementation->inFlight;
            if (!mailbox)
            {
                return false;
            }
            try
            {
                std::scoped_lock lock(mailbox->mutex);
                return mailbox->completed;
            }
            catch (...)
            {
                return false;
            }
        }

        bool OnlineServicesTestAccess::AgeCurrentTaskCompletion(
            OnlineServices& services,
            const float elapsedSeconds) noexcept
        {
            if (!std::isfinite(elapsedSeconds)
                || elapsedSeconds < 0.0f
                || elapsedSeconds > 31536000.0f)
            {
                return false;
            }
            const auto mailbox = services.m_implementation->inFlight;
            if (!mailbox)
            {
                return false;
            }
            try
            {
                std::scoped_lock lock(mailbox->mutex);
                if (!mailbox->completed)
                {
                    return false;
                }
                mailbox->completedAt -=
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                            std::chrono::duration<float>(
                                elapsedSeconds));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        void OnlinePersistenceAccess::Attach(
            OnlineServices& services,
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::filesystem::path trustedUserDataDirectory)
        {
            auto& implementation = *services.m_implementation;
            if (implementation.persistenceCoordinator)
            {
                throw std::logic_error(
                    "Online persistence is already attached.");
            }
            if (implementation.IsBusy()
                || implementation.inFlight
                || implementation.state == OnlineAccountState::SignedIn)
            {
                throw std::logic_error(
                    "Attach online persistence before signing in.");
            }

            auto coordinator =
                std::make_unique<OnlinePersistenceCoordinator>(
                    preferences,
                    saves,
                    std::move(trustedUserDataDirectory));
            if (implementation.client
                && !implementation.configuredGameId.empty())
            {
                coordinator->ConfigureNamespace(
                    implementation.configuredGameId,
                    implementation.configuredEnvironmentId,
                    implementation.client->ServiceBaseUrl(),
                    implementation.allowInsecureLoopback,
                    implementation.cloudSaveClient);
            }
            implementation.persistenceCoordinator =
                std::move(coordinator);
        }

        OnlinePersistenceDetachResult OnlinePersistenceAccess::Detach(
            OnlineServices& services) noexcept
        {
            services.m_implementation->ClearCloudConflictRegistry();
            auto* const coordinator =
                services.m_implementation->persistenceCoordinator.get();
            return coordinator
                ? coordinator->DetachToGuest()
                : OnlinePersistenceDetachResult::AlreadyGuest;
        }

        void OnlinePersistenceAccess::EndFrame(
            OnlineServices& services) noexcept
        {
            if (auto* const coordinator =
                    services.m_implementation
                        ->persistenceCoordinator.get())
            {
                coordinator->EndFrame();
            }
        }

        OnlinePersistenceCoordinator*
            OnlinePersistenceAccess::Coordinator(
                OnlineServices& services) noexcept
        {
            return services.m_implementation
                ->persistenceCoordinator.get();
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
