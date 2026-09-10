#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Core/Application.h"
#include "LamaPon/Scripting/Script.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
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
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                });
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
                && services->LastError().empty(),
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
                && services->AuthorizationUrl().empty(),
            "The authorized profile was not exposed correctly.");

        probe.SignOutOnline();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && services->Player().playerId.empty(),
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
