#include "LamaPon/Online/OnlinePersistenceCoordinator.h"

#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Online/CloudSaveJournal.h"
#include "LamaPon/Online/CloudSaveSynchronizer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using LamaPon::Detail::LocalPersistenceDocument;
    using LamaPon::Detail::LocalPersistenceDocumentIdentity;
    using LamaPon::Detail::LocalPersistenceDocumentState;

    std::atomic<LamaPon::Detail::OnlinePersistenceRecoveryTestFailPoint>
        RecoveryTestFailPoint{};

    [[nodiscard]] bool IsReadableLocalState(
        const LocalPersistenceDocumentState state) noexcept
    {
        return state == LocalPersistenceDocumentState::Loaded
            || state == LocalPersistenceDocumentState::Missing;
    }

    [[nodiscard]] bool SameRecoveryObservation(
        const LocalPersistenceDocumentState leftState,
        const std::vector<std::uint8_t>& leftBytes,
        const LocalPersistenceDocumentIdentity& leftIdentity,
        const LocalPersistenceDocumentState rightState,
        const std::vector<std::uint8_t>& rightBytes,
        const LocalPersistenceDocumentIdentity& rightIdentity) noexcept
    {
        if (leftState != rightState)
        {
            return false;
        }
        if (leftState == LocalPersistenceDocumentState::Missing
            || leftState == LocalPersistenceDocumentState::Unavailable)
        {
            return true;
        }
        if (leftIdentity.completeBytes && rightIdentity.completeBytes)
        {
            return leftBytes == rightBytes;
        }
        return leftIdentity.valid
            && rightIdentity.valid
            && leftIdentity == rightIdentity;
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

    void EraseSecret(std::string& secret) noexcept
    {
        if (!secret.empty())
        {
            SecureZeroMemory(secret.data(), secret.size());
            secret.clear();
        }
    }

    struct ScopedSecretErase final
    {
        ~ScopedSecretErase()
        {
            EraseSecret(value);
        }
        std::string& value;
    };

    [[nodiscard]] std::uint64_t MonotonicMilliseconds() noexcept
    {
        const auto count = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return count > 0 ? static_cast<std::uint64_t>(count) : 0u;
    }
}

namespace LamaPon::Detail
{
    void SetOnlinePersistenceRecoveryTestFailPoint(
        const OnlinePersistenceRecoveryTestFailPoint failPoint) noexcept
    {
        RecoveryTestFailPoint.store(failPoint, std::memory_order_release);
    }

    struct PreparedOnlineAccount::State final
    {
        OnlinePersistenceCoordinator* owner{};
        std::uint64_t profileEpoch{};
        std::unique_ptr<PersistenceProfilePaths> profile;
        std::unique_ptr<PlayerPrefs> preferences;
        std::unique_ptr<CloudSaveJournal> journal;
        std::unique_ptr<CloudSaveProfileSessionLease> profileSessionLease;
        std::unique_ptr<PreparedCloudSaveAttachment> cloudAttachment;
        std::filesystem::path accountSaveDirectory;
        std::filesystem::path guestSaveDirectory;
        std::filesystem::path recoverySidecarPath;
        LocalPersistenceDocument recoverySidecarObservation;
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
            // detached workerはshared client/mailboxだけを保持します。journal
            // fieldより先にsynchronizer本体を破棄してraw bindingを残しません。
            synchronizer.reset();
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

        void AdvanceRecoveryRevision() noexcept
        {
            ++recoveryRevisionCounter;
            if (recoveryRevisionCounter == 0)
            {
                ++recoveryRevisionCounter;
            }
            recoveryRevision = recoveryRevisionCounter;
        }

        [[nodiscard]] bool SetLastRecoveryObservation(
            const LocalPersistenceDocumentState state,
            std::vector<std::uint8_t> bytes,
            const LocalPersistenceDocumentIdentity identity = {}) noexcept
        {
            if (SameRecoveryObservation(
                    lastRecoverySidecarState,
                    lastRecoverySidecarObservedBytes,
                    lastRecoverySidecarIdentity,
                    state,
                    bytes,
                    identity))
            {
                return false;
            }
            lastRecoverySidecarState = state;
            lastRecoverySidecarObservedBytes.swap(bytes);
            lastRecoverySidecarIdentity = identity;
            AdvanceRecoveryRevision();
            return true;
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
            const bool insecureLoopback,
            std::shared_ptr<const CloudSaveClient> cloudSaveClient)
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
            std::unique_ptr<CloudSaveSynchronizer> replacementSynchronizer;
            if (cloudSaveClient)
            {
                replacementSynchronizer =
                    std::make_unique<CloudSaveSynchronizer>(
                        *preferences,
                        *saves,
                        std::move(cloudSaveClient));
            }
            auto replacementGameId = std::move(gameId);
            auto replacementEnvironmentId = std::move(environmentId);
            auto replacementBackendBaseUrl =
                std::move(normalizedBackendBaseUrl);

            profiles = std::move(replacementProfiles);
            synchronizer = std::move(replacementSynchronizer);
            configuredGameId = std::move(replacementGameId);
            configuredEnvironmentId =
                std::move(replacementEnvironmentId);
            backendBaseUrl = std::move(replacementBackendBaseUrl);
            allowInsecureLoopback = insecureLoopback;
            localCommitObserved = false;
            observerFailed = false;
            cloudUnauthorizedPending = false;
            cloudUnauthorizedLatched = false;
            cloudHealthyPending = false;
            cloudAwaitingHealthyAfterTokenUpdate = false;
            AdvanceEpoch();
        }

        void DisableNamespace() noexcept
        {
            if (!IsOwnerThread())
            {
                return;
            }
            // recoveryのbinding材料を消すと明示restore/discardが不能になる
            // ため、解決前のnamespace無効化は何も変更しません。
            if (HasPendingRecovery())
            {
                return;
            }
            static_cast<void>(DetachToGuest());
            if (synchronizer)
            {
                synchronizer->Detach();
            }
            synchronizer.reset();
            profiles.reset();
            configuredGameId.clear();
            configuredEnvironmentId.clear();
            backendBaseUrl.clear();
            allowInsecureLoopback = false;
            localCommitObserved = false;
            observerFailed = false;
            cloudUnauthorizedPending = false;
            cloudUnauthorizedLatched = false;
            cloudHealthyPending = false;
            cloudAwaitingHealthyAfterTokenUpdate = false;
            AdvanceEpoch();
        }

        [[nodiscard]] std::unique_ptr<PreparedOnlineAccount::State>
            PrepareAccount(
            const std::string_view playerId,
            std::string accessToken)
        {
            const ScopedSecretErase eraseAccessToken{ accessToken };
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
            auto preparedJournal = std::make_unique<CloudSaveJournal>(
                trustedUserDataDirectory,
                *profile,
                configuredGameId,
                configuredEnvironmentId,
                backendBaseUrl,
                allowInsecureLoopback);
            auto profileSessionLease =
                std::make_unique<CloudSaveProfileSessionLease>(
                    *preparedJournal);
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
                auto pendingProfile =
                    std::make_unique<PersistenceProfilePaths>(*profile);
                auto observedBytes = recovered.bytes;
                auto lastObservedBytes = observedBytes;
                quarantinedProfile = std::move(pendingProfile);
                quarantinedJournal = std::move(preparedJournal);
                quarantinedProfileSessionLease =
                    std::move(profileSessionLease);
                recoverySidecarPath = std::move(recoveryPath);
                recoverySidecarState = recovered.state;
                recoverySidecarObservedBytes = std::move(observedBytes);
                recoverySidecarIdentity = recovered.identity;
                lastRecoverySidecarState = recoverySidecarState;
                lastRecoverySidecarObservedBytes =
                    std::move(lastObservedBytes);
                lastRecoverySidecarIdentity = recovered.identity;
                AdvanceRecoveryRevision();
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

            std::unique_ptr<PreparedCloudSaveAttachment> cloudAttachment;
            if (synchronizer)
            {
                cloudAttachment =
                    std::make_unique<PreparedCloudSaveAttachment>(
                        synchronizer->PrepareAttachment(
                            *preparedJournal,
                            profile->playerPrefsFile,
                            profile->saveDataDirectory,
                            profile->accountStorageKey,
                            std::move(accessToken)));
            }

            auto state = std::make_unique<PreparedOnlineAccount::State>();
            state->owner = owner;
            state->profileEpoch = profileEpoch;
            // noexcept commit用のpath複製はここで済ませます。
            state->accountSaveDirectory = profile->saveDataDirectory;
            state->guestSaveDirectory = saves->Directory();
            state->recoverySidecarPath =
                MakeRecoverySidecarPath(*profile);
            state->recoverySidecarObservation = std::move(recovered);
            state->profile = std::move(profile);
            state->preferences = std::move(accountPreferences);
            state->journal = std::move(preparedJournal);
            state->profileSessionLease =
                std::move(profileSessionLease);
            state->cloudAttachment = std::move(cloudAttachment);
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
                || !state->profileSessionLease
                || (synchronizer
                    && (!state->cloudAttachment
                        || !synchronizer->CanCommitPreparedAttachment(
                            *state->cloudAttachment)))
                || (!synchronizer && state->cloudAttachment)
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
            activeProfileSessionLease =
                std::move(state->profileSessionLease);
            guestSaveDirectory = std::move(state->guestSaveDirectory);
            activeRecoverySidecarPath =
                std::move(state->recoverySidecarPath);
            activeRecoverySidecarObservation =
                std::move(state->recoverySidecarObservation);

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
            if (synchronizer)
            {
                synchronizer->CommitPreparedAttachment(
                    std::move(*state->cloudAttachment),
                    profileEpoch);
            }
            cloudUnauthorizedPending = false;
            cloudUnauthorizedLatched = false;
            cloudHealthyPending = false;
            cloudAwaitingHealthyAfterTokenUpdate = false;
            nextPeriodicReconcileMilliseconds = 0u;
            observerToken = AttachLocalPersistenceCommitObserver(
                &Implementation::ObserveLocalCommit,
                this,
                profileEpoch,
                &Implementation::PrepareLocalDelete);
            return true;
        }

        [[nodiscard]] OnlinePersistenceDetachResult
            DetachToGuest() noexcept
        {
            if (!IsOwnerThread() || !AccountActive())
            {
                return OnlinePersistenceDetachResult::AlreadyGuest;
            }

            // 全操作は同じmain thread上なのでmailbox結果はTickまでlocalへ
            // 適用されません。dirty prefsを先にdurable化し、その直後にworker
            // fenceとdelete-intent checkpointを確定します。
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

            bool cloudCheckpointFailed{};
            CloudSaveDetachCheckpointRecovery cloudCheckpointRecovery;
            if (synchronizer)
            {
                try
                {
                    // networkを開始せず、baseline確立前を含むlocal deleteを
                    // journalへwrite-aheadしてからbindingをguestへ戻します。
                    synchronizer->CheckpointLocalStateForDetach();
                }
                catch (...)
                {
                    cloudCheckpointRecovery = synchronizer
                        ->TakeFailedDetachCheckpointRecovery();
                    // dirty PlayerPrefsのpublish自体が失敗した場合はmemory
                    // quarantineがaccount leaseを保持し、EndFrame Save成功後に
                    // 解決できます。deleteはcommit前WAL済みなので、同じdirty
                    // 状態をstrict scanできないことを別の永久checkpoint失敗へ
                    // 二重化しません。
                    cloudCheckpointFailed = !saveFailed;
                }
                synchronizer->Detach();
            }
            cloudUnauthorizedPending = false;
            cloudUnauthorizedLatched = false;
            cloudHealthyPending = false;
            cloudAwaitingHealthyAfterTokenUpdate = false;
            nextPeriodicReconcileMilliseconds = 0u;

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
            auto detachedJournal = std::move(journal);
            auto detachedProfileSessionLease =
                std::move(activeProfileSessionLease);
            auto detachedRecoveryPath =
                std::move(activeRecoverySidecarPath);
            auto detachedRecoveryObservation =
                std::move(activeRecoverySidecarObservation);
            localCommitObserved = false;
            observerFailed = false;

            // clean load failureには失われる未保存差分がありません。
            // diskは一切変更せずpimplを解放し、次回activationのstrict
            // readで修復済みかを改めて判定します。
            if (!cloudCheckpointFailed
                && accountPreferences->HasLoadFailure()
                && !accountPreferences->IsDirty())
            {
                return OnlinePersistenceDetachResult::SavedAccount;
            }

            const bool needsQuarantine = cloudCheckpointFailed
                || saveFailed
                || accountPreferences->IsDirty();
            if (needsQuarantine)
            {
                // PrepareAccountは既存quarantine中のactivationを拒否するため、
                // ここで未保存accountを上書きすることはありません。
                quarantinedPreferences = std::move(accountPreferences);
                quarantinedProfile = std::move(detachedProfile);
                quarantinedProfileSessionLease =
                    std::move(detachedProfileSessionLease);
                if (cloudCheckpointFailed)
                {
                    // checkpoint不能を「初期Missing」として再ログインさせず、
                    // 明示discardまでjournalとaccount leaseを保持します。
                    quarantinedJournal = std::move(detachedJournal);
                    quarantinedCloudCheckpointRecovery =
                        std::move(cloudCheckpointRecovery);
                    quarantinedCloudCheckpointFailure = true;
                }
                quarantinedLoadFailure =
                    quarantinedPreferences->HasLoadFailure();
                AdvanceRecoveryRevision();
                if (quarantinedLoadFailure)
                {
                    recoverySidecarPath =
                        std::move(detachedRecoveryPath);
                    recoverySidecarCreationObservation =
                        std::move(detachedRecoveryObservation);
                    recoverySidecarState =
                        recoverySidecarCreationObservation.state;
                    recoverySidecarObservedBytes.clear();
                    recoverySidecarIdentity =
                        recoverySidecarCreationObservation.identity;
                    lastRecoverySidecarState = recoverySidecarState;
                    lastRecoverySidecarObservedBytes.clear();
                    lastRecoverySidecarIdentity =
                        recoverySidecarIdentity;
                    recoverySidecarCreationBlocked = false;
                    recoverySidecarPublicationPending = false;
                    static_cast<void>(PersistRecoverySidecar());
                }
                return OnlinePersistenceDetachResult::QuarantinedAccount;
            }

            return OnlinePersistenceDetachResult::SavedAccount;
        }

        [[nodiscard]] bool RetryQuarantinedCloudCheckpoint() noexcept
        {
            if (!quarantinedCloudCheckpointFailure)
            {
                return true;
            }
            if (!quarantinedJournal
                || !quarantinedProfile
                || !quarantinedCloudCheckpointRecovery
                    .operationsDetermined)
            {
                // strict inventoryから操作列を確定できなかった
                // incidentは自動で解放しません。明示discardだけが
                // account leaseを放棄できます。
                return false;
            }
            try
            {
                // Persistが曖昧に失敗したjournal instanceはblockedになる
                // ため再利用しません。保持中のprofile session lease下で
                // diskからfresh instanceを構築し、batch全件成功後にだけ
                // quarantineのjournalを差し替えます。
                auto retryJournal = std::make_unique<CloudSaveJournal>(
                    trustedUserDataDirectory,
                    *quarantinedProfile,
                    configuredGameId,
                    configuredEnvironmentId,
                    backendBaseUrl,
                    allowInsecureLoopback);
                retryJournal->ApplyLocalDeleteIntentOperations(
                    quarantinedCloudCheckpointRecovery.operations);
                quarantinedJournal = std::move(retryJournal);
            }
            catch (...)
            {
                // batchは1generationでpublishされるため、失敗時は
                // 0件か全件です。曖昧成功も同じ列の再適用で
                // 冪等に回復します。
                return false;
            }
            quarantinedCloudCheckpointFailure = false;
            quarantinedCloudCheckpointRecovery = {};
            return true;
        }

        void EndFrame() noexcept
        {
            if (!IsOwnerThread())
            {
                return;
            }
            bool localCommitSignalled{};
            if (observerToken != 0)
            {
                observerFailed = observerFailed
                    || ConsumeLocalPersistenceObserverFailure();
                localCommitSignalled = localCommitObserved || observerFailed;
                localCommitObserved = false;
                observerFailed = false;
            }
            if (synchronizer && synchronizer->IsAttached())
            {
                const auto now = MonotonicMilliseconds();
                const bool periodic = now >= nextPeriodicReconcileMilliseconds;
                try
                {
                    if (localCommitSignalled || periodic)
                    {
                        synchronizer->RequestReconcile();
                        nextPeriodicReconcileMilliseconds =
                            now + 30'000u;
                    }
                    synchronizer->Tick(now);
                    if (synchronizer->Status().state
                            == CloudSaveSynchronizerState::Unauthorized
                        && !cloudUnauthorizedLatched)
                    {
                        cloudUnauthorizedLatched = true;
                        cloudUnauthorizedPending = true;
                    }
                    if (cloudAwaitingHealthyAfterTokenUpdate
                        && synchronizer
                            ->ConsumeAuthorizedWireSuccessSignal())
                    {
                        cloudAwaitingHealthyAfterTokenUpdate = false;
                        cloudHealthyPending = true;
                    }
                }
                catch (...)
                {
                    // local commit自体は既にdurableです。次frameのperiodic
                    // reconcileへ残し、例外本文やpathを公開しません。
                    nextPeriodicReconcileMilliseconds = now;
                }
            }
            if (!quarantinedPreferences)
            {
                return;
            }
            static_cast<void>(RetryQuarantinedCloudCheckpoint());
            if (quarantinedPreferences->HasLoadFailure())
            {
                if (!quarantinedPreferences->IsDirty())
                {
                    if (!quarantinedCloudCheckpointFailure)
                    {
                        ClearPendingRecovery();
                    }
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
                    if (!quarantinedCloudCheckpointFailure)
                    {
                        ClearPendingRecovery();
                    }
                }
            }
            catch (...)
            {
                // memory snapshotを維持し、次のframeで再試行します。
            }
        }

        void UpdateCloudSaveAccessToken(std::string accessToken)
        {
            const ScopedSecretErase eraseAccessToken{ accessToken };
            RequireOwnerThread();
            if (!synchronizer || !AccountActive()
                || !synchronizer->IsAttached())
            {
                throw std::logic_error(
                    "Cloud save synchronization is not active.");
            }
            synchronizer->UpdateAccessToken(std::move(accessToken));
            cloudUnauthorizedPending = false;
            cloudUnauthorizedLatched = false;
            cloudHealthyPending = false;
            cloudAwaitingHealthyAfterTokenUpdate = true;
            nextPeriodicReconcileMilliseconds = 0u;
        }

        [[nodiscard]] bool ConsumeCloudSaveUnauthorizedSignal() noexcept
        {
            if (!IsOwnerThread())
            {
                return false;
            }
            return std::exchange(cloudUnauthorizedPending, false);
        }

        [[nodiscard]] bool ConsumeCloudSaveHealthySignal() noexcept
        {
            if (!IsOwnerThread())
            {
                return false;
            }
            return std::exchange(cloudHealthyPending, false);
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
            std::string snapshot;
            try
            {
                snapshot = quarantinedPreferences->SerializeToJson();
                ValidatePlayerPrefsFullDocument(snapshot);
                const auto publishResult =
                    DurablePublishLocalDocumentIfUnchanged(
                        recoverySidecarPath,
                        recoverySidecarCreationObservation,
                        snapshot,
                        CloudPreferencesMaxBytes);
                if (publishResult
                    == LocalPersistenceConditionalApplyResult::Applied)
                {
                    // publish後のstrict再読だけが一時失敗した場合に限り、
                    // 次回同一bytesを自分の曖昧成功として採用できます。
                    recoverySidecarPublicationPending = true;
                    if (RecoveryTestFailPoint.exchange(
                            OnlinePersistenceRecoveryTestFailPoint::None,
                            std::memory_order_acq_rel)
                        == OnlinePersistenceRecoveryTestFailPoint::
                            AfterSidecarPublishBeforeVerification)
                    {
                        throw std::runtime_error(
                            "Recovery verification test failure.");
                    }
                }
                if (publishResult
                        == LocalPersistenceConditionalApplyResult::LocalChanged
                    && !recoverySidecarPublicationPending)
                {
                    // activation時のMissing観測後に別fileが現れた場合は、
                    // 内容が偶然同じでも所有物とはみなさずmemoryを保持します。
                    recoverySidecarCreationBlocked = true;
                    PlayerPrefs currentSidecar(recoverySidecarPath);
                    auto current = LocalPersistenceDocuments::ReadPlayerPrefs(
                        currentSidecar);
                    static_cast<void>(SetLastRecoveryObservation(
                        current.state,
                        std::move(current.bytes),
                        current.identity));
                    return false;
                }

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
                    recoverySidecarCreationBlocked =
                        !SameRecoveryObservation(
                            recoverySidecarCreationObservation.state,
                            recoverySidecarCreationObservation.bytes,
                            recoverySidecarCreationObservation.identity,
                            document.state,
                            document.bytes,
                            document.identity);
                    static_cast<void>(SetLastRecoveryObservation(
                        document.state,
                        std::move(document.bytes),
                        document.identity));
                    return false;
                }
                auto expectedBytes = document.bytes;
                auto observedBytes = document.bytes;
                recoverySidecarState =
                    LocalPersistenceDocumentState::Loaded;
                recoverySidecarObservedBytes.swap(expectedBytes);
                recoverySidecarIdentity = document.identity;
                lastRecoverySidecarState =
                    LocalPersistenceDocumentState::Loaded;
                lastRecoverySidecarObservedBytes.swap(observedBytes);
                lastRecoverySidecarIdentity = document.identity;
                recoverySidecarCreationBlocked = false;
                recoverySidecarPublicationPending = false;
                quarantinedPreferences.reset();
                // strictに再読できるdurable sidecarへsnapshotを退避した時点で、
                // 元account fileを旧memoryから自動更新する経路はなくなります。
                if (!quarantinedCloudCheckpointFailure)
                {
                    quarantinedProfileSessionLease.reset();
                }
                quarantinedLoadFailure = false;
                // expected bytesを固定したままmemory snapshotからdurable
                // sidecarへ状態が変わりました。
                AdvanceRecoveryRevision();
                return true;
            }
            catch (...)
            {
                // 元fileは保護したままmemory snapshotを維持し、sidecarの
                // 現在状態だけをbounded strict readで更新します。
                try
                {
                    PlayerPrefs currentSidecar(recoverySidecarPath);
                    auto current = LocalPersistenceDocuments::ReadPlayerPrefs(
                        currentSidecar);
                    const bool pendingSnapshotMatches =
                        recoverySidecarPublicationPending
                        && current.state
                            == LocalPersistenceDocumentState::Loaded
                        && current.bytes.size() == snapshot.size()
                        && std::equal(
                            current.bytes.begin(),
                            current.bytes.end(),
                            reinterpret_cast<const std::uint8_t*>(
                                snapshot.data()));
                    recoverySidecarCreationBlocked =
                        !pendingSnapshotMatches
                        && !SameRecoveryObservation(
                            recoverySidecarCreationObservation.state,
                            recoverySidecarCreationObservation.bytes,
                            recoverySidecarCreationObservation.identity,
                            current.state,
                            current.bytes,
                            current.identity);
                    static_cast<void>(SetLastRecoveryObservation(
                        current.state,
                        std::move(current.bytes),
                        current.identity));
                }
                catch (...)
                {
                    recoverySidecarCreationBlocked = true;
                    static_cast<void>(SetLastRecoveryObservation(
                        LocalPersistenceDocumentState::Unavailable,
                        {}));
                }
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
                return (quarantinedCloudCheckpointFailure
                            && !quarantinedCloudCheckpointRecovery
                                .operationsDetermined)
                        || recoverySidecarCreationBlocked
                    ? OnlinePersistenceRecoveryState::UnavailableSidecar
                    : OnlinePersistenceRecoveryState::MemorySnapshot;
            }
            if (!quarantinedProfile)
            {
                return OnlinePersistenceRecoveryState::None;
            }
            return recoverySidecarState
                        == LocalPersistenceDocumentState::Loaded
                    && SameRecoveryObservation(
                        recoverySidecarState,
                        recoverySidecarObservedBytes,
                        recoverySidecarIdentity,
                        lastRecoverySidecarState,
                        lastRecoverySidecarObservedBytes,
                        lastRecoverySidecarIdentity)
                ? OnlinePersistenceRecoveryState::DurableSidecar
                : OnlinePersistenceRecoveryState::UnavailableSidecar;
        }

        [[nodiscard]] OnlinePersistenceRecoverySnapshot
            RecoveryStatus() noexcept
        {
            if (!IsOwnerThread())
            {
                return {
                    OnlinePersistenceRecoveryState::UnavailableSidecar,
                    recoveryRevision
                };
            }
            if (!HasPendingRecovery())
            {
                recoveryRevision = 0;
                return {};
            }
            if (quarantinedPreferences)
            {
                if (quarantinedLoadFailure
                    && !recoverySidecarPath.empty())
                {
                    try
                    {
                        PlayerPrefs sidecar(recoverySidecarPath);
                        auto document =
                            LocalPersistenceDocuments::ReadPlayerPrefs(
                                sidecar);
                        bool pendingSnapshotMatches{};
                        if (recoverySidecarPublicationPending
                            && document.state
                                == LocalPersistenceDocumentState::Loaded)
                        {
                            const auto snapshot =
                                quarantinedPreferences->SerializeToJson();
                            pendingSnapshotMatches =
                                document.bytes.size() == snapshot.size()
                                && std::equal(
                                    document.bytes.begin(),
                                    document.bytes.end(),
                                    reinterpret_cast<const std::uint8_t*>(
                                        snapshot.data()));
                        }
                        recoverySidecarCreationBlocked =
                            !pendingSnapshotMatches
                            && !SameRecoveryObservation(
                                recoverySidecarCreationObservation.state,
                                recoverySidecarCreationObservation.bytes,
                                recoverySidecarCreationObservation.identity,
                                document.state,
                                document.bytes,
                                document.identity);
                        static_cast<void>(SetLastRecoveryObservation(
                            document.state,
                            std::move(document.bytes),
                            document.identity));
                    }
                    catch (...)
                    {
                        recoverySidecarCreationBlocked = true;
                        static_cast<void>(SetLastRecoveryObservation(
                            LocalPersistenceDocumentState::Unavailable,
                            {}));
                    }
                }
                return {
                    RecoveryState(),
                    recoveryRevision
                };
            }

            try
            {
                if (recoverySidecarPath.empty())
                {
                    static_cast<void>(SetLastRecoveryObservation(
                        LocalPersistenceDocumentState::Unavailable,
                        {}));
                }
                else
                {
                    PlayerPrefs sidecar(recoverySidecarPath);
                    auto document =
                        LocalPersistenceDocuments::ReadPlayerPrefs(sidecar);
                    static_cast<void>(SetLastRecoveryObservation(
                        document.state,
                        std::move(document.bytes),
                        document.identity));
                }
            }
            catch (...)
            {
                static_cast<void>(SetLastRecoveryObservation(
                    LocalPersistenceDocumentState::Unavailable,
                    {}));
            }
            return { RecoveryState(), recoveryRevision };
        }

        void ClearPendingRecovery() noexcept
        {
            quarantinedPreferences.reset();
            quarantinedJournal.reset();
            quarantinedCloudCheckpointRecovery = {};
            quarantinedProfileSessionLease.reset();
            quarantinedProfile.reset();
            recoverySidecarPath.clear();
            recoverySidecarCreationObservation = {};
            recoverySidecarObservedBytes.clear();
            recoverySidecarIdentity = {};
            recoverySidecarState =
                LocalPersistenceDocumentState::Missing;
            lastRecoverySidecarObservedBytes.clear();
            lastRecoverySidecarIdentity = {};
            lastRecoverySidecarState =
                LocalPersistenceDocumentState::Missing;
            recoveryRevision = 0;
            recoverySidecarCreationBlocked = false;
            recoverySidecarPublicationPending = false;
            quarantinedLoadFailure = false;
            quarantinedCloudCheckpointFailure = false;
        }

        [[nodiscard]] OnlinePersistenceRecoveryOperationResult
            RestorePendingRecovery(
            const std::uint64_t expectedRevision) noexcept
        {
            if (!IsOwnerThread())
            {
                return OnlinePersistenceRecoveryOperationResult::Failed;
            }
            if (expectedRevision == 0
                || expectedRevision != recoveryRevision)
            {
                return OnlinePersistenceRecoveryOperationResult::Stale;
            }
            if (quarantinedPreferences)
            {
                EndFrame();
                // この明示Restoreがmemory Save/checkpoint再適用を完了し
                // quarantineを解放した場合は、Clearによるrevision=0を
                // staleとせずこの操作の成功として返します。
                if (!HasPendingRecovery())
                {
                    return OnlinePersistenceRecoveryOperationResult::Succeeded;
                }
            }
            if (expectedRevision != recoveryRevision)
            {
                return OnlinePersistenceRecoveryOperationResult::Stale;
            }
            if (quarantinedPreferences
                || !quarantinedProfile
                || recoverySidecarPath.empty())
            {
                return HasPendingRecovery()
                    ? OnlinePersistenceRecoveryOperationResult::Failed
                    : OnlinePersistenceRecoveryOperationResult::Unavailable;
            }
            try
            {
                // Prepareでsidecarを検出したleaseはthrowと共に解放されます。
                // 明示解決時に同じaccount leaseを再取得し、別Coordinatorが
                // accountを公開中ならcached pathから書換えません。
                std::unique_ptr<CloudSaveJournal> recoveryJournal;
                std::unique_ptr<CloudSaveProfileSessionLease> recoveryLease;
                if (!quarantinedProfileSessionLease)
                {
                    recoveryJournal = std::make_unique<CloudSaveJournal>(
                        trustedUserDataDirectory,
                        *quarantinedProfile,
                        configuredGameId,
                        configuredEnvironmentId,
                        backendBaseUrl,
                        allowInsecureLoopback);
                    recoveryLease =
                        std::make_unique<CloudSaveProfileSessionLease>(
                            *recoveryJournal);
                }
                PlayerPrefs sidecar(recoverySidecarPath);
                const auto document =
                    LocalPersistenceDocuments::ReadPlayerPrefs(sidecar);
                if (document.state
                        != LocalPersistenceDocumentState::Loaded
                    || recoverySidecarState
                        != LocalPersistenceDocumentState::Loaded
                    || !SameRecoveryObservation(
                        recoverySidecarState,
                        recoverySidecarObservedBytes,
                        recoverySidecarIdentity,
                        document.state,
                        document.bytes,
                        document.identity))
                {
                    const bool changed = SetLastRecoveryObservation(
                        document.state,
                        std::move(document.bytes),
                        document.identity);
                    return changed
                        ? OnlinePersistenceRecoveryOperationResult::Stale
                        : OnlinePersistenceRecoveryOperationResult::Failed;
                }

                // 明示操作だけが元fileを更新します。適用が成功した後に
                // sidecarを削除し、削除失敗時は状態を残して再実行可能に
                // します。
                PlayerPrefs target(quarantinedProfile->playerPrefsFile);
                LocalPersistenceDocuments::ApplyPlayerPrefs(
                    target,
                    document.bytes);
                if (DurableDeleteLocalDocumentIfUnchanged(
                        recoverySidecarPath,
                        document,
                        CloudPreferencesMaxBytes)
                    != LocalPersistenceConditionalApplyResult::Applied)
                {
                    static_cast<void>(RecoveryStatus());
                    return recoveryRevision != expectedRevision
                        ? OnlinePersistenceRecoveryOperationResult::Stale
                        : OnlinePersistenceRecoveryOperationResult::Failed;
                }
                ClearPendingRecovery();
                AdvanceEpoch();
                return OnlinePersistenceRecoveryOperationResult::Succeeded;
            }
            catch (const CloudSaveJournalBusyError&)
            {
                return OnlinePersistenceRecoveryOperationResult::Busy;
            }
            catch (...)
            {
                return OnlinePersistenceRecoveryOperationResult::Failed;
            }
        }

        [[nodiscard]] OnlinePersistenceRecoveryOperationResult
            DiscardPendingRecovery(
            const std::uint64_t expectedRevision) noexcept
        {
            if (!IsOwnerThread())
            {
                return OnlinePersistenceRecoveryOperationResult::Failed;
            }
            if (expectedRevision == 0
                || expectedRevision != recoveryRevision)
            {
                return OnlinePersistenceRecoveryOperationResult::Stale;
            }
            if (!HasPendingRecovery())
            {
                return OnlinePersistenceRecoveryOperationResult::Unavailable;
            }
            try
            {
                std::unique_ptr<CloudSaveJournal> recoveryJournal;
                std::unique_ptr<CloudSaveProfileSessionLease> recoveryLease;
                if (!quarantinedProfileSessionLease)
                {
                    recoveryJournal = std::make_unique<CloudSaveJournal>(
                        trustedUserDataDirectory,
                        *quarantinedProfile,
                        configuredGameId,
                        configuredEnvironmentId,
                        backendBaseUrl,
                        allowInsecureLoopback);
                    recoveryLease =
                        std::make_unique<CloudSaveProfileSessionLease>(
                            *recoveryJournal);
                }
                if (!recoverySidecarPath.empty())
                {
                    PlayerPrefs sidecar(recoverySidecarPath);
                    auto document =
                        LocalPersistenceDocuments::ReadPlayerPrefs(sidecar);
                    const bool matchesLatestObservation =
                        SameRecoveryObservation(
                            lastRecoverySidecarState,
                            lastRecoverySidecarObservedBytes,
                            lastRecoverySidecarIdentity,
                            document.state,
                            document.bytes,
                            document.identity);
                    if (!matchesLatestObservation)
                    {
                        const bool changed = SetLastRecoveryObservation(
                            document.state,
                            std::move(document.bytes),
                            document.identity);
                        return changed || recoveryRevision != expectedRevision
                            ? OnlinePersistenceRecoveryOperationResult::Stale
                            : OnlinePersistenceRecoveryOperationResult::Failed;
                    }

                    if (document.state
                        == LocalPersistenceDocumentState::Missing)
                    {
                        // 別process等が既にsidecarを削除済みなら、最新revision
                        // での明示discardは冪等にquarantineだけを解放します。
                    }
                    else if (document.state
                            == LocalPersistenceDocumentState::Loaded
                        || document.state
                            == LocalPersistenceDocumentState::Corrupt)
                    {
                        if (quarantinedPreferences)
                        {
                            // memory snapshotの保護中にsidecarが現れた場合、
                            // 最初のDiscardはmemoryだけを破棄し、外部fileを
                            // 消さず新しいrecovery incidentとして昇格します。
                            // publicationPendingの自分のsnapshotも同じ二段階
                            // 解決とし、曖昧成功時に誤削除しません。
                            quarantinedPreferences.reset();
                            quarantinedLoadFailure = false;
                            recoverySidecarState = document.state;
                            recoverySidecarObservedBytes =
                                std::move(document.bytes);
                            recoverySidecarIdentity = document.identity;
                            recoverySidecarCreationObservation = {};
                            recoverySidecarCreationBlocked = false;
                            recoverySidecarPublicationPending = false;
                            AdvanceRecoveryRevision();
                            return OnlinePersistenceRecoveryOperationResult::Succeeded;
                        }

                        // durable incidentの外部置換も、最新revisionが現在の
                        // documentを観測した後の明示Discardなら、その同一
                        // handleだけを条件付き削除します。Restoreは依然として
                        // original snapshotと一致しない置換を採用しません。
                        // emptyを含む完全raw bytes、またはoversizeの固定size
                        // file identityが一致する対象だけを同一handleで削除します。
                        if (DurableDeleteLocalDocumentIfUnchanged(
                                recoverySidecarPath,
                                document,
                                CloudPreferencesMaxBytes)
                            != LocalPersistenceConditionalApplyResult::Applied)
                        {
                            static_cast<void>(RecoveryStatus());
                            return recoveryRevision != expectedRevision
                                ? OnlinePersistenceRecoveryOperationResult::Stale
                                : OnlinePersistenceRecoveryOperationResult::Failed;
                        }
                    }
                    else
                    {
                        return OnlinePersistenceRecoveryOperationResult::Failed;
                    }
                }
                ClearPendingRecovery();
                AdvanceEpoch();
                return OnlinePersistenceRecoveryOperationResult::Succeeded;
            }
            catch (const CloudSaveJournalBusyError&)
            {
                return OnlinePersistenceRecoveryOperationResult::Busy;
            }
            catch (...)
            {
                return OnlinePersistenceRecoveryOperationResult::Failed;
            }
        }

        [[nodiscard]] bool RestorePendingRecovery() noexcept
        {
            const auto status = RecoveryStatus();
            return status.revision != 0
                && RestorePendingRecovery(status.revision)
                    == OnlinePersistenceRecoveryOperationResult::Succeeded;
        }

        [[nodiscard]] bool DiscardPendingRecovery() noexcept
        {
            const auto status = RecoveryStatus();
            return status.revision != 0
                && DiscardPendingRecovery(status.revision)
                    == OnlinePersistenceRecoveryOperationResult::Succeeded;
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

        [[nodiscard]] static bool PrepareLocalDelete(
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
            return implementation->OnLocalPreDelete(eventEpoch, event);
        }

        [[nodiscard]] bool OnLocalPreDelete(
            const std::uint64_t eventEpoch,
            const LocalPersistenceCommitEvent& event) noexcept
        {
            if (!IsOwnerThread()
                || eventEpoch != profileEpoch
                || !AccountActive()
                || !activeProfile
                || !event.deleted)
            {
                return false;
            }
            try
            {
                CloudSaveResource resource;
                if (event.kind
                    == LocalPersistenceResourceKind::PlayerPrefs)
                {
                    if (event.source != preferences
                        || event.filePath == nullptr
                        || !event.slot.empty()
                        || *event.filePath
                            != activeProfile->playerPrefsFile)
                    {
                        return false;
                    }
                    resource = CloudSaveResource::Preferences();
                }
                else
                {
                    if (event.source != saves
                        || event.filePath == nullptr
                        || event.slot.empty()
                        || *event.filePath != saves->SlotPath(event.slot))
                    {
                        return false;
                    }
                    resource = CloudSaveResource::SaveSlot(
                        std::string(event.slot));
                }
                if (synchronizer && synchronizer->IsAttached())
                {
                    synchronizer->PrepareLocalDelete(resource);
                }
                return true;
            }
            catch (...)
            {
                // WALを先に確定できないdeleteはdisk commitへ進めません。
                return false;
            }
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
        std::unique_ptr<CloudSaveSynchronizer> synchronizer;
        std::string configuredGameId;
        std::string configuredEnvironmentId;
        std::string backendBaseUrl;
        std::uint64_t profileEpoch{ 1 };
        LocalPersistenceObserverToken observerToken{};
        std::unique_ptr<PersistenceProfilePaths> activeProfile;
        std::unique_ptr<PlayerPrefs> suspendedGuestPreferences;
        std::filesystem::path guestSaveDirectory;
        std::filesystem::path activeRecoverySidecarPath;
        LocalPersistenceDocument activeRecoverySidecarObservation;
        std::unique_ptr<CloudSaveJournal> journal;
        std::unique_ptr<CloudSaveProfileSessionLease>
            activeProfileSessionLease;
        std::unique_ptr<PersistenceProfilePaths> quarantinedProfile;
        std::unique_ptr<PlayerPrefs> quarantinedPreferences;
        std::unique_ptr<CloudSaveJournal> quarantinedJournal;
        CloudSaveDetachCheckpointRecovery
            quarantinedCloudCheckpointRecovery;
        std::unique_ptr<CloudSaveProfileSessionLease>
            quarantinedProfileSessionLease;
        std::filesystem::path recoverySidecarPath;
        LocalPersistenceDocument recoverySidecarCreationObservation;
        std::vector<std::uint8_t> recoverySidecarObservedBytes;
        LocalPersistenceDocumentIdentity recoverySidecarIdentity;
        LocalPersistenceDocumentState recoverySidecarState{
            LocalPersistenceDocumentState::Missing
        };
        std::vector<std::uint8_t> lastRecoverySidecarObservedBytes;
        LocalPersistenceDocumentIdentity lastRecoverySidecarIdentity;
        LocalPersistenceDocumentState lastRecoverySidecarState{
            LocalPersistenceDocumentState::Missing
        };
        std::uint64_t recoveryRevisionCounter{};
        std::uint64_t recoveryRevision{};
        bool allowInsecureLoopback{};
        bool localCommitObserved{};
        bool observerFailed{};
        bool cloudUnauthorizedPending{};
        bool cloudUnauthorizedLatched{};
        bool cloudHealthyPending{};
        bool cloudAwaitingHealthyAfterTokenUpdate{};
        std::uint64_t nextPeriodicReconcileMilliseconds{};
        bool quarantinedLoadFailure{};
        bool quarantinedCloudCheckpointFailure{};
        bool recoverySidecarCreationBlocked{};
        bool recoverySidecarPublicationPending{};
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
        const bool allowInsecureLoopback,
        std::shared_ptr<const CloudSaveClient> cloudSaveClient)
    {
        m_implementation->ConfigureNamespace(
            std::move(gameId),
            std::move(environmentId),
            std::move(normalizedBackendBaseUrl),
            allowInsecureLoopback,
            std::move(cloudSaveClient));
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
        const std::string_view playerId,
        std::string accessToken)
    {
        return PreparedOnlineAccount(
            m_implementation->PrepareAccount(
                playerId,
                std::move(accessToken)));
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

    OnlinePersistenceRecoverySnapshot
        OnlinePersistenceCoordinator::RecoveryStatus() noexcept
    {
        return m_implementation->RecoveryStatus();
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

    OnlinePersistenceRecoveryOperationResult
        OnlinePersistenceCoordinator::RestorePendingRecovery(
        const std::uint64_t expectedRevision) noexcept
    {
        return m_implementation->RestorePendingRecovery(expectedRevision);
    }

    OnlinePersistenceRecoveryOperationResult
        OnlinePersistenceCoordinator::DiscardPendingRecovery(
        const std::uint64_t expectedRevision) noexcept
    {
        return m_implementation->DiscardPendingRecovery(expectedRevision);
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

    void OnlinePersistenceCoordinator::UpdateCloudSaveAccessToken(
        std::string accessToken)
    {
        m_implementation->UpdateCloudSaveAccessToken(
            std::move(accessToken));
    }

    bool OnlinePersistenceCoordinator::
        ConsumeCloudSaveUnauthorizedSignal() noexcept
    {
        return m_implementation->ConsumeCloudSaveUnauthorizedSignal();
    }

    bool OnlinePersistenceCoordinator::
        ConsumeCloudSaveHealthySignal() noexcept
    {
        return m_implementation->ConsumeCloudSaveHealthySignal();
    }

    CloudSaveSynchronizer*
        OnlinePersistenceCoordinator::Synchronizer() noexcept
    {
        return m_implementation->synchronizer.get();
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
