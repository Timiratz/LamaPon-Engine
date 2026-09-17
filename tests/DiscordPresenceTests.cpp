#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Online/DiscordPresence.h"
#include "LamaPon/Online/DiscordPresenceTesting.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Scripting/Script.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    using LamaPon::Detail::FakeDiscordPresenceBackend;

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create a test file.");
        }
        output << contents;
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // DiscordPresenceがbackendを破棄した後も記録を読めるよう、Fakeは
    // テスト側で持ち続け、Presenceへは転送するだけのbackendを渡します。
    std::optional<FakeDiscordPresenceBackend> g_backend;

    [[nodiscard]] FakeDiscordPresenceBackend& Backend()
    {
        return *g_backend;
    }

    class ForwardingBackend final
        : public LamaPon::DiscordPresenceBackend
    {
    public:
        [[nodiscard]] bool Initialize(
            const std::string_view applicationId) override
        {
            return Backend().Initialize(applicationId);
        }

        void Shutdown() noexcept override
        {
            Backend().Shutdown();
        }

        [[nodiscard]] bool SetActivity(
            const LamaPon::DiscordActivity& activity) override
        {
            return Backend().SetActivity(activity);
        }

        void ClearActivity() noexcept override
        {
            Backend().ClearActivity();
        }

        void Tick(const float elapsedSeconds) noexcept override
        {
            Backend().Tick(elapsedSeconds);
        }

        [[nodiscard]] bool IsAvailable() const noexcept override
        {
            return Backend().IsAvailable();
        }

        [[nodiscard]] std::string_view
            LastError() const noexcept override
        {
            return Backend().LastError();
        }
    };

    void InstallFakeBackendFactory()
    {
        g_backend.reset();
        g_backend.emplace();
        LamaPon::SetDiscordPresenceBackendFactory(
            []
            {
                return std::unique_ptr<
                    LamaPon::DiscordPresenceBackend>(
                        std::make_unique<ForwardingBackend>());
            });
    }

    // Discord SDKアダプターを同梱しない既定状態を再現します。
    void RemoveBackendFactory()
    {
        LamaPon::SetDiscordPresenceBackendFactory({});
    }

    [[nodiscard]] LamaPon::DiscordPresenceConfiguration
        EnabledConfiguration()
    {
        LamaPon::DiscordPresenceConfiguration configuration;
        configuration.enabled = true;
        configuration.applicationId = "123456789012345678";
        return configuration;
    }

    // Discordの更新間隔を跨いだことにして、保留中の更新を送らせます。
    void AdvancePastUpdateInterval(
        LamaPon::DiscordPresence& presence)
    {
        presence.Tick(
            LamaPon::DiscordPresenceUpdateIntervalSeconds
            + 1.0f);
    }

    [[nodiscard]] std::unique_ptr<LamaPon::OnlineServices>
        MakeOfflineOnlineServices(
            LamaPon::OnlineServiceConfiguration configuration)
    {
        // 通信は行いません。senderが呼ばれたら失敗として扱います。
        return LamaPon::Detail::OnlineServicesTestAccess::Create(
            std::move(configuration),
            [](const LamaPon::HttpRequest&)
            {
                LamaPon::HttpResponse response;
                response.transportError =
                    "Presence tests never talk to a backend.";
                return response;
            });
    }

    // Presenceが無効なら、backendを一切作りません。
    void TestDisabledPresenceNeverInitializes()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;

        LamaPon::DiscordPresenceConfiguration configuration;
        configuration.enabled = false;
        configuration.applicationId = "123456789012345678";
        presence.Configure(configuration);

        Require(
            Backend().initializeCount == 0,
            "a disabled presence must not create a backend");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Disabled,
            "a disabled presence must report Disabled");
        Require(
            !presence.IsAvailable(),
            "a disabled presence must not be available");

        presence.Tick(1.0f);
        AdvancePastUpdateInterval(presence);
        Require(
            Backend().initializeCount == 0
                && Backend().tickCount == 0,
            "ticking a disabled presence must not reach a"
            " backend");
    }

    // Application IDが無ければ、有効設定でも安全に無効化します。
    void TestMissingApplicationIdDisablesSafely()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;

        LamaPon::DiscordPresenceConfiguration configuration;
        configuration.enabled = true;
        presence.Configure(configuration);

        Require(
            Backend().initializeCount == 0,
            "presence without an application ID must not"
            " create a backend");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Disabled,
            "presence without an application ID must be"
            " Disabled");
        Require(
            !presence.LastError().empty(),
            "presence without an application ID must explain"
            " why it is disabled");

        LamaPon::DiscordActivity activity;
        activity.details = "Stage 5";
        Require(
            !presence.SetActivity(activity),
            "setting an activity without an application ID"
            " must fail safely");
        presence.ClearActivity();
        presence.Tick(1.0f);

        // client_secretやtokenを貼り付けてしまった場合も、
        // Discordへ送らずに無効化します。
        LamaPon::DiscordPresenceConfiguration pasted;
        pasted.enabled = true;
        pasted.applicationId = "not-an-application-id";
        presence.Configure(pasted);
        Require(
            Backend().initializeCount == 0
                && presence.State()
                    == LamaPon::DiscordPresenceState::Disabled,
            "a malformed application ID must disable presence");
    }

    // アダプター未登録（Discord SDKを同梱しない既定状態）でも
    // クラッシュせず、ゲームは動き続けます。
    void TestMissingBackendDoesNotCrash()
    {
        RemoveBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());

        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Unavailable,
            "presence without an adapter must report"
            " Unavailable");
        Require(
            !presence.IsAvailable(),
            "presence without an adapter must not be"
            " available");

        LamaPon::DiscordActivity activity;
        activity.details = "Year 12";
        activity.state = "Population 120000";
        Require(
            !presence.SetActivity(activity),
            "setting an activity without an adapter must fail"
            " safely");
        presence.ClearActivity();
        presence.ClearActivity();
        for (int frame = 0; frame < 120; ++frame)
        {
            presence.Tick(1.0f / 60.0f);
        }
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Unavailable,
            "presence without an adapter must stay Unavailable");
    }

    // Discordが起動していない状態を再現します。
    void TestUnavailableBackendKeepsRunning()
    {
        InstallFakeBackendFactory();
        Backend().initializeSucceeds = false;
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());

        Require(
            Backend().initializeCount == 1,
            "the adapter factory must be used");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Unavailable,
            "a backend that cannot connect must leave presence"
            " Unavailable");

        LamaPon::DiscordActivity activity;
        activity.details = "Circuit A";
        activity.state = "Time Attack";
        Require(
            !presence.SetActivity(activity),
            "an activity must not report success while the"
            " backend is unavailable");

        // Discordを後から起動した場合に拾い直します。
        Backend().initializeSucceeds = true;
        AdvancePastUpdateInterval(presence);
        Require(
            presence.IsAvailable(),
            "presence must reconnect once the backend accepts"
            " initialization");
        Require(
            Backend().CurrentActivity().has_value()
                && Backend().CurrentActivity()->details
                    == "Circuit A",
            "the last requested activity must be sent once the"
            " backend becomes available");
    }

    // 初期化していないPresenceを触っても落ちません。
    void TestUninitializedPresenceIsSafe()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;

        LamaPon::DiscordActivity activity;
        activity.details = "Puzzle 48";
        activity.state = "87% Complete";
        Require(
            !presence.SetActivity(activity),
            "an unconfigured presence must fail to set an"
            " activity");
        Require(
            !presence.SetActivity("Puzzle 48", "87% Complete"),
            "the short form must also fail while unconfigured");
        Require(
            !presence.HasActivity(),
            "an unconfigured presence must not remember an"
            " activity");
        presence.ClearActivity();
        presence.ClearActivity();
        presence.ClearActivity();
        presence.Tick(0.016f);
        presence.Shutdown();
        presence.Shutdown();
        Require(
            Backend().initializeCount == 0,
            "an unconfigured presence must not create a"
            " backend");
    }

    void TestActivityIsSentAndCleared()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        auto configuration = EnabledConfiguration();
        configuration.defaultLargeImageKey = "game_icon";
        configuration.defaultLargeImageText = "My Awesome Game";
        presence.Configure(configuration);

        Require(
            Backend().initializeCount == 1
                && Backend().applicationId
                    == "123456789012345678",
            "the configured application ID must reach the"
            " backend");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Ready,
            "a connected presence without an activity must be"
            " Ready");

        LamaPon::DiscordActivity activity;
        activity.details = "Chapter 3";
        activity.state = "Boss Battle";
        activity.startTimestamp =
            LamaPon::DiscordPresenceUnixTime();
        Require(
            presence.SetActivity(activity),
            "a valid activity must be accepted");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Active,
            "a sent activity must move presence to Active");
        Require(
            Backend().activities.size() == 1,
            "the activity must reach the backend once");

        const auto& sent = Backend().activities.front();
        Require(
            sent.details == "Chapter 3"
                && sent.state == "Boss Battle",
            "details and state must reach the backend"
            " unchanged");
        Require(
            sent.largeImageKey == "game_icon"
                && sent.largeImageText == "My Awesome Game",
            "project defaults must fill in an empty large"
            " image");
        Require(
            sent.startTimestamp == activity.startTimestamp
                && sent.endTimestamp == 0,
            "timestamps must reach the backend unchanged");

        presence.ClearActivity();
        Require(
            !presence.HasActivity(),
            "clearing must drop the requested activity"
            " immediately");
        // Discordは15秒に1回しか受け付けないため、解除も次の更新枠で
        // 送ります。
        AdvancePastUpdateInterval(presence);
        Require(
            Backend().clearCount >= 1
                && !Backend().CurrentActivity().has_value(),
            "clearing must remove the activity from Discord");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Ready,
            "a cleared presence must return to Ready");

        const auto clearCount = Backend().clearCount;
        presence.ClearActivity();
        presence.ClearActivity();
        AdvancePastUpdateInterval(presence);
        Require(
            Backend().clearCount == clearCount,
            "repeated clears must not send redundant updates");
    }

    // ジャンルを決め打ちしないことを、代表的な組み合わせで確認します。
    void TestGenreNeutralActivities()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());

        const std::pair<const char*, const char*> samples[]{
            { "Stage 5", "Boss Battle" },
            { "Year 12", "City Population 120,000" },
            { "Royal Capital", "Quest: The Lost Sword" },
            { "Puzzle 48", "87% Complete" },
            { "Circuit A", "Time Attack" }
        };
        for (const auto& [details, state] : samples)
        {
            Require(
                presence.SetActivity(details, state),
                "any genre must be able to describe itself");
            Require(
                presence.Activity().details == details
                    && presence.Activity().state == state,
                "the pending activity must keep what the game"
                " asked for");
            AdvancePastUpdateInterval(presence);
        }
        Require(
            Backend().activities.size() == std::size(samples),
            "every genre sample must reach the backend");
    }

    // Discordは15秒に1回しか受け付けないので、最新の要求だけを送ります。
    void TestUpdatesAreCoalescedToDiscordInterval()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());

        Require(
            presence.SetActivity("Stage 1", "Exploring"),
            "the first activity must be accepted");
        Require(
            Backend().activities.size() == 1,
            "the first activity must be sent immediately");

        for (int stage = 2; stage <= 6; ++stage)
        {
            Require(
                presence.SetActivity(
                    "Stage " + std::to_string(stage),
                    "Exploring"),
                "rapid updates must still be accepted");
            presence.Tick(1.0f);
        }
        Require(
            Backend().activities.size() == 1,
            "updates inside Discord's interval must be"
            " coalesced");

        AdvancePastUpdateInterval(presence);
        Require(
            Backend().activities.size() == 2
                && Backend().activities.back().details
                    == "Stage 6",
            "only the newest activity must be sent when the"
            " interval elapses");
    }

    void TestInvalidActivitiesAreRejected()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());

        LamaPon::DiscordActivity tooLong;
        tooLong.details = std::string(
            LamaPon::DiscordActivityTextMaxBytes + 1,
            'a');
        Require(
            !presence.SetActivity(tooLong),
            "text longer than Discord allows must be rejected");

        LamaPon::DiscordActivity controlCharacter;
        controlCharacter.details = "Chapter\n3";
        Require(
            !presence.SetActivity(controlCharacter),
            "control characters must be rejected");

        LamaPon::DiscordActivity negative;
        negative.details = "Chapter 3";
        negative.startTimestamp = -1;
        Require(
            !presence.SetActivity(negative),
            "negative timestamps must be rejected");

        LamaPon::DiscordActivity reversed;
        reversed.details = "Chapter 3";
        reversed.startTimestamp = 2000;
        reversed.endTimestamp = 1000;
        Require(
            !presence.SetActivity(reversed),
            "an end before the start must be rejected");

        LamaPon::DiscordActivity smallOnly;
        smallOnly.details = "Chapter 3";
        smallOnly.smallImageKey = "badge";
        Require(
            !presence.SetActivity(smallOnly),
            "a small image without a large image must be"
            " rejected");

        Require(
            Backend().activities.empty(),
            "rejected activities must never reach the"
            " backend");
    }

    void TestBackendFailureIsRetried()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());
        Backend().setActivitySucceeds = false;

        Require(
            presence.SetActivity("Stage 5", "Boss Battle"),
            "an accepted activity must report success even if"
            " the backend later refuses it");
        Require(
            Backend().activities.empty()
                && !presence.LastError().empty(),
            "a refused activity must be reported");

        Backend().setActivitySucceeds = true;
        AdvancePastUpdateInterval(presence);
        Require(
            Backend().activities.size() == 1
                && presence.State()
                    == LamaPon::DiscordPresenceState::Active,
            "a refused activity must be retried on the next"
            " interval");
    }

    void TestDisconnectAndReconnect()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());
        Require(
            presence.SetActivity("Stage 5", "Boss Battle"),
            "the first activity must be accepted");

        Backend().available = false;
        presence.Tick(1.0f);
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Unavailable
                && !presence.IsAvailable(),
            "losing the Discord client must not crash the"
            " game");

        Backend().available = true;
        presence.Tick(1.0f);
        Require(
            Backend().activities.size() == 2
                && Backend().activities.back().details
                    == "Stage 5",
            "the activity must be restored after reconnecting");
        Require(
            presence.State()
                == LamaPon::DiscordPresenceState::Active,
            "a restored activity must move presence to Active");
    }

    void TestShutdownReleasesBackend()
    {
        InstallFakeBackendFactory();
        {
            LamaPon::DiscordPresence presence;
            presence.Configure(EnabledConfiguration());
            Require(
                presence.SetActivity("Stage 5", "Boss Battle"),
                "the first activity must be accepted");

            presence.Shutdown();
            Require(
                Backend().shutdownCount == 1
                    && Backend().clearCount >= 1,
                "shutting down must clear the activity and"
                " close the backend");
            Require(
                presence.State()
                    == LamaPon::DiscordPresenceState::Disabled
                    && !presence.IsAvailable(),
                "a shut down presence must report Disabled");
            presence.Tick(1.0f);
            presence.ClearActivity();
        }
        Require(
            Backend().shutdownCount == 1,
            "destroying an already shut down presence must not"
            " close the backend twice");
    }

    // 既定画像だけを変えたときにDiscordとの接続を切りません。
    void TestReconfiguringKeepsConnection()
    {
        InstallFakeBackendFactory();
        LamaPon::DiscordPresence presence;
        presence.Configure(EnabledConfiguration());

        auto updated = EnabledConfiguration();
        updated.defaultLargeImageText = "My Awesome Game";
        presence.Configure(updated);
        Require(
            Backend().initializeCount == 1
                && Backend().shutdownCount == 0,
            "changing only the default image must keep the"
            " Discord connection");

        auto other = EnabledConfiguration();
        other.applicationId = "876543210987654321";
        presence.Configure(other);
        Require(
            Backend().shutdownCount == 1
                && Backend().initializeCount == 2
                && Backend().applicationId
                    == "876543210987654321",
            "changing the application ID must rebuild the"
            " backend");
    }

    // Account Linkingが無効でもPresenceだけを使えます。
    void TestPresenceWorksWithoutAccountLinking()
    {
        InstallFakeBackendFactory();
        LamaPon::OnlineServices services;
        Require(
            services.State()
                == LamaPon::OnlineAccountState::Unconfigured
                && !services.IsSignedIn(),
            "this test must run without Discord account"
            " linking");

        services.ConfigureDiscordPresence(EnabledConfiguration());
        Require(
            services.Presence().IsAvailable(),
            "presence must work while account linking is"
            " unconfigured");
        Require(
            services.Presence().SetActivity(
                "Royal Capital",
                "Quest: The Lost Sword"),
            "an activity must be accepted without signing in");
        Require(
            services.State()
                == LamaPon::OnlineAccountState::Unconfigured
                && !services.IsSignedIn(),
            "presence must never start account linking");

        // OnlineServices::Updateから毎フレーム進みます。
        services.Update(1.0f);
        Require(
            Backend().tickCount >= 1,
            "OnlineServices::Update must tick the presence"
            " backend");
    }

    // Account LinkingがONでもPresenceをOFFにできます。
    void TestAccountLinkingWithoutPresence()
    {
        InstallFakeBackendFactory();
        LamaPon::OnlineServiceConfiguration online;
        online.serviceBaseUrl = "https://online.example.test";
        online.gameId = "com.example.presence-test";
        online.environmentId = "staging";
        const auto services =
            MakeOfflineOnlineServices(std::move(online));
        Require(
            services->State()
                == LamaPon::OnlineAccountState::SignedOut,
            "account linking must be configured for this test");
        Require(
            Backend().initializeCount == 0,
            "configuring account linking must not start"
            " presence");
        Require(
            !services->Presence().IsAvailable()
                && services->Presence().State()
                    == LamaPon::DiscordPresenceState::Disabled,
            "presence must stay disabled while only account"
            " linking is configured");

        Require(
            !services->Presence().SetActivity(
                "Stage 5",
                "Boss Battle"),
            "a disabled presence must fail safely even with"
            " account linking on");
        services->Presence().ClearActivity();
        services->Update(1.0f);
        Require(
            Backend().initializeCount == 0,
            "updating must not start a disabled presence");

        // Presenceを後から足しても、アカウント状態は変わりません。
        services->ConfigureDiscordPresence(
            EnabledConfiguration());
        Require(
            services->State()
                == LamaPon::OnlineAccountState::SignedOut,
            "configuring presence must not disturb account"
            " linking");
        Require(
            services->Presence().IsAvailable(),
            "presence must start while account linking stays"
            " signed out");
    }

    // ActiveOnlineServicesが無くてもScript APIは落ちません。
    void TestScriptApi()
    {
        InstallFakeBackendFactory();
        LamaPon::SetActiveOnlineServices(nullptr);

        class PresenceScript final : public LamaPon::Script
        {
        public:
            void RunWithoutServices()
            {
                Require(
                    !IsDiscordPresenceAvailable(),
                    "presence must be unavailable without"
                    " active online services");
                Require(
                    DiscordPresenceStatus()
                        == LamaPon::DiscordPresenceState::
                            Disabled,
                    "presence status must fall back to"
                    " Disabled");
                Require(
                    !SetDiscordActivity(
                        "Chapter 3",
                        "Boss Battle"),
                    "setting an activity must fail safely"
                    " without active online services");
                ClearDiscordActivity();
                Require(
                    DiscordPresenceError().empty(),
                    "there is no error to report without"
                    " active online services");
            }

            void RunWithServices(
                LamaPon::OnlineServices& services)
            {
                Require(
                    IsDiscordPresenceAvailable(),
                    "the script API must see the configured"
                    " presence");
                LamaPon::DiscordActivity activity;
                activity.details = "Chapter 3";
                activity.state = "Boss Battle";
                activity.largeImageKey = "game_icon";
                activity.largeImageText = "My Awesome Game";
                activity.startTimestamp =
                    LamaPon::DiscordPresenceUnixTime();
                Require(
                    SetDiscordActivity(activity),
                    "the script API must set an activity");
                Require(
                    services.Presence().HasActivity(),
                    "the activity must reach OnlineServices");
                Require(
                    DiscordPresenceStatus()
                        == LamaPon::DiscordPresenceState::
                            Active,
                    "the script API must report the live"
                    " state");
                ClearDiscordActivity();
                Require(
                    !services.Presence().HasActivity(),
                    "the script API must clear the activity");
            }
        };

        PresenceScript script;
        script.RunWithoutServices();

        LamaPon::OnlineServices services;
        services.ConfigureDiscordPresence(EnabledConfiguration());
        LamaPon::SetActiveOnlineServices(&services);
        script.RunWithServices(services);
        LamaPon::SetActiveOnlineServices(nullptr);
    }

    void TestProjectSettingsRoundTrip(
        const std::filesystem::path& root)
    {
        const auto path = root / "presence-round-trip.json";
        LamaPon::ProjectSettings settings;
        // アカウント連携は無効のまま、Presenceだけを保存します。
        settings.online.enabled = false;
        settings.online.discordPresence.enabled = true;
        settings.online.discordPresence.applicationId =
            "123456789012345678";
        settings.online.discordPresence.defaultLargeImageKey =
            "game_icon";
        settings.online.discordPresence.defaultLargeImageText =
            "My Awesome Game";
        LamaPon::ValidateProjectSettings(settings);
        LamaPon::SaveProjectSettings(
            path,
            settings,
            LamaPon::ProjectSettingsFileType::Project);

        const auto loaded = LamaPon::LoadProjectSettings(path);
        Require(
            !loaded.online.enabled
                && loaded.online.discordPresence.enabled
                && loaded.online.discordPresence.applicationId
                    == "123456789012345678"
                && loaded.online.discordPresence
                        .defaultLargeImageKey
                    == "game_icon"
                && loaded.online.discordPresence
                        .defaultLargeImageText
                    == "My Awesome Game",
            "rich presence settings must survive a project"
            " round trip without account linking");

        const auto document =
            nlohmann::json::parse(ReadFile(path));
        const auto& presence =
            document.at("online").at("discordPresence");
        Require(
            presence.size() == 4
                && !presence.contains("clientSecret")
                && !presence.contains("client_secret")
                && !presence.contains("accessToken")
                && !presence.contains("refreshToken"),
            "rich presence must store only its four public"
            " settings");
    }

    // discordPresenceを持たない古いproject.jsonは無効として扱います。
    void TestLegacyProjectSettingsRemainDisabled(
        const std::filesystem::path& root)
    {
        const auto path = root / "legacy-project.json";
        WriteFile(
            path,
            R"({"format":"LamaPonProject","version":1,)"
            R"("gameName":"Legacy Game",)"
            R"("online":{"enabled":true,)"
            R"("serviceBaseUrl":"https://online.example.test",)"
            R"("gameId":"com.example.legacy",)"
            R"("environmentId":"production"}})");

        const auto loaded = LamaPon::LoadProjectSettings(path);
        Require(
            loaded.online.enabled
                && loaded.online.gameId
                    == "com.example.legacy",
            "existing online settings must keep working");
        Require(
            !loaded.online.discordPresence.enabled
                && loaded.online.discordPresence.applicationId
                    .empty(),
            "a project.json without discordPresence must be"
            " treated as disabled");
        LamaPon::ValidateProjectSettings(loaded);

        // 保存し直すと、既定値のdiscordPresenceが追加されます。
        LamaPon::SaveProjectSettings(
            path,
            loaded,
            LamaPon::ProjectSettingsFileType::Project);
        const auto upgraded =
            LamaPon::LoadProjectSettings(path);
        Require(
            !upgraded.online.discordPresence.enabled
                && upgraded.online.enabled,
            "upgrading an old project must leave rich presence"
            " disabled and account linking untouched");
    }

    // 手編集で壊れたdiscordPresenceは、暗黙変換せずに拒否します。
    void TestMalformedProjectSettingsAreRejected(
        const std::filesystem::path& root)
    {
        const auto path = root / "malformed-presence.json";
        const char* const documents[]{
            R"({"format":"LamaPonProject",)"
            R"("online":{"discordPresence":true}})",
            R"({"format":"LamaPonProject",)"
            R"("online":{"discordPresence":{"enabled":1}}})",
            R"({"format":"LamaPonProject",)"
            R"("online":{"discordPresence":)"
            R"({"applicationId":123456789012345678}}})"
        };
        for (const auto* const document : documents)
        {
            WriteFile(path, document);
            bool rejected = false;
            try
            {
                static_cast<void>(
                    LamaPon::LoadProjectSettings(path));
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "a malformed discordPresence object must be"
                " rejected instead of silently converted");
        }
    }

    void TestProjectSettingsValidation(
        const std::filesystem::path& root)
    {
        const auto path = root / "presence-validation.json";
        LamaPon::ProjectSettings settings;
        settings.online.discordPresence.enabled = true;

        // Application IDが空でも保存できます（実行時に安全に無効化）。
        LamaPon::ValidateProjectSettings(settings);
        LamaPon::SaveProjectSettings(
            path,
            settings,
            LamaPon::ProjectSettingsFileType::GamePackage);
        Require(
            LamaPon::LoadProjectSettings(path)
                .online.discordPresence.enabled,
            "an application ID may be filled in later");

        const char* const rejected[]{
            "MTIzNDU2Nzg5MDEyMzQ1Njc4.secret",
            "123456789012345678901234567890123",
            "12345678901234567 "
        };
        for (const auto* const applicationId : rejected)
        {
            auto invalid = settings;
            invalid.online.discordPresence.applicationId =
                applicationId;
            bool threw = false;
            try
            {
                LamaPon::ValidateProjectSettings(invalid);
            }
            catch (const std::invalid_argument&)
            {
                threw = true;
            }
            Require(
                threw,
                "only an ASCII digit application ID may be"
                " stored");
        }

        auto longText = settings;
        longText.online.discordPresence.defaultLargeImageText =
            std::string(
                LamaPon::DiscordActivityTextMaxBytes + 1,
                'a');
        bool threwForText = false;
        try
        {
            LamaPon::ValidateProjectSettings(longText);
        }
        catch (const std::invalid_argument&)
        {
            threwForText = true;
        }
        Require(
            threwForText,
            "default image text longer than Discord allows"
            " must be rejected");
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "discord-presence";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        TestDisabledPresenceNeverInitializes();
        TestMissingApplicationIdDisablesSafely();
        TestMissingBackendDoesNotCrash();
        TestUnavailableBackendKeepsRunning();
        TestUninitializedPresenceIsSafe();
        TestActivityIsSentAndCleared();
        TestGenreNeutralActivities();
        TestUpdatesAreCoalescedToDiscordInterval();
        TestInvalidActivitiesAreRejected();
        TestBackendFailureIsRetried();
        TestDisconnectAndReconnect();
        TestShutdownReleasesBackend();
        TestReconfiguringKeepsConnection();
        TestPresenceWorksWithoutAccountLinking();
        TestAccountLinkingWithoutPresence();
        TestScriptApi();
        TestProjectSettingsRoundTrip(root);
        TestLegacyProjectSettingsRemainDisabled(root);
        TestMalformedProjectSettingsAreRejected(root);
        TestProjectSettingsValidation(root);

        LamaPon::SetActiveOnlineServices(nullptr);
        LamaPon::SetDiscordPresenceBackendFactory({});
        g_backend.reset();
    }
    catch (const std::exception& error)
    {
        LamaPon::SetActiveOnlineServices(nullptr);
        LamaPon::SetDiscordPresenceBackendFactory({});
        g_backend.reset();
        std::cerr
            << "Discord rich presence tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    std::cout << "Discord rich presence tests passed.\n";
    return 0;
}
