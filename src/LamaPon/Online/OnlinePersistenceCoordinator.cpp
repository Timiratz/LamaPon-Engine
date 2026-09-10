#include "LamaPon/Online/OnlinePersistenceCoordinator.h"

#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Online/CloudSaveJournal.h"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
    using LamaPon::Detail::LocalPersistenceDocumentState;

    [[nodiscard]] bool IsReadableLocalState(
        const LocalPersistenceDocumentState state) noexcept
    {
        return state == LocalPersistenceDocumentState::Loaded
            || state == LocalPersistenceDocumentState::Missing;
    }

    [[noreturn]] void ThrowUnavailableAccountDocument()
    {
        // path、playerId、server由来本文を上位の公開errorへ渡しません。
        throw std::runtime_error(
            "An account persistence document is unavailable or corrupt.");
    }

    [[nodiscard]] std::filesystem::path MakeRecoverySidecarPath(
        const LamaPon::PersistenceProfilePaths& profile)
    {
        // Stage7Aの`.lock`/`.writing` suffix込みでも従来Win32 path上限を
        // 不必要に圧迫しない、account hash root直下の短い固定名です。
        return profile.rootDirectory / L"Recovery.prefs";
    }
}

namespace LamaPon::Detail
{
    struct PreparedOnlineAccount::State final
    {
        OnlinePersistenceCoordinator* owner{};
        std::uint64_t profileEpoch{};
        std::unique_ptr<PersistenceProfilePaths> profile;
        std::unique_ptr<PlayerPrefs> preferences;
        std::unique_ptr<CloudSaveJournal> journal;
        std::filesystem::path accountSaveDirectory;
        std::filesystem::path guestSaveDirectory;
        std::filesystem::path recoverySidecarPath;
    };

    PreparedOnlineAccount::PreparedOnlineAccount() noexcept = default;
    PreparedOnlineAccount::~PreparedOnlineAccount() = default;
    PreparedOnlineAccount::PreparedOnlineAccount(
        PreparedOnlineAccount&&) noexcept = default;
    PreparedOnlineAccount& PreparedOnlineAccount::operator=(
        PreparedOnlineAccount&&) noexcept = default;

    PreparedOnlineAccount::PreparedOnlineAccount(
        std::unique_ptr<State> state) noexcept
        : m_state(std::move(state))
    {
    }

    bool PreparedOnlineAccount::IsValid() const noexcept
    {
        return m_state != nullptr;
    }

    struct OnlinePersistenceCoordinator::Implementation final
    {
        Implementation(
            PlayerPrefs& activePreferences,
            SaveDataStore& activeSaves,
            std::filesystem::path userDataDirectory)
            : preferences(&activePreferences)
            , saves(&activeSaves)
            , trustedUserDataDirectory(std::move(userDataDirectory))
            , ownerThread(std::this_thread::get_id())
        {
            if (trustedUserDataDirectory.empty())
            {
                throw std::invalid_argument(
                    "Online persistence user data directory is required.");
            }

            const auto expectedPreferences =
                trustedUserDataDirectory / L"PlayerPrefs.json";
            const auto expectedSaves =
                trustedUserDataDirectory / L"Saves";
            if (preferences->FilePath() != expectedPreferences
                || saves->Directory() != expectedSaves
                || preferences->IsBindingLeased()
                || saves->IsBindingLeased())
            {
                throw std::invalid_argument(
                    "Online persistence must be attached to the guest profile.");
            }
        }

        ~Implementation()
        {
            static_cast<void>(DetachToGuest());
            // Application終了時にもquarantineを一度だけ再試行します。
            // 失敗が続く場合はmemory snapshotを破棄するほかありませんが、
            // Application側が破棄前に残留を検知して明示ログします。
            EndFrame();
            DetachObserver();
        }

        void DetachObserver() noexcept
        {
            if (observerToken != 0)
            {
                static_cast<void>(
                    DetachLocalPersistenceCommitObserver(observerToken));
                observerToken = 0;
            }
        }

        void RequireOwnerThread() const
        {
            if (std::this_thread::get_id() != ownerThread)
            {
                throw std::logic_error(
                    "Online persistence must be used from its owner thread.");
            }
        }

        [[nodiscard]] bool IsOwnerThread() const noexcept
        {
            return std::this_thread::get_id() == ownerThread;
        }

        void AdvanceEpoch() noexcept
        {
            ++profileEpoch;
            if (profileEpoch == 0)
            {
                ++profileEpoch;
            }
        }

        [[nodiscard]] bool AccountActive() const noexcept
        {
            return suspendedGuestPreferences != nullptr;
        }

        [[nodiscard]] bool GuestBindingIsCurrent() const noexcept
        {
            if (!profiles || AccountActive())
            {
                return false;
            }
            try
            {
                const auto guest = profiles->Guest();
                return !preferences->IsBindingLeased()
                    && !saves->IsBindingLeased()
                    && preferences->FilePath() == guest.playerPrefsFile
                    && saves->Directory() == guest.saveDataDirectory;
            }
            catch (...)
            {
                return false;
            }
        }

        void ConfigureNamespace(
            std::string gameId,
            std::string environmentId,
            std::string normalizedBackendBaseUrl,
            const bool insecureLoopback)
        {
            RequireOwnerThread();
            if (AccountActive())
            {
                throw std::logic_error(
                    "Sign out before changing the persistence namespace.");
            }
            if (quarantinedPreferences)
            {
                EndFrame();
                if (HasPendingRecovery())
                {
                    throw std::logic_error(
                        "Unsaved account persistence is quarantined.");
                }
            }
            else if (HasPendingRecovery())
            {
                throw std::logic_error(
                    "Account persistence recovery is pending.");
            }
            if (normalizedBackendBaseUrl.empty())
            {
                throw std::invalid_argument(
                    "Online persistence backend URL is required.");
            }

            // 以降のpublish前に、validationとallocationを完了します。
            auto replacementProfiles =
                std::make_unique<PersistenceProfiles>(
                    trustedUserDataDirectory,
                    gameId,
                    environmentId);
            auto replacementGameId = std::move(gameId);
            auto replacementEnvironmentId = std::move(environmentId);
            auto replacementBackendBaseUrl =
                std::move(normalizedBackendBaseUrl);

            profiles = std::move(replacementProfiles);
            configuredGameId = std::move(replacementGameId);
            configuredEnvironmentId =
                std::move(replacementEnvironmentId);
            backendBaseUrl = std::move(replacementBackendBaseUrl);
            allowInsecureLoopback = insecureLoopback;
            localCommitObserved = false;
            observerFailed = false;
            AdvanceEpoch();
        }

        void DisableNamespace() noexcept
        {
            if (!IsOwnerThread())
            {
                return;
            }
            static_cast<void>(DetachToGuest());
            profiles.reset();
            configuredGameId.clear();
            configuredEnvironmentId.clear();
            backendBaseUrl.clear();
            allowInsecureLoopback = false;
            localCommitObserved = false;
            observerFailed = false;
            AdvanceEpoch();
        }

        [[nodiscard]] std::unique_ptr<PreparedOnlineAccount::State>
            PrepareAccount(
            const std::string_view playerId)
        {
            RequireOwnerThread();
            if (!profiles)
            {
                throw std::logic_error(
                    "Online persistence namespace is not configured.");
            }
            if (AccountActive())
            {
                throw std::logic_error(
                    "An online account profile is already active.");
            }

            if (quarantinedPreferences)
            {
                EndFrame();
                if (HasPendingRecovery())
                {
                    throw std::runtime_error(
                        "Unsaved account persistence is quarantined.");
                }
            }
            else if (HasPendingRecovery())
            {
                throw std::runtime_error(
                    "Account persistence recovery is pending.");
            }
            if (!GuestBindingIsCurrent())
            {
                throw std::logic_error(
                    "Guest persistence is not active.");
            }
            if (preferences->HasLoadFailure())
            {
                ThrowUnavailableAccountDocument();
            }

            // accountへ切り替えている間にguest dirty memoryがprocess終了で
            // 消えないよう、切替より前にdurable化します。
            if (preferences->IsDirty())
            {
                preferences->Save();
            }

            auto profile = std::make_unique<PersistenceProfilePaths>(
                profiles->Account(playerId));
            auto recoveryPath = MakeRecoverySidecarPath(*profile);
            PlayerPrefs recoveryDocument(recoveryPath);
            const auto recovered =
                LocalPersistenceDocuments::ReadPlayerPrefs(
                    recoveryDocument);
            if (recovered.state
                != LocalPersistenceDocumentState::Missing)
            {
                // process再起動後もsidecarを無視してaccountを公開しません。
                // strict read結果を状態へ載せ、明示restore/discardだけを
                // 許可します。
                quarantinedProfile =
                    std::make_unique<PersistenceProfilePaths>(*profile);
                recoverySidecarPath = std::move(recoveryPath);
                recoverySidecarState = recovered.state;
                ThrowUnavailableAccountDocument();
            }
            auto accountPreferences =
                std::make_unique<PlayerPrefs>(profile->playerPrefsFile);

            const auto preferencesDocument =
                LocalPersistenceDocuments::ReadPlayerPrefs(
                    *accountPreferences);
            if (!IsReadableLocalState(preferencesDocument.state))
            {
                ThrowUnavailableAccountDocument();
            }
            LocalPersistenceDocuments::LoadPlayerPrefsSnapshot(
                *accountPreferences,
                preferencesDocument);

            SaveDataStore accountSaves(profile->saveDataDirectory);
            const auto slots =
                LocalPersistenceDocuments::ListSaveData(accountSaves);
            if (!IsReadableLocalState(slots.state))
            {
                ThrowUnavailableAccountDocument();
            }
            if (slots.state == LocalPersistenceDocumentState::Loaded)
            {
                for (const auto& slot : slots.slots)
                {
                    const auto document =
                        LocalPersistenceDocuments::ReadSaveData(
                            accountSaves,
                            slot);
                    // List後に消えたfileもraceとしてfail-closedです。
                    if (document.state
                        != LocalPersistenceDocumentState::Loaded)
                    {
                        ThrowUnavailableAccountDocument();
                    }
                }
            }

            auto preparedJournal = std::make_unique<CloudSaveJournal>(
                trustedUserDataDirectory,
                *profile,
                configuredGameId,
                configuredEnvironmentId,
                backendBaseUrl,
                allowInsecureLoopback);

            auto state = std::make_unique<PreparedOnlineAccount::State>();
            state->owner = owner;
            state->profileEpoch = profileEpoch;
            // noexcept commit用のpath複製はここで済ませます。
            state->accountSaveDirectory = profile->saveDataDirectory;
            state->guestSaveDirectory = saves->Directory();
            state->recoverySidecarPath =
                MakeRecoverySidecarPath(*profile);
            state->profile = std::move(profile);
            state->preferences = std::move(accountPreferences);
            state->journal = std::move(preparedJournal);
            return state;
        }

        [[nodiscard]] bool CommitPrepared(
            std::unique_ptr<PreparedOnlineAccount::State> state) noexcept
        {
            if (!IsOwnerThread()
                || !state
                || state->owner != owner
                || state->profileEpoch != profileEpoch
                || AccountActive()
                || HasPendingRecovery()
                || !profiles)
            {
                return false;
            }

            if (!state->profile
                || !state->preferences
                || !state->journal
                || preferences->IsBindingLeased()
                || state->preferences->IsBindingLeased()
                || saves->IsBindingLeased()
                || preferences->FilePath()
                    != state->guestSaveDirectory.parent_path()
                        / L"PlayerPrefs.json"
                || saves->Directory() != state->guestSaveDirectory)
            {
                return false;
            }

            if (!preferences->AcquireBindingLease(owner))
            {
                return false;
            }
            if (!state->preferences->AcquireBindingLease(owner))
            {
                static_cast<void>(
                    preferences->ReleaseBindingLease(owner));
                return false;
            }
            if (!saves->AcquireBindingLease(owner))
            {
                static_cast<void>(
                    state->preferences->ReleaseBindingLease(owner));
                static_cast<void>(
                    preferences->ReleaseBindingLease(owner));
                return false;
            }

            DetachObserver();
            activeProfile = std::move(state->profile);
            journal = std::move(state->journal);
            guestSaveDirectory = std::move(state->guestSaveDirectory);
            activeRecoverySidecarPath =
                std::move(state->recoverySidecarPath);

            LocalPersistenceDocuments::SwapPlayerPrefsLoadedState(
                *preferences,
                *state->preferences);
            saves->RelocateBinding(
                std::move(state->accountSaveDirectory),
                owner);
            suspendedGuestPreferences = std::move(state->preferences);

            localCommitObserved = false;
            observerFailed = false;
            AdvanceEpoch();
            observerToken = AttachLocalPersistenceCommitObserver(
                &Implementation::ObserveLocalCommit,
                this,
                profileEpoch);
            return true;
        }

        [[nodiscard]] OnlinePersistenceDetachResult
            DetachToGuest() noexcept
        {
            if (!IsOwnerThread() || !AccountActive())
            {
                return OnlinePersistenceDetachResult::AlreadyGuest;
            }

            bool saveFailed{};
            try
            {
                if (preferences->IsDirty())
                {
                    preferences->Save();
                }
            }
            catch (...)
            {
                saveFailed = true;
            }

            if (observerToken != 0)
            {
                observerFailed = observerFailed
                    || ConsumeLocalPersistenceObserverFailure();
            }
            DetachObserver();
            AdvanceEpoch();

            auto accountPreferences =
                std::move(suspendedGuestPreferences);
            LocalPersistenceDocuments::SwapPlayerPrefsLoadedState(
                *preferences,
                *accountPreferences);
            saves->RelocateBinding(
                std::move(guestSaveDirectory),
                owner);
            static_cast<void>(
                preferences->ReleaseBindingLease(owner));
            static_cast<void>(
                accountPreferences->ReleaseBindingLease(owner));
            static_cast<void>(
                saves->ReleaseBindingLease(owner));

            auto detachedProfile = std::move(activeProfile);
            journal.reset();
            auto detachedRecoveryPath =
                std::move(activeRecoverySidecarPath);
            localCommitObserved = false;
            observerFailed = false;

            // clean load failureには失われる未保存差分がありません。
            // diskは一切変更せずpimplを解放し、次回activationのstrict
            // readで修復済みかを改めて判定します。
            if (accountPreferences->HasLoadFailure()
                && !accountPreferences->IsDirty())
            {
                return OnlinePersistenceDetachResult::SavedAccount;
            }

            const bool needsQuarantine = saveFailed
                || accountPreferences->IsDirty();
            if (needsQuarantine)
            {
                // PrepareAccountは既存quarantine中のactivationを拒否するため、
                // ここで未保存accountを上書きすることはありません。
                quarantinedPreferences = std::move(accountPreferences);
                quarantinedProfile = std::move(detachedProfile);
                quarantinedLoadFailure =
                    quarantinedPreferences->HasLoadFailure();
                if (quarantinedLoadFailure)
                {
                    recoverySidecarPath =
                        std::move(detachedRecoveryPath);
                    static_cast<void>(PersistRecoverySidecar());
                }
                return OnlinePersistenceDetachResult::QuarantinedAccount;
            }

            return OnlinePersistenceDetachResult::SavedAccount;
        }

        void EndFrame() noexcept
        {
            if (!IsOwnerThread())
            {
                return;
            }
            if (observerToken != 0)
            {
                observerFailed = observerFailed
                    || ConsumeLocalPersistenceObserverFailure();
            }
            if (!quarantinedPreferences)
            {
                return;
            }
            if (quarantinedPreferences->HasLoadFailure())
            {
                if (!quarantinedPreferences->IsDirty())
                {
                    ClearPendingRecovery();
                    return;
                }
                static_cast<void>(PersistRecoverySidecar());
                return;
            }
            try
            {
                if (quarantinedPreferences->IsDirty())
                {
                    quarantinedPreferences->Save();
                }
                if (!quarantinedPreferences->IsDirty())
                {
                    ClearPendingRecovery();
                }
            }
            catch (...)
            {
                // memory snapshotを維持し、次のframeで再試行します。
            }
        }

        [[nodiscard]] bool PersistRecoverySidecar() noexcept
        {
            if (!quarantinedPreferences
                || !quarantinedProfile
                || !quarantinedLoadFailure
                || recoverySidecarPath.empty())
            {
                return false;
            }
            try
            {
                const auto snapshot =
                    quarantinedPreferences->SerializeToJson();
                ValidatePlayerPrefsFullDocument(snapshot);
                DurablePublishLocalDocument(
                    recoverySidecarPath,
                    snapshot);

                // publish後もStage7Aのsecure strict readerでfull bytesを
                // 再検証し、書いたsnapshotと一致する場合だけmemoryを
                // 解放します。
                PlayerPrefs verification(recoverySidecarPath);
                const auto document =
                    LocalPersistenceDocuments::ReadPlayerPrefs(
                        verification);
                if (document.state
                        != LocalPersistenceDocumentState::Loaded
                    || document.bytes.size() != snapshot.size()
                    || !std::equal(
                        document.bytes.begin(),
                        document.bytes.end(),
                        reinterpret_cast<const std::uint8_t*>(
                            snapshot.data())))
                {
                    return false;
                }
                recoverySidecarState =
                    LocalPersistenceDocumentState::Loaded;
                quarantinedPreferences.reset();
                quarantinedLoadFailure = false;
                return true;
            }
            catch (...)
            {
                // 元fileは保護したままmemory snapshotを維持します。
                return false;
            }
        }

        [[nodiscard]] bool HasPendingRecovery() const noexcept
        {
            return quarantinedPreferences != nullptr
                || quarantinedProfile != nullptr;
        }

        [[nodiscard]] OnlinePersistenceRecoveryState
            RecoveryState() const noexcept
        {
            if (quarantinedPreferences)
            {
                return OnlinePersistenceRecoveryState::MemorySnapshot;
            }
            if (!quarantinedProfile)
            {
                return OnlinePersistenceRecoveryState::None;
            }
            return recoverySidecarState
                    == LocalPersistenceDocumentState::Loaded
                ? OnlinePersistenceRecoveryState::DurableSidecar
                : OnlinePersistenceRecoveryState::UnavailableSidecar;
        }

        void ClearPendingRecovery() noexcept
        {
            quarantinedPreferences.reset();
            quarantinedProfile.reset();
            recoverySidecarPath.clear();
            recoverySidecarState =
                LocalPersistenceDocumentState::Missing;
            quarantinedLoadFailure = false;
        }

        [[nodiscard]] bool RestorePendingRecovery() noexcept
        {
            if (!IsOwnerThread())
            {
                return false;
            }
            if (quarantinedPreferences)
            {
                EndFrame();
            }
            if (quarantinedPreferences
                || !quarantinedProfile
                || recoverySidecarPath.empty())
            {
                return false;
            }
            try
            {
                PlayerPrefs sidecar(recoverySidecarPath);
                const auto document =
                    LocalPersistenceDocuments::ReadPlayerPrefs(sidecar);
                if (document.state
                    != LocalPersistenceDocumentState::Loaded)
                {
                    recoverySidecarState = document.state;
                    return false;
                }

                // 明示操作だけが元fileを更新します。適用が成功した後に
                // sidecarを削除し、削除失敗時は状態を残して再実行可能に
                // します。
                PlayerPrefs target(quarantinedProfile->playerPrefsFile);
                LocalPersistenceDocuments::ApplyPlayerPrefs(
                    target,
                    document.bytes);
                static_cast<void>(
                    DurableDeleteLocalDocument(recoverySidecarPath));
                ClearPendingRecovery();
                AdvanceEpoch();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] bool DiscardPendingRecovery() noexcept
        {
            if (!IsOwnerThread() || !HasPendingRecovery())
            {
                return false;
            }
            try
            {
                if (!recoverySidecarPath.empty())
                {
                    static_cast<void>(
                        DurableDeleteLocalDocument(
                            recoverySidecarPath));
                }
                ClearPendingRecovery();
                AdvanceEpoch();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] static bool ObserveLocalCommit(
            void* context,
            const std::uint64_t eventEpoch,
            const LocalPersistenceCommitEvent& event) noexcept
        {
            auto* implementation =
                static_cast<Implementation*>(context);
            if (!implementation)
            {
                return false;
            }
            return implementation->OnLocalCommit(eventEpoch, event);
        }

        [[nodiscard]] bool OnLocalCommit(
            const std::uint64_t eventEpoch,
            const LocalPersistenceCommitEvent& event) noexcept
        {
            if (!IsOwnerThread()
                || eventEpoch != profileEpoch
                || !AccountActive()
                || !activeProfile)
            {
                return false;
            }
            try
            {
                bool matches{};
                if (event.kind
                    == LocalPersistenceResourceKind::PlayerPrefs)
                {
                    matches = event.source == preferences
                        && event.filePath != nullptr
                        && event.slot.empty()
                        && *event.filePath
                            == activeProfile->playerPrefsFile;
                }
                else
                {
                    matches = event.source == saves
                        && event.filePath != nullptr
                        && !event.slot.empty()
                        && *event.filePath == saves->SlotPath(event.slot);
                }
                if (!matches)
                {
                    return false;
                }
                localCommitObserved = true;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        OnlinePersistenceCoordinator* owner{};
        PlayerPrefs* preferences{};
        SaveDataStore* saves{};
        std::filesystem::path trustedUserDataDirectory;
        std::thread::id ownerThread;
        std::unique_ptr<PersistenceProfiles> profiles;
        std::string configuredGameId;
        std::string configuredEnvironmentId;
        std::string backendBaseUrl;
        std::uint64_t profileEpoch{ 1 };
        LocalPersistenceObserverToken observerToken{};
        std::unique_ptr<PersistenceProfilePaths> activeProfile;
        std::unique_ptr<PlayerPrefs> suspendedGuestPreferences;
        std::filesystem::path guestSaveDirectory;
        std::filesystem::path activeRecoverySidecarPath;
        std::unique_ptr<CloudSaveJournal> journal;
        std::unique_ptr<PersistenceProfilePaths> quarantinedProfile;
        std::unique_ptr<PlayerPrefs> quarantinedPreferences;
        std::filesystem::path recoverySidecarPath;
        LocalPersistenceDocumentState recoverySidecarState{
            LocalPersistenceDocumentState::Missing
        };
        bool allowInsecureLoopback{};
        bool localCommitObserved{};
        bool observerFailed{};
        bool quarantinedLoadFailure{};
    };

    OnlinePersistenceCoordinator::OnlinePersistenceCoordinator(
        PlayerPrefs& preferences,
        SaveDataStore& saves,
        std::filesystem::path trustedUserDataDirectory)
        : m_implementation(std::make_unique<Implementation>(
            preferences,
            saves,
            std::move(trustedUserDataDirectory)))
    {
        m_implementation->owner = this;
    }

    OnlinePersistenceCoordinator::~OnlinePersistenceCoordinator() = default;

    void OnlinePersistenceCoordinator::ConfigureNamespace(
        std::string gameId,
        std::string environmentId,
        std::string normalizedBackendBaseUrl,
        const bool allowInsecureLoopback)
    {
        m_implementation->ConfigureNamespace(
            std::move(gameId),
            std::move(environmentId),
            std::move(normalizedBackendBaseUrl),
            allowInsecureLoopback);
    }

    void OnlinePersistenceCoordinator::DisableNamespace() noexcept
    {
        m_implementation->DisableNamespace();
    }

    bool OnlinePersistenceCoordinator::IsNamespaceEnabled() const noexcept
    {
        return m_implementation->profiles != nullptr;
    }

    bool OnlinePersistenceCoordinator::IsAccountActive() const noexcept
    {
        return m_implementation->AccountActive();
    }

    PreparedOnlineAccount OnlinePersistenceCoordinator::PrepareAccount(
        const std::string_view playerId)
    {
        return PreparedOnlineAccount(
            m_implementation->PrepareAccount(playerId));
    }

    bool OnlinePersistenceCoordinator::CommitPrepared(
        PreparedOnlineAccount&& prepared) noexcept
    {
        return m_implementation->CommitPrepared(
            std::move(prepared.m_state));
    }

    OnlinePersistenceDetachResult
        OnlinePersistenceCoordinator::DetachToGuest() noexcept
    {
        return m_implementation->DetachToGuest();
    }

    void OnlinePersistenceCoordinator::EndFrame() noexcept
    {
        m_implementation->EndFrame();
    }

    bool OnlinePersistenceCoordinator::HasQuarantinedAccount() const noexcept
    {
        return m_implementation->quarantinedPreferences != nullptr;
    }

    bool OnlinePersistenceCoordinator::HasPendingRecovery() const noexcept
    {
        return m_implementation->HasPendingRecovery();
    }

    OnlinePersistenceRecoveryState
        OnlinePersistenceCoordinator::RecoveryState() const noexcept
    {
        return m_implementation->RecoveryState();
    }

    const std::filesystem::path&
        OnlinePersistenceCoordinator::RecoverySidecarPath() const noexcept
    {
        return m_implementation->recoverySidecarPath;
    }

    bool OnlinePersistenceCoordinator::RestorePendingRecovery() noexcept
    {
        return m_implementation->RestorePendingRecovery();
    }

    bool OnlinePersistenceCoordinator::DiscardPendingRecovery() noexcept
    {
        return m_implementation->DiscardPendingRecovery();
    }

    std::uint64_t OnlinePersistenceCoordinator::ProfileEpoch() const noexcept
    {
        return m_implementation->profileEpoch;
    }

    std::string_view
        OnlinePersistenceCoordinator::ActiveAccountStorageKey() const noexcept
    {
        if (!m_implementation->activeProfile)
        {
            return {};
        }
        return m_implementation->activeProfile->accountStorageKey;
    }

    CloudSaveJournal* OnlinePersistenceCoordinator::Journal() noexcept
    {
        return m_implementation->journal.get();
    }

    bool OnlinePersistenceCoordinator::ConsumeLocalCommitSignal() noexcept
    {
        auto& implementation = *m_implementation;
        if (implementation.observerToken != 0)
        {
            implementation.observerFailed = implementation.observerFailed
                || ConsumeLocalPersistenceObserverFailure();
        }
        const bool signalled = implementation.localCommitObserved
            || implementation.observerFailed;
        implementation.localCommitObserved = false;
        implementation.observerFailed = false;
        return signalled;
    }
}
