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

    // 次回復旧検証で再現する失敗段階
    std::atomic<LamaPon::Detail::OnlinePersistenceRecoveryTestFailPoint>
        RecoveryTestFailPoint{};

    // 正常読込または不在として扱える状態かを返す(state: 文書の読込状態)。
    [[nodiscard]] bool IsReadableLocalState(
        const LocalPersistenceDocumentState state) noexcept
    {
        return state == LocalPersistenceDocumentState::Loaded
            || state == LocalPersistenceDocumentState::Missing;
    }

    // 状態と完全バイト、取得不能ならファイル識別情報で復旧文書を照合する(leftState: 前の観測状態, leftBytes: 前の全バイト, leftIdentity: 前のファイル識別情報, rightState: 新しい観測状態, rightBytes: 新しい全バイト, rightIdentity: 新しいファイル識別情報)。
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

    // 保存文書の検証失敗を内部情報を含まない固定エラーで通知する。
    [[noreturn]] void ThrowUnavailableAccountDocument()
    {
        // 公開エラーへパスやプレイヤーID、サーバー本文を含めない。
        throw std::runtime_error(
            "An account persistence document is unavailable or corrupt.");
    }

    // account内の短い固定名の復旧パスを返す(profile: 対象accountの保存先)。
    [[nodiscard]] std::filesystem::path MakeRecoverySidecarPath(
        const LamaPon::PersistenceProfilePaths& profile)
    {
        // 派生する.lock・.writingもWindowsのパス上限に収まるよう短い固定名を使う。
        return profile.rootDirectory / L"Recovery.prefs";
    }

    // 文字列の使用済みバイトを消去して空にする(secret: 消去する秘密文字列)。
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
        // 借用した秘密文字列を消去する。
        ~ScopedSecretErase()
        {
            EraseSecret(value);
        }
        // 終了時に消去する文字列の借用
        std::string& value;
    };

    // 同期に使う非負の単調時刻をミリ秒で返す。
    [[nodiscard]] std::uint64_t MonotonicMilliseconds() noexcept
    {
        // 単調時計の経過ミリ秒
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
        // 準備を適用できる切替処理の借用
        OnlinePersistenceCoordinator* owner{};
        // 準備時の保存先の世代
        std::uint64_t profileEpoch{};
        // 検証済みaccount保存先の所有先
        std::unique_ptr<PersistenceProfilePaths> profile;
        // 検証済みaccount設定の所有先
        std::unique_ptr<PlayerPrefs> preferences;
        // 回復済みjournalの所有先
        std::unique_ptr<CloudSaveJournal> journal;
        // 準備中accountの使用ロック
        std::unique_ptr<CloudSaveProfileSessionLease> profileSessionLease;
        // 検証済みクラウド接続の準備
        std::unique_ptr<PreparedCloudSaveAttachment> cloudAttachment;
        // commitで移すaccount保存先
        std::filesystem::path accountSaveDirectory;
        // guestへ戻すための保存先
        std::filesystem::path guestSaveDirectory;
        // 復旧文書の切替先パス
        std::filesystem::path recoverySidecarPath;
        // 切替準備時に観測した復旧文書
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
        // guest保存先の一致と未使用状態を検証して構築元スレッドを記録する(activePreferences: 存続する設定の借用, activeSaves: 存続する保存領域の借用, userDataDirectory: 信頼済みUserDataの絶対パス)。
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

            // 必須のguest設定ファイルのパス
            const auto expectedPreferences =
                trustedUserDataDirectory / L"PlayerPrefs.json";
            // 必須のguest保存領域のパス
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

        // guestへ戻し同期処理の破棄後に隔離中の保存を一度再試行する。
        ~Implementation()
        {
            static_cast<void>(DetachToGuest());
            // workerは共有clientとmailboxだけを所有し、同期処理をjournalより先に破棄する。
            synchronizer.reset();
            // 終了時にも隔離中の保存を一度再試行し、残ったメモリの破棄はApplication側で記録する。
            EndFrame();
            DetachObserver();
        }

        // ローカル保存監視を解除して登録IDを失効させる。
        void DetachObserver() noexcept
        {
            if (observerToken != 0)
            {
                static_cast<void>(
                    DetachLocalPersistenceCommitObserver(observerToken));
                observerToken = 0;
            }
        }

        // 構築元以外のスレッドなら例外を出す。
        void RequireOwnerThread() const
        {
            if (std::this_thread::get_id() != ownerThread)
            {
                throw std::logic_error(
                    "Online persistence must be used from its owner thread.");
            }
        }

        // 構築元のスレッドから呼ばれたかを返す。
        [[nodiscard]] bool IsOwnerThread() const noexcept
        {
            return std::this_thread::get_id() == ownerThread;
        }

        // 0を飛ばして保存先の世代を更新する。
        void AdvanceEpoch() noexcept
        {
            ++profileEpoch;
            if (profileEpoch == 0)
            {
                ++profileEpoch;
            }
        }

        // 0を飛ばして復旧操作の世代を採番する。
        void AdvanceRecoveryRevision() noexcept
        {
            ++recoveryRevisionCounter;
            if (recoveryRevisionCounter == 0)
            {
                ++recoveryRevisionCounter;
            }
            recoveryRevision = recoveryRevisionCounter;
        }

        // 復旧文書の観測が変わったときだけ保持内容と操作世代を更新する(state: 新しい読込状態, bytes: 観測した全バイトの所有先, identity: 文書のファイル識別情報)。
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

        // guest設定の退避がありaccount保存先を使用中かを返す。
        [[nodiscard]] bool AccountActive() const noexcept
        {
            return suspendedGuestPreferences != nullptr;
        }

        // 公開オブジェクトの保存先が未使用のguest領域と一致するかを返す。
        [[nodiscard]] bool GuestBindingIsCurrent() const noexcept
        {
            if (!profiles || AccountActive())
            {
                return false;
            }
            try
            {
                // guestの保存先の構成
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

        // 公開前に保存領域と同期処理を準備し未解決の復旧があれば拒否する(gameId: ゲーム固有のID, environmentId: 保存領域を分ける環境ID, normalizedBackendBaseUrl: 正規化済み認証サービスURL, insecureLoopback: loopbackのHTTPを許可するか, cloudSaveClient: 同期用クライアントの共有所有先)。
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

            // 失敗時に旧領域を維持できるよう新領域の検証と割当は公開前に終える。
            auto replacementProfiles =
                std::make_unique<PersistenceProfiles>(
                    trustedUserDataDirectory,
                    gameId,
                    environmentId);
            // 公開前に準備する新しい同期処理
            std::unique_ptr<CloudSaveSynchronizer> replacementSynchronizer;
            if (cloudSaveClient)
            {
                replacementSynchronizer =
                    std::make_unique<CloudSaveSynchronizer>(
                        *preferences,
                        *saves,
                        std::move(cloudSaveClient));
            }
            // 公開前に準備するゲームID
            auto replacementGameId = std::move(gameId);
            // 公開前に準備する環境ID
            auto replacementEnvironmentId = std::move(environmentId);
            // 公開前に準備する認証サービスURL
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

        // 復旧待ちの対象を保持し、解決済みならguestへ戻して保存領域を無効化する。
        void DisableNamespace() noexcept
        {
            if (!IsOwnerThread())
            {
                return;
            }
            // 復旧の対象を失わないよう解決前はnamespaceを無効化しない。
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

        // guestを保存しaccountの全文書とjournalを検証して切替状態を準備する(playerId: サービスの公開プレイヤーID, accessToken: 同期用tokenの所有先)。
        [[nodiscard]] std::unique_ptr<PreparedOnlineAccount::State>
            PrepareAccount(
            const std::string_view playerId,
            std::string accessToken)
        {
            // 引数tokenを終了時に消去する番人
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

            // process終了時にもguestの未保存設定が残るよう切替前に保存する。
            if (preferences->IsDirty())
            {
                preferences->Save();
            }

            // 切替先アカウントの保存先
            auto profile = std::make_unique<PersistenceProfilePaths>(
                profiles->Account(playerId));
            // 切替前に回復したjournal
            auto preparedJournal = std::make_unique<CloudSaveJournal>(
                trustedUserDataDirectory,
                *profile,
                configuredGameId,
                configuredEnvironmentId,
                backendBaseUrl,
                allowInsecureLoopback);
            // 切替先accountの使用ロック
            auto profileSessionLease =
                std::make_unique<CloudSaveProfileSessionLease>(
                    *preparedJournal);
            // 切替先の復旧ファイルのパス
            auto recoveryPath = MakeRecoverySidecarPath(*profile);
            // 切替時に検証する復旧文書
            PlayerPrefs recoveryDocument(recoveryPath);
            // 復旧ファイルの厳格な読込結果
            const auto recovered =
                LocalPersistenceDocuments::ReadPlayerPrefs(
                    recoveryDocument);
            if (recovered.state
                != LocalPersistenceDocumentState::Missing)
            {
                // 再起動後もaccountを公開せず明示的解決を待つため復旧対象の保存先を保持する。
                auto pendingProfile =
                    std::make_unique<PersistenceProfilePaths>(*profile);
                // 保持用に複製した観測バイト
                auto observedBytes = recovered.bytes;
                // 直近の観測用に複製した全バイト
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
            // 切替先または退避したaccount設定
            auto accountPreferences =
                std::make_unique<PlayerPrefs>(profile->playerPrefsFile);

            // account設定の厳格な読込結果
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

            // 切替前に検証するaccount保存領域
            SaveDataStore accountSaves(profile->saveDataDirectory);
            // account保存スロットの一覧
            const auto slots =
                LocalPersistenceDocuments::ListSaveData(accountSaves);
            if (!IsReadableLocalState(slots.state))
            {
                ThrowUnavailableAccountDocument();
            }
            if (slots.state == LocalPersistenceDocumentState::Loaded)
            {
                // 切替前に検証する保存スロット名
                for (const auto& slot : slots.slots)
                {
                    // 復旧文書・保存文書の読込結果
                    const auto document =
                        LocalPersistenceDocuments::ReadSaveData(
                            accountSaves,
                            slot);
                    // 一覧取得後の消失も競合として切替を拒否する。
                    if (document.state
                        != LocalPersistenceDocumentState::Loaded)
                    {
                        ThrowUnavailableAccountDocument();
                    }
                }
            }

            // 切替前に検証した同期の接続準備
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

            // 所有者と世代を持つ切替準備
            auto state = std::make_unique<PreparedOnlineAccount::State>();
            state->owner = owner;
            state->profileEpoch = profileEpoch;
            // commit中の割当を避けるためパスの複製は準備時に済ませる。
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

        // 所有者・世代・保存先を照合しロックを取得して割当なしでaccountへ切り替える(state: 消費する切替準備)。
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

        // 設定保存と削除意図の確定を試してguestへ戻し未保存分をロック付きで隔離する。
        [[nodiscard]] OnlinePersistenceDetachResult
            DetachToGuest() noexcept
        {
            if (!IsOwnerThread() || !AccountActive())
            {
                return OnlinePersistenceDetachResult::AlreadyGuest;
            }

            // main threadで設定保存後にworker応答を失効させ削除意図を確定する。
            // account設定の保存が失敗したか
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

            // 削除意図の確定を別途保留するか
            bool cloudCheckpointFailed{};
            // 確定に失敗した削除意図の操作列
            CloudSaveDetachCheckpointRecovery cloudCheckpointRecovery;
            if (synchronizer)
            {
                try
                {
                    // 通信を始めず、baseline未確立のローカル削除もjournalに先行記録してからguestへ戻す。
                    synchronizer->CheckpointLocalStateForDetach();
                }
                catch (...)
                {
                    cloudCheckpointRecovery = synchronizer
                        ->TakeFailedDetachCheckpointRecovery();
                    // 設定の保存失敗は隔離メモリで再試行し、同じ未保存状態の走査失敗を別のcheckpoint失敗にしない。
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

            // 切替先または退避したaccount設定
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

            // guest切替後のaccount保存先
            auto detachedProfile = std::move(activeProfile);
            // 切替後のaccount journal
            auto detachedJournal = std::move(journal);
            // 切替後のaccount使用ロック
            auto detachedProfileSessionLease =
                std::move(activeProfileSessionLease);
            // guest切替後の復旧ファイルのパス
            auto detachedRecoveryPath =
                std::move(activeRecoverySidecarPath);
            // 切替準備時に観測した復旧文書
            auto detachedRecoveryObservation =
                std::move(activeRecoverySidecarObservation);
            localCommitObserved = false;
            observerFailed = false;

            // 読込失敗だけで未保存差分がなければdiskを変えず解放し、次の切替準備で再検証する。
            if (!cloudCheckpointFailed
                && accountPreferences->HasLoadFailure()
                && !accountPreferences->IsDirty())
            {
                return OnlinePersistenceDetachResult::SavedAccount;
            }

            // 未保存データを隔離する必要があるか
            const bool needsQuarantine = cloudCheckpointFailed
                || saveFailed
                || accountPreferences->IsDirty();
            if (needsQuarantine)
            {
                // 復旧中の切替を拒否しているため保持済みの未保存設定を上書きしない。
                quarantinedPreferences = std::move(accountPreferences);
                quarantinedProfile = std::move(detachedProfile);
                quarantinedProfileSessionLease =
                    std::move(detachedProfileSessionLease);
                if (cloudCheckpointFailed)
                {
                    // 削除意図が確定できなければjournalと使用ロックを保持して再試行または明示的破棄を待つ。
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

        // 使用ロック下でjournalを回復し確定済みの削除意図だけを一括再適用する。
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
                // 操作列を確定できなければ自動解放せず、明示的破棄までaccount使用ロックを保持する。
                return false;
            }
            try
            {
                // 不確かな失敗で使用不能になったjournalはロック下でdiskから作り直し、一括確定後に差し替える。
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
                // 一括確定は全件か0件なので同じ操作列を再適用して不確かな成功も回復する。
                return false;
            }
            quarantinedCloudCheckpointFailure = false;
            quarantinedCloudCheckpointRecovery = {};
            return true;
        }

        // 監視通知と30秒間隔の照合を同期へ渡し隔離中の保存とcheckpointを再試行する。
        void EndFrame() noexcept
        {
            if (!IsOwnerThread())
            {
                return;
            }
            // 再走査が必要なローカル変更があるか
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
                // 同期を進める単調時刻・ミリ秒
                const auto now = MonotonicMilliseconds();
                // 定期照合の開始時刻に達したか
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
                    // ローカル保存は完了済みなので次のフレームで照合を再試行し例外本文やパスは公開しない。
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
                // 隔離メモリを保持して次のフレームで保存を再試行する。
            }
        }

        // token更新で旧応答を失効させ次の検証済み通信成功を待つ(accessToken: 新しい同期用token)。
        void UpdateCloudSaveAccessToken(std::string accessToken)
        {
            // 引数tokenを終了時に消去する番人
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

        // 未通知の同期認証失敗を一度だけ取り出す。
        [[nodiscard]] bool ConsumeCloudSaveUnauthorizedSignal() noexcept
        {
            if (!IsOwnerThread())
            {
                return false;
            }
            return std::exchange(cloudUnauthorizedPending, false);
        }

        // token更新後の検証済み通信成功を一度だけ取り出す。
        [[nodiscard]] bool ConsumeCloudSaveHealthySignal() noexcept
        {
            if (!IsOwnerThread())
            {
                return false;
            }
            return std::exchange(cloudHealthyPending, false);
        }

        // 読込失敗した設定の隔離メモリを観測一致時だけ復旧ファイルへ保存し再読後に解放する。
        [[nodiscard]] bool PersistRecoverySidecar() noexcept
        {
            if (!quarantinedPreferences
                || !quarantinedProfile
                || !quarantinedLoadFailure
                || recoverySidecarPath.empty())
            {
                return false;
            }
            // 隔離設定を保存する完全なJSON
            std::string snapshot;
            try
            {
                snapshot = quarantinedPreferences->SerializeToJson();
                ValidatePlayerPrefsFullDocument(snapshot);
                // 観測一致時の復旧文書の書込結果
                const auto publishResult =
                    DurablePublishLocalDocumentIfUnchanged(
                        recoverySidecarPath,
                        recoverySidecarCreationObservation,
                        snapshot,
                        CloudPreferencesMaxBytes);
                if (publishResult
                    == LocalPersistenceConditionalApplyResult::Applied)
                {
                    // 書込後の再読が失敗した場合だけ、次回一致する全バイトを自分の不確かな成功として扱う。
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
                    // 切替時の不在観測後に他の文書が現れたら内容が同じでも上書きせず隔離メモリを保持する。
                    recoverySidecarCreationBlocked = true;
                    // 再読する現在の復旧文書
                    PlayerPrefs currentSidecar(recoverySidecarPath);
                    // 復旧ファイルの再読結果
                    auto current = LocalPersistenceDocuments::ReadPlayerPrefs(
                        currentSidecar);
                    static_cast<void>(SetLastRecoveryObservation(
                        current.state,
                        std::move(current.bytes),
                        current.identity));
                    return false;
                }

                // 書込後の復旧文書の検証用設定
                PlayerPrefs verification(recoverySidecarPath);
                // 復旧文書・保存文書の読込結果
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
                // 復元候補として固定する全バイト
                auto expectedBytes = document.bytes;
                // 保持用に複製した観測バイト
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
                // 復旧ファイルの再読確認後は元の設定へ自動反映しないため、checkpoint待ちでなければ使用ロックを解放する。
                if (!quarantinedCloudCheckpointFailure)
                {
                    quarantinedProfileSessionLease.reset();
                }
                quarantinedLoadFailure = false;
                // 復元候補のバイトを固定してメモリ隔離から復旧ファイルへ移行したため操作世代を更新する。
                AdvanceRecoveryRevision();
                return true;
            }
            catch (...)
            {
                // 元の設定と隔離メモリを保持して復旧ファイルの観測だけを制限付きで更新する。
                try
                {
                    // 再読する現在の復旧文書
                    PlayerPrefs currentSidecar(recoverySidecarPath);
                    // 復旧ファイルの再読結果
                    auto current = LocalPersistenceDocuments::ReadPlayerPrefs(
                        currentSidecar);
                    // 書込済みの未検証文書と一致するか
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

        // 未解決の隔離メモリまたは復旧ファイルがあるかを返す。
        [[nodiscard]] bool HasPendingRecovery() const noexcept
        {
            return quarantinedPreferences != nullptr
                || quarantinedProfile != nullptr;
        }

        // 復元候補と最新の観測が一致するかに応じて復旧状態を返す。
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

        // 復旧文書を再読し変更に応じて状態と操作世代を更新する。
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
                        // 操作世代を検証する復旧文書
                        PlayerPrefs sidecar(recoverySidecarPath);
                        // 復旧文書・保存文書の読込結果
                        auto document =
                            LocalPersistenceDocuments::ReadPlayerPrefs(
                                sidecar);
                        // 書込済みの未検証文書と一致するか
                        bool pendingSnapshotMatches{};
                        if (recoverySidecarPublicationPending
                            && document.state
                                == LocalPersistenceDocumentState::Loaded)
                        {
                            // 隔離設定を保存する完全なJSON
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
                    // 操作世代を検証する復旧文書
                    PlayerPrefs sidecar(recoverySidecarPath);
                    // 復旧文書・保存文書の読込結果
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

        // 隔離データと使用ロックを解放して復旧状態と操作世代を初期化する。
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

        // 指定世代の復旧を再試行し元の設定への適用後に同じ復旧文書を削除する(expectedRevision: 表示時に取得した非0の世代)。
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
                // この復元要求で保存とcheckpointが完了したなら世代0への解放はStaleではなく成功とする。
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
                // 別Coordinatorが使用中のaccountを書き換えないよう復旧用journalから使用ロックを再取得する。
                std::unique_ptr<CloudSaveJournal> recoveryJournal;
                // 復旧操作中のaccount使用ロック
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
                // 明示的操作で検証する復旧文書
                PlayerPrefs sidecar(recoverySidecarPath);
                // 復旧文書・保存文書の読込結果
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
                    // 再読で復旧文書の観測が変わったか
                    const bool changed = SetLastRecoveryObservation(
                        document.state,
                        std::move(document.bytes),
                        document.identity);
                    return changed
                        ? OnlinePersistenceRecoveryOperationResult::Stale
                        : OnlinePersistenceRecoveryOperationResult::Failed;
                }

                // 明示的に復元するaccount設定
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

        // 指定世代の隔離メモリまたは観測一致の復旧文書を明示的に破棄する(expectedRevision: 表示時に取得した非0の世代)。
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
                // 復旧操作用journalの所有先
                std::unique_ptr<CloudSaveJournal> recoveryJournal;
                // 復旧操作中のaccount使用ロック
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
                    // 操作世代を検証する復旧文書
                    PlayerPrefs sidecar(recoverySidecarPath);
                    // 復旧文書・保存文書の読込結果
                    auto document =
                        LocalPersistenceDocuments::ReadPlayerPrefs(sidecar);
                    // 最新の復旧文書の観測と一致するか
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
                        // 再読で復旧文書の観測が変わったか
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
                        // 復旧ファイルが削除済みなら最新世代の破棄要求で隔離状態だけを解放する。
                    }
                    else if (document.state
                            == LocalPersistenceDocumentState::Loaded
                        || document.state
                            == LocalPersistenceDocumentState::Corrupt)
                    {
                        if (quarantinedPreferences)
                        {
                            // 隔離中に復旧文書が現れたら最初の破棄ではメモリだけを捨て、不確かな自分の書込も新しい復旧案件として残す。
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

                        // 復元は元のsnapshotとの一致を要求し、破棄は最新世代で観測した全バイトまたは大容量文書の識別情報が同じhandleだけを削除する。
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

        // 最新の復旧世代を取得して復元を試す。
        [[nodiscard]] bool RestorePendingRecovery() noexcept
        {
            // 操作に使用する最新の復旧状態
            const auto status = RecoveryStatus();
            return status.revision != 0
                && RestorePendingRecovery(status.revision)
                    == OnlinePersistenceRecoveryOperationResult::Succeeded;
        }

        // 最新の復旧世代を取得して破棄を試す。
        [[nodiscard]] bool DiscardPendingRecovery() noexcept
        {
            // 操作に使用する最新の復旧状態
            const auto status = RecoveryStatus();
            return status.revision != 0
                && DiscardPendingRecovery(status.revision)
                    == OnlinePersistenceRecoveryOperationResult::Succeeded;
        }

        // ローカル保存完了を切替状態へ渡す(context: 切替状態の借用, eventEpoch: 登録時の保存先の世代, event: 保存完了した文書の通知)。
        [[nodiscard]] static bool ObserveLocalCommit(
            void* context,
            const std::uint64_t eventEpoch,
            const LocalPersistenceCommitEvent& event) noexcept
        {
            // コールバック先の切替状態の借用
            auto* implementation =
                static_cast<Implementation*>(context);
            if (!implementation)
            {
                return false;
            }
            return implementation->OnLocalCommit(eventEpoch, event);
        }

        // 削除前に切替状態へ通知して削除意図を先行確定する(context: 切替状態の借用, eventEpoch: 登録時の保存先の世代, event: 削除予定の文書の通知)。
        [[nodiscard]] static bool PrepareLocalDelete(
            void* context,
            const std::uint64_t eventEpoch,
            const LocalPersistenceCommitEvent& event) noexcept
        {
            // コールバック先の切替状態の借用
            auto* implementation =
                static_cast<Implementation*>(context);
            if (!implementation)
            {
                return false;
            }
            return implementation->OnLocalPreDelete(eventEpoch, event);
        }

        // 所有者・世代・保存先の一致を確認して削除意図を先行記録する(eventEpoch: 登録時の保存先の世代, event: 削除予定の文書の通知)。
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
                // 削除意図を先に記録する保存対象
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
                // 削除意図をjournalへ先行確定できなければローカル削除を中止する。
                return false;
            }
        }

        // 使用中の文書の保存完了だけを受理して次の照合を要求する(eventEpoch: 登録時の保存先の世代, event: 保存完了した文書の通知)。
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
                // 使用中の保存先の通知と一致するか
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

        // 公開する切替処理の借用
        OnlinePersistenceCoordinator* owner{};
        // 存続する公開設定の借用
        PlayerPrefs* preferences{};
        // 存続する公開保存領域の借用
        SaveDataStore* saves{};
        // 信頼済みUserDataの絶対パス
        std::filesystem::path trustedUserDataDirectory;
        // 全操作を行う構築元スレッド
        std::thread::id ownerThread;
        // guestとaccountの保存先
        std::unique_ptr<PersistenceProfiles> profiles;
        // クラウド同期処理の所有先
        std::unique_ptr<CloudSaveSynchronizer> synchronizer;
        // 保存領域を識別するゲームID
        std::string configuredGameId;
        // 保存領域を分ける環境ID
        std::string configuredEnvironmentId;
        // 正規化済み認証サービスURL
        std::string backendBaseUrl;
        // 保存先切替を識別する非0の世代
        std::uint64_t profileEpoch{ 1 };
        // ローカル保存監視の登録ID
        LocalPersistenceObserverToken observerToken{};
        // 使用中のアカウント保存先
        std::unique_ptr<PersistenceProfilePaths> activeProfile;
        // 切替中に保持するguestの設定
        std::unique_ptr<PlayerPrefs> suspendedGuestPreferences;
        // guestへ戻す保存領域のパス
        std::filesystem::path guestSaveDirectory;
        // 使用中アカウントの復旧パス
        std::filesystem::path activeRecoverySidecarPath;
        // 切替準備時の復旧文書の観測
        LocalPersistenceDocument activeRecoverySidecarObservation;
        // 使用中のjournalの所有先
        std::unique_ptr<CloudSaveJournal> journal;
        // 使用中accountのprocess間ロック
        std::unique_ptr<CloudSaveProfileSessionLease>
            activeProfileSessionLease;
        // 解決待ちアカウントの保存先
        std::unique_ptr<PersistenceProfilePaths> quarantinedProfile;
        // 保存失敗した設定メモリの所有先
        std::unique_ptr<PlayerPrefs> quarantinedPreferences;
        // 再試行用journalの所有先
        std::unique_ptr<CloudSaveJournal> quarantinedJournal;
        // 再試行する削除意図の確定済み操作列
        CloudSaveDetachCheckpointRecovery
            quarantinedCloudCheckpointRecovery;
        // 隔離中accountのprocess間ロック
        std::unique_ptr<CloudSaveProfileSessionLease>
            quarantinedProfileSessionLease;
        // 解決待ち復旧ファイルのパス
        std::filesystem::path recoverySidecarPath;
        // 復旧作成前の文書の観測
        LocalPersistenceDocument recoverySidecarCreationObservation;
        // 復元候補として固定した全バイト
        std::vector<std::uint8_t> recoverySidecarObservedBytes;
        // 復元候補のファイル識別情報
        LocalPersistenceDocumentIdentity recoverySidecarIdentity;
        // 復元候補を検出したときの状態
        LocalPersistenceDocumentState recoverySidecarState{
            LocalPersistenceDocumentState::Missing
        };
        // 直近に観測した復旧文書の全バイト
        std::vector<std::uint8_t> lastRecoverySidecarObservedBytes;
        // 直近の復旧ファイル識別情報
        LocalPersistenceDocumentIdentity lastRecoverySidecarIdentity;
        // 直近に観測した復旧文書の状態
        LocalPersistenceDocumentState lastRecoverySidecarState{
            LocalPersistenceDocumentState::Missing
        };
        // 復旧操作の世代の採番値
        std::uint64_t recoveryRevisionCounter{};
        // 現在の復旧世代・解決後は0
        std::uint64_t recoveryRevision{};
        // loopbackのHTTPを許可するか
        bool allowInsecureLoopback{};
        // ローカル保存の完了を観測したか
        bool localCommitObserved{};
        // 保存監視の失敗で再走査するか
        bool observerFailed{};
        // 未通知の同期認証失敗があるか
        bool cloudUnauthorizedPending{};
        // 同じ認証失敗を通知済みか
        bool cloudUnauthorizedLatched{};
        // 未通知の同期認証成功があるか
        bool cloudHealthyPending{};
        // token更新後の通信成功を待つか
        bool cloudAwaitingHealthyAfterTokenUpdate{};
        // 次の定期照合を始める単調時刻
        std::uint64_t nextPeriodicReconcileMilliseconds{};
        // 隔離メモリに読込失敗があるか
        bool quarantinedLoadFailure{};
        // 削除意図のjournal確定待ちか
        bool quarantinedCloudCheckpointFailure{};
        // 復旧作成前の文書が変わったか
        bool recoverySidecarCreationBlocked{};
        // 復旧文書の書込後の検証待ちか
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

    // 未通知の同期認証失敗を一度だけ取り出す。
    bool OnlinePersistenceCoordinator::
        ConsumeCloudSaveUnauthorizedSignal() noexcept
    {
        return m_implementation->ConsumeCloudSaveUnauthorizedSignal();
    }

    // token更新後の検証済み通信成功を一度だけ取り出す。
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
        // コールバック先の切替状態の借用
        auto& implementation = *m_implementation;
        if (implementation.observerToken != 0)
        {
            implementation.observerFailed = implementation.observerFailed
                || ConsumeLocalPersistenceObserverFailure();
        }
        // ローカル変更または監視失敗があるか
        const bool signalled = implementation.localCommitObserved
            || implementation.observerFailed;
        implementation.localCommitObserved = false;
        implementation.observerFailed = false;
        return signalled;
    }
}
