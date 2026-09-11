#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Editor/PersistencePanelState.h"
#include "LamaPon/Online/CloudSaveClient.h"
#include "LamaPon/Online/CloudSaveJournal.h"
#include "LamaPon/Online/OnlinePersistenceCoordinator.h"
#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Online/CloudSaveSynchronizer.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    using Coordinator =
        LamaPon::Detail::OnlinePersistenceCoordinator;
    using DetachResult =
        LamaPon::Detail::OnlinePersistenceDetachResult;

    static_assert(!std::is_copy_constructible_v<LamaPon::SaveDataStore>);
    static_assert(!std::is_copy_assignable_v<LamaPon::SaveDataStore>);
    static_assert(!std::is_move_constructible_v<LamaPon::SaveDataStore>);
    static_assert(!std::is_move_assignable_v<LamaPon::SaveDataStore>);

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template<class Function>
    bool Throws(Function&& function)
    {
        try
        {
            std::forward<Function>(function)();
            return false;
        }
        catch (...)
        {
            return true;
        }
    }

    struct FileHandle final
    {
        HANDLE value{ INVALID_HANDLE_VALUE };

        ~FileHandle()
        {
            Close();
        }

        void Close() noexcept
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
                value = INVALID_HANDLE_VALUE;
            }
        }
    };

    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const nlohmann::json& document = nlohmann::json::object())
    {
        LamaPon::HttpResponse response;
        response.statusCode = status;
        if (status != 204u)
        {
            const auto text = document.dump();
            response.body.assign(text.begin(), text.end());
        }
        return response;
    }

    LamaPon::HttpResponse EmptyCloudManifestResponse()
    {
        auto response = JsonResponse(
            200u,
            {
                { "protocolVersion", 1u },
                { "items", nlohmann::json::array() }
            });
        response.headers.emplace_back(
            L"Content-Type",
            L"application/json; charset=utf-8");
        response.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(response.body.size()));
        return response;
    }

    nlohmann::json SessionJson(
        const std::string_view accessToken,
        const std::string_view refreshToken,
        const std::string_view playerId,
        const std::uint32_t expiresIn = 30u)
    {
        return {
            { "accessToken", accessToken },
            { "refreshToken", refreshToken },
            { "expiresIn", expiresIn },
            {
                "player",
                {
                    { "id", playerId },
                    { "displayName", "test-player" },
                    { "avatarUrl", "" },
                    { "linkedProvider", "discord" }
                }
            }
        };
    }

    class ScriptedBackend final
    {
    public:
        explicit ScriptedBackend(
            std::deque<LamaPon::HttpResponse> responses,
            const std::optional<std::size_t> blockedRequest = std::nullopt)
            : m_responses(std::move(responses))
            , m_blockedRequest(blockedRequest)
        {
        }

        LamaPon::HttpResponse Send(const LamaPon::HttpRequest& request)
        {
            std::unique_lock lock(m_mutex);
            const auto requestIndex = m_requests.size();
            m_requests.push_back(request);
            if (m_responses.empty())
            {
                LamaPon::HttpResponse unexpected;
                unexpected.transportError = "Unexpected request.";
                return unexpected;
            }
            auto response = std::move(m_responses.front());
            m_responses.pop_front();
            if (m_blockedRequest == requestIndex)
            {
                m_requestBlocked = true;
                m_condition.notify_all();
                m_condition.wait(
                    lock,
                    [this]
                    {
                        return m_releaseBlockedRequest;
                    });
            }
            return response;
        }

        void WaitUntilRequestBlocked()
        {
            std::unique_lock lock(m_mutex);
            if (!m_condition.wait_for(
                    lock,
                    std::chrono::seconds(10),
                    [this]
                    {
                        return m_requestBlocked;
                    }))
            {
                throw std::runtime_error(
                    "Expected online request did not block.");
            }
        }

        void ReleaseBlockedRequest() noexcept
        {
            {
                std::scoped_lock lock(m_mutex);
                m_releaseBlockedRequest = true;
            }
            m_condition.notify_all();
        }

        [[nodiscard]] std::vector<LamaPon::HttpRequest> Requests() const
        {
            std::scoped_lock lock(m_mutex);
            return m_requests;
        }

    private:
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        std::deque<LamaPon::HttpResponse> m_responses;
        std::vector<LamaPon::HttpRequest> m_requests;
        std::optional<std::size_t> m_blockedRequest;
        bool m_requestBlocked{};
        bool m_releaseBlockedRequest{};
    };

    struct TokenStoreState final
    {
        LamaPon::Detail::RefreshTokenLoadStatus loadStatus{
            LamaPon::Detail::RefreshTokenLoadStatus::NotFound
        };
        std::string token;
        std::size_t saveCount{};
        std::size_t deleteCount{};
        bool failSave{};
        bool failDelete{};
        std::function<void()> onSave;
    };

    class MemoryTokenStore final
        : public LamaPon::Detail::IRefreshTokenStore
    {
    public:
        explicit MemoryTokenStore(
            std::shared_ptr<TokenStoreState> state)
            : m_state(std::move(state))
        {
        }

        LamaPon::Detail::RefreshTokenLoadResult Load() override
        {
            LamaPon::Detail::RefreshTokenLoadResult result;
            result.status = m_state->loadStatus;
            if (result.Loaded())
            {
                result.refreshToken = m_state->token;
            }
            return result;
        }

        LamaPon::Detail::OnlinePlatformResult Save(
            const std::string_view refreshToken) override
        {
            ++m_state->saveCount;
            if (m_state->onSave)
            {
                m_state->onSave();
            }
            if (m_state->failSave)
            {
                return { false, "private", "private" };
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
                return { false, "private", "private" };
            }
            m_state->token.clear();
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::NotFound;
            return { true, {}, {} };
        }

    private:
        std::shared_ptr<TokenStoreState> m_state;
    };

    LamaPon::OnlineServiceConfiguration OnlineConfiguration()
    {
        LamaPon::OnlineServiceConfiguration configuration;
        configuration.serviceBaseUrl =
            "https://online.example.test/tenant";
        configuration.gameId = "coordinator-game";
        configuration.environmentId = "test";
        configuration.openAuthorizationBrowser = false;
        return configuration;
    }

    template<class Predicate>
    void UpdateUntil(
        LamaPon::OnlineServices& services,
        Predicate&& predicate,
        const char* timeoutMessage)
    {
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
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

    void WaitUntilRequestCount(
        const std::shared_ptr<ScriptedBackend>& backend,
        const std::size_t expectedCount,
        const char* timeoutMessage)
    {
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        while (backend->Requests().size() < expectedCount)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    void WaitUntilCurrentTaskCompleted(
        LamaPon::OnlineServices& services,
        const char* timeoutMessage)
    {
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        while (!LamaPon::Detail::OnlineServicesTestAccess::
            CurrentTaskCompleted(services))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    void CompleteDiscordLogin(LamaPon::OnlineServices& services)
    {
        Require(
            services.BeginDiscordSignIn(),
            "Could not begin the integration login.");
        UpdateUntil(
            services,
            [&]
            {
                return services.State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Login start did not complete.");
        services.Update(1.0f);
    }

    bool HasBearer(
        const LamaPon::HttpRequest& request,
        const std::wstring_view token)
    {
        const auto expected = std::wstring(L"Bearer ")
            + std::wstring(token);
        for (const auto& [name, value] : request.headers)
        {
            if (name == L"Authorization" && value == expected)
            {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path TestRoot()
    {
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"online-profile").lexically_normal();
        Require(
            root.parent_path().filename() == L"test-output",
            "Coordinator test cleanup root escaped test-output.");
        return root;
    }

    void ResetTestRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(TestRoot(), error);
        Require(!error, "Coordinator test cleanup failed.");
        std::filesystem::create_directories(TestRoot(), error);
        Require(!error, "Coordinator test root creation failed.");
    }

    std::filesystem::path CaseRoot(const std::string_view name)
    {
        return TestRoot()
            / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(name.data()),
                reinterpret_cast<const char8_t*>(
                    name.data() + name.size())))
            / L"GuestDisplayName";
    }

    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        Require(static_cast<bool>(output), "Could not write test fixture.");
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(static_cast<bool>(input), "Could not read test fixture.");
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    void ConfigureCoordinator(Coordinator& coordinator)
    {
        coordinator.ConfigureNamespace(
            "coordinator-game",
            "test",
            "https://online.example.test/tenant/",
            false);
    }

    void ConfigureCloudCoordinator(Coordinator& coordinator)
    {
        auto client = std::make_shared<
            LamaPon::Detail::CloudSaveClient>(
                "https://online.example.test/tenant/",
                "coordinator-game",
                "test",
                false,
                [](const LamaPon::HttpRequest&)
                {
                    LamaPon::HttpResponse response;
                    response.transportError =
                        "Unexpected checkpoint fixture request.";
                    return response;
                });
        coordinator.ConfigureNamespace(
            "coordinator-game",
            "test",
            "https://online.example.test/tenant/",
            false,
            std::move(client));
    }

    void TestEditorDraftsAreInvalidatedAcrossProfileBindings()
    {
        LamaPon::Detail::PersistencePanelState state(
            L"C:/test/guest/PlayerPrefs.json",
            L"C:/test/guest/Saves");
        strncpy_s(
            state.playerPrefKey.data(),
            state.playerPrefKey.size(),
            "guest-key",
            _TRUNCATE);
        strncpy_s(
            state.playerPrefValue.data(),
            state.playerPrefValue.size(),
            "guest-value",
            _TRUNCATE);
        strncpy_s(
            state.saveSlot.data(),
            state.saveSlot.size(),
            "guest-slot",
            _TRUNCATE);
        strncpy_s(
            state.saveJson.data(),
            state.saveJson.size(),
            R"({"guest":true})",
            _TRUNCATE);
        state.selectedSaveSlot = "guest-slot";
        state.playerPrefType = 3;
        state.playerPrefBoolean = true;
        const auto initialRevision = state.BindingRevision();

        Require(
            !state.SynchronizeBinding(
                L"C:/test/guest/PlayerPrefs.json",
                L"C:/test/guest/Saves")
                && std::string(state.playerPrefKey.data()) == "guest-key"
                && state.BindingRevision() == initialRevision,
            "Unchanged persistence binding discarded the editor draft.");

        Require(
            state.SynchronizeBinding(
                L"C:/test/account/PlayerPrefs.json",
                L"C:/test/account/Saves")
                && state.BindingRevision() != initialRevision
                && state.playerPrefKey.front() == '\0'
                && state.playerPrefValue.front() == '\0'
                && state.saveSlot.front() == '\0'
                && std::string(state.saveJson.data()) == "{}"
                && state.selectedSaveSlot.empty()
                && state.playerPrefType == 0
                && !state.playerPrefBoolean
                && state.CloseDeleteAllPopupRequested(),
            "Profile switch retained an editor persistence draft.");
        state.AcknowledgeCloseDeleteAllPopup();
        Require(
            !state.CloseDeleteAllPopupRequested(),
            "Stale PlayerPrefs delete popup was not invalidated.");
    }

    void TestValidatedPlayerPrefsSnapshotClosesReopenRace()
    {
        const auto root = CaseRoot("snapshot-race");
        const auto path = root / L"Account" / L"PlayerPrefs.json";
        LamaPon::PlayerPrefs source(path);
        source.Load();
        source.SetString("snapshot", "validated-value");
        source.Save();
        const auto snapshot =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                source);

        WriteText(path, "{strict-invalid");
        LamaPon::PlayerPrefs replacedAfterRead(path);
        LamaPon::Detail::LocalPersistenceDocuments::
            LoadPlayerPrefsSnapshot(replacedAfterRead, snapshot);
        Require(
            replacedAfterRead.GetString("snapshot") == "validated-value"
                && !replacedAfterRead.IsDirty()
                && !replacedAfterRead.HasLoadFailure()
                && LamaPon::Detail::LocalPersistenceDocuments::
                    ReadPlayerPrefs(replacedAfterRead).state
                    == LamaPon::Detail::
                        LocalPersistenceDocumentState::Corrupt,
            "Strict-invalid replacement changed or was overwritten by the prepared snapshot.");

        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        Require(!removeError, "Could not delete snapshot race fixture.");
        LamaPon::PlayerPrefs deletedAfterRead(path);
        LamaPon::Detail::LocalPersistenceDocuments::
            LoadPlayerPrefsSnapshot(deletedAfterRead, snapshot);
        Require(
            deletedAfterRead.GetString("snapshot") == "validated-value"
                && !std::filesystem::exists(path),
            "Deletion after strict read was mistaken for an empty account.");
    }

    void TestTransactionalProfileSwitchAndStableObjects()
    {
        const auto root = CaseRoot("transactional-switch");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        auto* const preferencesAddress = &preferences;
        auto* const savesAddress = &saves;
        const auto guestPreferencesPath = preferences.FilePath();
        const auto guestSaveDirectory = saves.Directory();

        auto preparedA = coordinator.PrepareAccount("player-A");
        Require(
            preparedA.IsValid()
                && coordinator.CommitPrepared(std::move(preparedA)),
            "Could not commit account A.");
        Require(
            &preferences == preferencesAddress
                && &saves == savesAddress
                && preferences.FilePath() != guestPreferencesPath
                && saves.Directory() != guestSaveDirectory
                && preferences.GetString("owner").empty()
                && coordinator.IsAccountActive()
                && coordinator.Journal() != nullptr
                && !coordinator.ActiveAccountStorageKey().empty(),
            "Account A did not preserve object identity and isolation.");

        preferences.SetString("owner", "account-A");
        preferences.Save();
        saves.SaveJson("slot-a", R"({"level":7})");
        Require(
            coordinator.ConsumeLocalCommitSignal(),
            "Account local commits did not reach the coordinator.");

        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount
                && !coordinator.IsAccountActive()
                && preferences.FilePath() == guestPreferencesPath
                && saves.Directory() == guestSaveDirectory
                && preferences.GetString("owner") == "guest"
                && &preferences == preferencesAddress
                && &saves == savesAddress,
            "Detach did not restore the exact guest objects and state.");

        auto preparedB = coordinator.PrepareAccount("player-B");
        Require(
            coordinator.CommitPrepared(std::move(preparedB))
                && preferences.GetString("owner").empty()
                && !saves.HasSlot("slot-a"),
            "Account B observed account A persistence.");
        static_cast<void>(coordinator.DetachToGuest());
    }

    void TestActiveAccountOwnsBothPersistenceBindings()
    {
        const auto root = CaseRoot("binding-lease");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);
        auto prepared = coordinator.PrepareAccount("lease-player");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate binding-lease fixture account.");

        const auto accountPreferencesPath = preferences.FilePath();
        const auto accountSaveDirectory = saves.Directory();
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        Require(
            Throws([&]
            {
                preferences.Rebind(root / L"EscapedPrefs.json");
            }),
            "Public PlayerPrefs::Rebind bypassed the account lease.");
        saves.Rebind(root / L"EscapedSaves");
        Require(
            preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory,
            "Public SaveDataStore::Rebind bypassed the account lease.");
        Require(
            Throws([&]
            {
                profiles.RebindGuest(preferences, saves);
            })
                && preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory,
            "PersistenceProfiles created a half-rebound active account.");
        const auto importResult = profiles.ImportGuestToAccount(
            preferences,
            saves,
            "lease-import-player");
        Require(
            importResult.status
                    == LamaPon::GuestPersistenceImportStatus::Failed
                && importResult.error
                    == "Persistence binding is owned by online persistence."
                && preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory,
            "Guest import bypassed the active account lease.");

        LamaPon::PlayerPrefs remoteSource(root / L"RemoteSource.json");
        remoteSource.Load();
        remoteSource.SetString("remote", "replacement");
        const auto remoteText = remoteSource.SerializeToJson();
        const std::vector<std::uint8_t> remoteBytes(
            remoteText.begin(),
            remoteText.end());
        LamaPon::Detail::LocalPersistenceDocuments::ApplyPlayerPrefs(
            preferences,
            remoteBytes);
        Require(
            preferences.GetString("remote") == "replacement"
                && Throws([&]
                {
                    preferences.Rebind(root / L"EscapedAfterApply.json");
                }),
            "PlayerPrefs replacement state dropped the account lease.");

        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount,
            "Binding-lease fixture did not detach cleanly.");
        const auto reboundPreferences = root / L"ReboundPrefs.json";
        const auto reboundSaves = root / L"ReboundSaves";
        preferences.Rebind(reboundPreferences);
        saves.Rebind(reboundSaves);
        Require(
            preferences.FilePath() == reboundPreferences
                && saves.Directory() == reboundSaves,
            "Guest detach did not release both persistence leases.");
    }

    void TestAccountProfileSessionLeaseRejectsSecondActivation()
    {
        const auto root = CaseRoot("profile-session-lease");
        LamaPon::PlayerPrefs firstPreferences(root / L"PlayerPrefs.json");
        firstPreferences.Load();
        LamaPon::SaveDataStore firstSaves(root / L"Saves");
        Coordinator first(firstPreferences, firstSaves, root);
        ConfigureCoordinator(first);
        auto firstPrepared = first.PrepareAccount("shared-account");
        Require(
            first.CommitPrepared(std::move(firstPrepared)),
            "First account activation did not acquire its session lease.");

        LamaPon::PlayerPrefs secondPreferences(root / L"PlayerPrefs.json");
        secondPreferences.Load();
        LamaPon::SaveDataStore secondSaves(root / L"Saves");
        Coordinator second(secondPreferences, secondSaves, root);
        ConfigureCoordinator(second);
        Require(
            Throws([&]
            {
                (void)second.PrepareAccount("shared-account");
            })
                && !second.IsAccountActive(),
            "A second coordinator activated an account with an active lease.");

        Require(
            first.DetachToGuest() == DetachResult::SavedAccount,
            "First account did not release its session lease on detach.");
        auto secondPrepared = second.PrepareAccount("shared-account");
        Require(
            second.CommitPrepared(std::move(secondPrepared))
                && second.IsAccountActive(),
            "Account activation did not recover after the first lease released.");
        static_cast<void>(second.DetachToGuest());
    }

    void TestPreparedCommitRejectsChangedGuestBinding()
    {
        const auto root = CaseRoot("prepared-binding-change");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        auto prepared = coordinator.PrepareAccount("prepared-player");
        const auto changedPreferences = root / L"ChangedPrefs.json";
        const auto changedSaves = root / L"ChangedSaves";
        preferences.Rebind(changedPreferences);
        saves.Rebind(changedSaves);
        Require(
            !coordinator.CommitPrepared(std::move(prepared))
                && !coordinator.IsAccountActive()
                && preferences.FilePath() == changedPreferences
                && saves.Directory() == changedSaves,
            "Prepared transaction committed after its guest binding changed.");
    }

    void TestStalePreparedTransactionCannotCommit()
    {
        const auto root = CaseRoot("stale-transaction");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        const auto guestPath = preferences.FilePath();
        auto prepared = coordinator.PrepareAccount("player-stale");
        coordinator.ConfigureNamespace(
            "coordinator-game",
            "other-environment",
            "https://online.example.test/tenant/",
            false);
        Require(
            !coordinator.CommitPrepared(std::move(prepared))
                && preferences.FilePath() == guestPath
                && !coordinator.IsAccountActive(),
            "A stale prepared account changed the active profile.");
    }

    void TestCorruptAccountDocumentsFailClosed()
    {
        {
            const auto root = CaseRoot("corrupt-preferences");
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            LamaPon::SaveDataStore saves(root / L"Saves");
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            const LamaPon::PersistenceProfiles profiles(
                root,
                "coordinator-game",
                "test");
            const auto account = profiles.Account("player-corrupt-prefs");
            WriteText(account.playerPrefsFile, "{not-json");

            Require(
                Throws([&]
                {
                    static_cast<void>(coordinator.PrepareAccount(
                        "player-corrupt-prefs"));
                })
                    && !coordinator.IsAccountActive()
                    && preferences.GetString("owner") == "guest",
                "Corrupt account PlayerPrefs replaced the guest profile.");
        }

        {
            const auto root = CaseRoot("corrupt-save");
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            LamaPon::SaveDataStore saves(root / L"Saves");
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            const LamaPon::PersistenceProfiles profiles(
                root,
                "coordinator-game",
                "test");
            const auto account = profiles.Account("player-corrupt-save");
            WriteText(
                account.saveDataDirectory / L"slot.save.json",
                R"({"format":"LamaPonSaveData","version":1,"slot":"other","data":{}})");

            Require(
                Throws([&]
                {
                    static_cast<void>(coordinator.PrepareAccount(
                        "player-corrupt-save"));
                })
                    && !coordinator.IsAccountActive()
                    && preferences.FilePath() == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves",
                "Corrupt account SaveData was mistaken for an empty profile.");
        }
    }

    void TestFailedAccountSaveIsQuarantinedAndRetried()
    {
        const auto root = CaseRoot("quarantine");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        auto prepared = coordinator.PrepareAccount("player-quarantine");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate quarantine fixture account.");
        preferences.SetString("unsaved", "memory-value");

        const auto accountPath = preferences.FilePath();
        std::filesystem::create_directories(accountPath.parent_path());
        WriteText(
            accountPath,
            R"({"format":"LamaPonPlayerPrefs","version":1,"values":{}})");
        FileHandle lock;
        lock.value = CreateFileW(
            accountPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            lock.value != INVALID_HANDLE_VALUE,
            "Could not lock the account PlayerPrefs fixture.");

        Require(
            coordinator.DetachToGuest()
                    == DetachResult::QuarantinedAccount
                && coordinator.HasQuarantinedAccount()
                && preferences.GetString("owner") == "guest"
                && preferences.FilePath() == root / L"PlayerPrefs.json",
            "A failed account Save did not restore guest and quarantine memory.");
        Require(
            Throws([&]
            {
                static_cast<void>(
                    coordinator.PrepareAccount("another-player"));
            }),
            "A second account replaced quarantined unsaved memory.");

        LamaPon::PlayerPrefs peerPreferences(root / L"PlayerPrefs.json");
        peerPreferences.Load();
        LamaPon::SaveDataStore peerSaves(root / L"Saves");
        Coordinator peer(
            peerPreferences,
            peerSaves,
            root);
        ConfigureCoordinator(peer);
        Require(
            Throws([&]
            {
                static_cast<void>(
                    peer.PrepareAccount("player-quarantine"));
            }),
            "A quarantined account released its process lifetime lease early.");

        const auto recovery = coordinator.RecoveryStatus();
        lock.Close();
        Require(
            recovery.revision != 0
                && coordinator.RestorePendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasQuarantinedAccount(),
            "Explicit Restore reported a completed memory Save as stale.");

        LamaPon::PlayerPrefs verification(accountPath);
        verification.Load();
        Require(
            verification.GetString("unsaved") == "memory-value",
            "Quarantined memory was not durably recovered.");
        auto peerPrepared = peer.PrepareAccount("player-quarantine");
        Require(
            peerPrepared.IsValid(),
            "Recovered quarantine did not release the account lease.");
    }

    void TestImmediateDetachCheckpointsBaselineLessDelete()
    {
        const auto root = CaseRoot("detach-delete-checkpoint");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("checkpoint-player");
        {
            LamaPon::SaveDataStore seed(account.saveDataDirectory);
            seed.SaveJson("checkpoint-slot", R"({"delete":true})");
        }

        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCloudCoordinator(coordinator);

        auto prepared = coordinator.PrepareAccount(
            "checkpoint-player",
            "checkpoint-access-token");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate the detach checkpoint fixture.");
        FileHandle journalLock;
        const auto journalLockPath = coordinator.Journal()->FilePath()
            .parent_path() / L"CloudSaveJournal.lock";
        journalLock.value = CreateFileW(
            journalLockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            journalLock.value != INVALID_HANDLE_VALUE
                && !saves.DeleteSlot("checkpoint-slot")
                && LamaPon::Detail::LocalPersistenceDocuments::ReadSaveData(
                    saves,
                    "checkpoint-slot").state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "Local delete advanced before its intent WAL was durable.");
        journalLock.Close();

        FileHandle targetLock;
        const auto targetPath = saves.SlotPath("checkpoint-slot");
        targetLock.value = CreateFileW(
            targetPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            targetLock.value != INVALID_HANDLE_VALUE
                && Throws([&]
                {
                    static_cast<void>(saves.DeleteSlot("checkpoint-slot"));
                })
                && coordinator.Journal()->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::SaveSlot(
                        "checkpoint-slot")),
            "A failed local delete did not leave a recoverable WAL intent.");
        targetLock.Close();
        coordinator.Synchronizer()->RequestReconcile();
        Require(
            !coordinator.Journal()->HasLocalDeleteIntent(
                LamaPon::CloudSaveResource::SaveSlot("checkpoint-slot"))
                && LamaPon::Detail::LocalPersistenceDocuments::ReadSaveData(
                    saves,
                    "checkpoint-slot").state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "A loaded local document did not clear an aborted delete intent.");
        Require(
            saves.DeleteSlot("checkpoint-slot"),
            "Could not commit the pre-WAL local delete.");

        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount,
            "A durable detach checkpoint unexpectedly quarantined the account.");
        auto reopened = coordinator.PrepareAccount(
            "checkpoint-player",
            "checkpoint-access-token-2");
        Require(
            coordinator.CommitPrepared(std::move(reopened))
                && coordinator.Journal()
                && coordinator.Journal()->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::SaveSlot(
                        "checkpoint-slot")),
            "Immediate sign-out lost a baseline-less local delete intent.");
        static_cast<void>(coordinator.DetachToGuest());
    }

    void TestFailedDetachCheckpointKeepsAccountLeaseUntilDiscard()
    {
        // CTestの深いbuild-directoryからでもatomic publish用suffixを含めて
        // legacy MAX_PATH内に収まる短いfixture名を使います。
        const auto root = CaseRoot("checkpoint-q");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("checkpoint-player");
        {
            LamaPon::SaveDataStore seed(account.saveDataDirectory);
            seed.SaveJson("checkpoint-slot", R"({"delete":true})");
        }

        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCloudCoordinator(coordinator);
        auto prepared = coordinator.PrepareAccount(
            "checkpoint-player",
            "checkpoint-access");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate the failed checkpoint fixture.");
        {
            // pre-delete hookを通らない旧経路相当を作り、detach checkpoint
            // 自体のI/O失敗がquarantineされることを固定します。
            LamaPon::Detail::ScopedLocalPersistenceObserverSuppression suppress;
            Require(
                saves.DeleteSlot("checkpoint-slot"),
                "Could not prepare the failed checkpoint deletion edge.");
        }

        const auto lockPath = coordinator.Journal()->FilePath()
            .parent_path() / L"CloudSaveJournal.lock";
        FileHandle lock;
        lock.value = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            lock.value != INVALID_HANDLE_VALUE,
            "Could not hold the detach checkpoint journal lock.");
        Require(
            coordinator.DetachToGuest()
                    == DetachResult::QuarantinedAccount
                && coordinator.HasPendingRecovery(),
            "A failed detach checkpoint was not quarantined.");

        LamaPon::PlayerPrefs peerPreferences(root / L"PlayerPrefs.json");
        peerPreferences.Load();
        LamaPon::SaveDataStore peerSaves(root / L"Saves");
        Coordinator peer(peerPreferences, peerSaves, root);
        ConfigureCoordinator(peer);
        Require(
            Throws([&]
            {
                static_cast<void>(peer.PrepareAccount(
                    "checkpoint-player"));
            }),
            "A checkpoint-failed quarantine released its account lease.");

        const auto recovery = coordinator.RecoveryStatus();
        Require(
            recovery.revision != 0
                && coordinator.RestorePendingRecovery(recovery.revision)
                    != LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && coordinator.HasPendingRecovery(),
            "A locked checkpoint was incorrectly reported as recovered.");
        lock.Close();
        Require(
            coordinator.RestorePendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery(),
            "A durable checkpoint plan could not be retried after lock recovery.");
        auto peerPrepared = peer.PrepareAccount("checkpoint-player");
        Require(
            peerPrepared.IsValid()
                && peer.CommitPrepared(std::move(peerPrepared))
                && peer.Journal()
                && peer.Journal()->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::SaveSlot(
                        "checkpoint-slot")),
            "Checkpoint recovery did not preserve intent or release the account lease.");
        static_cast<void>(peer.DetachToGuest());
    }

    void TestUndeterminedDetachCheckpointRemainsFailClosed()
    {
        const auto root = CaseRoot("checkpoint-scan-failure");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("checkpoint-scan-player");
        {
            LamaPon::SaveDataStore seed(account.saveDataDirectory);
            seed.SaveJson("scan-slot", R"({"value":1})");
        }

        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCloudCoordinator(coordinator);
        auto prepared = coordinator.PrepareAccount(
            "checkpoint-scan-player",
            "checkpoint-access");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate the checkpoint scan fixture.");

        WriteText(saves.SlotPath("scan-slot"), "{not-json");
        Require(
            coordinator.DetachToGuest()
                    == DetachResult::QuarantinedAccount
                && coordinator.HasPendingRecovery(),
            "An undetermined detach checkpoint was not quarantined.");
        const auto recovery = coordinator.RecoveryStatus();
        Require(
            recovery.state
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryState::UnavailableSidecar
                && recovery.revision != 0,
            "An undetermined checkpoint incorrectly exposed Restore.");

        {
            LamaPon::SaveDataStore repaired(account.saveDataDirectory);
            repaired.SaveJson("scan-slot", R"({"value":2})");
        }
        coordinator.EndFrame();
        Require(
            coordinator.HasPendingRecovery()
                && coordinator.RestorePendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Failed,
            "A checkpoint with no determined operation plan auto-recovered.");
        Require(
            coordinator.DiscardPendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery(),
            "Explicit discard could not release an undetermined checkpoint.");
    }

    void TestCleanLoadFailureDoesNotPermanentlyBlockProfiles()
    {
        const auto root = CaseRoot("clean-load-failure");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        auto prepared = coordinator.PrepareAccount("clean-failure-player");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate clean load-failure account.");
        preferences.SetString("saved", "account-value");
        preferences.Save();
        WriteText(preferences.FilePath(), "{not-json");
        Require(
            Throws([&]
            {
                preferences.Reload();
            })
                && preferences.HasLoadFailure()
                && !preferences.IsDirty(),
            "Clean PlayerPrefs reload failure fixture was invalid.");

        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount
                && !coordinator.HasPendingRecovery()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && preferences.GetString("owner") == "guest",
            "Clean load failure created a permanent quarantine.");
        Require(
            Throws([&]
            {
                static_cast<void>(
                    coordinator.PrepareAccount("clean-failure-player"));
            })
                && !coordinator.HasPendingRecovery(),
            "Corrupt original account was not rejected independently.");
        auto other = coordinator.PrepareAccount("clean-failure-other");
        Require(
            coordinator.CommitPrepared(std::move(other)),
            "Clean load failure blocked every later account.");
        static_cast<void>(coordinator.DetachToGuest());
    }

    void TestDirtyLoadFailureUsesRecoverableStrictSidecar()
    {
        const auto root = CaseRoot("dirty-load-failure-recovery");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("recovery-player");
        std::filesystem::path recoverySidecar;

        {
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            preferences.Save();
            LamaPon::SaveDataStore saves(root / L"Saves");
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            auto prepared = coordinator.PrepareAccount("recovery-player");
            Require(
                coordinator.CommitPrepared(std::move(prepared)),
                "Could not activate dirty load-failure account.");
            preferences.SetString("saved", "before-corruption");
            preferences.Save();
            preferences.SetString("unsaved", "memory-snapshot");
            WriteText(account.playerPrefsFile, "{not-json");
            Require(
                Throws([&]
                {
                    preferences.Reload();
                })
                    && preferences.HasLoadFailure()
                    && preferences.IsDirty()
                    && preferences.GetString("unsaved")
                        == "memory-snapshot",
                "Dirty PlayerPrefs reload failure lost its memory state.");

            // activation時はMissingだったsidecarを外部writerが先に作成した
            // 場合、内容がmemory snapshotと偶然同一でも所有物として採用・
            // 上書きせず、memory quarantineを維持します。
            const auto blockedRecoverySidecar =
                account.rootDirectory / L"Recovery.prefs";
            const auto externalSnapshot = preferences.SerializeToJson();
            WriteText(blockedRecoverySidecar, externalSnapshot);
            Require(
                coordinator.DetachToGuest()
                        == DetachResult::QuarantinedAccount
                    && coordinator.RecoveryState()
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::UnavailableSidecar
                    && coordinator.HasQuarantinedAccount()
                    && coordinator.HasPendingRecovery()
                    && ReadText(blockedRecoverySidecar)
                        == externalSnapshot
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && preferences.GetString("owner") == "guest",
                "Failed sidecar publish discarded dirty account memory.");

            std::error_code removeError;
            std::filesystem::remove(
                blockedRecoverySidecar,
                removeError);
            Require(
                !removeError,
                "Could not unblock recovery sidecar target.");
            LamaPon::Detail::SetOnlinePersistenceRecoveryTestFailPoint(
                LamaPon::Detail::OnlinePersistenceRecoveryTestFailPoint::
                    AfterSidecarPublishBeforeVerification);
            coordinator.EndFrame();
            Require(
                coordinator.RecoveryState()
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::MemorySnapshot
                    && coordinator.HasQuarantinedAccount()
                    && std::filesystem::exists(blockedRecoverySidecar),
                "An ambiguous sidecar publish discarded memory before strict verification.");
            const auto recoveryDeadline =
                std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            while (coordinator.RecoveryState()
                    != LamaPon::Detail::
                        OnlinePersistenceRecoveryState::DurableSidecar
                && std::chrono::steady_clock::now()
                    < recoveryDeadline)
            {
                coordinator.EndFrame();
                std::this_thread::yield();
            }
            recoverySidecar = coordinator.RecoverySidecarPath();
            Require(
                coordinator.RecoveryState()
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::DurableSidecar
                    && !coordinator.HasQuarantinedAccount()
                    && coordinator.HasPendingRecovery()
                    && !recoverySidecar.empty()
                    && std::filesystem::is_regular_file(recoverySidecar),
                "Dirty memory was not moved to a durable recovery sidecar.");

            LamaPon::PlayerPrefs strictSidecar(recoverySidecar);
            strictSidecar.Load();
            Require(
                strictSidecar.GetString("unsaved") == "memory-snapshot",
                "Recovery sidecar did not contain the full memory snapshot.");
        }

        // 新しいCoordinatorでも対象accountのsidecarをstrict検出し、
        // 通常activationとして見落としません。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator restarted(preferences, saves, root);
        ConfigureCoordinator(restarted);
        Require(
            Throws([&]
            {
                static_cast<void>(
                    restarted.PrepareAccount("recovery-player"));
            })
                && restarted.RecoveryState()
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryState::DurableSidecar
                && restarted.RecoverySidecarPath() == recoverySidecar,
            "Restarted coordinator ignored an account recovery sidecar.");

        FileHandle sidecarLock;
        sidecarLock.value = CreateFileW(
            recoverySidecar.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            sidecarLock.value != INVALID_HANDLE_VALUE,
            "Could not lock recovery sidecar fixture.");
        Require(
            !restarted.RestorePendingRecovery()
                && restarted.HasPendingRecovery(),
            "Sidecar delete failure was not retained for retry.");
        LamaPon::PlayerPrefs appliedBeforeDelete(account.playerPrefsFile);
        appliedBeforeDelete.Load();
        Require(
            appliedBeforeDelete.GetString("unsaved")
                == "memory-snapshot",
            "Recovery did not apply the original before sidecar deletion.");

        sidecarLock.Close();
        Require(
            restarted.RestorePendingRecovery()
                && !restarted.HasPendingRecovery()
                && !std::filesystem::exists(recoverySidecar),
            "Recovery sidecar could not be retried after delete failure.");
        auto restored = restarted.PrepareAccount("recovery-player");
        Require(
            restarted.CommitPrepared(std::move(restored))
                && preferences.GetString("unsaved")
                    == "memory-snapshot",
            "Explicit recovery did not restore the account profile.");
        static_cast<void>(restarted.DetachToGuest());
    }

    void TestExplicitRecoveryDiscardPreservesOriginal()
    {
        const auto root = CaseRoot("recovery-discard");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("discard-player");
        LamaPon::PlayerPrefs original(account.playerPrefsFile);
        original.Load();
        original.SetString("owner", "original-disk");
        original.Save();
        const auto sidecarPath =
            account.rootDirectory / L"Recovery.prefs";
        LamaPon::PlayerPrefs sidecar(sidecarPath);
        sidecar.Load();
        sidecar.SetString("owner", "discarded-snapshot");
        sidecar.Save();

        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);
        const bool prepareRejected = Throws([&]
        {
            static_cast<void>(
                coordinator.PrepareAccount("discard-player"));
        });
        const auto discardStatus = coordinator.RecoveryStatus();
        const auto discardResult = coordinator.DiscardPendingRecovery(
            discardStatus.revision);
        Require(
            prepareRejected
                && discardResult
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery()
                && !std::filesystem::exists(sidecarPath),
            "Explicit recovery discard did not remove only the sidecar.");
        LamaPon::PlayerPrefs verification(account.playerPrefsFile);
        verification.Load();
        Require(
            verification.GetString("owner") == "original-disk",
            "Recovery discard overwrote the original account document.");

        WriteText(sidecarPath, "{not-json");
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount("discard-player");
            })
                && coordinator.RecoveryState()
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryState::UnavailableSidecar,
            "Corrupt recovery sidecar was not detected.");
        const auto corrupt = coordinator.RecoveryStatus();
        WriteText(sidecarPath, "{different-corrupt-json");
        const auto changedCorrupt = coordinator.RecoveryStatus();
        Require(
            changedCorrupt.revision != corrupt.revision
                && coordinator.DiscardPendingRecovery(corrupt.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Stale
                && coordinator.DiscardPendingRecovery(
                    changedCorrupt.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath),
            "A latest-revision explicit discard could not remove the observed replacement.");
        WriteText(sidecarPath, "{not-json");
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount("discard-player");
            }),
            "A second corrupt recovery incident was not detected.");
        const auto restoredCorrupt = coordinator.RecoveryStatus();
        Require(
            restoredCorrupt.revision != 0
                && coordinator.DiscardPendingRecovery(
                    restoredCorrupt.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath),
            "Explicit discard could not remove an unchanged corrupt sidecar under its profile lease.");
    }

    void TestMemoryRecoveryDiscardPromotesExistingSidecar()
    {
        const auto run = [](
            const std::string_view caseName,
            const bool externalSidecar)
        {
            const auto root = CaseRoot(caseName);
            const LamaPon::PersistenceProfiles profiles(
                root,
                "coordinator-game",
                "test");
            const auto account = profiles.Account("discard-memory-player");
            const auto sidecarPath =
                account.rootDirectory / L"Recovery.prefs";

            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            LamaPon::SaveDataStore saves(root / L"Saves");
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            auto prepared = coordinator.PrepareAccount(
                "discard-memory-player");
            Require(
                coordinator.CommitPrepared(std::move(prepared)),
                "Could not activate the memory discard fixture.");

            preferences.SetString("unsaved", "memory-snapshot");
            WriteText(account.playerPrefsFile, "{not-json");
            Require(
                Throws([&] { preferences.Reload(); })
                    && preferences.HasLoadFailure()
                    && preferences.IsDirty(),
                "Could not create a dirty load-failure snapshot.");

            if (externalSidecar)
            {
                LamaPon::PlayerPrefs external(sidecarPath);
                external.Load();
                external.SetString("owner", "external-sidecar");
                external.Save();
            }
            else
            {
                LamaPon::Detail::SetOnlinePersistenceRecoveryTestFailPoint(
                    LamaPon::Detail::
                        OnlinePersistenceRecoveryTestFailPoint::
                            AfterSidecarPublishBeforeVerification);
            }

            Require(
                coordinator.DetachToGuest()
                        == DetachResult::QuarantinedAccount
                    && coordinator.HasPendingRecovery()
                    && std::filesystem::exists(sidecarPath),
                "Memory recovery collision was not quarantined.");
            const auto preserved = ReadText(sidecarPath);
            const auto first = coordinator.RecoveryStatus();
            Require(
                first.revision != 0
                    && coordinator.DiscardPendingRecovery(first.revision)
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryOperationResult::Succeeded
                    && coordinator.HasPendingRecovery()
                    && std::filesystem::exists(sidecarPath)
                    && ReadText(sidecarPath) == preserved,
                "First-stage discard removed or replaced the observed sidecar.");

            const auto promoted = coordinator.RecoveryStatus();
            Require(
                promoted.revision != first.revision
                    && promoted.state
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::DurableSidecar
                    && coordinator.DiscardPendingRecovery(promoted.revision)
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryOperationResult::Succeeded
                    && !coordinator.HasPendingRecovery()
                    && !std::filesystem::exists(sidecarPath),
                "Promoted recovery incident could not be discarded explicitly.");
        };

        run("discard-memory-external", true);
        run("discard-memory-published", false);
    }

    void TestRecoverySidecarResolutionReacquiresProfileLease()
    {
        const auto root = CaseRoot("recovery-sidecar-lease");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("recovery-race-player");
        const auto sidecarPath = account.rootDirectory / L"Recovery.prefs";
        LamaPon::PlayerPrefs sidecar(sidecarPath);
        sidecar.Load();
        sidecar.SetString("owner", "recovered-by-peer");
        sidecar.Save();

        LamaPon::PlayerPrefs firstPreferences(root / L"PlayerPrefs.json");
        firstPreferences.Load();
        LamaPon::SaveDataStore firstSaves(root / L"Saves");
        Coordinator first(firstPreferences, firstSaves, root);
        ConfigureCoordinator(first);
        Require(
            Throws([&]
            {
                (void)first.PrepareAccount("recovery-race-player");
            })
                && first.HasPendingRecovery(),
            "First coordinator did not cache the detected sidecar.");

        LamaPon::PlayerPrefs secondPreferences(root / L"PlayerPrefs.json");
        secondPreferences.Load();
        LamaPon::SaveDataStore secondSaves(root / L"Saves");
        Coordinator second(secondPreferences, secondSaves, root);
        ConfigureCoordinator(second);
        Require(
            Throws([&]
            {
                (void)second.PrepareAccount("recovery-race-player");
            })
                && !second.HasPendingRecovery(),
            "A peer inspected recovery state while the detecting owner held its lease.");
        Require(
            first.RestorePendingRecovery(),
            "Detecting coordinator could not resolve the sidecar under its lease.");
        auto prepared = first.PrepareAccount("recovery-race-player");
        Require(
            first.CommitPrepared(std::move(prepared))
                && firstPreferences.GetString("owner")
                    == "recovered-by-peer",
            "Detecting coordinator did not activate the recovered profile.");

        Require(
            Throws([&]
            {
                (void)second.PrepareAccount("recovery-race-player");
            })
                && !second.IsAccountActive(),
            "A peer activated the recovered account before its owner detached.");
        static_cast<void>(first.DetachToGuest());
        auto secondPrepared = second.PrepareAccount("recovery-race-player");
        Require(
            second.CommitPrepared(std::move(secondPrepared)),
            "Peer could not activate after recovery owner released its lease.");
        static_cast<void>(second.DetachToGuest());
    }

    void TestEmptyAndOversizedRecoverySidecarsCanBeDiscardedSafely()
    {
        const auto root = CaseRoot("recovery-corrupt-availability");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("corrupt-sidecar-player");
        const auto sidecarPath = account.rootDirectory / L"Recovery.prefs";
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        WriteText(sidecarPath, {});
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "An empty recovery sidecar was not quarantined.");
        const auto empty = coordinator.RecoveryStatus();
        Require(
            empty.revision != 0u
                && coordinator.DiscardPendingRecovery(empty.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery()
                && !std::filesystem::exists(sidecarPath),
            "An unchanged empty recovery sidecar could not be discarded.");

        const std::string oversized(
            LamaPon::CloudPreferencesMaxBytes + 1u,
            'a');
        WriteText(sidecarPath, oversized);
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "An oversized recovery sidecar was not quarantined.");
        const auto unchangedLarge = coordinator.RecoveryStatus();
        Require(
            coordinator.DiscardPendingRecovery(unchangedLarge.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath),
            "An unchanged oversized recovery sidecar could not be discarded.");

        WriteText(sidecarPath, oversized);
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "The replacement recovery incident was not quarantined.");
        const auto beforeReplacement = coordinator.RecoveryStatus();
        const auto replacementPath =
            account.rootDirectory / L"Recovery.replacement";
        WriteText(
            replacementPath,
            std::string(oversized.size(), 'b'));
        Require(
            MoveFileExW(
                replacementPath.c_str(),
                sidecarPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
            "Oversized recovery replacement could not be published.");
        const auto afterReplacement = coordinator.RecoveryStatus();
        Require(
            afterReplacement.revision != beforeReplacement.revision
                && coordinator.DiscardPendingRecovery(
                    beforeReplacement.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Stale
                && coordinator.DiscardPendingRecovery(
                    afterReplacement.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath)
                && !coordinator.HasPendingRecovery(),
            "The latest observed oversized replacement could not be discarded safely.");

        WriteText(sidecarPath, oversized);
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "The missing-sidecar recovery incident was not quarantined.");
        const auto beforeMissing = coordinator.RecoveryStatus();
        std::error_code removeError;
        std::filesystem::remove(sidecarPath, removeError);
        Require(!removeError, "Recovery sidecar removal fixture failed.");
        const auto missing = coordinator.RecoveryStatus();
        Require(
            missing.revision != beforeMissing.revision
                && coordinator.DiscardPendingRecovery(missing.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery(),
            "An already-missing sidecar kept recovery permanently blocked.");
    }

    void TestPublicRecoveryRevisionAndOperationGuards()
    {
        const auto root = CaseRoot("public-recovery-api");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "recovery-busy" },
                        { "pollToken", "recovery-busy-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/recovery-busy"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    })
            },
            0u);
        auto services = LamaPon::Detail::OnlineServicesTestAccess::Create(
            OnlineConfiguration(),
            [backend](const LamaPon::HttpRequest& request)
            {
                return backend->Send(request);
            },
            std::make_unique<MemoryTokenStore>(
                std::make_shared<TokenStoreState>()));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(*services);
        Require(coordinator != nullptr, "Recovery API coordinator is missing.");

        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("public-recovery-player");
        const auto sidecarPath = account.rootDirectory / L"Recovery.prefs";
        const auto writeSidecar = [&](const std::string_view value)
        {
            LamaPon::PlayerPrefs sidecar(sidecarPath);
            sidecar.Load();
            sidecar.SetString("snapshot", std::string(value));
            sidecar.Save();
        };
        writeSidecar("first");
        Require(
            Throws([&]
            {
                (void)coordinator->PrepareAccount("public-recovery-player");
            }),
            "Recovery API fixture was not detected.");
        const auto first = services->PersistenceRecoveryStatus();
        Require(
            first.state
                    == LamaPon::OnlinePersistenceRecoveryState::DurableSidecar
                && first.revision != 0
                && services->PersistenceRecoveryStatus().revision
                    == first.revision,
            "Recovery status did not expose a stable nonzero revision.");

        const auto requestsBefore = backend->Requests().size();
        Require(
            !services->BeginDiscordSignIn()
                && services->LastErrorCode()
                    == "persistence_recovery_required"
                && backend->Requests().size() == requestsBefore,
            "Pending recovery did not reject sign-in before HTTP dispatch.");
        LamaPon::OnlineServiceConfiguration disabled;
        Require(
            Throws([&]
            {
                services->Configure(disabled);
            })
                && coordinator->IsNamespaceEnabled()
                && services->PersistenceRecoveryStatus().revision
                    == first.revision,
            "Pending recovery changed namespace during rejected Configure.");

        LamaPon::PlayerPrefs expectedSidecar(sidecarPath);
        const auto expectedDocument =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                expectedSidecar);
        writeSidecar("tampered");
        const auto changed = services->PersistenceRecoveryStatus();
        Require(
            expectedDocument.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded
                && changed.state
                    == LamaPon::OnlinePersistenceRecoveryState::UnavailableSidecar
                && changed.revision != first.revision
                && services->DiscardPersistence(first.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Stale
                && services->RestorePersistence(changed.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Failed,
            "Sidecar replacement was adopted by Restore as a trusted snapshot.");
        Require(
            services->DiscardPersistence(changed.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Succeeded
                && services->PersistenceRecoveryStatus().state
                    == LamaPon::OnlinePersistenceRecoveryState::None
                && !std::filesystem::exists(sidecarPath),
            "Latest-revision replacement discard failed.");

        // revision counterはNoneを公開する間も内部で巻き戻さず、別incident
        // に旧UI tokenが一致するABAを防ぎます。
        writeSidecar("second");
        Require(
            Throws([&]
            {
                (void)coordinator->PrepareAccount("public-recovery-player");
            }),
            "Second recovery incident was not detected.");
        const auto second = services->PersistenceRecoveryStatus();
        Require(
            second.revision != 0
                && second.revision != first.revision
                && services->DiscardPersistence(first.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Stale
                && services->DiscardPersistence(second.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "Recovery revision allowed a stale cross-incident operation.");

        Require(
            services->BeginDiscordSignIn(),
            "Could not start Busy recovery ordering fixture.");
        backend->WaitUntilRequestBlocked();
        writeSidecar("busy-third");
        Require(
            Throws([&]
            {
                (void)coordinator->PrepareAccount("public-recovery-player");
            }),
            "Busy recovery incident was not detected.");
        const auto busyRecovery = services->PersistenceRecoveryStatus();
        Require(
            services->DiscardPersistence(first.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Stale
                && services->DiscardPersistence(busyRecovery.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Busy,
            "Recovery stale revision was not checked before Busy.");
        backend->ReleaseBlockedRequest();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Busy recovery login start did not finish.");
        services->CancelDiscordSignIn();
        const auto currentRecovery = services->PersistenceRecoveryStatus();
        Require(
            services->DiscardPersistence(currentRecovery.revision)
                == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "Recovery could not be discarded after Busy ended.");
    }

    void TestPublicCloudConflictRegistryAndResolution()
    {
        const auto root = CaseRoot("public-cloud-conflicts");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "public-cloud-access",
            "public-cloud-refresh",
            "public-cloud-player",
            900u);
        authorized["status"] = "authorized";
        const auto refreshed = SessionJson(
            "public-cloud-access-2",
            "public-cloud-refresh-2",
            "public-cloud-player",
            900u);
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "public-cloud-login" },
                        { "pollToken", "public-cloud-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/public-cloud"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(200u, refreshed),
                JsonResponse(204u)
            },
            2u);
        auto services = LamaPon::Detail::OnlineServicesTestAccess::Create(
            OnlineConfiguration(),
            [backend](const LamaPon::HttpRequest& request)
            {
                return backend->Send(request);
            },
            std::make_unique<MemoryTokenStore>(
                std::make_shared<TokenStoreState>()));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Public cloud conflict login did not complete.");

        preferences.SetString("local", "preferences");
        preferences.Save();
        saves.SaveJson("public-slot", R"({"local":"slot"})");
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(*services);
        auto* const journal = coordinator ? coordinator->Journal() : nullptr;
        Require(journal != nullptr, "Public cloud journal is missing.");
        const auto preferencesDocument =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                preferences);
        const auto slotDocument =
            LamaPon::Detail::LocalPersistenceDocuments::ReadSaveData(
                saves,
                "public-slot");
        Require(
            preferencesDocument.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded
                && slotDocument.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "Public cloud local fixtures were not readable.");

        const auto preferencesResource =
            LamaPon::CloudSaveResource::Preferences();
        const auto slotResource =
            LamaPon::CloudSaveResource::SaveSlot("public-slot");
        constexpr std::string_view PreferencesMutation =
            "11111111-1111-4111-8111-111111111111";
        constexpr std::string_view SlotMutation =
            "22222222-2222-4222-8222-222222222222";
        journal->QueuePut(
            preferencesResource,
            preferencesDocument.bytes,
            PreferencesMutation);
        journal->RecordConflict(
            preferencesResource,
            PreferencesMutation,
            { preferencesResource, "\"remote-prefs\"", true, {}, {} });
        journal->QueuePut(
            slotResource,
            slotDocument.bytes,
            SlotMutation);
        journal->RecordConflict(
            slotResource,
            SlotMutation,
            { slotResource, "\"remote-slot\"", true, {}, {} });

        const auto first = services->CloudConflicts();
        const auto second = services->CloudConflicts();
        Require(
            first.size() == 2u && second.size() == 2u,
            "Public conflict enumeration lost journal conflicts.");
        const auto findByKind = [](const auto& conflicts, const auto kind)
        {
            return std::find_if(
                conflicts.begin(),
                conflicts.end(),
                [kind](const LamaPon::OnlineCloudConflict& conflict)
                {
                    return conflict.kind == kind;
                });
        };
        const auto firstPreferences = findByKind(
            first,
            LamaPon::OnlineCloudResourceKind::Preferences);
        const auto firstSlot = findByKind(
            first,
            LamaPon::OnlineCloudResourceKind::SaveSlot);
        const auto secondPreferences = findByKind(
            second,
            LamaPon::OnlineCloudResourceKind::Preferences);
        const auto secondSlot = findByKind(
            second,
            LamaPon::OnlineCloudResourceKind::SaveSlot);
        const auto canonicalOpaque = [](const std::string_view value)
        {
            return value.size() == 32u
                && value.find_first_not_of("0123456789abcdef")
                    == std::string_view::npos;
        };
        Require(
            firstPreferences != first.end()
                && firstSlot != first.end()
                && secondPreferences != second.end()
                && secondSlot != second.end()
                && canonicalOpaque(firstPreferences->id)
                && canonicalOpaque(firstSlot->id)
                && firstPreferences->id == secondPreferences->id
                && firstSlot->id == secondSlot->id
                && firstPreferences->id != PreferencesMutation
                && firstSlot->id != SlotMutation
                && firstPreferences->slot.empty()
                && firstPreferences->localByteLength
                    == preferencesDocument.bytes.size()
                && firstPreferences->remoteDeleted
                && firstSlot->slot == "public-slot"
                && firstSlot->localByteLength == slotDocument.bytes.size()
                && firstSlot->remoteDeleted,
            "Public conflict DTO exposed unstable or incorrect metadata.");
        Require(
            services->ResolveCloudConflict(
                "stale-opaque-id",
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Stale,
            "Unknown conflict ID was not rejected as stale.");

        services->Update(850.0f);
        backend->WaitUntilRequestBlocked();
        Require(
            services->RequestCloudSync()
                    == LamaPon::OnlinePersistenceOperationResult::Busy
                && services->ResolveCloudConflict(
                firstPreferences->id,
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Busy
                && services->CloudConflicts().front().id
                    == firstPreferences->id,
            "A valid conflict ID was not preserved while auth was Busy.");
        auto* const synchronizer = coordinator->Synchronizer();
        Require(synchronizer != nullptr, "Public cloud synchronizer is missing.");
        synchronizer->ResolveConflict(
            preferencesResource,
            PreferencesMutation,
            LamaPon::Detail::CloudSaveConflictResolution::UseRemote);
        Require(
            services->ResolveCloudConflict(
                firstPreferences->id,
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Stale,
            "A disappeared conflict was reported Busy instead of Stale.");
        const auto remainingWhileBusy = services->CloudConflicts();
        Require(
            remainingWhileBusy.size() == 1u
                && remainingWhileBusy.front().id == firstSlot->id,
            "Pruning a stale conflict invalidated an unrelated ID.");
        backend->ReleaseBlockedRequest();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Public conflict refresh did not complete.");
        Require(
            services->ResolveCloudConflict(
                firstSlot->id,
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "Public UseLocal conflict resolution failed.");
        const auto remaining = services->CloudConflicts();
        Require(
            remaining.empty(),
            "Resolved conflicts remained in the public registry.");

        preferences.SetString("local", "after-resolution");
        preferences.Save();
        const auto recreatedPreferences =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                preferences);
        constexpr std::string_view SignOutMutation =
            "33333333-3333-4333-8333-333333333333";
        journal->QueuePut(
            preferencesResource,
            recreatedPreferences.bytes,
            SignOutMutation,
            std::optional<std::string>{ "\"remote-prefs\"" });
        journal->RecordConflict(
            preferencesResource,
            SignOutMutation,
            { preferencesResource, "\"remote-prefs\"", true, {}, {} });
        const auto beforeSignOut = services->CloudConflicts();
        Require(
            beforeSignOut.size() == 1u,
            "Sign-out conflict fixture was not published.");
        const auto staleAfterSignOut = beforeSignOut.front().id;
        services->SignOut();
        Require(
            services->ResolveCloudConflict(
                staleAfterSignOut,
                LamaPon::OnlineCloudConflictResolution::UseRemote)
                    == LamaPon::OnlinePersistenceOperationResult::Stale,
            "Sign-out did not invalidate conflict IDs before Busy checks.");
    }

    void TestCredentialSaveFailureRollsBackAndRevokesSession()
    {
        const auto root = CaseRoot("credential-save-rollback");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "rejected-access-token",
            "rejected-refresh-token",
            "rejected-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "transaction" },
                        { "pollToken", "poll-token" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/authorize"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        storeState->failSave = true;
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Rejected session cleanup did not finish.");

        const auto requests = backend->Requests();
        Require(
            !services->IsSignedIn()
                && services->Player().playerId.empty()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 3u
                && requests.back().url.ends_with(
                    L"/v1/auth/session/logout")
                && HasBearer(
                    requests.back(),
                    L"rejected-access-token"),
            "Credential Save failure exposed or retained a session.");
    }

    void TestRequestCloudSyncReportsBusyAndTerminalStop()
    {
        const auto root = CaseRoot("public-cloud-request-state");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");
        auto authorized = SessionJson(
            "request-cloud-access",
            "request-cloud-refresh",
            "request-cloud-player",
            900u);
        authorized["status"] = "authorized";
        auto requestCount = std::make_shared<std::atomic_size_t>();
        auto services = LamaPon::Detail::OnlineServicesTestAccess::Create(
            OnlineConfiguration(),
            [authorized, requestCount](const LamaPon::HttpRequest& request)
            {
                requestCount->fetch_add(1u, std::memory_order_relaxed);
                if (request.url.ends_with(L"/v1/auth/login/start"))
                {
                    return JsonResponse(
                        201u,
                        {
                            { "transactionId", "request-cloud-login" },
                            { "pollToken", "request-cloud-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/request-cloud"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        });
                }
                if (request.url.ends_with(L"/v1/auth/login/complete"))
                {
                    return JsonResponse(200u, authorized);
                }
                if (request.url.ends_with(
                        L"/v1/cloud-saves/manifest"))
                {
                    return EmptyCloudManifestResponse();
                }
                if (request.url.ends_with(L"/v1/auth/session/logout"))
                {
                    return JsonResponse(204u);
                }
                LamaPon::HttpResponse unexpected;
                unexpected.transportError = "Unexpected request.";
                return unexpected;
            },
            std::make_unique<MemoryTokenStore>(
                std::make_shared<TokenStoreState>()));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        const auto idleDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        while ((services->State()
                    != LamaPon::OnlineAccountState::SignedIn
                || services->CloudSyncStatus().state
                    != LamaPon::OnlineCloudSyncState::Idle)
            && std::chrono::steady_clock::now() < idleDeadline)
        {
            services->Update(0.0f);
            LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
            std::this_thread::yield();
        }
        const auto initialCloudStatus = services->CloudSyncStatus();
        if (services->State() != LamaPon::OnlineAccountState::SignedIn
            || initialCloudStatus.state
                != LamaPon::OnlineCloudSyncState::Idle)
        {
            throw std::runtime_error(
                "Cloud request fixture did not become idle (state="
                + std::to_string(static_cast<int>(initialCloudStatus.state))
                + ", stop="
                + std::to_string(
                    static_cast<int>(initialCloudStatus.stopReason))
                + ", requests="
                + std::to_string(requestCount->load())
                + ").");
        }

        WriteText(preferences.FilePath(), "{corrupt-local");
        Require(
            services->RequestCloudSync()
                == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "A healthy synchronizer rejected an explicit request.");
        const auto stoppedDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        while (services->CloudSyncStatus().state
                    != LamaPon::OnlineCloudSyncState::Stopped
            && std::chrono::steady_clock::now() < stoppedDeadline)
        {
            services->Update(0.0f);
            LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
            std::this_thread::yield();
        }
        Require(
            services->CloudSyncStatus().state
                    == LamaPon::OnlineCloudSyncState::Stopped
                && services->RequestCloudSync()
                == LamaPon::OnlinePersistenceOperationResult::Failed,
            "A terminally stopped synchronizer reported request success.");
    }

    void TestFailedCredentialSaveKeepsPriorCommitPendingUntilDeleted()
    {
        const auto runCase = [](const bool logoutSucceeds)
        {
            const auto root = CaseRoot(
                logoutSucceeds
                    ? "failed-save-logout-success"
                    : "failed-save-logout-failure");
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            LamaPon::SaveDataStore saves(root / L"Saves");

            auto authorized = SessionJson(
                "failed-save-access",
                "failed-save-candidate-refresh",
                "failed-save-player");
            authorized["status"] = "authorized";
            const auto logoutResponse = logoutSucceeds
                ? JsonResponse(204u)
                : JsonResponse(
                    503u,
                    {
                        {
                            "error",
                            { { "code", "logout_failed" } }
                        }
                    });
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            { "transactionId", "failed-save-transaction" },
                            { "pollToken", "failed-save-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/failed-save"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    logoutResponse
                });
            auto storeState = std::make_shared<TokenStoreState>();
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            // Configure時のLoad後に以前commit済みの資格情報を模擬します。
            // Save失敗はcandidateへ変更せず、rollback Delete失敗中も
            // この旧値だけを保持するのがIRefreshTokenStoreの契約です。
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "previous-committed-refresh";
            storeState->failSave = true;
            storeState->failDelete = true;
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            CompleteDiscordLogin(*services);
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Ambiguous credential rollback did not finish.");
            Require(
                services->LastErrorCode() == "credential_save_failed"
                    && storeState->saveCount == 1u
                    && storeState->deleteCount == 1u
                    && storeState->token
                        == "previous-committed-refresh"
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json",
                "Ambiguous credential rollback lost its primary error or pending token.");

            Require(
                !services->BeginDiscordSignIn()
                    && services->LastErrorCode()
                        == "credential_delete_failed"
                    && storeState->deleteCount == 2u
                    && backend->Requests().size() == 3u,
                "New sign-in did not fail closed on a pending credential delete.");
            Require(
                Throws([&]
                {
                    services->Configure(OnlineConfiguration());
                })
                    && storeState->deleteCount == 3u
                    && backend->Requests().size() == 3u
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json",
                "Configure replaced state while credential deletion was pending.");

            storeState->failDelete = false;
            services.reset();
            Require(
                storeState->deleteCount == 4u
                    && storeState->token.empty(),
                "Destruction did not resolve the sticky credential delete.");
        };

        runCase(true);
        runCase(false);
    }

    void TestSignOutImmediatelyRestoresGuestAndReloginRestoresAccount()
    {
        const auto root = CaseRoot("signout-immediate-guest");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto firstAuthorized = SessionJson(
            "account-a-access-1",
            "account-a-refresh-1",
            "player-A",
            900u);
        firstAuthorized["status"] = "authorized";
        auto secondAuthorized = SessionJson(
            "account-a-access-2",
            "account-a-refresh-2",
            "player-A",
            900u);
        secondAuthorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "account-a-first" },
                        { "pollToken", "account-a-first-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/account-a-first"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, firstAuthorized),
                JsonResponse(204u),
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "account-a-second" },
                        { "pollToken", "account-a-second-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/account-a-second"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, secondAuthorized)
            },
            2u);
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        auto* const preferencesAddress = &preferences;
        auto* const savesAddress = &saves;
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Account A login did not complete.");
        const auto accountPreferencesPath = preferences.FilePath();
        const auto accountSaveDirectory = saves.Directory();
        preferences.SetString("account-only", "persisted-A");
        saves.SaveJson("account-slot", R"({"owner":"A"})");

        services->SignOut();
        // logout HTTPはまだblockedですが、公開bindingはこの呼出しから
        // 戻る前にguestへ復帰しなければなりません。
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("account-only").empty()
                && &preferences == preferencesAddress
                && &saves == savesAddress,
            "SignOut returned while account persistence was still exposed.");
        backend->WaitUntilRequestBlocked();
        backend->ReleaseBlockedRequest();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "Account A logout did not finish.");
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Account A second login did not complete.");
        Require(
            preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory
                && preferences.GetString("account-only") == "persisted-A"
                && saves.HasSlot("account-slot")
                && &preferences == preferencesAddress
                && &saves == savesAddress,
            "Account A persistence was not restored on re-login.");
    }

    void TestStoredRestoreActivatesAccountBeforePublishingSession()
    {
        const auto root = CaseRoot("stored-restore-order");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("restored-player");
        LamaPon::PlayerPrefs accountPreferences(account.playerPrefsFile);
        accountPreferences.Load();
        accountPreferences.SetString("owner", "restored-account");
        accountPreferences.Save();

        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    200u,
                    SessionJson(
                        "restored-access",
                        "rotated-refresh",
                        "restored-player",
                        900u))
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        bool observedAccountBeforeTokenSave{};
        bool observedSessionStillPrivate{};
        storeState->onSave = [&]
        {
            const auto* const coordinator =
                LamaPon::Detail::OnlinePersistenceAccess::Coordinator(
                    *services);
            observedAccountBeforeTokenSave = coordinator
                && coordinator->IsAccountActive()
                && preferences.FilePath() == account.playerPrefsFile
                && saves.Directory() == account.saveDataDirectory
                && preferences.GetString("owner") == "restored-account";
            observedSessionStillPrivate =
                services->State()
                    == LamaPon::OnlineAccountState::RestoringSession
                && !services->IsSignedIn()
                && services->Player().playerId.empty();
        };
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "stored-refresh";
        services->Configure(OnlineConfiguration());
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::RestoringSession
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && preferences.GetString("owner") == "guest",
            "Stored restore changed persistence before its response arrived.");

        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Stored session restore did not finish.");
        Require(
            observedAccountBeforeTokenSave
                && observedSessionStillPrivate
                && services->Player().playerId == "restored-player"
                && preferences.FilePath() == account.playerPrefsFile
                && preferences.GetString("owner") == "restored-account"
                && storeState->token == "rotated-refresh"
                && storeState->saveCount == 1u,
            "Stored restore published the session before account activation.");
    }

    void TestCorruptAccountRejectsStoredRestoreAndDeletesCredential()
    {
        const auto root = CaseRoot("stored-restore-corrupt-account");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        const auto account = profiles.Account("corrupt-restored-player");
        WriteText(account.playerPrefsFile, "{not-json");

        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    200u,
                    SessionJson(
                        "corrupt-restore-access",
                        "corrupt-restore-rotation",
                        "corrupt-restored-player",
                        900u)),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "stored-corrupt-profile-refresh";
        services->Configure(OnlineConfiguration());

        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Corrupt restored account cleanup did not finish.");
        const auto requests = backend->Requests();
        Require(
            !services->IsSignedIn()
                && services->Player().playerId.empty()
                && services->LastErrorCode()
                    == "persistence_activation_failed"
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 0u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 2u
                && requests.back().url.ends_with(
                    L"/v1/auth/session/logout")
                && HasBearer(
                    requests.back(),
                    L"corrupt-restore-access"),
            "Corrupt account restore exposed data or retained credentials.");
    }

    void TestRefreshCredentialSaveFailureRevokesRotatedSession()
    {
        const auto root = CaseRoot("refresh-save-rollback");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "initial-access",
            "initial-refresh",
            "rotation-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "rotation-transaction" },
                        { "pollToken", "rotation-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/rotation"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "rotated-access",
                        "rotated-refresh",
                        "rotation-player",
                        900u)),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Rotation fixture login did not complete.");

        storeState->failSave = true;
        services->Update(24.0f);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Rotated token Save failure cleanup did not finish.");
        const auto requests = backend->Requests();
        Require(
            !services->IsSignedIn()
                && services->LastErrorCode() == "credential_save_failed"
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 2u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 4u
                && HasBearer(requests.back(), L"rotated-access"),
            "Refresh token Save failure did not revoke the rotated session.");
    }

    void TestRefreshSignOutRaceKeepsImmediateGuestBinding()
    {
        const auto root = CaseRoot("refresh-signout-binding");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "race-old-access",
            "race-old-refresh",
            "race-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "race-transaction" },
                        { "pollToken", "race-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/race"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "race-new-access",
                        "race-new-refresh",
                        "race-player",
                        900u)),
                JsonResponse(204u)
            },
            2u);
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Refresh race fixture login did not complete.");
        preferences.SetString("race-account", "private");

        services->Update(24.0f);
        Require(
            services->State()
                == LamaPon::OnlineAccountState::RefreshingSession,
            "Proactive refresh was not started.");
        backend->WaitUntilRequestBlocked();
        services->SignOut();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("race-account").empty(),
            "Refresh-race SignOut retained the account binding.");
        backend->ReleaseBlockedRequest();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "Refresh-race logout did not finish.");
        const auto requests = backend->Requests();
        Require(
            storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 4u
                && HasBearer(requests.back(), L"race-new-access"),
            "Late refresh was published or its new session was not revoked.");
    }

    void TestTerminalRefreshFailuresDetachAccountPersistence()
    {
        const auto runCase = [](
            const std::string_view caseName,
            LamaPon::HttpResponse refreshResponse,
            const bool throwFromWorker,
            const bool crossExpiryWithCompletedWorker,
            const std::string_view expectedError,
            const bool expectCredentialDelete)
        {
            const auto root = CaseRoot(caseName);
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            LamaPon::SaveDataStore saves(root / L"Saves");

            auto authorized = SessionJson(
                "terminal-old-access",
                "terminal-old-refresh",
                "terminal-player");
            authorized["status"] = "authorized";
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            { "transactionId", "terminal-transaction" },
                            { "pollToken", "terminal-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/terminal"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    std::move(refreshResponse)
                },
                2u);
            auto storeState = std::make_shared<TokenStoreState>();
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend, throwFromWorker](
                        const LamaPon::HttpRequest& request)
                    {
                        auto response = backend->Send(request);
                        if (throwFromWorker
                            && request.url.ends_with(
                                L"/v1/auth/session/refresh"))
                        {
                            throw std::runtime_error(
                                "private refresh worker exception");
                        }
                        return response;
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            CompleteDiscordLogin(*services);
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::SignedIn;
                },
                "Terminal refresh fixture login did not complete.");
            const auto accountPath = preferences.FilePath();
            preferences.SetString("terminal-account", "durable-value");

            // 6秒残してrefreshを開始し、invalidは通常完了、transport/
            // exceptionは完了済みmailboxをreapするframeのelapsedで期限を
            // またがせます。どちらも同じframeでguestへ戻ります。
            services->Update(24.0f);
            backend->WaitUntilRequestBlocked();
            backend->ReleaseBlockedRequest();
            if (crossExpiryWithCompletedWorker)
            {
                const auto deadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::seconds(10);
                while (!LamaPon::Detail::OnlineServicesTestAccess::
                    CurrentTaskCompleted(*services))
                {
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        throw std::runtime_error(
                            "Terminal refresh worker did not complete.");
                    }
                    std::this_thread::yield();
                }
                services->Update(6.0f);
            }
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Terminal refresh failure did not finish.");
            Require(
                !services->IsSignedIn()
                    && services->Player().playerId.empty()
                    && services->LastErrorCode() == expectedError
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && preferences.GetString("terminal-account").empty()
                    && storeState->deleteCount
                        == (expectCredentialDelete ? 1u : 0u),
                "Terminal refresh failure retained the account binding.");
            LamaPon::PlayerPrefs persisted(accountPath);
            persisted.Load();
            Require(
                persisted.GetString("terminal-account")
                    == "durable-value",
                "Terminal refresh detach lost dirty account data.");
        };

        runCase(
            "terminal-invalid",
            JsonResponse(
                401u,
                {
                    {
                        "error",
                        { { "code", "invalid_refresh_token" } }
                    }
                }),
            false,
            false,
            "stored_session_invalid",
            true);
        LamaPon::HttpResponse transportFailure;
        transportFailure.transportError = "private terminal network error";
        runCase(
            "terminal-transport",
            std::move(transportFailure),
            false,
            true,
            "network_error",
            false);
        runCase(
            "terminal-exception",
            LamaPon::HttpResponse{},
            true,
            true,
            "request_failed",
            true);
    }

    void TestRefreshExpiryImmediatelyDetachesAndRevokesLateSession()
    {
        const auto runCase = [](const bool exactExpiryBoundary)
        {
            const auto root = CaseRoot(
                exactExpiryBoundary
                    ? "refresh-expiry-exact"
                    : "refresh-expiry-inflight");
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            LamaPon::SaveDataStore saves(root / L"Saves");

            auto authorized = SessionJson(
                "expiry-old-access",
                "expiry-old-refresh",
                "expiry-player");
            authorized["status"] = "authorized";
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            { "transactionId", "expiry-transaction" },
                            { "pollToken", "expiry-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/expiry"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    JsonResponse(
                        200u,
                        SessionJson(
                            "expiry-new-access",
                            "expiry-new-refresh",
                            "expiry-player",
                            900u)),
                    JsonResponse(204u)
                },
                2u);
            auto storeState = std::make_shared<TokenStoreState>();
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            CompleteDiscordLogin(*services);
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::SignedIn;
                },
                "Refresh-expiry fixture login did not complete.");
            const auto accountPath = preferences.FilePath();
            preferences.SetString("expiry-account", "durable-value");

            if (exactExpiryBoundary)
            {
                services->Update(30.0f);
            }
            else
            {
                services->Update(24.0f);
                backend->WaitUntilRequestBlocked();
                services->Update(6.0f);
            }
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && services->Player().playerId.empty()
                    && services->LastErrorCode() == "request_failed"
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && preferences.GetString("expiry-account").empty()
                    && storeState->deleteCount == 1u
                    && storeState->token.empty(),
                "Expired in-flight refresh remained publicly signed in.");
            if (exactExpiryBoundary)
            {
                backend->WaitUntilRequestBlocked();
            }
            backend->ReleaseBlockedRequest();
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Expired refresh cleanup did not finish.");

            const auto requests = backend->Requests();
            Require(
                requests.size() == 4u
                    && HasBearer(
                        requests.back(),
                        L"expiry-new-access")
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && !services->IsSignedIn(),
                "Late refresh success was published or not revoked.");
            LamaPon::PlayerPrefs persisted(accountPath);
            persisted.Load();
            Require(
                persisted.GetString("expiry-account")
                    == "durable-value",
                "Expiry detach lost dirty account PlayerPrefs.");
        };

        runCase(false);
        runCase(true);
    }

    void TestCompletedRefreshExpiresBeforeReap()
    {
        const auto root = CaseRoot("refresh-completion-expiry");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "completion-old-access",
            "completion-old-refresh",
            "completion-player",
            30u);
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "completion-transaction" },
                        { "pollToken", "completion-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/completion"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "completion-new-access",
                        "completion-new-refresh",
                        "completion-player",
                        1u)),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Completion-expiry fixture login did not complete.");
        preferences.SetString("completion-account", "durable-value");

        services->Update(24.0f);
        WaitUntilCurrentTaskCompleted(
            *services,
            "Completed refresh was not ready for TTL aging.");
        Require(
            LamaPon::Detail::OnlineServicesTestAccess::
                AgeCurrentTaskCompletion(*services, 31.0f),
            "Could not age the completed refresh result.");
        services->Update(0.0f);

        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && services->Player().playerId.empty()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("completion-account").empty()
                && storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty(),
            "An expired completed refresh was published or kept account persistence active.");
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Expired completed refresh cleanup did not finish.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 4u
                && HasBearer(
                    requests.back(),
                    L"completion-new-access")
                && services->LastErrorCode() == "request_failed",
            "Expired completed refresh was not revoked exactly once.");
    }

    void TestCompletedInitialSessionsExpireBeforeReap()
    {
        {
            const auto root = CaseRoot("restore-completion-expiry");
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            LamaPon::SaveDataStore saves(root / L"Saves");
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        200u,
                        SessionJson(
                            "expired-restore-access",
                            "expired-restore-candidate",
                            "expired-restore-player",
                            1u)),
                    JsonResponse(204u)
                });
            auto storeState = std::make_shared<TokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "expired-restore-stored";
            auto disabledConfiguration = OnlineConfiguration();
            disabledConfiguration.serviceBaseUrl.clear();
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    std::move(disabledConfiguration),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            services->Configure(OnlineConfiguration());

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
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && storeState->saveCount == 0u
                    && storeState->deleteCount == 1u
                    && storeState->token.empty(),
                "Expired completed restore exposed an account binding or credential.");
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Expired completed restore cleanup did not finish.");
            const auto requests = backend->Requests();
            Require(
                requests.size() == 2u
                    && HasBearer(
                        requests.back(),
                        L"expired-restore-access"),
                "Expired completed restore access token was not revoked.");
        }

        {
            const auto root = CaseRoot("poll-completion-expiry");
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            LamaPon::SaveDataStore saves(root / L"Saves");
            auto authorized = SessionJson(
                "expired-poll-access",
                "expired-poll-candidate",
                "expired-poll-player",
                1u);
            authorized["status"] = "authorized";
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            {
                                "transactionId",
                                "expired-poll-transaction"
                            },
                            { "pollToken", "expired-poll-secret" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/expired-poll"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    JsonResponse(204u)
                });
            auto storeState = std::make_shared<TokenStoreState>();
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            CompleteDiscordLogin(*services);
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
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && storeState->saveCount == 0u
                    && storeState->deleteCount == 0u,
                "Expired completed authorization exposed an account binding or credential.");
            UpdateUntil(
                *services,
                [&]
                {
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Expired completed authorization cleanup did not finish.");
            const auto requests = backend->Requests();
            Require(
                requests.size() == 3u
                    && HasBearer(
                        requests.back(),
                        L"expired-poll-access"),
                "Expired completed authorization access token was not revoked.");
        }
    }

    void TestDestructionDuringRefreshImmediatelyRestoresGuest()
    {
        const auto root = CaseRoot("destructor-refresh");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "destructor-old-access",
            "destructor-old-refresh",
            "destructor-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "destructor-transaction" },
                        { "pollToken", "destructor-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/destructor"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "destructor-new-access",
                        "destructor-new-refresh",
                        "destructor-player",
                        900u)),
                JsonResponse(204u)
            },
            2u);
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Destructor fixture login did not complete.");
        preferences.SetString("destructor-account", "private");
        services->Update(24.0f);
        backend->WaitUntilRequestBlocked();

        const auto started = std::chrono::steady_clock::now();
        services.reset();
        const auto elapsed =
            std::chrono::steady_clock::now() - started;
        Require(
            elapsed < std::chrono::seconds(1)
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("destructor-account").empty()
                && storeState->deleteCount == 1u
                && storeState->token.empty(),
            "OnlineServices destruction waited or retained account binding.");
        backend->ReleaseBlockedRequest();
        WaitUntilRequestCount(
            backend,
            4u,
            "Late refresh success was not revoked after destruction.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 4u
                && HasBearer(
                    requests.back(),
                    L"destructor-new-access")
                && storeState->deleteCount == 1u,
            "Destroyed refresh performed incomplete or duplicate cleanup.");
    }

    void TestCompletedUnreapedRefreshIsRevokedAfterDestruction()
    {
        auto authorized = SessionJson(
            "unreaped-old-access",
            "unreaped-old-refresh",
            "unreaped-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "unreaped-transaction" },
                        { "pollToken", "unreaped-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/unreaped"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "unreaped-new-access",
                        "unreaped-new-refresh",
                        "unreaped-player",
                        900u)),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Completed-unreaped fixture login did not complete.");
        services->Update(24.0f);
        Require(
            services->State()
                == LamaPon::OnlineAccountState::RefreshingSession,
            "Completed-unreaped refresh did not start.");

        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        while (!LamaPon::Detail::OnlineServicesTestAccess::
            CurrentTaskCompleted(*services))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "Refresh worker did not complete before destruction.");
            }
            std::this_thread::yield();
        }

        services.reset();
        Require(
            storeState->deleteCount == 1u
                && storeState->token.empty(),
            "Completed-unreaped refresh retained its old credential.");
        WaitUntilRequestCount(
            backend,
            4u,
            "Completed-unreaped refresh did not revoke its new access token.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 4u
                && HasBearer(requests.back(), L"unreaped-new-access")
                && storeState->deleteCount == 1u,
            "Completed-unreaped refresh cleanup ran incorrectly.");
    }

    void TestDestroyedRestoreDeletesCredentialAndRevokesLateSession()
    {
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    200u,
                    SessionJson(
                        "destroyed-restore-access",
                        "destroyed-restore-refresh",
                        "destroyed-restore-player",
                        900u)),
                JsonResponse(204u)
            },
            0u);
        auto storeState = std::make_shared<TokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "destroyed-restore-old-refresh";
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        backend->WaitUntilRequestBlocked();

        const auto started = std::chrono::steady_clock::now();
        services.reset();
        const auto elapsed =
            std::chrono::steady_clock::now() - started;
        Require(
            elapsed < std::chrono::seconds(1)
                && storeState->deleteCount == 1u
                && storeState->token.empty(),
            "Destroyed restore waited or retained its old credential.");
        backend->ReleaseBlockedRequest();
        WaitUntilRequestCount(
            backend,
            2u,
            "Destroyed restore did not revoke its late session.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 2u
                && HasBearer(
                    requests.back(),
                    L"destroyed-restore-access")
                && storeState->deleteCount == 1u,
            "Destroyed restore cleanup ran incorrectly.");
    }

    void TestDestroyedPollRevokesLateAuthorizedSession()
    {
        auto authorized = SessionJson(
            "destroyed-poll-access",
            "destroyed-poll-refresh",
            "destroyed-poll-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "destroyed-poll-transaction" },
                        { "pollToken", "destroyed-poll-secret" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/destroyed-poll"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u)
            },
            1u);
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        Require(
            services->BeginDiscordSignIn(),
            "Destroyed-poll login did not start.");
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Destroyed-poll login start did not complete.");
        services->Update(1.0f);
        backend->WaitUntilRequestBlocked();

        services.reset();
        Require(
            storeState->deleteCount == 0u,
            "Destroyed poll deleted an unrelated stored credential.");
        backend->ReleaseBlockedRequest();
        WaitUntilRequestCount(
            backend,
            3u,
            "Destroyed poll did not revoke its late authorized session.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 3u
                && HasBearer(
                    requests.back(),
                    L"destroyed-poll-access")
                && storeState->deleteCount == 0u,
            "Destroyed poll cleanup ran incorrectly.");
    }

    void TestSignedInDestructionPreservesStoredRefreshToken()
    {
        auto authorized = SessionJson(
            "preserved-access",
            "preserved-refresh",
            "preserved-player",
            900u);
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "preserved-transaction" },
                        { "pollToken", "preserved-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/preserved"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Preserved-credential fixture login did not complete.");
        services.reset();

        Require(
            storeState->saveCount == 1u
                && storeState->deleteCount == 0u
                && storeState->token == "preserved-refresh"
                && backend->Requests().size() == 2u,
            "Normal signed-in destruction discarded the resumable credential.");
    }

    void TestDestroyedSignOutRetriesFailedCredentialDeletion()
    {
        auto authorized = SessionJson(
            "retry-delete-old-access",
            "retry-delete-old-refresh",
            "retry-delete-player");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "retry-delete-transaction" },
                        { "pollToken", "retry-delete-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/retry-delete"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "retry-delete-new-access",
                        "retry-delete-new-refresh",
                        "retry-delete-player",
                        900u)),
                JsonResponse(204u)
            },
            2u);
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Delete-retry fixture login did not complete.");
        services->Update(24.0f);
        backend->WaitUntilRequestBlocked();

        storeState->failDelete = true;
        services->SignOut();
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && storeState->deleteCount == 1u
                && !storeState->token.empty(),
            "Delete-retry fixture did not retain the failed credential.");
        storeState->failDelete = false;
        services.reset();
        Require(
            storeState->deleteCount == 2u
                && storeState->token.empty(),
            "Destruction did not retry the failed SignOut credential delete.");

        backend->ReleaseBlockedRequest();
        WaitUntilRequestCount(
            backend,
            4u,
            "Destroyed SignOut did not revoke the late rotated session.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 4u
                && HasBearer(
                    requests.back(),
                    L"retry-delete-new-access")
                && storeState->deleteCount == 2u,
            "Destroyed SignOut duplicated or skipped abandoned cleanup.");
    }

    void TestCompletedSignOutKeepsFailedCredentialDeletePending()
    {
        auto authorized = SessionJson(
            "completed-signout-access",
            "completed-signout-refresh",
            "completed-signout-player",
            900u);
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        {
                            "transactionId",
                            "completed-signout-transaction"
                        },
                        { "pollToken", "completed-signout-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/completed-signout"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Completed-SignOut fixture login did not complete.");

        storeState->failDelete = true;
        services->SignOut();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "SignOut logout did not complete after credential failure.");
        Require(
            services->LastErrorCode() == "credential_delete_failed"
                && storeState->deleteCount == 1u
                && !storeState->token.empty(),
            "Completed SignOut forgot its failed credential deletion.");

        storeState->failDelete = false;
        services.reset();
        Require(
            storeState->deleteCount == 2u
                && storeState->token.empty(),
            "Destructor did not retry a completed SignOut deletion failure.");
    }

    void TestConfigureFailurePreservesClientNamespaceAndBinding()
    {
        const auto root = CaseRoot("configure-strong-guarantee");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "configure-access",
            "configure-refresh",
            "configure-player",
            900u);
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "configure-transaction" },
                        { "pollToken", "configure-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/configure"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u),
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "old-client-probe" },
                        { "pollToken", "old-client-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/old-client"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    })
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Configure fixture login did not complete.");
        preferences.SetString("durable", "before-lock");
        preferences.Save();
        preferences.SetString("quarantined", "memory");
        const auto accountPath = preferences.FilePath();
        FileHandle lock;
        lock.value = CreateFileW(
            accountPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            lock.value != INVALID_HANDLE_VALUE,
            "Could not lock Configure account fixture.");

        services->SignOut();
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "Configure fixture logout did not finish.");
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(
                *services);
        Require(
            coordinator && coordinator->HasQuarantinedAccount(),
            "Configure fixture did not retain quarantined memory.");
        const auto oldEpoch = coordinator->ProfileEpoch();

        auto replacement = OnlineConfiguration();
        replacement.serviceBaseUrl =
            "https://replacement.example.test/new-tenant";
        replacement.gameId = "replacement-game";
        replacement.environmentId = "replacement";
        Require(
            Throws([&]
            {
                services->Configure(std::move(replacement));
            })
                && coordinator->ProfileEpoch() == oldEpoch
                && coordinator->HasQuarantinedAccount()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && services->State()
                    == LamaPon::OnlineAccountState::SignedOut,
            "Failed Configure changed client state, namespace, or binding.");

        lock.Close();
        coordinator->EndFrame();
        Require(
            !coordinator->HasQuarantinedAccount(),
            "Configure fixture quarantine did not recover.");
        const LamaPon::PersistenceProfiles oldProfiles(
            root,
            "coordinator-game",
            "test");
        const LamaPon::PersistenceProfiles replacementProfiles(
            root,
            "replacement-game",
            "replacement");
        const auto expectedOld = oldProfiles.Account("namespace-probe");
        const auto unexpectedReplacement =
            replacementProfiles.Account("namespace-probe");
        auto prepared = coordinator->PrepareAccount(
            "namespace-probe",
            "namespace-probe-access-token");
        Require(
            coordinator->CommitPrepared(std::move(prepared))
                && preferences.FilePath() == expectedOld.playerPrefsFile
                && preferences.FilePath()
                    != unexpectedReplacement.playerPrefsFile,
            "Failed Configure replaced the persistence namespace.");
        static_cast<void>(coordinator->DetachToGuest());

        Require(
            services->BeginDiscordSignIn(),
            "Old client was unusable after failed Configure.");
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Old client probe did not complete.");
        const auto requests = backend->Requests();
        Require(
            requests.size() == 4u
                && requests.back().url.starts_with(
                    L"https://online.example.test/tenant/"),
            "Failed Configure replaced the active authentication client.");
        services->CancelDiscordSignIn();
    }

    void TestRefreshIdentityChangeFailsClosed()
    {
        const auto root = CaseRoot("refresh-identity-change");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "account-a-access",
            "account-a-refresh",
            "player-A");
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "identity-transaction" },
                        { "pollToken", "identity-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/identity"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "account-b-access",
                        "account-b-refresh",
                        "player-B")),
                JsonResponse(204u)
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Account A login did not complete.");
        Require(
            services->Player().playerId == "player-A"
                && preferences.FilePath() != root / L"PlayerPrefs.json",
            "Account A persistence was not activated.");

        services->Update(24.0f);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Cross-account refresh was not rejected.");

        const auto requests = backend->Requests();
        Require(
            !services->IsSignedIn()
                && services->Player().playerId.empty()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 4u
                && HasBearer(requests.back(), L"account-b-access"),
            "Cross-account refresh changed the active account profile.");
    }

    void TestCloudUnauthorizedRefreshUsesCooldownUntilWireSuccess()
    {
        const auto root = CaseRoot("cloud-401-cooldown");
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        LamaPon::SaveDataStore saves(root / L"Saves");

        auto authorized = SessionJson(
            "cloud-access-1",
            "cloud-refresh-1",
            "cloud-player",
            900u);
        authorized["status"] = "authorized";
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "cloud-transaction" },
                        { "pollToken", "cloud-poll" },
                        { "authorizationUrl", "https://login.example.test/cloud" },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(401u),
                JsonResponse(
                    200u,
                    SessionJson(
                        "cloud-access-2",
                        "cloud-refresh-2",
                        "cloud-player",
                        900u)),
                JsonResponse(401u),
                JsonResponse(
                    200u,
                    SessionJson(
                        "cloud-access-3",
                        "cloud-refresh-3",
                        "cloud-player",
                        900u)),
                EmptyCloudManifestResponse(),
                JsonResponse(401u),
                JsonResponse(
                    200u,
                    SessionJson(
                        "cloud-access-4",
                        "cloud-refresh-4",
                        "cloud-player",
                        900u))
            });
        auto storeState = std::make_shared<TokenStoreState>();
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        CompleteDiscordLogin(*services);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Cloud cooldown login did not complete.");
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(*services);
        Require(
            coordinator && coordinator->Synchronizer(),
            "Cloud synchronizer was not activated with the account.");

        const auto pumpUntilRequestCount = [&](const std::size_t count)
        {
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            while (backend->Requests().size() < count)
            {
                LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
                services->Update(0.0f);
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    throw std::runtime_error(
                        "Cloud cooldown request sequence timed out.");
                }
                std::this_thread::yield();
            }
        };
        const auto pumpUntilCloudState = [&](const auto expected)
        {
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            while (coordinator->Synchronizer()->Status().state != expected)
            {
                LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
                services->Update(0.0f);
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    throw std::runtime_error(
                        "Cloud cooldown state transition timed out.");
                }
                std::this_thread::yield();
            }
        };

        pumpUntilRequestCount(4u);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn
                    && backend->Requests().size() >= 4u;
            },
            "First cloud-triggered refresh did not complete.");
        pumpUntilRequestCount(5u);
        pumpUntilCloudState(
            LamaPon::Detail::CloudSaveSynchronizerState::Unauthorized);
        services->Update(0.0f);

        services->Update(4.9f);
        Require(
            backend->Requests().size() == 5u,
            "A repeated cloud 401 bypassed the minimum cooldown.");
        services->Update(0.2f);
        pumpUntilRequestCount(6u);
        UpdateUntil(
            *services,
            [&]
            {
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Cooled-down cloud refresh did not complete.");
        pumpUntilRequestCount(7u);
        pumpUntilCloudState(
            LamaPon::Detail::CloudSaveSynchronizerState::Idle);
        services->Update(0.0f);

        preferences.SetInteger("after-healthy-wire", 1);
        preferences.Save();
        pumpUntilRequestCount(8u);
        pumpUntilCloudState(
            LamaPon::Detail::CloudSaveSynchronizerState::Unauthorized);
        services->Update(0.0f);
        pumpUntilRequestCount(9u);
        const auto requests = backend->Requests();
        const auto refreshToken = [&requests](const std::size_t index)
        {
            return nlohmann::json::parse(std::string{
                requests[index].body.begin(),
                requests[index].body.end()
            }).value("refreshToken", std::string{});
        };
        Require(
            requests.size() == 9u
                && HasBearer(requests[2], L"cloud-access-1")
                && HasBearer(requests[4], L"cloud-access-2")
                && HasBearer(requests[6], L"cloud-access-3")
                && HasBearer(requests[7], L"cloud-access-3")
                && requests[3].url.ends_with(
                    L"/v1/auth/session/refresh")
                && requests[5].url.ends_with(
                    L"/v1/auth/session/refresh")
                && requests[8].url.ends_with(
                    L"/v1/auth/session/refresh")
                && refreshToken(3u) == "cloud-refresh-1"
                && refreshToken(5u) == "cloud-refresh-2"
                && refreshToken(8u) == "cloud-refresh-3",
            "Cloud 401 refresh fencing/cooldown did not follow token generations.");
    }
}

int main()
{
    try
    {
        ResetTestRoot();
        const auto run = [](const char* name, const auto test)
        {
            try
            {
                test();
            }
            catch (const std::exception& exception)
            {
                throw std::runtime_error(
                    std::string(name) + ": " + exception.what());
            }
        };
        run("editor draft", TestEditorDraftsAreInvalidatedAcrossProfileBindings);
        run("snapshot race", TestValidatedPlayerPrefsSnapshotClosesReopenRace);
        run("transactional switch", TestTransactionalProfileSwitchAndStableObjects);
        run("binding lease", TestActiveAccountOwnsBothPersistenceBindings);
        run("profile session lease", TestAccountProfileSessionLeaseRejectsSecondActivation);
        run("prepared binding", TestPreparedCommitRejectsChangedGuestBinding);
        run("stale transaction", TestStalePreparedTransactionCannotCommit);
        run("corrupt documents", TestCorruptAccountDocumentsFailClosed);
        run("save quarantine", TestFailedAccountSaveIsQuarantinedAndRetried);
        run("detach delete checkpoint", TestImmediateDetachCheckpointsBaselineLessDelete);
        run("checkpoint quarantine", TestFailedDetachCheckpointKeepsAccountLeaseUntilDiscard);
        run("checkpoint scan failure", TestUndeterminedDetachCheckpointRemainsFailClosed);
        run("clean load failure", TestCleanLoadFailureDoesNotPermanentlyBlockProfiles);
        run("dirty recovery", TestDirtyLoadFailureUsesRecoverableStrictSidecar);
        run("recovery discard", TestExplicitRecoveryDiscardPreservesOriginal);
        run("memory recovery discard", TestMemoryRecoveryDiscardPromotesExistingSidecar);
        run("corrupt recovery availability", TestEmptyAndOversizedRecoverySidecarsCanBeDiscardedSafely);
        run("recovery sidecar lease", TestRecoverySidecarResolutionReacquiresProfileLease);
        run("public recovery API", TestPublicRecoveryRevisionAndOperationGuards);
        run("public cloud conflict API", TestPublicCloudConflictRegistryAndResolution);
        run("public cloud request state", TestRequestCloudSyncReportsBusyAndTerminalStop);
        run("credential rollback", TestCredentialSaveFailureRollsBackAndRevokesSession);
        run("failed credential pending", TestFailedCredentialSaveKeepsPriorCommitPendingUntilDeleted);
        run("immediate signout", TestSignOutImmediatelyRestoresGuestAndReloginRestoresAccount);
        run("stored restore", TestStoredRestoreActivatesAccountBeforePublishingSession);
        run("corrupt restore", TestCorruptAccountRejectsStoredRestoreAndDeletesCredential);
        run("refresh save rollback", TestRefreshCredentialSaveFailureRevokesRotatedSession);
        run("refresh signout", TestRefreshSignOutRaceKeepsImmediateGuestBinding);
        run("terminal refresh", TestTerminalRefreshFailuresDetachAccountPersistence);
        run("refresh expiry", TestRefreshExpiryImmediatelyDetachesAndRevokesLateSession);
        run("completed refresh expiry", TestCompletedRefreshExpiresBeforeReap);
        run("completed initial expiry", TestCompletedInitialSessionsExpireBeforeReap);
        run("inflight destruction", TestDestructionDuringRefreshImmediatelyRestoresGuest);
        run("unreaped destruction", TestCompletedUnreapedRefreshIsRevokedAfterDestruction);
        run("restore destruction", TestDestroyedRestoreDeletesCredentialAndRevokesLateSession);
        run("poll destruction", TestDestroyedPollRevokesLateAuthorizedSession);
        run("signed-in destruction", TestSignedInDestructionPreservesStoredRefreshToken);
        run("destructor delete retry", TestDestroyedSignOutRetriesFailedCredentialDeletion);
        run("completed delete retry", TestCompletedSignOutKeepsFailedCredentialDeletePending);
        run("configure guarantee", TestConfigureFailurePreservesClientNamespaceAndBinding);
        run("refresh identity", TestRefreshIdentityChangeFailsClosed);
        run("cloud 401 cooldown", TestCloudUnauthorizedRefreshUsesCooldownUntilWireSuccess);
        std::cout << "Online persistence coordinator tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Online persistence coordinator tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
