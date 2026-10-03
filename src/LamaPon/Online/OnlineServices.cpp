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
    // volatile書き込みで秘密値の使用領域をゼロ化して空にする(value: 消去する文字列)。
    void EraseSecret(std::string& value) noexcept
    {
        // 文字列の使用領域をゼロ化してからclearします。
        volatile char* bytes = value.empty()
            ? nullptr
            : value.data();
        // 消去または16進変換の処理位置
        for (std::size_t index = 0;
            index < value.size();
            ++index)
        {
            bytes[index] = 0;
        }
        value.clear();
    }

    // access・refresh tokenを消去し公開前のセッション情報を空にする(session: 消去する認証結果)。
    void EraseSession(
        LamaPon::Detail::OnlineSession& session) noexcept
    {
        EraseSecret(session.accessToken);
        EraseSecret(session.refreshToken);
        session.expiresInSeconds = 0;
        session.player = {};
    }

    // 認証確認tokenを消去し認証取引を初期状態にする(transaction: 消去する認証取引)。
    void EraseTransaction(
        LamaPon::Detail::DiscordLoginTransaction& transaction) noexcept
    {
        EraseSecret(transaction.pollToken);
        transaction.transactionId.clear();
        transaction.authorizationUrl.clear();
        transaction.expiresInSeconds = 0;
        transaction.pollIntervalSeconds = 1;
    }

    // 公開してよい固定エラー識別子かを返す(code: 内部のエラー識別子)。
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

    // 許可外の診断識別子を固定のservice_errorへ置き換える(code: 内部のエラー識別子)。
    std::string PublicErrorCode(const std::string_view code)
    {
        return IsKnownErrorCode(code)
            ? std::string(code)
            : std::string("service_error");
    }

    // 固定エラー識別子に対応する表示用の日本語を返す(code: 公開用エラー識別子)。
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

    // 単調時計の現在時刻を0以上のミリ秒で返す。
    [[nodiscard]] std::uint64_t SteadyMilliseconds() noexcept
    {
        // 単調時計の経過ms
        const auto count = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return count > 0 ? static_cast<std::uint64_t>(count) : 0u;
    }

    // CNGでprocess内だけの128ビット競合IDを生成する。
    [[nodiscard]] std::string GenerateOpaqueConflictId()
    {
        // 公開用競合IDの16バイト乱数
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
        // 小文字の16進数字一覧
        static constexpr char Hex[] = "0123456789abcdef";
        // 公開用競合IDの小文字16進表記
        std::string result(randomBytes.size() * 2u, '0');
        // 消去または16進変換の処理位置
        for (std::size_t index = 0; index < randomBytes.size(); ++index)
        {
            result[index * 2u] = Hex[randomBytes[index] >> 4u];
            result[index * 2u + 1u] = Hex[randomBytes[index] & 0x0fu];
        }
        SecureZeroMemory(randomBytes.data(), randomBytes.size());
        return result;
    }

    // 保存済みtokenを削除すべき認証失敗かを返す(code: バックエンドの固定エラー識別子)。
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
        // 消去責任を持つ秘密値を複製する(value: 入力tokenの借用)。
        explicit SecretText(const std::string_view value)
            : text(value)
        {
        }

        // 保持する秘密値をゼロ化して破棄する。
        ~SecretText()
        {
            EraseSecret(text);
        }

        // 秘密値の複製を禁止する。
        SecretText(const SecretText&) = delete;
        // 秘密値のコピー代入を禁止する。
        SecretText& operator=(const SecretText&) = delete;

        // 消去責任を持つtokenの文字列
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
            // 認証開始の通信結果
            Detail::DiscordLoginStartResult loginStart;
            // 認証状態確認の通信結果
            Detail::DiscordLoginPollResult loginPoll;
            // セッション復元・更新の通信結果
            Detail::OnlineSessionResult sessionRefresh;
            // サーバーのセッション失効が成功か
            bool logoutSucceeded{};
        };

        struct AsyncMailbox final
        {
            // 認証通信の処理種別
            TaskKind kind{};
            // 古い結果を失効する接続世代
            std::uint64_t generation{};
            // 認証結果の受け渡し用排他ロック
            std::mutex mutex;
            // 非同期認証または公開APIの結果
            AsyncResult result;
            // 遅延セッション失効の共有client
            std::shared_ptr<Detail::DiscordAuthClient> cleanupClient;
            // 後処理まで利用ロックを保つstore
            std::shared_ptr<Detail::IRefreshTokenStore> usageLeaseStore;
            // 認証HTTPの完了時刻
            std::chrono::steady_clock::time_point completedAt{};
            // 認証workerが完了したか
            bool completed{};
            // 認証workerが例外で失敗したか
            bool failed{};
            // 所有サービスが結果を放棄したか
            bool abandoned{};

            // 新しく発行され失効処理が必要なセッションを借用する(taskKind: 処理種別, taskFailed: workerが失敗したか, taskResult: 通信結果)。
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

            // 放棄した受領セッションの失効を試し全tokenを消去する(taskKind: 処理種別, taskFailed: workerの失敗, authClient: 認証clientの共有所有先, taskResult: 後処理する通信結果)。
            static void CleanupAbandonedResult(
                const TaskKind taskKind,
                const bool taskFailed,
                const std::shared_ptr<Detail::DiscordAuthClient>&
                    authClient,
                AsyncResult& taskResult) noexcept
            {
                // 後処理で失効する受領セッション
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

            // 受領セッションの失効を別workerへ渡し利用ロックを保持する(taskKind: 処理種別, taskFailed: workerの失敗, authClient: 認証client, usageLeaseStore: 利用ロックを保持するstore, taskResult: 所有を移す通信結果)。
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

                // 後処理まで保持する認証結果
                std::shared_ptr<AsyncResult> cleanupResult;
                try
                {
                    cleanupResult = std::make_shared<AsyncResult>(
                        std::move(taskResult));
                    // 放棄した結果を失効して利用ロックを保つworker
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

            // 結果を放棄し完了済みなら受領セッションの後処理を開始する。
            void Abandon() noexcept
            {
                // 放棄時に引き取る完了結果
                AsyncResult completedResult;
                // 完了後の失効処理用クライアント
                std::shared_ptr<Detail::DiscordAuthClient>
                    completedClient;
                // 後処理完了までの資格情報ロック
                std::shared_ptr<Detail::IRefreshTokenStore>
                    completedUsageLeaseStore;
                // 完了した認証結果を後処理するか
                bool dispatchCleanup{};
                // 放棄する認証通信が失敗したか
                bool taskFailed{};
                try
                {
                    // 放棄と完了結果の引き取り用排他ロック
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

            // ownerよりworkerが長く存続した場合も受領結果のtokenを消去する。
            ~AsyncMailbox()
            {

                EraseTransaction(result.loginStart.transaction);
                EraseSession(result.loginPoll.session);
                EraseSession(result.sessionRefresh.session);
            }
        };

        // テスト用の通信・保存・URL起動境界を受け取る(sender: HTTP送信処理, store: 資格情報保存の所有先, launcher: URL起動処理の所有先, useWindowsDefaults: 標準の保存と起動を使うか)。
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

        // 通信を待たず結果を放棄し必要な資格情報削除とguest切替を行う。
        ~Implementation()
        {
            AdvanceGeneration();
            // workerと後処理は共有client・token・mailbox・利用ロックを所有し、ここでは通信の完了を待ちません。
            bool credentialDeleteAttempted{};
            if (localRefreshTokenDeleteFailed)
            {
                // 削除失敗の資格情報を次回起動へ残さないよう、owner破棄時にも再試行します。
                (void)DeleteRefreshTokenTracked();
                credentialDeleteAttempted = true;
            }
            if (inFlight)
            {
                // 破棄時に実行中の認証処理種別
                const auto taskKind = inFlight->kind;
                if ((taskKind == TaskKind::RestoreSession
                        || taskKind == TaskKind::RefreshSession)
                    && state != OnlineAccountState::SigningOut
                    && !credentialDeleteAttempted)
                {
                    // 更新の成否がowner破棄後に確定する場合は旧資格情報を削除し、明示SignOut済みなら重複削除しません。
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

        // 古い認証結果を失効させる世代を0以外へ進める。
        void AdvanceGeneration() noexcept
        {
            ++generation;
            if (generation == 0)
            {
                ++generation;
            }
        }

        // 認証・復元・更新・サインアウトが進行中かを返す。
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
            // process内だけで有効な公開用競合ID
            std::string opaqueId;
            // 公開するアカウントの識別世代
            std::uint64_t profileEpoch{};
            // 内部の競合識別値と管理情報
            Detail::CloudSaveConflictDescriptor descriptor;
        };

        // 共通のスロット名規則で保存先を照合する(left: 比較元の保存先, right: 比較先の保存先)。
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

        // 公開先と保存先と更新IDで競合の同一性を照合する(binding: 公開用IDとの対応記録, profileEpoch: 現在の公開先の世代, descriptor: 現在の内部競合)。
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

        // 内部の更新IDを消去して公開用競合IDを失効させる。
        void ClearCloudConflictRegistry() noexcept
        {
            // 公開用IDと内部競合の対応記録
            for (auto& binding : cloudConflictRegistry)
            {
                EraseSecret(binding.descriptor.expectedMutationId);
            }
            cloudConflictRegistry.clear();
        }

        // 有効な競合だけに公開用IDを引き継ぎ管理情報を返す。
        [[nodiscard]] std::vector<OnlineCloudConflict>
            RefreshCloudConflictRegistry()
        {
            if (!persistenceCoordinator
                || !persistenceCoordinator->IsAccountActive())
            {
                ClearCloudConflictRegistry();
                return {};
            }
            // 現在のアカウント同期処理の借用
            auto* const synchronizer =
                persistenceCoordinator->Synchronizer();
            if (!synchronizer || !synchronizer->IsAttached())
            {
                ClearCloudConflictRegistry();
                return {};
            }

            // 現在のアカウントの公開世代
            const auto epoch = persistenceCoordinator->ProfileEpoch();
            // 本文を含まない内部競合の一覧
            const auto descriptors = synchronizer->Conflicts();
            // 同じ競合IDを引き継ぐ次の対応表
            std::vector<CloudConflictBinding> nextRegistry;
            // ゲームへ公開する競合一覧
            std::vector<OnlineCloudConflict> publicConflicts;
            nextRegistry.reserve(descriptors.size());
            publicConflicts.reserve(descriptors.size());
            // 内部の競合識別値と管理情報
            for (const auto& descriptor : descriptors)
            {
                // process内だけで有効な公開用競合ID
                std::string opaqueId;
                // 同じ内部競合のIDを引き継ぐ(binding: 照合する対応記録)。
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
                    // 競合ID衝突時の再生成回数
                    for (unsigned int attempt = 0; attempt < 16u; ++attempt)
                    {
                        opaqueId = GenerateOpaqueConflictId();
                        // 公開用IDの重複を検出する(binding: 確認済みの競合対応記録)。
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

                // ゲームへ公開する保存先の種別
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

        // 公開用IDの対応記録を借用し不在ならnullを返す(opaqueId: process内の競合ID)。
        [[nodiscard]] CloudConflictBinding* FindConflictBinding(
            const std::string_view opaqueId) noexcept
        {
            // 公開用IDを内部競合と照合する(binding: 比較する対応記録)。
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

        // 内部更新IDを消去して対応する公開用競合IDを失効させる(opaqueId: 解決・失効した競合ID)。
        void EraseCloudConflictBinding(
            const std::string_view opaqueId) noexcept
        {
            // 公開用IDを内部競合と照合する(binding: 比較する対応記録)。
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

        // 公開用の診断識別子と表示文を空にする。
        void ClearError() noexcept
        {
            errorCode.clear();
            errorMessage.clear();
        }

        // 固定診断を設定し認証失敗なら結果の世代とセッションを失効する(code: 内部のエラー識別子, signInFailure: 認証開始・確認の失敗か)。
        void SetError(
            const std::string_view code,
            const bool signInFailure)
        {
            // 公開を許可した固定のエラー識別子
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

        // 認証確認tokenとURLを消去し待機時間と起動状態を初期化する。
        void ClearLoginTransaction() noexcept
        {
            EraseTransaction(loginTransaction);
            authorizationUrl.clear();
            loginRemainingSeconds = 0.0f;
            pollRemainingSeconds = 0.0f;
            browserLaunchAttempted = false;
            browserLaunchFailed = false;
        }

        // tokenを消去しセッションの期限と再試行状態を初期化する。
        void ClearSession() noexcept
        {
            EraseSession(session);
            sessionRemainingSeconds = 0.0f;
            sessionRefreshLeadSeconds = 0.0f;
            sessionRefreshRetrySeconds = 0.0f;
        }

        // 認証結果に含まれる取引・セッションのtokenを消去する(result: 処理済みの通信結果)。
        static void EraseAsyncResult(AsyncResult& result) noexcept
        {
            EraseTransaction(result.loginStart.transaction);
            EraseSession(result.loginPoll.session);
            EraseSession(result.sessionRefresh.session);
        }

        // 公開可能な固定診断だけを設定する(code: 内部のエラー識別子)。
        void SetFixedError(const std::string_view code)
        {
            // 公開を許可した固定のエラー識別子
            const auto publicCode = PublicErrorCode(code);
            errorCode = publicCode;
            errorMessage = PublicErrorMessage(publicCode);
        }

        // 利用ロックを確保して次回起動用tokenを保存し例外もfalseとする(refreshToken: 保存する更新用token)。
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

        // 利用ロック下で端末tokenを削除し例外もfalseとする。
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

        // 端末tokenの削除を試し未解決の失敗を記録する。
        [[nodiscard]] bool DeleteRefreshTokenTracked() noexcept
        {
            localRefreshTokenDeleteFailed =
                !DeleteRefreshToken();
            return !localRefreshTokenDeleteFailed;
        }

        // 端末tokenの削除失敗が残っている場合だけ再試行する。
        [[nodiscard]] bool ResolvePendingRefreshTokenDelete() noexcept
        {
            return !localRefreshTokenDeleteFailed
                || DeleteRefreshTokenTracked();
        }

        // 資格情報storeの利用ロックを未取得なら確保する。
        [[nodiscard]] bool EnsureCredentialUsageLease() noexcept
        {
            if (!refreshTokenStore || credentialUsageLeaseHeld)
            {
                return true;
            }
            try
            {
                // 資格情報の利用ロック取得結果
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

        // 認証・後処理・未解決の削除が残っていない場合だけ利用ロックを解放する。
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

        // 受領した新tokenを遅延失効へ引き渡し旧値を消去する(completedSession: tokenを取り出して消去する結果)。
        [[nodiscard]] bool ReplacePendingLogoutAccessToken(
            Detail::OnlineSession& completedSession)
        {
            if (completedSession.accessToken.empty())
            {
                EraseSession(completedSession);
                return false;
            }

            // 旧tokenは完了結果側へ移して消去し、新tokenだけを失効workerへ渡します。
            pendingLogoutAccessToken.swap(
                completedSession.accessToken);
            EraseSession(completedSession);
            return true;
        }

        // 未実行時だけ認証workerを起動し共有する結果受け口へ渡す(kind: 認証通信の種別, work: thisを捕捉しない通信処理)。
        template<class Work>
        [[nodiscard]] bool Launch(
            const TaskKind kind,
            Work&& work)
        {
            if (inFlight)
            {
                return false;
            }

            // 認証workerの結果を共有する受け口
            auto mailbox = std::make_shared<AsyncMailbox>();
            mailbox->kind = kind;
            mailbox->generation = generation;
            mailbox->cleanupClient = client;
            if (credentialUsageLeaseHeld)
            {
                // owner破棄後も通信と後処理が終わるまでstoreを保持し、資格情報の利用ロックを解放しません。
                mailbox->usageLeaseStore = refreshTokenStore;
            }
            inFlight = mailbox;
            try
            {
                // 認証結果を受け口へ渡すworker
                std::thread worker(
                    [mailbox,
                        work = std::forward<Work>(work)]() mutable noexcept
                    {
                        // workerから渡す認証結果
                        AsyncResult result;
                        // 認証workerが例外で失敗したか
                        bool failed{};
                        try
                        {
                            result = work();
                        }
                        catch (...)
                        {
                            failed = true;
                        }
                        // 認証HTTPの完了時刻
                        const auto completedAt =
                            std::chrono::steady_clock::now();

                        // 所有者放棄後に後処理するか
                        bool cleanupAbandoned{};
                        std::shared_ptr<Detail::DiscordAuthClient>
                            cleanupClient;
                        try
                        {
                            // 完了結果の受け渡し用排他ロック
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
                            // 秘密値を含む例外本文を保存せず、可能なら失敗完了だけを通知します。
                            try
                            {
                                // 完了結果の受け渡し用排他ロック
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

        // 共有する認証clientで新しい認証取引を非同期に開始する。
        [[nodiscard]] bool LaunchLoginStart()
        {
            // workerで使用する認証client
            const auto authClient = client;
            return Launch(
                TaskKind::StartLogin,
                [authClient]
                {
                    // 認証取引の開始結果
                    AsyncResult result;
                    result.loginStart = authClient->BeginLogin();
                    return result;
                });
        }

        // 取引IDと消去責任付き確認tokenで認証状態を非同期に取得する。
        [[nodiscard]] bool LaunchLoginPoll()
        {
            // workerで使用する認証client
            const auto authClient = client;
            // 確認する認証トランザクションID
            const auto transactionId =
                loginTransaction.transactionId;
            // 共有して消去する認証確認token
            const auto pollToken =
                std::make_shared<SecretText>(
                    loginTransaction.pollToken);
            return Launch(
                TaskKind::PollLogin,
                [authClient,
                    transactionId,
                    pollToken]
                {
                    // 認証状態確認の結果
                    AsyncResult result;
                    result.loginPoll = authClient->PollLogin(
                        transactionId,
                        pollToken->text);
                    return result;
                });
        }

        // 復元または更新へtokenを引き渡し呼出し元の値を消去する(refreshToken: 引き渡して消去するtoken, kind: 復元または期限前更新の種別)。
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
            // workerで使用する認証client
            const auto authClient = client;
            // workerへ渡して消去するtoken
            const auto secret = std::make_shared<SecretText>(
                refreshToken);
            EraseSecret(refreshToken);
            return Launch(
                kind,
                [authClient, secret]
                {
                    // セッション復元または更新の結果
                    AsyncResult result;
                    result.sessionRefresh =
                        authClient->RefreshSession(secret->text);
                    return result;
                });
        }

        // tokenを消去責任付きでworkerへ渡し失効を開始する(accessToken: 引き渡して消去するtoken)。
        [[nodiscard]] bool LaunchLogout(std::string& accessToken)
        {
            // workerで使用する認証client
            const auto authClient = client;
            // workerへ渡して消去するtoken
            const auto secret = std::make_shared<SecretText>(
                accessToken);
            EraseSecret(accessToken);
            return Launch(
                TaskKind::Logout,
                [authClient, secret]
                {
                    // サーバー上の失効処理の結果
                    AsyncResult result;
                    result.logoutSucceeded = authClient->Logout(
                        secret->text);
                    return result;
                });
        }

        // 処理の開始世代と現在の公開状態が一致するかを返す(kind: 認証処理の種別, taskGeneration: 処理の開始世代)。
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

        // 設定された認証URLの自動起動を一度だけ試し失敗を固定診断で記録する。
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
            // 認証URLをブラウザーで開けたか
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

        // ゲームへ公開可能なプロフィールだけを複製する(source: バックエンドのプロフィール)。
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

        // 公開用競合IDを失効しguestへ戻して保存失敗を記録する。
        void DetachAccountPersistence() noexcept
        {
            // UIへ渡したopaque IDはsign-out/Detach開始時点で失効します。
            ClearCloudConflictRegistry();
            if (!persistenceCoordinator)
            {
                return;
            }
            // guest切替時の保存・隔離の結果
            const auto result =
                persistenceCoordinator->DetachToGuest();
            accountPersistenceSaveFailed =
                result
                    == Detail::OnlinePersistenceDetachResult::
                        QuarantinedAccount
                || accountPersistenceSaveFailed;
        }

        // 認証拒否でtoken更新を要求し検証済み成功で待機系列を解除する(elapsedSeconds: 前回からの経過秒数)。
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
            // クラウド同期が認証を拒否したか
            const bool unauthorized = persistenceCoordinator
                ->ConsumeCloudSaveUnauthorizedSignal();
            if (unauthorized
                && (state == OnlineAccountState::SignedIn
                    || state == OnlineAccountState::RefreshingSession))
            {
                if (cloudUnauthorizedRefreshAttempts == 0u)
                {
                    // 認証拒否の系列の最初だけ即時更新し、進行中の期限前更新も一回目として数えます。
                    cloudUnauthorizedRefreshAttempts = 1u;
                    if (state == OnlineAccountState::SignedIn)
                    {
                        cloudRefreshRequested = true;
                        sessionRefreshRetrySeconds = 0.0f;
                    }
                }
                else if (!cloudUnauthorizedRefreshQueued)
                {
                    // 認証拒否後の最大更新待機秒数
                    constexpr float MaximumCooldownSeconds = 300.0f;
                    // 認証拒否後の指数待機の回数
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

        // HTTP完了から現在までの時間をtoken期限から引く(receivedSession: 受領したセッション, completedAt: HTTP完了の単調時計時刻)。
        [[nodiscard]] static float RemainingReceivedSessionSeconds(
            const Detail::OnlineSession& receivedSession,
            const std::chrono::steady_clock::time_point
                completedAt) noexcept
        {
            // HTTP完了から公開までの経過秒数
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

        // 公開直前に期限を再確認しtokenとプロフィールを例外なく入れ替える(nextSession: 引き取るセッション, nextPlayer: 準備済みプロフィール, completedAt: HTTP完了の単調時計時刻)。
        [[nodiscard]] bool PublishSession(
            Detail::OnlineSession& nextSession,
            OnlinePlayerProfile nextPlayer,
            const std::chrono::steady_clock::time_point
                completedAt) noexcept
        {
            // main threadの停止時間も有効期限から引き、公開直前に失効済みなら呼出し側の失効処理へ戻します。
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

        // 採用できないセッションをguest切替と遅延失効で処理する(rejectedSession: 消去する受領結果, code: 固定の拒否理由, deleteStoredCredential: 端末tokenも削除するか)。
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

        // 保存先の切替とtoken保存を終えて期限内の初回セッションを公開する(nextSession: 初回の受領結果, restoring: 保存済みtokenからの復元か, completedAt: HTTP完了時刻)。
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
            // 公開前に準備するプロフィール
            OnlinePlayerProfile nextPlayer;
            try
            {
                nextPlayer = MakePublicProfile(nextSession.player);
                if (persistenceCoordinator
                    && persistenceCoordinator->IsNamespaceEnabled())
                {
                    // 切替前に検証した保存先の状態
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

            // アカウントの保存先切替後だけtokenを保存し、失敗時はguestに戻して資格情報削除と受領セッションの失効を試します。
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
                // 保存後に期限切れになった場合も資格情報を削除し、受領セッションの失効を試します。
                RejectReceivedSession(
                    nextSession,
                    "request_failed",
                    true);
                return;
            }
        }

        // 所有者を照合しtoken保存と同期先更新後に新セッションを公開する(nextSession: 更新の受領結果, completedAt: HTTP完了時刻)。
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

            // 公開前に準備するプロフィール
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
                    // 公開前にクラウド同期のtokenと世代を更新し、検証・確保の失敗時は採用しません。
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

        // 利用ロック下で保存済みtokenを読み込み非同期復元を開始する。
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

            // 端末の更新用tokenの読み込み結果
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

            // 通信へ引き渡す更新用token
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

        // 認証開始の結果を公開し確認待機とブラウザー起動を設定する(result: 認証取引の開始結果)。
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

        // 確認結果から待機・採用・拒否・期限切れへ遷移する(result: 認証状態の結果, completedAt: HTTP完了時刻)。
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

        // 復元・更新の結果を採用し失敗を期限と再試行可能性で処理する(result: セッション更新の結果, restoring: 起動時の復元か, completedAt: HTTP完了時刻)。
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
                // 端末の資格情報を削除できたか
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
                // 一時的な通信失敗では更新用tokenを残し、次回起動で復元を再試行できます。
                state = OnlineAccountState::Error;
                SetError(result.errorCode, false);
                return;
            }

            if (sessionRemainingSeconds > 0.0f)
            {
                // access tokenがまだ有効な間は現在のセッションを保持し、5秒後にrefreshを再試行します。
                state = OnlineAccountState::SignedIn;
                sessionRefreshRetrySeconds = std::min(
                    5.0f,
                    sessionRemainingSeconds);
                SetError(result.errorCode, false);
                return;
            }

            // セッションは失効していますが、ネットワーク障害で保存済みtokenを消すと復旧できなくなります。
            DetachAccountPersistence();
            ClearSession();
            player = {};
            state = OnlineAccountState::Error;
            SetError(result.errorCode, false);
        }

        // tokenを消去し端末削除や保存失敗を優先して終了状態を公開する(succeeded: サーバーの失効が成功したか)。
        void CompleteLogout(const bool succeeded)
        {
            EraseSecret(pendingLogoutAccessToken);
            awaitingCancelledTask = false;
            // 取消後に期限切れの表示へ戻すか
            const bool returnToExpiredLoginError =
                std::exchange(cancelledPollExpired, false);
            // 端末tokenの削除が未完了か
            const bool deleteFailed =
                localRefreshTokenDeleteFailed;
            // アカウントのローカル保存が失敗か
            const bool persistenceSaveFailed =
                std::exchange(accountPersistenceSaveFailed, false);
            // セッション採用失敗の診断を保つか
            const bool preserveFailure =
                std::exchange(preserveErrorAfterLogout, false);
            if (preserveFailure)
            {
                // セッション採用失敗の主因を、後処理の失効結果で上書きしません。
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

        // 取消後の受領セッションを公開せず遅延失効へ引き渡す(kind: 取消した処理種別, failed: workerの失敗, result: 遅れて届いた通信結果)。
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
                    // Update末尾で、復元時に新しく発行されたaccess tokenの失効を開始します。
                    return true;
                }

                // 復元でセッションを受領しなかった場合は、端末資格情報の削除だけで終了します。
                CompleteLogout(true);
                return true;
            }

            if (kind == TaskKind::RefreshSession)
            {
                if (!failed && result.sessionRefresh.Succeeded())
                {
                    // 更新後は旧access tokenが無効になり得るため、受領した新tokenを失効に使います。
                    (void)ReplacePendingLogoutAccessToken(
                        result.sessionRefresh.session);
                }
                // 更新失敗時はSignOutで退避した旧tokenを保ち、成功時は受領した新tokenを使います。
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
                    // 取消と認証完了が競合した場合も、受領したセッションを公開せず失効を試します。
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
                    // 期限切れの表示を保ち、認証成功以外の遅延結果ではセッション失効を行いません。
                    cancelledPollExpired = false;
                    return true;
                }
                CompleteLogout(true);
                return true;
            }

            return false;
        }

        // 取消した処理の例外時に利用可能なtokenで後処理を続ける(kind: 取消した処理種別)。
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

        // 処理種別に対応する認証結果の反映へ引き渡す(kind: 完了した処理種別, result: 認証の通信結果, completedAt: HTTP完了時刻)。
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

        // 秘密値を含み得る例外本文を公開せず固定診断と期限で処理する(kind: 失敗した処理種別)。
        void CompleteTaskException(const TaskKind kind)
        {
            // 例外本文にはtokenが含まれ得るため、公開診断には固定の失敗理由だけを使います。
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
                    // 応答後の解析・確保失敗でも更新の成否が不確かになるため、期限切れ時は旧資格情報を残しません。
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

        // 完了結果を引き取り世代と状態を照合し公開状態が変わったかを返す。
        [[nodiscard]] bool ReapCompletedTask()
        {
            // 認証workerの結果を共有する受け口
            const auto mailbox = inFlight;
            if (!mailbox)
            {
                return false;
            }

            // 完了した認証workerの結果
            AsyncResult result;
            // 認証通信の処理種別
            TaskKind kind{};
            // 完了した認証処理の開始世代
            std::uint64_t taskGeneration{};
            // 認証HTTPの完了時刻
            std::chrono::steady_clock::time_point completedAt{};
            // 認証workerが例外で失敗したか
            bool failed{};
            {
                // 完了結果の引き取り・確認用排他ロック
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

            // 通信結果を反映する前の公開状態
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

        // 認証期限と確認間隔を進め期限内なら次の確認を開始する(elapsedSeconds: 前回からの経過秒数)。
        void TickLogin(const float elapsedSeconds)
        {
            // 認証の待機または確認が進行中か
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

        // 更新中の期限切れをguest切替と旧資格情報削除と遅延失効で処理する。
        void ExpireRefreshingSession()
        {
            DetachAccountPersistence();
            // 更新中はサーバーでtokenを置換済みか分からないため、旧資格情報を次回起動へ残しません。
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

        // 残り期限と再試行を進め必要ならtoken更新を開始する(elapsedSeconds: この状態で経過した秒数)。
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

            // 通信へ引き渡す更新用token
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

        // 取消した処理が終わりtokenが利用可能なら失効通信を開始する。
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

        // テスト等で注入するHTTP送信処理
        Detail::OnlineServicesTestAccess::HttpSender senderOverride;
        // 非同期認証clientの共有所有先
        std::shared_ptr<Detail::DiscordAuthClient> client;
        // 同期HTTP処理の共有所有先
        std::shared_ptr<const Detail::CloudSaveClient> cloudSaveClient;
        // 実行中の認証結果の受け渡し先
        std::shared_ptr<AsyncMailbox> inFlight;
        // guestとアカウントの保存先切替の所有先
        std::unique_ptr<Detail::OnlinePersistenceCoordinator>
            persistenceCoordinator;
        // 公開用IDと内部の競合の対応一覧
        std::vector<CloudConflictBinding> cloudConflictRegistry;
        // 利用ロックも持つ資格情報store
        std::shared_ptr<Detail::IRefreshTokenStore> refreshTokenStore;
        // 認証URLのブラウザー起動処理の所有先
        std::unique_ptr<Detail::IAuthorizationLauncher>
            authorizationLauncher;
        // 確認用tokenを保持する認証取引
        Detail::DiscordLoginTransaction loginTransaction;
        // 短命・更新用tokenを持つ認証状態
        Detail::OnlineSession session;
        // ゲームへ公開するプロフィール
        OnlinePlayerProfile player;
        // 遅延して失効するaccess token
        std::string pendingLogoutAccessToken;
        // ブラウザーで開く認証URL
        std::string authorizationUrl;
        // 秘密値を含まない診断識別子
        std::string errorCode;
        // 秘密値を含まない表示用診断
        std::string errorMessage;
        // 現在のゲームの名前空間ID
        std::string configuredGameId;
        // 現在の環境の名前空間ID
        std::string configuredEnvironmentId;
        // 現在のオンラインアカウント状態
        OnlineAccountState state{ OnlineAccountState::Unconfigured };
        // 古い結果を失効する接続世代
        std::uint64_t generation{ 1 };
        // 認証取引の残り有効秒数
        float loginRemainingSeconds{};
        // 次の認証状態確認までの秒数
        float pollRemainingSeconds{};
        // access tokenの残り有効秒数
        float sessionRemainingSeconds{};
        // 期限前に更新を始める猶予秒数
        float sessionRefreshLeadSeconds{};
        // token更新の再試行までの秒数
        float sessionRefreshRetrySeconds{};
        // Windows標準の保存と起動を使うか
        bool useWindowsPlatformDefaults{ true };
        // ローカルHTTPを許可するか
        bool allowInsecureLoopback{};
        // 認証URLを自動起動するか
        bool openAuthorizationBrowser{ true };
        // ブラウザー起動を試したか
        bool browserLaunchAttempted{};
        // ブラウザー起動が失敗したか
        bool browserLaunchFailed{};
        // 端末の更新用tokenの削除が失敗か
        bool localRefreshTokenDeleteFailed{};
        // guest切替時の保存が失敗したか
        bool accountPersistenceSaveFailed{};
        // 遅延失効後も採用失敗の診断を保つか
        bool preserveErrorAfterLogout{};
        // 失効した認証workerの完了待ちか
        bool awaitingCancelledTask{};
        // 認証確認の取消が期限切れ由来か
        bool cancelledPollExpired{};
        // クラウド同期からtoken更新要求か
        bool cloudRefreshRequested{};
        // 認証拒否の系列のtoken更新回数
        std::uint32_t cloudUnauthorizedRefreshAttempts{};
        // 認証拒否後のtoken更新待機秒数
        float cloudUnauthorizedRefreshCooldownSeconds{};
        // 待機後のtoken更新を予約済みか
        bool cloudUnauthorizedRefreshQueued{};
        // 資格情報の利用ロックを保持中か
        bool credentialUsageLeaseHeld{};
        // 失効した認証workerの処理種別
        TaskKind cancelledTaskKind{ TaskKind::StartLogin };
        // アカウント認証から独立した表示機能
        DiscordPresence presence;
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
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        if (implementation.persistenceCoordinator
            && implementation.persistenceCoordinator
                ->HasPendingRecovery())
        {
            // 空設定も含め、未解決の保存復旧があれば資格情報や名前空間を変更する前に拒否します。
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

        // 切替前に準備するゲームの名前空間
        auto nextGameId = configuration.gameId;
        // 切替前に準備する環境の名前空間
        auto nextEnvironmentId = configuration.environmentId;
        // 切替前に準備する認証client
        std::shared_ptr<Detail::DiscordAuthClient> nextClient;
        // 切替前に準備する同期HTTP処理
        std::shared_ptr<const Detail::CloudSaveClient> nextCloudSaveClient;
        // namespace公開後をnoexceptの移動だけにするため、共有所有先の確保も先に終えます。
        std::shared_ptr<Detail::IRefreshTokenStore> nextStore;
        // 切替前に準備する認証URL起動処理
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
                // 認証とクラウドのclientを両方構築してから保存先と公開設定を切り替えます。
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

        // 保存先の名前空間変更が成功した後だけ公開用競合IDを失効し、設定失敗時は保ちます。
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
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        if (!std::isfinite(elapsedSeconds)
            || elapsedSeconds < 0.0f)
        {
            elapsedSeconds = 0.0f;
        }
        // 早期returnより先にPresenceを進め、認証処理が表示の再接続を止めないようにします。
        implementation.presence.Tick(elapsedSeconds);
        implementation.ConsumeCloudSaveSignals(elapsedSeconds);
        // 旧tokenの時間を減算済みか
        const bool refreshElapsedApplied =
            implementation.state
                == OnlineAccountState::RefreshingSession;
        if (refreshElapsedApplied)
        {
            // 完了結果の反映前に旧tokenの時間を一度だけ減算し、失敗時も同じフレームで失効を判定します。
            implementation.sessionRemainingSeconds = std::max(
                0.0f,
                implementation.sessionRemainingSeconds
                    - elapsedSeconds);
        }
        if (implementation.ReapCompletedTask())
        {
            implementation.ReleaseCredentialUsageLeaseIfSafe();
            // 完了によって遷移したばかりの状態へ、前状態で経過したelapsedSecondsを同じフレーム中に適用しません。
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
        // 非同期認証と保存連携の状態の借用
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
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        // 認証の開始・待機・確認が進行中か
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

        // 同期HTTPを途中で破棄せず、接続世代で結果を失効させてゲームループは完了を待ちません。
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
        // 非同期認証と保存連携の状態の借用
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
        // 採用失敗の後処理中でも明示SignOutはSignedOutの終了を優先します。
        implementation.preserveErrorAfterLogout = false;
        if (implementation.state == OnlineAccountState::SigningOut)
        {
            // 後処理中の明示SignOutでも端末tokenの削除を試し、前回失敗していれば再試行します。
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

        // 通信の完了を待たず、端末の再ログイン情報はこの呼び出し中に削除します。
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

        // サインアウト時にtoken更新中か
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

    void OnlineServices::ConfigureDiscordPresence(
        DiscordPresenceConfiguration configuration)
    {
        m_implementation->presence.Configure(
            std::move(configuration));
    }

    DiscordPresence& OnlineServices::Presence() noexcept
    {
        return m_implementation->presence;
    }

    const DiscordPresence& OnlineServices::Presence() const noexcept
    {
        return m_implementation->presence;
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
        // 非同期認証と保存連携の状態の借用
        const auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator
            || !implementation.persistenceCoordinator->IsAccountActive())
        {
            return {};
        }
        // 現在のアカウント同期処理の借用
        auto* const synchronizer =
            implementation.persistenceCoordinator->Synchronizer();
        if (!synchronizer || !synchronizer->IsAttached())
        {
            return {};
        }

        // 公開用に変換する内部の状態
        const auto internal = synchronizer->Status();
        // 内部識別値を含まない同期状態
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
        // 単調時計の現在時刻ms
        const auto now = SteadyMilliseconds();
        if (internal.retryAtMilliseconds > now)
        {
            // 再試行までの残り待機ms
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
            // 内部の保存先・token・ETag・更新IDを含み得る例外を公開しません。
            throw std::runtime_error(
                "Cloud conflict enumeration failed.");
        }
    }

    OnlinePersistenceOperationResult
        OnlineServices::RequestCloudSync() noexcept
    {
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator
            || !implementation.persistenceCoordinator->IsAccountActive())
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        // 現在のアカウント同期処理の借用
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
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        // 公開用IDと内部競合の対応記録
        auto* const binding =
            implementation.FindConflictBinding(conflictId);
        if (!binding
            || !implementation.persistenceCoordinator
            || !implementation.persistenceCoordinator->IsAccountActive())
        {
            return OnlinePersistenceOperationResult::Stale;
        }
        // 現在のアカウント同期処理の借用
        auto* const synchronizer =
            implementation.persistenceCoordinator->Synchronizer();
        if (!synchronizer || !synchronizer->IsAttached())
        {
            return OnlinePersistenceOperationResult::Stale;
        }

        try
        {
            // stale判定をwire/auth Busyより先に行います。
            // 現在のアカウントの競合管理一覧
            const auto currentConflicts = synchronizer->Conflicts();
            // 現在の公開先と競合IDを照合する(descriptor: 現在の競合の内部管理情報)。
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

            // journalへ渡す競合解決方針
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
            // 解決中の競合に対応する保存先
            const auto resource = binding->descriptor.resource;
            // 解決する競合の内部更新ID
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
        // 非同期認証と保存連携の状態の借用
        const auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator)
        {
            return {};
        }
        // 公開用に変換する内部の状態
        const auto internal =
            implementation.persistenceCoordinator->RecoveryStatus();
        // 公開する復旧状態と識別版
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
        // 内部の復旧操作結果を公開用の結果種別へ変換する(result: 内部の操作結果)。
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
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator)
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        // 明示復旧前に照合する復旧対象の状態
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
        // 非同期認証と保存連携の状態の借用
        auto& implementation = *m_implementation;
        if (!implementation.persistenceCoordinator)
        {
            return OnlinePersistenceOperationResult::Unavailable;
        }
        // 明示復旧前に照合する復旧対象の状態
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
        // テスト用の依存処理でサービスを作る(configuration: 認証設定, sender: 必須のHTTP送信処理, refreshTokenStore: 資格情報保存の所有先, authorizationLauncher: URL起動処理の所有先)。
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
            // テストで構築するサービスの所有先
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

        // 結果を反映せずworkerの完了を確認する(services: 検査するサービス)。
        bool OnlineServicesTestAccess::CurrentTaskCompleted(
            const OnlineServices& services) noexcept
        {
            // 認証workerの結果を共有する受け口
            const auto mailbox = services.m_implementation->inFlight;
            if (!mailbox)
            {
                return false;
            }
            try
            {
                // 完了結果の引き取り・確認用排他ロック
                std::scoped_lock lock(mailbox->mutex);
                return mailbox->completed;
            }
            catch (...)
            {
                return false;
            }
        }

        // 完了時刻だけを過去へずらす(services: 操作するサービス, elapsedSeconds: 遡る秒数・0以上1年以内)。
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
            // 認証workerの結果を共有する受け口
            const auto mailbox = services.m_implementation->inFlight;
            if (!mailbox)
            {
                return false;
            }
            try
            {
                // 完了結果の引き取り・確認用排他ロック
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

        // 認証開始前に保存先切替を接続する(services: 接続先, preferences: 存続する設定の借用, saves: 存続する保存領域の借用, trustedUserDataDirectory: 信頼済みUserDataの絶対パス)。
        void OnlinePersistenceAccess::Attach(
            OnlineServices& services,
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::filesystem::path trustedUserDataDirectory)
        {
            // 非同期認証と保存連携の状態の借用
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

            // アカウントの保存先切替処理の借用
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

        // 競合IDを失効させてguestの保存先へ戻す(services: 切替対象)。
        OnlinePersistenceDetachResult OnlinePersistenceAccess::Detach(
            OnlineServices& services) noexcept
        {
            services.m_implementation->ClearCloudConflictRegistry();
            // アカウントの保存先切替処理の借用
            auto* const coordinator =
                services.m_implementation->persistenceCoordinator.get();
            return coordinator
                ? coordinator->DetachToGuest()
                : OnlinePersistenceDetachResult::AlreadyGuest;
        }

        // フレーム終端で隔離中の保存を再試行する(services: 再試行対象)。
        void OnlinePersistenceAccess::EndFrame(
            OnlineServices& services) noexcept
        {
            // アカウントの保存先切替処理の借用
            if (auto* const coordinator =
                    services.m_implementation
                        ->persistenceCoordinator.get())
            {
                coordinator->EndFrame();
            }
        }

        // 保存先切替処理を借用し未接続ならnullを返す(services: 参照対象)。
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
        // Application所有の現在のサービス
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
