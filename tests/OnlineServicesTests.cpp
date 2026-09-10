#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Core/Application.h"
#include "LamaPon/Scripting/Script.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const nlohmann::json& body)
    {
        LamaPon::HttpResponse response;
        response.statusCode = status;
        const auto text = body.dump();
        response.body.assign(text.begin(), text.end());
        return response;
    }

    class ScriptedBackend final
    {
    public:
        explicit ScriptedBackend(
            std::deque<LamaPon::HttpResponse> responses)
            : m_responses(std::move(responses))
        {
        }

        LamaPon::HttpResponse Send(
            const LamaPon::HttpRequest& request)
        {
            std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
            if (m_responses.empty())
            {
                LamaPon::HttpResponse response;
                response.transportError = "Unexpected request.";
                return response;
            }
            auto response = std::move(m_responses.front());
            m_responses.pop_front();
            return response;
        }

        [[nodiscard]] std::vector<LamaPon::HttpRequest>
            Requests() const
        {
            std::scoped_lock lock(m_mutex);
            return m_requests;
        }

    private:
        mutable std::mutex m_mutex;
        std::deque<LamaPon::HttpResponse> m_responses;
        std::vector<LamaPon::HttpRequest> m_requests;
    };

    class BlockingScriptedBackend final
    {
    public:
        BlockingScriptedBackend(
            std::deque<LamaPon::HttpResponse> responses,
            const std::size_t blockedRequest)
            : m_responses(std::move(responses))
            , m_blockedRequest(blockedRequest)
        {
        }

        LamaPon::HttpResponse Send(
            const LamaPon::HttpRequest& request)
        {
            std::unique_lock lock(m_mutex);
            m_requests.push_back(request);
            const auto requestNumber = m_requests.size();
            LamaPon::HttpResponse response;
            if (m_responses.empty())
            {
                response.transportError = "Unexpected request.";
            }
            else
            {
                response = std::move(m_responses.front());
                m_responses.pop_front();
            }

            if (requestNumber == m_blockedRequest)
            {
                m_requestBlocked = true;
                m_condition.notify_all();
                // テスト失敗時もdetached workerを永久停止させません。
                (void)m_condition.wait_for(
                    lock,
                    3s,
                    [this]
                    {
                        return m_released;
                    });
            }
            return response;
        }

        [[nodiscard]] bool WaitUntilBlocked()
        {
            std::unique_lock lock(m_mutex);
            return m_condition.wait_for(
                lock,
                3s,
                [this]
                {
                    return m_requestBlocked;
                });
        }

        void Release()
        {
            {
                std::scoped_lock lock(m_mutex);
                m_released = true;
            }
            m_condition.notify_all();
        }

        [[nodiscard]] std::vector<LamaPon::HttpRequest>
            Requests() const
        {
            std::scoped_lock lock(m_mutex);
            return m_requests;
        }

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        std::deque<LamaPon::HttpResponse> m_responses;
        std::vector<LamaPon::HttpRequest> m_requests;
        std::size_t m_blockedRequest{};
        bool m_requestBlocked{};
        bool m_released{};
    };

    struct MemoryTokenStoreState final
    {
        LamaPon::Detail::RefreshTokenLoadStatus loadStatus{
            LamaPon::Detail::RefreshTokenLoadStatus::NotFound
        };
        std::string token;
        std::size_t loadCount{};
        std::size_t saveCount{};
        std::size_t deleteCount{};
        bool failSave{};
        bool failDelete{};
        std::string privateError{
            "platform-secret-must-not-be-public"
        };
    };

    class MemoryTokenStore final
        : public LamaPon::Detail::IRefreshTokenStore
    {
    public:
        explicit MemoryTokenStore(
            std::shared_ptr<MemoryTokenStoreState> state)
            : m_state(std::move(state))
        {
        }

        LamaPon::Detail::RefreshTokenLoadResult Load() override
        {
            ++m_state->loadCount;
            LamaPon::Detail::RefreshTokenLoadResult result;
            result.status = m_state->loadStatus;
            if (result.Loaded())
            {
                result.refreshToken = m_state->token;
            }
            else if (result.status
                != LamaPon::Detail::RefreshTokenLoadStatus::NotFound)
            {
                result.errorCode = m_state->privateError;
                result.errorMessage = m_state->privateError;
            }
            return result;
        }

        LamaPon::Detail::OnlinePlatformResult Save(
            const std::string_view refreshToken) override
        {
            ++m_state->saveCount;
            if (m_state->failSave)
            {
                return {
                    false,
                    m_state->privateError,
                    m_state->privateError
                };
            }
            m_state->token = refreshToken;
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            return { true, {}, {} };
        }

        LamaPon::Detail::OnlinePlatformResult Delete() override
        {
            ++m_state->deleteCount;
            if (m_state->failDelete)
            {
                return {
                    false,
                    m_state->privateError,
                    m_state->privateError
                };
            }
            m_state->token.clear();
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::NotFound;
            return { true, {}, {} };
        }

    private:
        std::shared_ptr<MemoryTokenStoreState> m_state;
    };

    struct MemoryLauncherState final
    {
        std::size_t launchCount{};
        std::string lastUrl;
        bool lastAllowInsecureLoopback{};
        bool succeed{ true };
        std::string privateError{
            "launcher-secret-must-not-be-public"
        };
    };

    class MemoryAuthorizationLauncher final
        : public LamaPon::Detail::IAuthorizationLauncher
    {
    public:
        explicit MemoryAuthorizationLauncher(
            std::shared_ptr<MemoryLauncherState> state)
            : m_state(std::move(state))
        {
        }

        LamaPon::Detail::OnlinePlatformResult Launch(
            const std::string_view authorizationUrl,
            const bool allowInsecureLoopback) override
        {
            ++m_state->launchCount;
            m_state->lastUrl = authorizationUrl;
            m_state->lastAllowInsecureLoopback =
                allowInsecureLoopback;
            return m_state->succeed
                ? LamaPon::Detail::OnlinePlatformResult{
                    true, {}, {} }
                : LamaPon::Detail::OnlinePlatformResult{
                    false,
                    m_state->privateError,
                    m_state->privateError
                };
        }

    private:
        std::shared_ptr<MemoryLauncherState> m_state;
    };

    LamaPon::OnlineServiceConfiguration TestConfiguration(
        const bool openAuthorizationBrowser = true)
    {
        LamaPon::OnlineServiceConfiguration configuration;
        configuration.serviceBaseUrl =
            "https://online.example.test";
        configuration.gameId = "online-services-tests";
        configuration.environmentId = "test";
        configuration.openAuthorizationBrowser =
            openAuthorizationBrowser;
        return configuration;
    }

    nlohmann::json SessionJson(
        const std::string_view accessToken,
        const std::string_view refreshToken,
        const std::string_view playerId = "player-42",
        const std::uint32_t expiresIn = 900)
    {
        return {
            { "accessToken", accessToken },
            { "refreshToken", refreshToken },
            { "expiresIn", expiresIn },
            {
                "player",
                {
                    { "id", playerId },
                    { "displayName", "ラマポン" },
                    {
                        "avatarUrl",
                        "https://cdn.discordapp.com/avatar.png"
                    },
                    { "linkedProvider", "discord" }
                }
            }
        };
    }

    template<class Predicate>
    void UpdateUntil(
        LamaPon::OnlineServices& services,
        Predicate&& predicate,
        const char* timeoutMessage)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + 3s;
        while (!std::forward<Predicate>(predicate)())
        {
            services.Update(0.0f);
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    void UpdateUntilState(
        LamaPon::OnlineServices& services,
        const LamaPon::OnlineAccountState expected,
        const char* timeoutMessage)
    {
        UpdateUntil(
            services,
            [&services, expected]
            {
                return services.State() == expected;
            },
            timeoutMessage);
    }

    bool Contains(
        const std::string_view text,
        const std::string_view value)
    {
        return text.find(value) != std::string_view::npos;
    }

    bool HasHeader(
        const LamaPon::HttpRequest& request,
        const std::wstring_view name,
        const std::wstring_view value)
    {
        for (const auto& [headerName, headerValue] : request.headers)
        {
            if (headerName == name && headerValue == value)
            {
                return true;
            }
        }
        return false;
    }

    class OnlineProbe final : public LamaPon::Script
    {
    public:
        using Script::CancelDiscordSignIn;
        using Script::IsOnlineSignedIn;
        using Script::OnlineAuthorizationUrl;
        using Script::OnlineError;
        using Script::OnlinePlayerId;
        using Script::OnlinePlayerName;
        using Script::OnlineState;
        using Script::SignInWithDiscord;
        using Script::SignOutOnline;
    };

    void TestScriptFallbackAndActiveService()
    {
        LamaPon::SetActiveOnlineServices(nullptr);
        const OnlineProbe probe;
        Require(
            LamaPon::ActiveOnlineServices() == nullptr,
            "The active online service must initially be null.");
        Require(
            !probe.SignInWithDiscord()
                && probe.OnlineState()
                    == LamaPon::OnlineAccountState::Unconfigured
                && !probe.IsOnlineSignedIn()
                && probe.OnlinePlayerId().empty()
                && probe.OnlinePlayerName().empty()
                && probe.OnlineAuthorizationUrl().empty()
                && probe.OnlineError().empty(),
            "Script online helpers were not harmless without Application.");
        probe.CancelDiscordSignIn();
        probe.SignOutOnline();
    }

    void TestActiveServiceLifetime()
    {
        auto services = std::make_unique<LamaPon::OnlineServices>();
        LamaPon::SetActiveOnlineServices(services.get());
        services.reset();
        Require(
            LamaPon::ActiveOnlineServices() == nullptr,
            "Destroying the active service left a dangling pointer.");

        LamaPon::OnlineServices external;
        LamaPon::SetActiveOnlineServices(&external);
        {
            // 未初期化ApplicationはOnlineServicesを所有していません。
            // その破棄で別所有者のactive登録を横取り解除しないこと。
            LamaPon::Application application;
        }
        Require(
            LamaPon::ActiveOnlineServices() == &external,
            "Application cleared another owner's active service.");
        LamaPon::SetActiveOnlineServices(nullptr);
    }

    void TestLoginPollingAndLocalLogout()
    {
        constexpr std::string_view pollSecret =
            "poll-secret-must-not-be-public";
        constexpr std::string_view accessSecret =
            "access-secret-must-not-be-public";
        constexpr std::string_view refreshSecret =
            "refresh-secret-must-not-be-public";

        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            201,
            {
                { "transactionId", "login-1" },
                { "pollToken", pollSecret },
                {
                    "authorizationUrl",
                    "https://login.example.test/discord"
                },
                { "expiresIn", 60 },
                { "pollInterval", 1 }
            }));
        responses.push_back(JsonResponse(
            202,
            {
                { "status", "pending" },
                { "retryAfter", 2 }
            }));
        responses.push_back(JsonResponse(
            200,
            {
                { "status", "authorized" },
                { "accessToken", accessSecret },
                { "refreshToken", refreshSecret },
                { "expiresIn", 900 },
                {
                    "player",
                    {
                        { "id", "player-42" },
                        { "displayName", "ラマポン" },
                        {
                            "avatarUrl",
                            "https://cdn.discordapp.com/avatar.png"
                        },
                        { "linkedProvider", "discord" }
                    }
                }
            }));
        responses.push_back(JsonResponse(
            503,
            {
                {
                    "error",
                    { { "code", "service_unavailable" } }
                }
            }));

        const auto backend =
            std::make_shared<ScriptedBackend>(
                std::move(responses));
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        const auto launcherState =
            std::make_shared<MemoryLauncherState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState),
                std::make_unique<MemoryAuthorizationLauncher>(
                    launcherState));
        Require(
            services->State()
                == LamaPon::OnlineAccountState::SignedOut,
            "A configured service did not start signed out.");

        LamaPon::SetActiveOnlineServices(services.get());
        const OnlineProbe probe;
        Require(
            probe.SignInWithDiscord()
                && services->State()
                    == LamaPon::OnlineAccountState::StartingSignIn,
            "Discord login did not begin asynchronously.");
        Require(
            !services->BeginDiscordSignIn()
                && services->State()
                    == LamaPon::OnlineAccountState::StartingSignIn
                && services->LastErrorCode()
                    == "operation_in_progress",
            "A second login start was not rejected safely.");

        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out waiting for the login start response.");
        Require(
            probe.OnlineAuthorizationUrl()
                == "https://login.example.test/discord"
                && services->LastError().empty()
                && launcherState->launchCount == 1
                && launcherState->lastUrl
                    == "https://login.example.test/discord"
                && !launcherState->lastAllowInsecureLoopback,
            "The authorization URL was not published or stale errors remained.");

        services->Update(1.0f);
        Require(
            services->State()
                == LamaPon::OnlineAccountState::PollingAuthorization,
            "The first authorization poll was not scheduled.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out waiting for the pending poll response.");
        Require(
            !services->IsSignedIn()
                && !probe.OnlineAuthorizationUrl().empty(),
            "A pending response did not preserve the login transaction.");

        services->Update(2.0f);
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedIn,
            "Timed out waiting for the authorized poll response.");
        Require(
            probe.IsOnlineSignedIn()
                && probe.OnlinePlayerId() == "player-42"
                && probe.OnlinePlayerName() == "ラマポン"
                && services->Player().linkedProvider == "discord"
                && services->AuthorizationUrl().empty()
                && storeState->saveCount == 1
                && storeState->token == refreshSecret,
            "The authorized profile was not exposed correctly.");

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            services->Update(0.0f);
        }
        Require(
            launcherState->launchCount == 1,
            "The authorization browser was opened more than once.");

        probe.SignOutOnline();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && services->Player().playerId.empty()
                && storeState->deleteCount == 1
                && storeState->token.empty(),
            "Sign-out did not forget the local account immediately.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedOut,
            "Timed out waiting for the logout response.");
        Require(
            services->LastErrorCode() == "logout_failed"
                && !services->IsSignedIn()
                && services->Player().playerId.empty(),
            "A failed remote logout restored local credentials.");

        const auto publicText = services->LastErrorCode()
            + services->LastError()
            + services->AuthorizationUrl()
            + services->Player().playerId
            + services->Player().displayName;
        Require(
            !Contains(publicText, pollSecret)
                && !Contains(publicText, accessSecret)
                && !Contains(publicText, refreshSecret),
            "A secret escaped through the public OnlineServices API.");

        const auto requests = backend->Requests();
        Require(
            requests.size() == 4
                && requests[0].url.ends_with(
                    L"/v1/auth/login/start")
                && requests[1].url.ends_with(
                    L"/v1/auth/login/complete")
                && requests[2].url.ends_with(
                    L"/v1/auth/login/complete")
                && requests[3].url.ends_with(
                    L"/v1/auth/session/logout")
                && HasHeader(
                    requests[3],
                    L"Authorization",
                    L"Bearer access-secret-must-not-be-public"),
            "The asynchronous state machine sent an unexpected request sequence.");

        LamaPon::SetActiveOnlineServices(nullptr);
    }

    void TestStoredSessionRestoreAndProactiveRotation()
    {
        constexpr std::string_view storedSecret =
            "stored-refresh-secret";
        constexpr std::string_view firstRotatedSecret =
            "first-rotated-refresh-secret";
        constexpr std::string_view secondRotatedSecret =
            "second-rotated-refresh-secret";

        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            200,
            SessionJson(
                "restored-access-secret",
                firstRotatedSecret,
                "restored-player",
                30)));
        responses.push_back(JsonResponse(
            200,
            SessionJson(
                "refreshed-access-secret",
                secondRotatedSecret,
                "restored-player",
                900)));
        const auto backend =
            std::make_shared<ScriptedBackend>(
                std::move(responses));
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = storedSecret;

        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        Require(
            services->State()
                == LamaPon::OnlineAccountState::RestoringSession,
            "Configure did not begin stored-session restoration.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedIn,
            "Timed out restoring a stored session.");
        Require(
            services->IsSignedIn()
                && services->Player().playerId == "restored-player"
                && storeState->loadCount == 1
                && storeState->saveCount == 1
                && storeState->token == firstRotatedSecret,
            "Restoration did not atomically store the rotated token.");

        services->Update(24.0f);
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::RefreshingSession
                && services->IsSignedIn()
                && services->Player().playerId == "restored-player",
            "The session was not refreshed before expiry.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedIn,
            "Timed out refreshing the active session.");
        Require(
            storeState->saveCount == 2
                && storeState->token == secondRotatedSecret,
            "Proactive refresh did not persist token rotation.");

        const auto requests = backend->Requests();
        Require(
            requests.size() == 2
                && requests[0].url.ends_with(
                    L"/v1/auth/session/refresh")
                && requests[1].url.ends_with(
                    L"/v1/auth/session/refresh"),
            "Session restoration sent an unexpected request sequence.");
        const auto firstBody = nlohmann::json::parse(
            std::string(
                requests[0].body.begin(),
                requests[0].body.end()));
        const auto secondBody = nlohmann::json::parse(
            std::string(
                requests[1].body.begin(),
                requests[1].body.end()));
        Require(
            firstBody.value("refreshToken", "") == storedSecret
                && secondBody.value("refreshToken", "")
                    == firstRotatedSecret,
            "Session refresh did not use the expected token generation.");

        const auto publicText = services->LastErrorCode()
            + services->LastError()
            + services->Player().playerId;
        Require(
            !Contains(publicText, storedSecret)
                && !Contains(publicText, firstRotatedSecret)
                && !Contains(publicText, secondRotatedSecret),
            "A restored credential escaped through the public API.");
    }

    void TestAuthorizationBrowserFailureKeepsManualFallback()
    {
        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            201,
            {
                { "transactionId", "browser-failure" },
                { "pollToken", "browser-poll-secret" },
                {
                    "authorizationUrl",
                    "https://login.example.test/manual"
                },
                { "expiresIn", 60 },
                { "pollInterval", 10 }
            }));
        const auto backend =
            std::make_shared<ScriptedBackend>(
                std::move(responses));
        const auto launcherState =
            std::make_shared<MemoryLauncherState>();
        launcherState->succeed = false;
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                {},
                std::make_unique<MemoryAuthorizationLauncher>(
                    launcherState));

        Require(
            services->BeginDiscordSignIn(),
            "The browser fallback login did not start.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out waiting for browser launch fallback.");
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            services->Update(0.0f);
        }
        Require(
            launcherState->launchCount == 1
                && services->AuthorizationUrl()
                    == "https://login.example.test/manual"
                && services->LastErrorCode()
                    == "browser_launch_failed"
                && !Contains(
                    services->LastError(),
                    launcherState->privateError),
            "Browser launch failure did not preserve a safe manual fallback.");
        services->CancelDiscordSignIn();
    }

    void TestRestoreNetworkFailureRetainsCredential()
    {
        constexpr std::string_view storedSecret =
            "offline-refresh-secret";
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = storedSecret;
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [](const LamaPon::HttpRequest&)
                {
                    LamaPon::HttpResponse response;
                    response.transportError =
                        "transport-secret-must-not-be-public";
                    return response;
                },
                std::make_unique<MemoryTokenStore>(storeState));

        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::Error,
            "Timed out waiting for restore network failure.");
        Require(
            storeState->token == storedSecret
                && storeState->deleteCount == 0
                && storeState->saveCount == 0
                && services->LastErrorCode() == "network_error"
                && !Contains(services->LastError(), storedSecret)
                && !Contains(
                    services->LastError(),
                    "transport-secret-must-not-be-public"),
            "A temporary network error discarded or exposed the credential.");
    }

    void TestInvalidStoredTokenIsDeleted()
    {
        constexpr std::string_view invalidSecret =
            "invalid-stored-refresh-secret";
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = invalidSecret;
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [](const LamaPon::HttpRequest&)
                {
                    return JsonResponse(
                        401,
                        {
                            {
                                "error",
                                { { "code", "invalid_refresh_token" } }
                            }
                        });
                },
                std::make_unique<MemoryTokenStore>(storeState));

        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedOut,
            "Timed out rejecting an invalid stored token.");
        Require(
            storeState->deleteCount == 1
                && storeState->token.empty()
                && services->LastErrorCode()
                    == "stored_session_invalid"
                && !Contains(services->LastError(), invalidSecret),
            "An invalid stored token was not deleted safely.");
    }

    void TestRestoreSignOutRace()
    {
        constexpr std::string_view storedRefresh =
            "restore-race-stored-refresh";
        constexpr std::string_view newAccess =
            "restore-race-new-access";
        constexpr std::string_view newRefresh =
            "restore-race-new-refresh";

        {
            LamaPon::HttpResponse logoutResponse;
            logoutResponse.statusCode = 204;
            std::deque<LamaPon::HttpResponse> responses;
            responses.push_back(JsonResponse(
                200,
                SessionJson(newAccess, newRefresh)));
            responses.push_back(std::move(logoutResponse));
            const auto backend =
                std::make_shared<BlockingScriptedBackend>(
                    std::move(responses),
                    1);
            const auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = storedRefresh;
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));

            Require(
                backend->WaitUntilBlocked(),
                "Timed out blocking the restore request.");
            services->SignOut();
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && storeState->deleteCount == 1
                    && storeState->token.empty(),
                "Sign-out did not locally cancel session restoration.");
            backend->Release();
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::SignedOut,
                "Timed out invalidating the raced restore session.");

            const auto requests = backend->Requests();
            Require(
                requests.size() == 2
                    && requests[1].url.ends_with(
                        L"/v1/auth/session/logout")
                    && HasHeader(
                        requests[1],
                        L"Authorization",
                        L"Bearer restore-race-new-access")
                    && storeState->saveCount == 0
                    && services->LastError().empty(),
                "A restore completed after sign-out without invalidating its new session.");
            const auto publicText = services->LastErrorCode()
                + services->LastError();
            Require(
                !Contains(publicText, newAccess)
                    && !Contains(publicText, newRefresh),
                "A raced restore token escaped through the public API.");
        }

        {
            LamaPon::HttpResponse networkFailure;
            networkFailure.transportError =
                "restore-race-network-private";
            std::deque<LamaPon::HttpResponse> responses;
            responses.push_back(std::move(networkFailure));
            const auto backend =
                std::make_shared<BlockingScriptedBackend>(
                    std::move(responses),
                    1);
            const auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = storedRefresh;
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));

            Require(
                backend->WaitUntilBlocked(),
                "Timed out blocking the failed restore request.");
            storeState->failDelete = true;
            services->SignOut();
            Require(
                storeState->deleteCount == 1
                    && storeState->token == storedRefresh,
                "The restore race did not record its failed local delete.");
            storeState->failDelete = false;
            services->SignOut();
            Require(
                storeState->deleteCount == 2
                    && storeState->token.empty(),
                "A repeated sign-out did not retry local credential deletion.");
            backend->Release();
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::SignedOut,
                "A failed cancelled restore did not finish signed out.");
            Require(
                backend->Requests().size() == 1
                    && storeState->deleteCount == 2
                    && storeState->token.empty()
                    && services->LastError().empty(),
                "A failed restore race sent an unnecessary logout or retained credentials.");
        }
    }

    void TestRefreshSignOutRace()
    {
        constexpr std::string_view oldAccess =
            "refresh-race-old-access";
        constexpr std::string_view restoredRefresh =
            "refresh-race-restored-refresh";
        constexpr std::string_view newAccess =
            "refresh-race-new-access";
        constexpr std::string_view newRefresh =
            "refresh-race-new-refresh";

        const auto runCase = [=](const bool refreshSucceeds)
        {
            LamaPon::HttpResponse refreshResponse;
            if (refreshSucceeds)
            {
                refreshResponse = JsonResponse(
                    200,
                    SessionJson(newAccess, newRefresh));
            }
            else
            {
                refreshResponse.transportError =
                    "refresh-race-network-private";
            }
            LamaPon::HttpResponse logoutResponse;
            logoutResponse.statusCode = 204;
            std::deque<LamaPon::HttpResponse> responses;
            responses.push_back(JsonResponse(
                200,
                SessionJson(
                    oldAccess,
                    restoredRefresh,
                    "refresh-race-player",
                    30)));
            responses.push_back(std::move(refreshResponse));
            responses.push_back(std::move(logoutResponse));
            const auto backend =
                std::make_shared<BlockingScriptedBackend>(
                    std::move(responses),
                    2);
            const auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "refresh-race-initial-refresh";
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::SignedIn,
                "Timed out preparing the refresh race.");

            services->Update(24.0f);
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::RefreshingSession
                    && backend->WaitUntilBlocked(),
                "Timed out blocking the proactive refresh.");
            services->SignOut();
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && storeState->deleteCount == 1
                    && storeState->token.empty(),
                "Sign-out did not forget the refreshing session locally.");
            backend->Release();
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::SignedOut,
                "Timed out logging out after the refresh race.");

            const auto requests = backend->Requests();
            const auto expectedAuthorization = refreshSucceeds
                ? L"Bearer refresh-race-new-access"
                : L"Bearer refresh-race-old-access";
            Require(
                requests.size() == 3
                    && requests[2].url.ends_with(
                        L"/v1/auth/session/logout")
                    && HasHeader(
                        requests[2],
                        L"Authorization",
                        expectedAuthorization)
                    && storeState->saveCount == 1
                    && services->LastError().empty(),
                "Refresh/sign-out race used the wrong access token or persisted a late rotation.");
            if (refreshSucceeds)
            {
                Require(
                    !HasHeader(
                        requests[2],
                        L"Authorization",
                        L"Bearer refresh-race-old-access"),
                    "Successful refresh race logged out only the obsolete session.");
            }
        };

        runCase(true);
        runCase(false);
    }

    void TestCancelledAuthorizedPollIsLoggedOut()
    {
        constexpr std::string_view racedAccess =
            "cancel-race-new-access";
        constexpr std::string_view racedRefresh =
            "cancel-race-new-refresh";
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            201,
            {
                { "transactionId", "cancel-race" },
                { "pollToken", "cancel-race-poll" },
                {
                    "authorizationUrl",
                    "https://login.example.test/cancel-race"
                },
                { "expiresIn", 60 },
                { "pollInterval", 1 }
            }));
        responses.push_back(JsonResponse(
            200,
            {
                { "status", "authorized" },
                { "accessToken", racedAccess },
                { "refreshToken", racedRefresh },
                { "expiresIn", 900 },
                {
                    "player",
                    {
                        { "id", "cancel-race-player" },
                        { "displayName", "cancelled" },
                        { "avatarUrl", "" },
                        { "linkedProvider", "discord" }
                    }
                }
            }));
        responses.push_back(std::move(logoutResponse));
        const auto backend =
            std::make_shared<BlockingScriptedBackend>(
                std::move(responses),
                2);
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));

        Require(
            services->BeginDiscordSignIn(),
            "The cancellable authorization did not start.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out starting the cancellation race.");
        services->Update(1.0f);
        Require(
            backend->WaitUntilBlocked(),
            "Timed out blocking the authorization poll.");
        services->CancelDiscordSignIn();
        Require(
            services->State() == LamaPon::OnlineAccountState::SignedOut
                && services->Player().playerId.empty(),
            "Cancellation exposed a raced authorization.");
        backend->Release();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                        == LamaPon::OnlineAccountState::SignedOut
                    && backend->Requests().size() == 3;
            },
            "Timed out invalidating the cancelled authorization.");

        const auto requests = backend->Requests();
        Require(
            HasHeader(
                    requests[2],
                    L"Authorization",
                    L"Bearer cancel-race-new-access")
                && storeState->saveCount == 0
                && storeState->deleteCount == 0
                && services->Player().playerId.empty()
                && services->LastError().empty(),
            "A cancelled authorization left a live or published session.");
    }

    void TestExpiredAuthorizedPollIsLoggedOut()
    {
        constexpr std::string_view racedAccess =
            "expiry-race-new-access";
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            201,
            {
                { "transactionId", "expiry-race" },
                { "pollToken", "expiry-race-poll" },
                {
                    "authorizationUrl",
                    "https://login.example.test/expiry-race"
                },
                { "expiresIn", 30 },
                { "pollInterval", 1 }
            }));
        responses.push_back(JsonResponse(
            200,
            {
                { "status", "authorized" },
                { "accessToken", racedAccess },
                { "refreshToken", "expiry-race-new-refresh" },
                { "expiresIn", 900 },
                {
                    "player",
                    {
                        { "id", "expiry-race-player" },
                        { "displayName", "expired" },
                        { "avatarUrl", "" },
                        { "linkedProvider", "discord" }
                    }
                }
            }));
        responses.push_back(std::move(logoutResponse));
        const auto backend =
            std::make_shared<BlockingScriptedBackend>(
                std::move(responses),
                2);
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));

        Require(
            services->BeginDiscordSignIn(),
            "The expiring authorization did not start.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out starting the expiry race.");
        services->Update(29.0f);
        Require(
            backend->WaitUntilBlocked(),
            "Timed out blocking the expiring authorization poll.");
        services->Update(1.0f);
        Require(
            services->State() == LamaPon::OnlineAccountState::Error
                && services->LastErrorCode() == "login_expired",
            "An in-flight authorization did not expire locally.");

        backend->Release();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                        == LamaPon::OnlineAccountState::Error
                    && backend->Requests().size() == 3;
            },
            "Timed out invalidating the authorization completed after expiry.");
        const auto requests = backend->Requests();
        Require(
            HasHeader(
                    requests[2],
                    L"Authorization",
                    L"Bearer expiry-race-new-access")
                && services->LastErrorCode() == "login_expired"
                && services->Player().playerId.empty()
                && storeState->saveCount == 0,
            "A delayed authorization escaped the expiry cleanup path.");
    }

    void TestExplicitSignOutOverridesExpiredPollCleanup()
    {
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            201,
            {
                { "transactionId", "expiry-signout-race" },
                { "pollToken", "expiry-signout-poll" },
                {
                    "authorizationUrl",
                    "https://login.example.test/expiry-signout"
                },
                { "expiresIn", 30 },
                { "pollInterval", 1 }
            }));
        responses.push_back(JsonResponse(
            200,
            {
                { "status", "authorized" },
                {
                    "accessToken",
                    "expiry-signout-new-access"
                },
                {
                    "refreshToken",
                    "expiry-signout-new-refresh"
                },
                { "expiresIn", 900 },
                {
                    "player",
                    {
                        { "id", "expiry-signout-player" },
                        { "displayName", "expired-signout" },
                        { "avatarUrl", "" },
                        { "linkedProvider", "discord" }
                    }
                }
            }));
        responses.push_back(std::move(logoutResponse));
        const auto backend =
            std::make_shared<BlockingScriptedBackend>(
                std::move(responses),
                2);
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // Configure後に残存資格情報を模擬し、内部cleanup中の明示
        // SignOutがそれを削除することを検証します。
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "expiry-signout-stored-refresh";

        Require(
            services->BeginDiscordSignIn(),
            "The explicit expiry sign-out race did not start.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out starting the explicit expiry sign-out race.");
        services->Update(29.0f);
        Require(
            backend->WaitUntilBlocked(),
            "Timed out blocking the explicit expiry poll.");
        services->Update(1.0f);
        Require(
            services->State() == LamaPon::OnlineAccountState::Error
                && services->LastErrorCode() == "login_expired",
            "The explicit sign-out race did not first expire.");

        backend->Release();
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SigningOut,
            "Timed out reaching delayed authorization cleanup.");
        services->SignOut();
        Require(
            storeState->deleteCount == 1
                && storeState->token.empty()
                && services->LastError().empty(),
            "Explicit sign-out during cleanup did not delete local credentials.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedOut,
            "Explicit sign-out did not override the expiry result.");

        const auto requests = backend->Requests();
        Require(
            requests.size() == 3
                && HasHeader(
                    requests[2],
                    L"Authorization",
                    L"Bearer expiry-signout-new-access")
                && services->LastError().empty(),
            "Explicit sign-out did not finish delayed-session cleanup safely.");
    }

    void TestCancellationIgnoresOldCompletion()
    {
        std::promise<void> senderEnteredPromise;
        auto senderEntered = senderEnteredPromise.get_future();
        std::promise<void> releasePromise;
        const auto release = releasePromise.get_future().share();
        std::promise<void> senderFinishedPromise;
        auto senderFinished = senderFinishedPromise.get_future();
        std::atomic_uint requestCount{};

        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [&senderEnteredPromise,
                    release,
                    &senderFinishedPromise,
                    &requestCount](
                    const LamaPon::HttpRequest&)
                {
                    const auto request = ++requestCount;
                    if (request == 1)
                    {
                        senderEnteredPromise.set_value();
                        release.wait();
                        senderFinishedPromise.set_value();
                    }
                    return JsonResponse(
                        201,
                        {
                            { "transactionId", "cancelled-login" },
                            {
                                "pollToken",
                                "cancelled-poll-secret"
                            },
                            {
                                "authorizationUrl",
                                "https://login.example.test/old"
                            },
                            { "expiresIn", 60 },
                            { "pollInterval", 1 }
                        });
                });
        Require(
            services->BeginDiscordSignIn(),
            "The cancellable login did not start.");
        if (senderEntered.wait_for(3s) != std::future_status::ready)
        {
            releasePromise.set_value();
            throw std::runtime_error(
                "Timed out waiting for the blocked login request.");
        }

        services->CancelDiscordSignIn();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SignedOut
                && services->AuthorizationUrl().empty(),
            "Cancellation did not return to SignedOut immediately.");
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            Require(
                !services->BeginDiscordSignIn(),
                "A cancelled in-flight request allowed another worker.");
            services->CancelDiscordSignIn();
        }
        Require(
            requestCount.load() == 1,
            "Repeated cancel/retry created unbounded workers.");
        releasePromise.set_value();
        Require(
            senderFinished.wait_for(3s) == std::future_status::ready,
            "Timed out releasing the cancelled login request.");

        // sender完了通知とmailbox反映には僅かな差があり得るため、
        // 古い結果を回収して次の開始が受理されるまで更新します。
        const auto deadline =
            std::chrono::steady_clock::now() + 1s;
        bool retryStarted{};
        while (!retryStarted
            && std::chrono::steady_clock::now() < deadline)
        {
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SignedOut
                    && services->AuthorizationUrl().empty()
                    && services->Player().playerId.empty(),
                "A cancelled login's late completion changed public state.");
            services->Update(0.0f);
            retryStarted = services->BeginDiscordSignIn();
            std::this_thread::yield();
        }
        Require(
            retryStarted,
            "A completed cancelled request permanently blocked retry.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::WaitingForAuthorization,
            "Timed out waiting for the retry login response.");
        Require(
            requestCount.load() == 2,
            "Retry did not use exactly one new worker.");
        services->CancelDiscordSignIn();
    }

    void TestDestructionDoesNotWaitForBlockedRequest()
    {
        struct BlockingSender final
        {
            BlockingSender()
                : releaseFuture(release.get_future().share())
            {
            }

            std::promise<void> entered;
            std::promise<void> release;
            std::promise<void> finished;
            std::shared_future<void> releaseFuture;
        };

        const auto blocking = std::make_shared<BlockingSender>();
        auto entered = blocking->entered.get_future();
        auto finished = blocking->finished.get_future();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [blocking](const LamaPon::HttpRequest&)
                {
                    blocking->entered.set_value();
                    blocking->releaseFuture.wait();
                    blocking->finished.set_value();
                    return JsonResponse(
                        503,
                        { { "error", { { "code", "busy" } } } });
                });
        Require(
            services->BeginDiscordSignIn(),
            "The destruction test request did not start.");
        Require(
            entered.wait_for(3s) == std::future_status::ready,
            "Timed out waiting for the blocked request.");

        std::promise<void> destroyedPromise;
        auto destroyed = destroyedPromise.get_future();
        std::thread destroyer(
            [owned = std::move(services),
                &destroyedPromise]() mutable
            {
                owned.reset();
                destroyedPromise.set_value();
            });
        const bool returnedPromptly =
            destroyed.wait_for(500ms) == std::future_status::ready;
        blocking->release.set_value();
        destroyer.join();
        Require(
            finished.wait_for(3s) == std::future_status::ready,
            "Timed out releasing the detached request.");
        Require(
            returnedPromptly,
            "OnlineServices destruction waited for a blocked request.");
    }

    void TestTransportSecretsAreRedacted()
    {
        constexpr std::string_view transportSecret =
            "transport-secret-must-not-appear";
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [](const LamaPon::HttpRequest&)
                {
                    LamaPon::HttpResponse response;
                    response.transportError =
                        "transport-secret-must-not-appear";
                    return response;
                });
        Require(
            services->BeginDiscordSignIn(),
            "The redaction test login did not start.");
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::Error,
            "Timed out waiting for the transport failure.");
        Require(
            services->LastErrorCode() == "network_error"
                && !Contains(services->LastError(), transportSecret),
            "A transport-layer secret escaped through LastError.");
    }
}

int main()
{
    try
    {
        TestScriptFallbackAndActiveService();
        TestActiveServiceLifetime();
        TestLoginPollingAndLocalLogout();
        TestStoredSessionRestoreAndProactiveRotation();
        TestAuthorizationBrowserFailureKeepsManualFallback();
        TestRestoreNetworkFailureRetainsCredential();
        TestInvalidStoredTokenIsDeleted();
        TestRestoreSignOutRace();
        TestRefreshSignOutRace();
        TestCancelledAuthorizedPollIsLoggedOut();
        TestExpiredAuthorizedPollIsLoggedOut();
        TestExplicitSignOutOverridesExpiredPollCleanup();
        TestCancellationIgnoresOldCompletion();
        TestDestructionDoesNotWaitForBlockedRequest();
        TestTransportSecretsAreRedacted();
        LamaPon::SetActiveOnlineServices(nullptr);
    }
    catch (const std::exception& error)
    {
        LamaPon::SetActiveOnlineServices(nullptr);
        std::cerr
            << "Online services tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    std::cout << "Online services tests passed.\n";
    return 0;
}
