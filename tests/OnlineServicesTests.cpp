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

    // Require(condition: 判定条件, message: 失敗理由): 条件不成立をテスト失敗として通知する。
    void Require(const bool condition, const char* message)
    {
        // 条件不成立をruntime errorとして通知する。
        if (!condition)
        {
            // 待機または処理失敗の理由をテストrunnerへ通知する。
            throw std::runtime_error(message);
        }
    }

    // JsonResponse(status: HTTP状態コード, body: JSON本文): 指定状態とJSON本文からHTTP応答を作る。
    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const nlohmann::json& body)
    {
        // response: HTTP応答.
        LamaPon::HttpResponse response;
        response.statusCode = status;
        // text: JSON本文文字列.
        const auto text = body.dump();
        response.body.assign(text.begin(), text.end());
        // 処理対象のHTTP応答を返す。
        return response;
    }

    class ScriptedBackend final
    {
    public:
        // ScriptedBackend(responses: 順番に返す応答列): 応答を順番に返すテスト用バックエンドを作る。
        explicit ScriptedBackend(
            std::deque<LamaPon::HttpResponse> responses)
            : m_responses(std::move(responses))
        {
        }

        // Send(request: HTTP要求): 要求を記録して次の応答を返す。
        LamaPon::HttpResponse Send(
            const LamaPon::HttpRequest& request)
        {
            // lock: 状態を守るmutex lock.
            std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
            // 応答キューが尽きた場合はtransport errorを返す。
            if (m_responses.empty())
            {
                // response: HTTP応答.
                LamaPon::HttpResponse response;
                response.transportError = "Unexpected request.";
                // 処理対象のHTTP応答を返す。
                return response;
            }
            // response: HTTP応答.
            auto response = std::move(m_responses.front());
            m_responses.pop_front();
            // 処理対象のHTTP応答を返す。
            return response;
        }

        // Requests(): 記録済み要求のコピーを返す。
        [[nodiscard]] std::vector<LamaPon::HttpRequest>
            Requests() const
        {
            // lock: 状態を守るmutex lock.
            std::scoped_lock lock(m_mutex);
            // 記録済みHTTP要求のsnapshotを返す。
            return m_requests;
        }

    private:
        // m_mutex: 共有状態を守るmutex.
        mutable std::mutex m_mutex;
        // m_responses: 順番に返す応答列.
        std::deque<LamaPon::HttpResponse> m_responses;
        // m_requests: 記録したHTTP要求.
        std::vector<LamaPon::HttpRequest> m_requests;
    };

    class BlockingScriptedBackend final
    {
    public:
        // BlockingScriptedBackend(responses: 順番に返す応答列, blockedRequest: 停止する要求番号): 指定要求を停止できるテスト用バックエンドを作る。
        BlockingScriptedBackend(
            std::deque<LamaPon::HttpResponse> responses,
            const std::size_t blockedRequest)
            : m_responses(std::move(responses))
            , m_blockedRequest(blockedRequest)
        {
        }

        // Send(request: HTTP要求): 指定要求だけを解放まで待つ。
        LamaPon::HttpResponse Send(
            const LamaPon::HttpRequest& request)
        {
            // lock: 状態を守るmutex lock.
            std::unique_lock lock(m_mutex);
            m_requests.push_back(request);
            // requestNumber: HTTP要求の通番.
            const auto requestNumber = m_requests.size();
            // response: HTTP応答.
            LamaPon::HttpResponse response;
            // 応答キューが尽きた場合はtransport errorを返す。
            if (m_responses.empty())
            {
                response.transportError = "Unexpected request.";
            }
            // 応答が残っている場合はキュー先頭を取り出す。
            else
            {
                response = std::move(m_responses.front());
                m_responses.pop_front();
            }

            // 指定通番の要求だけを解放待ちにする。
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
                        // 待機要求を解放する状態を返す。
                        return m_released;
                    });
            }
            // 処理対象のHTTP応答を返す。
            return response;
        }

        // WaitUntilBlocked(): 要求が停止状態になるまで期限付きで待つ。
        [[nodiscard]] bool WaitUntilBlocked()
        {
            // lock: 状態を守るmutex lock.
            std::unique_lock lock(m_mutex);
            // 要求停止状態を期限付きで待った結果を返す。
            return m_condition.wait_for(
                lock,
                3s,
                [this]
                {
                    // backendが要求を停止した状態を返す。
                    return m_requestBlocked;
                });
        }

        // Release(): 停止中の要求を再開させる。
        void Release()
        {
            {
                // lock: 状態を守るmutex lock.
                std::scoped_lock lock(m_mutex);
                m_released = true;
            }
            m_condition.notify_all();
        }

        // Requests(): 記録済み要求のコピーを返す。
        [[nodiscard]] std::vector<LamaPon::HttpRequest>
            Requests() const
        {
            // lock: 状態を守るmutex lock.
            std::scoped_lock lock(m_mutex);
            // 記録済みHTTP要求のsnapshotを返す。
            return m_requests;
        }

    private:
        // m_mutex: 共有状態を守るmutex.
        mutable std::mutex m_mutex;
        // m_condition: 要求待機用condition variable.
        std::condition_variable m_condition;
        // m_responses: 順番に返す応答列.
        std::deque<LamaPon::HttpResponse> m_responses;
        // m_requests: 記録したHTTP要求.
        std::vector<LamaPon::HttpRequest> m_requests;
        // m_blockedRequest: 停止対象の要求番号.
        std::size_t m_blockedRequest{};
        // m_requestBlocked: 要求停止中の状態.
        bool m_requestBlocked{};
        // m_released: 待機解除状態.
        bool m_released{};
    };

    struct MemoryTokenStoreState final
    {
        // loadStatus: 保存tokenの読込状態.
        LamaPon::Detail::RefreshTokenLoadStatus loadStatus{
            LamaPon::Detail::RefreshTokenLoadStatus::NotFound
        };
        // token: refresh token.
        std::string token;
        // loadCount: 読込回数.
        std::size_t loadCount{};
        // saveCount: 保存回数.
        std::size_t saveCount{};
        // deleteCount: 削除回数.
        std::size_t deleteCount{};
        // failSave: 保存失敗の設定.
        bool failSave{};
        // failDelete: 削除失敗の設定.
        bool failDelete{};
        // usageLeaseOwner: 利用リースの所有者.
        std::atomic<const void*> usageLeaseOwner{};
        // usageLeaseAcquireCount: リース取得回数.
        std::atomic_size_t usageLeaseAcquireCount{};
        // usageLeaseRejectCount: リース拒否回数.
        std::atomic_size_t usageLeaseRejectCount{};
        // usageLeaseReleaseCount: リース解放回数.
        std::atomic_size_t usageLeaseReleaseCount{};
        // privateError: 公開しないplatform error.
        std::string privateError{
            "platform-secret-must-not-be-public"
        };
    };

    class MemoryTokenStore final
        : public LamaPon::Detail::IRefreshTokenStore
    {
    public:
        // MemoryTokenStore(state: 共有テスト状態): 共有状態を使うテスト用token storeを作る。
        explicit MemoryTokenStore(
            std::shared_ptr<MemoryTokenStoreState> state)
            : m_state(std::move(state))
        {
        }

        // ~MemoryTokenStore(): 保持中の利用リースを解放する。
        ~MemoryTokenStore() override
        {
            ReleaseUsageLease();
        }

        // MemoryTokenStore(): リース所有権の複製を防ぐためコピーを禁止する。
        MemoryTokenStore(const MemoryTokenStore&) = delete;
        MemoryTokenStore& operator=(const MemoryTokenStore&) = delete;
        // MemoryTokenStore(): リース状態を複製しないため移動を禁止する。
        MemoryTokenStore(MemoryTokenStore&&) = delete;
        MemoryTokenStore& operator=(MemoryTokenStore&&) = delete;

        // AcquireUsageLease(): refresh token利用リースを排他取得する。
        LamaPon::Detail::OnlinePlatformResult
            AcquireUsageLease() override
        {
            // 利用リースを既に保持する場合は再取得を省く。
            if (m_usageLeaseHeld)
            {
                // lease取得または削除の成功結果を返す。
                return { true, {}, {} };
            }
            // expected: compare-exchangeで期待する未所有値.
            const void* expected{};
            // 別serviceがリースを使用中なら取得を拒否する。
            if (!m_state->usageLeaseOwner.compare_exchange_strong(
                    expected,
                    this,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                ++m_state->usageLeaseRejectCount;
                // lease取得または保存失敗をplatform resultで返す。
                return {
                    false,
                    "credential_usage_unavailable",
                    "Credential usage is unavailable."
                };
            }
            m_usageLeaseHeld = true;
            ++m_state->usageLeaseAcquireCount;
            // lease取得または削除の成功結果を返す。
            return { true, {}, {} };
        }

        // ReleaseUsageLease(): 保持中のrefresh token利用リースを解放する。
        void ReleaseUsageLease() noexcept override
        {
            // 利用リースを既に保持する場合は再取得を省く。
            if (!m_usageLeaseHeld)
            {
                // 保持していないleaseの解放を終了する。
                return;
            }
            // expected: compare-exchangeで期待する現在所有者.
            const void* expected = this;
            // 別serviceがリースを使用中なら取得を拒否する。
            if (m_state->usageLeaseOwner.compare_exchange_strong(
                    expected,
                    nullptr,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                ++m_state->usageLeaseReleaseCount;
            }
            m_usageLeaseHeld = false;
        }

        // Load(): 保存済みrefresh tokenの状態を返す。
        LamaPon::Detail::RefreshTokenLoadResult Load() override
        {
            ++m_state->loadCount;
            // result: refresh token読込結果.
            LamaPon::Detail::RefreshTokenLoadResult result;
            result.status = m_state->loadStatus;
            // 保存tokenを読み込めた場合は値を返す。
            if (result.Loaded())
            {
                result.refreshToken = m_state->token;
            }
            // NotFound以外の読込失敗を非公開エラーとして返す。
            else if (result.status
                != LamaPon::Detail::RefreshTokenLoadStatus::NotFound)
            {
                result.errorCode = m_state->privateError;
                result.errorMessage = m_state->privateError;
            }
            // refresh tokenの読込結果を返す。
            return result;
        }

        // Save(refreshToken: 保存するrefresh token): refresh tokenの保存結果を状態へ反映する。
        LamaPon::Detail::OnlinePlatformResult Save(
            const std::string_view refreshToken) override
        {
            ++m_state->saveCount;
            // 保存失敗の設定をplatform errorとして返す。
            if (m_state->failSave)
            {
                // lease取得または保存失敗をplatform resultで返す。
                return {
                    false,
                    m_state->privateError,
                    m_state->privateError
                };
            }
            m_state->token = refreshToken;
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            // lease取得または削除の成功結果を返す。
            return { true, {}, {} };
        }

        // Delete(): 保存済みrefresh tokenを削除する。
        LamaPon::Detail::OnlinePlatformResult Delete() override
        {
            ++m_state->deleteCount;
            // 削除失敗の設定をplatform errorとして返す。
            if (m_state->failDelete)
            {
                // lease取得または保存失敗をplatform resultで返す。
                return {
                    false,
                    m_state->privateError,
                    m_state->privateError
                };
            }
            m_state->token.clear();
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::NotFound;
            // lease取得または削除の成功結果を返す。
            return { true, {}, {} };
        }

    private:
        // m_state: 共有テスト状態.
        std::shared_ptr<MemoryTokenStoreState> m_state;
        // m_usageLeaseHeld: 利用リース保持状態.
        bool m_usageLeaseHeld{};
    };

    struct MemoryLauncherState final
    {
        // launchCount: 認証起動回数.
        std::size_t launchCount{};
        // lastUrl: 最後に起動した認証URL.
        std::string lastUrl;
        // lastAllowInsecureLoopback: 最後のloopback許可.
        bool lastAllowInsecureLoopback{};
        // succeed: 認証起動の成功設定.
        bool succeed{ true };
        // privateError: 公開しないplatform error.
        std::string privateError{
            "launcher-secret-must-not-be-public"
        };
    };

    class MemoryAuthorizationLauncher final
        : public LamaPon::Detail::IAuthorizationLauncher
    {
    public:
        // MemoryAuthorizationLauncher(state: 共有テスト状態): 共有状態を使うテスト用認証launcherを作る。
        explicit MemoryAuthorizationLauncher(
            std::shared_ptr<MemoryLauncherState> state)
            : m_state(std::move(state))
        {
        }

        // Launch(authorizationUrl: 認証URL, allowInsecureLoopback: loopbackのHTTP許可): 起動条件と結果を共有状態へ記録する。
        LamaPon::Detail::OnlinePlatformResult Launch(
            const std::string_view authorizationUrl,
            const bool allowInsecureLoopback) override
        {
            ++m_state->launchCount;
            m_state->lastUrl = authorizationUrl;
            m_state->lastAllowInsecureLoopback =
                allowInsecureLoopback;
            // 認証launcherの設定結果をplatform resultとして返す。
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
        // m_state: 共有テスト状態.
        std::shared_ptr<MemoryLauncherState> m_state;
    };

    // TestConfiguration(openAuthorizationBrowser: ブラウザー起動設定): オンラインサービス用の基準設定を作る。
    LamaPon::OnlineServiceConfiguration TestConfiguration(
        const bool openAuthorizationBrowser = true)
    {
        // configuration: serviceの基準設定.
        LamaPon::OnlineServiceConfiguration configuration;
        configuration.serviceBaseUrl =
            "https://online.example.test";
        configuration.gameId = "online-services-tests";
        configuration.environmentId = "test";
        configuration.openAuthorizationBrowser =
            openAuthorizationBrowser;
        // 組み立てたservice設定を返す。
        return configuration;
    }

    // SessionJson(accessToken: access token, refreshToken: refresh token, playerId: player ID, expiresIn: seconds): API用session JSONを作る。
    nlohmann::json SessionJson(
        const std::string_view accessToken,
        const std::string_view refreshToken,
        const std::string_view playerId = "player-42",
        const std::uint32_t expiresIn = 900)
    {
        // API応答として使うsession JSONを返す。
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

    // UpdateUntil(services: オンラインservice, predicate: 完了条件, timeoutMessage: 期限切れの失敗理由): 完了条件が成立するまでserviceを更新する。
    template<class Predicate>
    void UpdateUntil(
        LamaPon::OnlineServices& services,
        Predicate&& predicate,
        const char* timeoutMessage)
    {
        // deadline: 待機期限.
        const auto deadline =
            std::chrono::steady_clock::now() + 10s;
        // predicateが成立するまでserviceを更新する。
        while (!std::forward<Predicate>(predicate)())
        {
            services.Update(0.0f);
            // 待機期限を超えた場合はテストを失敗させる。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // 待機または処理失敗の理由をテストrunnerへ通知する。
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    // UpdateUntilState(services: service, expected: 状態, timeoutMessage: timeout理由): 期待状態まで更新する。
    void UpdateUntilState(
        LamaPon::OnlineServices& services,
        const LamaPon::OnlineAccountState expected,
        const char* timeoutMessage)
    {
        UpdateUntil(
            services,
            [&services, expected]
            {
                // serviceが期待するaccount状態か返す。
                return services.State() == expected;
            },
            timeoutMessage);
    }

    // WaitUntilCurrentTaskCompleted(services: オンラインservice, timeoutMessage: 期限切れの失敗理由): 現在の非同期処理が完了するまで待つ。
    void WaitUntilCurrentTaskCompleted(
        LamaPon::OnlineServices& services,
        const char* timeoutMessage)
    {
        // deadline: 待機期限.
        const auto deadline =
            std::chrono::steady_clock::now() + 10s;
        // 現在のtaskが完了するまで状態を更新する。
        while (!LamaPon::Detail::OnlineServicesTestAccess::
            CurrentTaskCompleted(services))
        {
            // 待機期限を超えた場合はテストを失敗させる。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // 待機または処理失敗の理由をテストrunnerへ通知する。
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    // Contains(text: 検索対象文字列, value: 期待値): 文字列に指定値が含まれるか返す。
    bool Contains(
        const std::string_view text,
        const std::string_view value)
    {
        // 指定値が検索対象に含まれるか返す。
        return text.find(value) != std::string_view::npos;
    }

    // HasHeader(request: HTTP要求, name: ヘッダー名, value: 期待値): 要求ヘッダーに指定値があるか確認する。
    bool HasHeader(
        const LamaPon::HttpRequest& request,
        const std::wstring_view name,
        const std::wstring_view value)
    {
        // headerName（名前）とheaderValue（値）を各要求で照合する。
        for (const auto& [headerName, headerValue] : request.headers)
        {
            // 要求ヘッダーの名前と値が一致するか確認する。
            if (headerName == name && headerValue == value)
            {
                // 一致するヘッダーが見つかったことを返す。
                return true;
            }
        }
        // 一致するヘッダーがないことを返す。
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

    // TestScriptFallbackAndActiveService(): fallback処理とactive serviceの共有を検証する。
    void TestScriptFallbackAndActiveService()
    {
        LamaPon::SetActiveOnlineServices(nullptr);
        // probe: service状態の確認用probe.
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

    // TestActiveServiceLifetime(): active serviceの寿命と破棄後の再登録を検証する。
    void TestActiveServiceLifetime()
    {
        // services: オンラインservice instance.
        auto services = std::make_unique<LamaPon::OnlineServices>();
        LamaPon::SetActiveOnlineServices(services.get());
        services.reset();
        Require(
            LamaPon::ActiveOnlineServices() == nullptr,
            "Destroying the active service left a dangling pointer.");

        // external: 外部で登録するservice.
        LamaPon::OnlineServices external;
        LamaPon::SetActiveOnlineServices(&external);
        {
            // application: 未初期化instanceの破棄で外部所有者のactive登録を消さない。
            LamaPon::Application application;
        }
        Require(
            LamaPon::ActiveOnlineServices() == &external,
            "Application cleared another owner's active service.");
        LamaPon::SetActiveOnlineServices(nullptr);
    }

    // TestLoginPollingAndLocalLogout(): login pollingとローカルlogoutを検証する。
    void TestLoginPollingAndLocalLogout()
    {
        // pollSecret: poll応答用secret.
        constexpr std::string_view pollSecret =
            "poll-secret-must-not-be-public";
        // accessSecret: access token用secret.
        constexpr std::string_view accessSecret =
            "access-secret-must-not-be-public";
        // refreshSecret: refresh token用secret.
        constexpr std::string_view refreshSecret =
            "refresh-secret-must-not-be-public";

        // responses: script済みHTTP応答列.
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

        // backend: 記録機能付きHTTP backend.
        const auto backend =
            std::make_shared<ScriptedBackend>(
                std::move(responses));
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        // launcherState: 認証launcherの共有状態.
        const auto launcherState =
            std::make_shared<MemoryLauncherState>();
        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // この要求をscript済みHTTP backendへ渡して応答を返す。
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
        // probe: service状態の確認用probe.
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

        // attempt: pollingの再試行番号としてlogin状態を上限付きで待つ。
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

        // publicText: 利用者向けエラー文.
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

        // requests: 記録済みHTTP要求一覧.
        const auto requests = backend->Requests();
        // allRequestsNamespaced: 全要求のpath確認結果.
        bool allRequestsNamespaced = !requests.empty();
        // request: 記録要求ごとにAPI pathのnamespaceを確認する。
        for (const auto& request : requests)
        {
            allRequestsNamespaced = allRequestsNamespaced
                && HasHeader(
                    request,
                    L"X-LamaPon-Game-Id",
                    L"online-services-tests")
                && HasHeader(
                    request,
                    L"X-LamaPon-Environment-Id",
                    L"test");
        }
        Require(
            requests.size() == 4
                && allRequestsNamespaced
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

    // TestStoredSessionRestoreAndProactiveRotation(): 保存sessionの復元と期限前rotationを検証する。
    void TestStoredSessionRestoreAndProactiveRotation()
    {
        // storedSecret: 保存済みtoken用secret.
        constexpr std::string_view storedSecret =
            "stored-refresh-secret";
        // firstRotatedSecret: 1回目rotation後token.
        constexpr std::string_view firstRotatedSecret =
            "first-rotated-refresh-secret";
        // secondRotatedSecret: 2回目rotation後token.
        constexpr std::string_view secondRotatedSecret =
            "second-rotated-refresh-secret";

        // responses: script済みHTTP応答列.
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
        // backend: 記録機能付きHTTP backend.
        const auto backend =
            std::make_shared<ScriptedBackend>(
                std::move(responses));
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = storedSecret;

        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // この要求をscript済みHTTP backendへ渡して応答を返す。
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

        // requests: 記録済みHTTP要求一覧.
        const auto requests = backend->Requests();
        Require(
            requests.size() == 2
                && requests[0].url.ends_with(
                    L"/v1/auth/session/refresh")
                && requests[1].url.ends_with(
                    L"/v1/auth/session/refresh"),
            "Session restoration sent an unexpected request sequence.");
        // firstBody: 1回目のtoken応答JSON.
        const auto firstBody = nlohmann::json::parse(
            std::string(
                requests[0].body.begin(),
                requests[0].body.end()));
        // secondBody: 2回目のtoken応答JSON.
        const auto secondBody = nlohmann::json::parse(
            std::string(
                requests[1].body.begin(),
                requests[1].body.end()));
        Require(
            firstBody.value("refreshToken", "") == storedSecret
                && secondBody.value("refreshToken", "")
                    == firstRotatedSecret,
            "Session refresh did not use the expected token generation.");

        // publicText: 利用者向けエラー文.
        const auto publicText = services->LastErrorCode()
            + services->LastError()
            + services->Player().playerId;
        Require(
            !Contains(publicText, storedSecret)
                && !Contains(publicText, firstRotatedSecret)
                && !Contains(publicText, secondRotatedSecret),
            "A restored credential escaped through the public API.");
    }

    // TestAuthorizationBrowserFailureKeepsManualFallback(): ブラウザー起動失敗後の手動fallbackを検証する。
    void TestAuthorizationBrowserFailureKeepsManualFallback()
    {
        // responses: script済みHTTP応答列.
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
        // backend: 記録機能付きHTTP backend.
        const auto backend =
            std::make_shared<ScriptedBackend>(
                std::move(responses));
        // launcherState: 認証launcherの共有状態.
        const auto launcherState =
            std::make_shared<MemoryLauncherState>();
        launcherState->succeed = false;
        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // この要求をscript済みHTTP backendへ渡して応答を返す。
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
        // attempt: pollingの再試行番号としてlogin状態を上限付きで待つ。
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

    // TestRestoreNetworkFailureRetainsCredential(): restore時の通信失敗でcredentialを保持する。
    void TestRestoreNetworkFailureRetainsCredential()
    {
        // storedSecret: 保存済みtoken用secret.
        constexpr std::string_view storedSecret =
            "offline-refresh-secret";
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = storedSecret;
        // services: オンラインservice instance.
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [](const LamaPon::HttpRequest&)
                {
                    // response: HTTP応答.
                    LamaPon::HttpResponse response;
                    response.transportError =
                        "transport-secret-must-not-be-public";
                    // 処理対象のHTTP応答を返す。
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

    // TestInvalidStoredTokenIsDeleted(): 不正な保存tokenを削除する。
    void TestInvalidStoredTokenIsDeleted()
    {
        // invalidSecret: 不正な保存tokenのsecret.
        constexpr std::string_view invalidSecret =
            "invalid-stored-refresh-secret";
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = invalidSecret;
        // services: オンラインservice instance.
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [](const LamaPon::HttpRequest&)
                {
                    // このrequestに対応するscript済みHTTP応答を返す。
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

    // TestRestoreSignOutRace(): restore中のsign-out競合を検証する。
    void TestRestoreSignOutRace()
    {
        // storedRefresh: 保存済みrefresh token.
        constexpr std::string_view storedRefresh =
            "restore-race-stored-refresh";
        // newAccess: 新しいaccess token.
        constexpr std::string_view newAccess =
            "restore-race-new-access";
        // newRefresh: 新しいrefresh token.
        constexpr std::string_view newRefresh =
            "restore-race-new-refresh";

        {
            // logoutResponse: logout用HTTP応答.
            LamaPon::HttpResponse logoutResponse;
            logoutResponse.statusCode = 204;
            // responses: script済みHTTP応答列.
            std::deque<LamaPon::HttpResponse> responses;
            responses.push_back(JsonResponse(
                200,
                SessionJson(newAccess, newRefresh)));
            responses.push_back(std::move(logoutResponse));
            // backend: 記録機能付きHTTP backend.
            const auto backend =
                std::make_shared<BlockingScriptedBackend>(
                    std::move(responses),
                    1);
            // storeState: memory token storeの共有状態.
            const auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = storedRefresh;
            // services: テスト対象、request: fake backendへ渡す送信要求。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // この要求をscript済みHTTP backendへ渡して応答を返す。
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

            // requests: 記録済みHTTP要求一覧.
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
            // publicText: 利用者向けエラー文.
            const auto publicText = services->LastErrorCode()
                + services->LastError();
            Require(
                !Contains(publicText, newAccess)
                    && !Contains(publicText, newRefresh),
                "A raced restore token escaped through the public API.");
        }

        {
            // networkFailure: 失敗するHTTP応答.
            LamaPon::HttpResponse networkFailure;
            networkFailure.transportError =
                "restore-race-network-private";
            // responses: script済みHTTP応答列.
            std::deque<LamaPon::HttpResponse> responses;
            responses.push_back(std::move(networkFailure));
            // backend: 記録機能付きHTTP backend.
            const auto backend =
                std::make_shared<BlockingScriptedBackend>(
                    std::move(responses),
                    1);
            // storeState: memory token storeの共有状態.
            const auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = storedRefresh;
            // services: テスト対象、request: fake backendへ渡す送信要求。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // この要求をscript済みHTTP backendへ渡して応答を返す。
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

    // TestRefreshSignOutRace(): refresh中のsign-out競合を検証する。
    void TestRefreshSignOutRace()
    {
        // oldAccess: 古いaccess token.
        const std::string oldAccess =
            "refresh-race-old-access";
        // restoredRefresh: 復元後のrefresh token.
        const std::string restoredRefresh =
            "refresh-race-restored-refresh";
        // newAccess: 新しいaccess token.
        const std::string newAccess =
            "refresh-race-new-access";
        // newRefresh: 新しいrefresh token.
        const std::string newRefresh =
            "refresh-race-new-refresh";

        // runCase(refreshSucceeds: 成功応答を選ぶ条件): sign-outとrefreshの競合を1ケース実行する。
        const auto runCase = [=](const bool refreshSucceeds)
        {
            // refreshResponse: refresh用HTTP応答.
            LamaPon::HttpResponse refreshResponse;
            // refresh成功と失敗の応答を分ける。
            if (refreshSucceeds)
            {
                const auto refreshJson =
                    SessionJson(newAccess, newRefresh);
                refreshResponse = JsonResponse(200, refreshJson);
            }
            // refreshが失敗した場合はnetwork errorを返す。
            else
            {
                refreshResponse.transportError =
                    "refresh-race-network-private";
            }
            // logoutResponse: logout用HTTP応答.
            LamaPon::HttpResponse logoutResponse;
            logoutResponse.statusCode = 204;
            // responses: script済みHTTP応答列.
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
            // backend: 記録機能付きHTTP backend.
            const auto backend =
                std::make_shared<BlockingScriptedBackend>(
                    std::move(responses),
                    2);
            // storeState: memory token storeの共有状態.
            const auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "refresh-race-initial-refresh";
            // services: テスト対象、request: fake backendへ渡す送信要求。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // この要求をscript済みHTTP backendへ渡して応答を返す。
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

            // requests: 記録済みHTTP要求一覧.
            const auto requests = backend->Requests();
            // expectedAuthorization: 期待するAuthorization値.
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
            // refresh成功と失敗の応答を分ける。
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

    // TestCancelledAuthorizedPollIsLoggedOut(): 認可済みpollのcancel後にlogoutする。
    void TestCancelledAuthorizedPollIsLoggedOut()
    {
        // racedAccess: 競合時のaccess token.
        constexpr std::string_view racedAccess =
            "cancel-race-new-access";
        // racedRefresh: 競合時のrefresh token.
        constexpr std::string_view racedRefresh =
            "cancel-race-new-refresh";
        // logoutResponse: logout用HTTP応答.
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        // responses: script済みHTTP応答列.
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
        // backend: 記録機能付きHTTP backend.
        const auto backend =
            std::make_shared<BlockingScriptedBackend>(
                std::move(responses),
                2);
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // この要求をscript済みHTTP backendへ渡して応答を返す。
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
                // 計算した結果を呼び出し元へ返す。
                return services->State()
                        == LamaPon::OnlineAccountState::SignedOut
                    && backend->Requests().size() == 3;
            },
            "Timed out invalidating the cancelled authorization.");

        // requests: 記録済みHTTP要求一覧.
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

    // TestExpiredAuthorizedPollIsLoggedOut(): 期限切れの認可済みpollをlogoutする。
    void TestExpiredAuthorizedPollIsLoggedOut()
    {
        // racedAccess: 競合時のaccess token.
        constexpr std::string_view racedAccess =
            "expiry-race-new-access";
        // logoutResponse: logout用HTTP応答.
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        // responses: script済みHTTP応答列.
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
        // backend: 記録機能付きHTTP backend.
        const auto backend =
            std::make_shared<BlockingScriptedBackend>(
                std::move(responses),
                2);
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // この要求をscript済みHTTP backendへ渡して応答を返す。
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
                // 計算した結果を呼び出し元へ返す。
                return services->State()
                        == LamaPon::OnlineAccountState::Error
                    && backend->Requests().size() == 3;
            },
            "Timed out invalidating the authorization completed after expiry.");
        // requests: 記録済みHTTP要求一覧.
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

    // TestCompletedSessionsExpireBeforeReap(): 完了済みsessionをreap前に期限切れにする。
    void TestCompletedSessionsExpireBeforeReap()
    {
        {
            // expiredAccess: 期限切れaccess token.
            constexpr std::string_view expiredAccess =
                "expired-completed-restore-access";
            // logoutResponse: logout用HTTP応答.
            LamaPon::HttpResponse logoutResponse;
            logoutResponse.statusCode = 204;
            // backend: 記録機能付きHTTP backend.
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        200,
                        SessionJson(
                            expiredAccess,
                            "expired-completed-restore-refresh",
                            "expired-completed-restore-player",
                            1)),
                    std::move(logoutResponse)
                });
            // storeState: memory token storeの共有状態.
            auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "expired-completed-stored-refresh";
            // services: テスト対象、request: fake backendへ渡す送信要求。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // この要求をscript済みHTTP backendへ渡して応答を返す。
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));

            WaitUntilCurrentTaskCompleted(
                *services,
                "Completed restore was not ready for TTL aging.");
            Require(
                LamaPon::Detail::OnlineServicesTestAccess::
                    AgeCurrentTaskCompletion(*services, 31.0f),
                "Could not age the completed restore.");
            services->Update(0.0f);
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && services->Player().playerId.empty()
                    && storeState->saveCount == 0
                    && storeState->deleteCount == 1
                    && storeState->token.empty(),
                "An expired completed restore was published or persisted.");
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::Error,
                "Expired completed restore cleanup did not finish.");
            // requests: 記録済みHTTP要求一覧.
            const auto requests = backend->Requests();
            Require(
                requests.size() == 2
                    && HasHeader(
                        requests.back(),
                        L"Authorization",
                        L"Bearer expired-completed-restore-access")
                    && services->LastErrorCode() == "request_failed",
                "Expired completed restore was not revoked.");
        }

        {
            // expiredAccess: 期限切れaccess token.
            constexpr std::string_view expiredAccess =
                "expired-completed-poll-access";
            // authorized: 認可済みsession JSON.
            auto authorized = SessionJson(
                expiredAccess,
                "expired-completed-poll-refresh",
                "expired-completed-poll-player",
                1);
            authorized["status"] = "authorized";
            // logoutResponse: logout用HTTP応答.
            LamaPon::HttpResponse logoutResponse;
            logoutResponse.statusCode = 204;
            // backend: 記録機能付きHTTP backend.
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201,
                        {
                            {
                                "transactionId",
                                "expired-completed-transaction"
                            },
                            { "pollToken", "expired-completed-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/expired-completed"
                            },
                            { "expiresIn", 60 },
                            { "pollInterval", 1 }
                        }),
                    JsonResponse(200, authorized),
                    std::move(logoutResponse)
                });
            // storeState: memory token storeの共有状態.
            auto storeState =
                std::make_shared<MemoryTokenStoreState>();
            // services: テスト対象、request: fake backendへ渡す送信要求。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    TestConfiguration(false),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // この要求をscript済みHTTP backendへ渡して応答を返す。
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));

            Require(
                services->BeginDiscordSignIn(),
                "Completed-poll expiry login did not start.");
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::WaitingForAuthorization,
                "Completed-poll expiry login start did not finish.");
            services->Update(1.0f);
            WaitUntilCurrentTaskCompleted(
                *services,
                "Completed poll was not ready for TTL aging.");
            Require(
                LamaPon::Detail::OnlineServicesTestAccess::
                    AgeCurrentTaskCompletion(*services, 31.0f),
                "Could not age the completed poll.");
            services->Update(0.0f);
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && services->Player().playerId.empty()
                    && storeState->saveCount == 0
                    && storeState->deleteCount == 0,
                "An expired completed authorization was published or persisted.");
            UpdateUntilState(
                *services,
                LamaPon::OnlineAccountState::Error,
                "Expired completed authorization cleanup did not finish.");
            // requests: 記録済みHTTP要求一覧.
            const auto requests = backend->Requests();
            Require(
                requests.size() == 3
                    && HasHeader(
                        requests.back(),
                        L"Authorization",
                        L"Bearer expired-completed-poll-access")
                    && services->LastErrorCode() == "request_failed",
                "Expired completed authorization was not revoked.");
        }
    }

    // TestExplicitSignOutOverridesExpiredPollCleanup(): 明示的sign-outを期限切れpollの後始末より優先する。
    void TestExplicitSignOutOverridesExpiredPollCleanup()
    {
        // logoutResponse: logout用HTTP応答.
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        // responses: script済みHTTP応答列.
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
        // backend: 記録機能付きHTTP backend.
        const auto backend =
            std::make_shared<BlockingScriptedBackend>(
                std::move(responses),
                2);
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // この要求をscript済みHTTP backendへ渡して応答を返す。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // Configure後に残存資格情報を模擬し、内部cleanup中の明示SignOutがそれを削除することを検証します。
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

        // requests: 記録済みHTTP要求一覧.
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

    // TestCancellationIgnoresOldCompletion(): 古い完了通知がcancel後の状態を戻さないか検証する。
    void TestCancellationIgnoresOldCompletion()
    {
        // senderEnteredPromise: 送信開始通知のpromise.
        std::promise<void> senderEnteredPromise;
        // senderEntered: 送信開始通知のfuture.
        auto senderEntered = senderEnteredPromise.get_future();
        // releasePromise: 送信解除通知のpromise.
        std::promise<void> releasePromise;
        // release: 送信解除通知のfuture.
        const auto release = releasePromise.get_future().share();
        // senderFinishedPromise: 送信完了通知のpromise.
        std::promise<void> senderFinishedPromise;
        // senderFinished: 送信完了通知のfuture.
        auto senderFinished = senderFinishedPromise.get_future();
        // requestCount: HTTP要求数.
        std::atomic_uint requestCount{};

        // services: オンラインservice instance.
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [&senderEnteredPromise,
                    release,
                    &senderFinishedPromise,
                    &requestCount](
                    const LamaPon::HttpRequest&)
                {
                    // request: HTTP要求.
                    const auto request = ++requestCount;
                    // 最初のHTTP要求へログイン応答を返す。
                    if (request == 1)
                    {
                        senderEnteredPromise.set_value();
                        release.wait();
                        senderFinishedPromise.set_value();
                    }
                    // このrequestに対応するscript済みHTTP応答を返す。
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
        // 送信開始を期限付きで待ち、timeout時にsenderを解放する。
        if (senderEntered.wait_for(3s) != std::future_status::ready)
        {
            releasePromise.set_value();
            // 待機または処理失敗の理由をテストrunnerへ通知する。
            throw std::runtime_error(
                "Timed out waiting for the blocked login request.");
        }

        services->CancelDiscordSignIn();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SignedOut
                && services->AuthorizationUrl().empty(),
            "Cancellation did not return to SignedOut immediately.");
        // attempt: retry番号としてcancel後のworker再開を抑止する。
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

        // sender完了通知とmailbox反映には僅かな差があり得るため、古い結果を回収して次の開始が受理されるまで更新します。
        const auto deadline =
            std::chrono::steady_clock::now() + 1s;
        // retryStarted: retry開始の確認結果.
        bool retryStarted{};
        // 古いcompletionを回収し、新しいloginが始まるまで待つ。
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

    // TestDestructionDoesNotWaitForBlockedRequest(): blocked request中もservice破棄が戻ることを検証する。
    void TestDestructionDoesNotWaitForBlockedRequest()
    {
        struct BlockingSender final
        {
            // BlockingSender(): request終了を制御するtest senderを作る。
            BlockingSender()
                : releaseFuture(release.get_future().share())
            {
            }

            // entered: sender開始通知のpromise.
            std::promise<void> entered;
            // release: 送信解除通知のfuture.
            std::promise<void> release;
            // finished: sender完了通知のpromise.
            std::promise<void> finished;
            // releaseFuture: sender解放用shared future.
            std::shared_future<void> releaseFuture;
        };

        // blocking: 待機を制御するsender.
        const auto blocking = std::make_shared<BlockingSender>();
        // entered: sender開始通知のpromise.
        auto entered = blocking->entered.get_future();
        // finished: sender完了通知のpromise.
        auto finished = blocking->finished.get_future();
        // services: オンラインservice instance.
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [blocking](const LamaPon::HttpRequest&)
                {
                    blocking->entered.set_value();
                    blocking->releaseFuture.wait();
                    blocking->finished.set_value();
                    // このrequestに対応するscript済みHTTP応答を返す。
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

        // destroyedPromise: 破棄完了通知のpromise.
        std::promise<void> destroyedPromise;
        // destroyed: 破棄完了通知のfuture.
        auto destroyed = destroyedPromise.get_future();
        // destroyer: service破棄用thread.
        std::thread destroyer(
            [owned = std::move(services),
                &destroyedPromise]() mutable
            {
                owned.reset();
                destroyedPromise.set_value();
            });
        // returnedPromptly: 破棄がすぐ戻った結果.
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

    // TestDestroyedRefreshRetainsUsageLeaseThroughLateLogout(): 遅延logout中も破棄済みrefreshの利用権を保持する。
    void TestDestroyedRefreshRetainsUsageLeaseThroughLateLogout()
    {
        struct CleanupGate final
        {
            // CleanupGate(): refreshとlogoutの待機状態を共有するgateを作る。
            CleanupGate()
                : refreshReleaseFuture(
                    refreshRelease.get_future().share())
                , logoutReleaseFuture(
                    logoutRelease.get_future().share())
            {
            }

            // refreshEntered: refresh開始通知のpromise.
            std::promise<void> refreshEntered;
            // refreshRelease: refresh解除通知のpromise.
            std::promise<void> refreshRelease;
            // refreshReleaseFuture: refresh解除用shared future.
            std::shared_future<void> refreshReleaseFuture;
            // logoutEntered: logout開始通知のpromise.
            std::promise<void> logoutEntered;
            // logoutRelease: logout解除通知のpromise.
            std::promise<void> logoutRelease;
            // logoutReleaseFuture: logout解除用shared future.
            std::shared_future<void> logoutReleaseFuture;
            // requestCount: HTTP要求数.
            std::atomic_uint requestCount{};
            // logoutUsedLateAccessToken: logoutが遅延tokenを使った結果.
            std::atomic_bool logoutUsedLateAccessToken{};
        };

        // gate: refresh/logout同期gate.
        const auto gate = std::make_shared<CleanupGate>();
        // refreshEntered: refresh開始通知のpromise.
        auto refreshEntered = gate->refreshEntered.get_future();
        // logoutEntered: logout開始通知のpromise.
        auto logoutEntered = gate->logoutEntered.get_future();
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "usage-lease-initial-refresh";

        // services: テスト対象、request: fake backendへ渡す送信要求。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [gate](const LamaPon::HttpRequest& request)
                {
                    // requestNumber: HTTP要求の通番.
                    const auto requestNumber =
                        ++gate->requestCount;
                    // 最初のHTTP要求へログイン応答を返す。
                    if (requestNumber == 1u)
                    {
                        // このrequestに対応するscript済みHTTP応答を返す。
                        return JsonResponse(
                            200,
                            SessionJson(
                                "usage-lease-old-access",
                                "usage-lease-active-refresh",
                                "usage-lease-player",
                                30));
                    }
                    // 2番目のHTTP要求へrefresh応答を返す。
                    if (requestNumber == 2u)
                    {
                        gate->refreshEntered.set_value();
                        gate->refreshReleaseFuture.wait();
                        // このrequestに対応するscript済みHTTP応答を返す。
                        return JsonResponse(
                            200,
                            SessionJson(
                                "usage-lease-late-access",
                                "usage-lease-late-refresh",
                                "usage-lease-player",
                                900));
                    }
                    // 3番目のHTTP要求へlogout応答を返す。
                    if (requestNumber == 3u)
                    {
                        gate->logoutUsedLateAccessToken.store(
                            HasHeader(
                                request,
                                L"Authorization",
                                L"Bearer usage-lease-late-access"),
                            std::memory_order_release);
                        gate->logoutEntered.set_value();
                        gate->logoutReleaseFuture.wait();
                        // response: HTTP応答.
                        LamaPon::HttpResponse response;
                        response.statusCode = 204;
                        // 処理対象のHTTP応答を返す。
                        return response;
                    }
                    // response: HTTP応答.
                    LamaPon::HttpResponse response;
                    response.transportError = "Unexpected request.";
                    // 処理対象のHTTP応答を返す。
                    return response;
                },
                std::make_unique<MemoryTokenStore>(storeState));
        UpdateUntilState(
            *services,
            LamaPon::OnlineAccountState::SignedIn,
            "Timed out preparing the usage-lease destruction test.");

        services->Update(25.0f);
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::RefreshingSession
                && refreshEntered.wait_for(3s)
                    == std::future_status::ready,
            "Timed out blocking the usage-lease refresh worker.");

        // destroyedPromise: 破棄完了通知のpromise.
        std::promise<void> destroyedPromise;
        // destroyed: 破棄完了通知のfuture.
        auto destroyed = destroyedPromise.get_future();
        // destroyer: service破棄用thread.
        std::thread destroyer(
            [owned = std::move(services),
                &destroyedPromise]() mutable
            {
                owned.reset();
                destroyedPromise.set_value();
            });
        // returnedPromptly: 破棄がすぐ戻った結果.
        const bool returnedPromptly =
            destroyed.wait_for(500ms) == std::future_status::ready;
        // service破棄が待機し続けた場合はテストを失敗させる。
        if (!returnedPromptly)
        {
            gate->refreshRelease.set_value();
            gate->logoutRelease.set_value();
            destroyer.join();
            // 待機または処理失敗の理由をテストrunnerへ通知する。
            throw std::runtime_error(
                "OnlineServices destruction waited for a refresh worker.");
        }
        destroyer.join();

        // competingStore: 競合するtoken store.
        MemoryTokenStore competingStore(storeState);
        Require(
            !competingStore.AcquireUsageLease().succeeded,
            "Destruction released the lease while refresh was blocked.");
        gate->refreshRelease.set_value();
        // logoutStarted: logout開始状態.
        const bool logoutStarted =
            logoutEntered.wait_for(3s) == std::future_status::ready;
        // 遅延logoutが始まらない場合は待機を解放して失敗する。
        if (!logoutStarted)
        {
            gate->logoutRelease.set_value();
            // 待機または処理失敗の理由をテストrunnerへ通知する。
            throw std::runtime_error(
                "Late refresh completion did not start logout.");
        }
        Require(
            !competingStore.AcquireUsageLease().succeeded,
            "Late-session logout did not retain the usage lease.");
        gate->logoutRelease.set_value();

        // acquiredAfterCleanup: 後始末後の取得結果.
        bool acquiredAfterCleanup{};
        // acquireDeadline: リース取得の期限.
        const auto acquireDeadline =
            std::chrono::steady_clock::now() + 3s;
        // cleanup完了後にusage leaseが取れるまで待つ。
        while (!acquiredAfterCleanup
            && std::chrono::steady_clock::now() < acquireDeadline)
        {
            acquiredAfterCleanup =
                competingStore.AcquireUsageLease().succeeded;
            std::this_thread::yield();
        }
        Require(
            acquiredAfterCleanup
                && gate->requestCount.load(
                    std::memory_order_acquire) == 3u
                && gate->logoutUsedLateAccessToken.load(
                    std::memory_order_acquire)
                && storeState->deleteCount == 1,
            "The usage lease or late rotated session was not cleaned up.");
        competingStore.ReleaseUsageLease();
        Require(
            storeState->usageLeaseAcquireCount.load(
                    std::memory_order_acquire) == 2u
                && storeState->usageLeaseRejectCount.load(
                    std::memory_order_acquire) >= 2u
                && storeState->usageLeaseReleaseCount.load(
                    std::memory_order_acquire) == 2u
                && storeState->usageLeaseOwner.load(
                    std::memory_order_acquire) == nullptr,
            "Usage-lease ownership did not finish exactly once per owner.");
    }

    // TestFailedCredentialDeleteRetainsUsageLease(): credential削除失敗後も利用リースを保持する。
    void TestFailedCredentialDeleteRetainsUsageLease()
    {
        // storeState: memory token storeの共有状態.
        const auto storeState =
            std::make_shared<MemoryTokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Corrupt;
        storeState->token = "usage-lease-stale-refresh";
        storeState->failDelete = true;
        // services: オンラインservice instance.
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                TestConfiguration(false),
                [](const LamaPon::HttpRequest&)
                {
                    // response: HTTP応答.
                    LamaPon::HttpResponse response;
                    response.transportError = "Unexpected request.";
                    // 処理対象のHTTP応答を返す。
                    return response;
                },
                std::make_unique<MemoryTokenStore>(storeState));
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SignedOut
                && services->LastErrorCode()
                    == "credential_delete_failed"
                && storeState->deleteCount == 1,
            "The failed credential deletion did not remain terminal.");

        // competingStore: 競合するtoken store.
        MemoryTokenStore competingStore(storeState);
        Require(
            !competingStore.AcquireUsageLease().succeeded,
            "A terminal delete failure released the usage lease.");

        storeState->failDelete = false;
        services->SignOut();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SignedOut
                && services->LastError().empty()
                && storeState->deleteCount == 2
                && storeState->token.empty()
                && competingStore.AcquireUsageLease().succeeded,
            "The usage lease was not released after deletion recovered.");
        competingStore.ReleaseUsageLease();
        Require(
            storeState->usageLeaseAcquireCount.load(
                    std::memory_order_acquire) == 2u
                && storeState->usageLeaseRejectCount.load(
                    std::memory_order_acquire) == 1u
                && storeState->usageLeaseReleaseCount.load(
                    std::memory_order_acquire) == 2u
                && storeState->usageLeaseOwner.load(
                    std::memory_order_acquire) == nullptr,
            "Recovered credential deletion left stale lease ownership.");
    }

    // TestTransportSecretsAreRedacted(): transport errorからsecretが除去されることを検証する。
    void TestTransportSecretsAreRedacted()
    {
        // transportSecret: transportに含むsecret.
        constexpr std::string_view transportSecret =
            "transport-secret-must-not-appear";
        // services: オンラインservice instance.
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                { "https://online.example.test", false },
                [](const LamaPon::HttpRequest&)
                {
                    // response: HTTP応答.
                    LamaPon::HttpResponse response;
                    response.transportError =
                        "transport-secret-must-not-appear";
                    // 処理対象のHTTP応答を返す。
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

// main(): オンラインサービスのテストを実行する。
int main()
{
    // テスト例外を終了コードへ変換するため実行する。
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
        TestCompletedSessionsExpireBeforeReap();
        TestExplicitSignOutOverridesExpiredPollCleanup();
        TestCancellationIgnoresOldCompletion();
        TestDestructionDoesNotWaitForBlockedRequest();
        TestDestroyedRefreshRetainsUsageLeaseThroughLateLogout();
        TestFailedCredentialDeleteRetainsUsageLease();
        TestTransportSecretsAreRedacted();
        LamaPon::SetActiveOnlineServices(nullptr);
    }
    // 例外内容を出力してテスト失敗を報告する。
    catch (const std::exception& error)
    {
        LamaPon::SetActiveOnlineServices(nullptr);
        std::cerr
            << "Online services tests failed: "
            << error.what()
            << '\n';
        // テスト失敗を終了コードで示す。
        return 1;
    }

    std::cout << "Online services tests passed.\n";
    // テスト成功を終了コードで示す。
    return 0;
}
