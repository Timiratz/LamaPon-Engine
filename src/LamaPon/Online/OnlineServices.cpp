#include "LamaPon/Online/OnlineServices.h"

#include "LamaPon/Online/DiscordAuth.h"
#include "LamaPon/Online/OnlineServicesTesting.h"

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
            || code == "request_failed";
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
        return "オンライン認証に失敗しました。";
    }

    struct SecretText final
    {
        explicit SecretText(std::string value)
            : text(std::move(value))
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
            Logout
        };

        struct AsyncResult final
        {
            Detail::DiscordLoginStartResult loginStart;
            Detail::DiscordLoginPollResult loginPoll;
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
            }
        };

        explicit Implementation(
            Detail::OnlineServicesTestAccess::HttpSender sender = {})
            : senderOverride(std::move(sender))
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
        }

        void ClearSession() noexcept
        {
            EraseSession(session);
        }

        static void EraseAsyncResult(AsyncResult& result) noexcept
        {
            EraseTransaction(result.loginStart.transaction);
            EraseSession(result.loginPoll.session);
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

        [[nodiscard]] bool LaunchLogout(std::string accessToken)
        {
            const auto authClient = client;
            const auto secret = std::make_shared<SecretText>(
                std::move(accessToken));
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
            case TaskKind::Logout:
                return state == OnlineAccountState::SigningOut;
            }
            return false;
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
                ClearError();
                return;

            case Detail::DiscordLoginPollStatus::Authorized:
                ClearSession();
                session = std::move(result.session);
                player.playerId = session.player.playerId;
                player.displayName = session.player.displayName;
                player.avatarUrl = session.player.avatarUrl;
                player.linkedProvider = session.player.linkedProvider;
                ClearLoginTransaction();
                state = OnlineAccountState::SignedIn;
                ClearError();
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

        void CompleteLogout(const bool succeeded)
        {
            state = OnlineAccountState::SignedOut;
            if (succeeded)
            {
                ClearError();
            }
            else
            {
                errorCode = "logout_failed";
                errorMessage =
                    "サーバー上のセッションを失効できませんでした。"
                    "端末上ではサインアウト済みです。";
            }
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

        Detail::OnlineServicesTestAccess::HttpSender senderOverride;
        std::shared_ptr<Detail::DiscordAuthClient> client;
        std::shared_ptr<AsyncMailbox> inFlight;
        Detail::DiscordLoginTransaction loginTransaction;
        Detail::OnlineSession session;
        OnlinePlayerProfile player;
        std::string authorizationUrl;
        std::string errorCode;
        std::string errorMessage;
        OnlineAccountState state{ OnlineAccountState::Unconfigured };
        std::uint64_t generation{ 1 };
        float loginRemainingSeconds{};
        float pollRemainingSeconds{};
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
        if (!configuration.serviceBaseUrl.empty())
        {
            nextClient = std::make_shared<Detail::DiscordAuthClient>(
                std::move(configuration.serviceBaseUrl),
                configuration.allowInsecureLoopback,
                implementation.senderOverride);
        }

        implementation.AdvanceGeneration();
        implementation.ClearLoginTransaction();
        implementation.ClearSession();
        implementation.player = {};
        implementation.ClearError();
        implementation.client = std::move(nextClient);
        implementation.state = implementation.client
            ? OnlineAccountState::SignedOut
            : OnlineAccountState::Unconfigured;
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
        if (implementation.state
                == OnlineAccountState::StartingSignIn
            || implementation.state
                == OnlineAccountState::WaitingForAuthorization
            || implementation.state
                == OnlineAccountState::PollingAuthorization)
        {
            CancelDiscordSignIn();
            return;
        }
        if (implementation.state == OnlineAccountState::SigningOut)
        {
            return;
        }
        if (implementation.state != OnlineAccountState::SignedIn)
        {
            implementation.AdvanceGeneration();
            implementation.ClearLoginTransaction();
            implementation.ClearSession();
            implementation.player = {};
            implementation.ClearError();
            implementation.state = implementation.client
                ? OnlineAccountState::SignedOut
                : OnlineAccountState::Unconfigured;
            return;
        }

        implementation.AdvanceGeneration();
        auto accessToken = std::move(
            implementation.session.accessToken);
        implementation.ClearSession();
        implementation.ClearLoginTransaction();
        implementation.player = {};
        implementation.ClearError();
        implementation.state = OnlineAccountState::SigningOut;

        if (!implementation.LaunchLogout(
                std::move(accessToken)))
        {
            EraseSecret(accessToken);
            implementation.CompleteLogout(false);
        }
    }

    OnlineAccountState OnlineServices::State() const noexcept
    {
        return m_implementation->state;
    }

    bool OnlineServices::IsSignedIn() const noexcept
    {
        return m_implementation->state
            == OnlineAccountState::SignedIn;
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
                HttpSender sender)
        {
            if (!sender)
            {
                throw std::invalid_argument(
                    "OnlineServices test sender is required.");
            }
            auto services = std::unique_ptr<OnlineServices>(
                new OnlineServices(
                    std::make_unique<OnlineServices::Implementation>(
                        std::move(sender))));
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
