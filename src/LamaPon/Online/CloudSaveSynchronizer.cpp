#include "LamaPon/Online/CloudSaveSynchronizer.h"

#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Core/SaveSlotValidation.h"
#include "LamaPon/Online/CloudSaveClient.h"
#include "LamaPon/Online/OnlineHttpValidation.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using LamaPon::CloudSaveManifestItem;
    using LamaPon::CloudSaveResource;
    using LamaPon::CloudSaveResourceKind;
    using LamaPon::CloudSaveSnapshot;
    using LamaPon::Detail::CloudSaveItemResult;
    using LamaPon::Detail::CloudSaveManifestResult;
    using LamaPon::Detail::CloudSavePendingKind;
    using LamaPon::Detail::CloudSavePendingMutation;
    using LamaPon::Detail::CloudSaveWireOutcome;
    using LamaPon::Detail::CloudSaveWireStatus;
    using LamaPon::Detail::LocalPersistenceDocument;
    using LamaPon::Detail::LocalPersistenceDocumentState;

    constexpr std::uint64_t InitialRetryDelayMilliseconds = 1000u;
    constexpr std::uint64_t MaximumRetryDelayMilliseconds = 60000u;
    constexpr std::uint64_t MaximumRateLimitDelayMilliseconds = 300000u;
    constexpr std::size_t AccountStorageKeyBytes = 64u;

    bool SameResource(
        const CloudSaveResource& left,
        const CloudSaveResource& right) noexcept
    {
        if (left.kind != right.kind)
        {
            return false;
        }
        if (left.kind == CloudSaveResourceKind::Preferences)
        {
            return left.slot.empty() && right.slot.empty();
        }
        if (left.kind != CloudSaveResourceKind::SaveSlot)
        {
            return false;
        }
        return LamaPon::Detail::EquivalentSaveSlotNames(
            left.slot,
            right.slot);
    }

    bool IsValidResource(const CloudSaveResource& resource) noexcept
    {
        if (resource.kind == CloudSaveResourceKind::Preferences)
        {
            return resource.slot.empty();
        }
        return resource.kind == CloudSaveResourceKind::SaveSlot
            && LamaPon::Detail::IsValidSaveSlotName(resource.slot);
    }

    bool IsAccountStorageKey(const std::string_view value) noexcept
    {
        if (value.size() != AccountStorageKeyBytes)
        {
            return false;
        }
        for (const unsigned char character : value)
        {
            if (!((character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f')))
            {
                return false;
            }
        }
        return true;
    }

    void EraseSecret(std::string& value) noexcept
    {
        if (!value.empty())
        {
            ::SecureZeroMemory(value.data(), value.size());
        }
        value.clear();
    }

    struct SecretString final
    {
        explicit SecretString(const std::string_view source)
            : value(source)
        {
        }

        ~SecretString()
        {
            EraseSecret(value);
        }

        SecretString(const SecretString&) = delete;
        SecretString& operator=(const SecretString&) = delete;

        std::string value;
    };

    std::string GenerateMutationId()
    {
        std::array<std::uint8_t, 16> bytes{};
        if (BCryptGenRandom(
                nullptr,
                bytes.data(),
                static_cast<ULONG>(bytes.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            throw std::runtime_error(
                "Cloud save mutation ID generation failed.");
        }

        bytes[6] = static_cast<std::uint8_t>(
            (bytes[6] & 0x0fu) | 0x40u);
        bytes[8] = static_cast<std::uint8_t>(
            (bytes[8] & 0x3fu) | 0x80u);

        constexpr char Digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(36u);
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            if (index == 4u || index == 6u || index == 8u || index == 10u)
            {
                result.push_back('-');
            }
            result.push_back(Digits[(bytes[index] >> 4u) & 0x0fu]);
            result.push_back(Digits[bytes[index] & 0x0fu]);
        }
        return result;
    }

    enum class WireOperation : std::uint8_t
    {
        Manifest,
        Read,
        Put,
        Delete
    };

    struct RequestFence final
    {
        std::uint64_t sessionSerial{};
        std::uint64_t profileEpoch{};
        std::uint64_t journalGeneration{};
        std::string accountStorageKey;
        CloudSaveResource resource;
        std::string mutationId;
        std::string expectedEtag;
        bool localWasLoadedAtReadStart{};
    };

    struct WireResult final
    {
        WireOperation operation{ WireOperation::Manifest };
        RequestFence fence;
        CloudSaveManifestResult manifest;
        CloudSaveItemResult item;
    };

    struct WireMailbox final
    {
        std::mutex mutex;
        std::optional<WireResult> result;
        bool completed{};
        bool resultLost{};
    };

    CloudSaveWireOutcome FixedTransportFailure()
    {
        return {
            CloudSaveWireStatus::TransportError,
            0u,
            {},
            {}
        };
    }

    template<typename Callback>
    void LaunchDetached(
        std::shared_ptr<const LamaPon::Detail::CloudSaveClient> client,
        std::shared_ptr<SecretString> token,
        std::shared_ptr<WireMailbox> mailbox,
        WireOperation operation,
        RequestFence fence,
        Callback callback)
    {
        std::thread(
            [client = std::move(client),
             token = std::move(token),
             mailbox = std::move(mailbox),
             operation,
             fence = std::move(fence),
             callback = std::move(callback)]() mutable noexcept
            {
                WireResult result;
                result.operation = operation;
                result.fence = std::move(fence);
                try
                {
                    callback(*client, token->value, result);
                }
                catch (...)
                {
                    if (operation == WireOperation::Manifest)
                    {
                        result.manifest.outcome = FixedTransportFailure();
                    }
                    else
                    {
                        result.item.outcome = FixedTransportFailure();
                    }
                }

                std::scoped_lock lock(mailbox->mutex);
                try
                {
                    mailbox->result.emplace(std::move(result));
                }
                catch (...)
                {
                    mailbox->resultLost = true;
                }
                mailbox->completed = true;
            }).detach();
    }

    struct LocalResource final
    {
        CloudSaveResource resource;
        LocalPersistenceDocument document;
    };

    struct LocalInventory final
    {
        std::vector<LocalResource> resources;
    };

    const LocalResource* FindLocalResource(
        const LocalInventory& inventory,
        const CloudSaveResource& resource) noexcept
    {
        const auto iterator = std::find_if(
            inventory.resources.begin(),
            inventory.resources.end(),
            [&resource](const LocalResource& item)
            {
                return SameResource(item.resource, resource);
            });
        return iterator == inventory.resources.end()
            ? nullptr
            : &*iterator;
    }

    const LocalPersistenceDocument* FindLocal(
        const LocalInventory& inventory,
        const CloudSaveResource& resource) noexcept
    {
        const auto* found = FindLocalResource(inventory, resource);
        return found ? &found->document : nullptr;
    }

    bool FitsAccountQuotaAfterApply(
        const LocalInventory& inventory,
        const CloudSaveSnapshot& snapshot) noexcept
    {
        std::uint64_t total{};
        for (const auto& local : inventory.resources)
        {
            if (local.document.state
                == LocalPersistenceDocumentState::Loaded)
            {
                total += local.document.bytes.size();
            }
        }
        if (const auto* current = FindLocal(inventory, snapshot.resource);
            current
            && current->state == LocalPersistenceDocumentState::Loaded)
        {
            total -= current->bytes.size();
        }
        if (!snapshot.deleted)
        {
            if (snapshot.content.size()
                > LamaPon::CloudSaveAccountMaxBytes - total)
            {
                return false;
            }
            total += snapshot.content.size();
        }
        return total <= LamaPon::CloudSaveAccountMaxBytes;
    }

    const CloudSaveManifestItem* FindManifest(
        const std::vector<CloudSaveManifestItem>& manifest,
        const CloudSaveResource& resource) noexcept
    {
        const auto iterator = std::find_if(
            manifest.begin(),
            manifest.end(),
            [&resource](const CloudSaveManifestItem& item)
            {
                return SameResource(item.resource, resource);
            });
        return iterator == manifest.end() ? nullptr : &*iterator;
    }

    bool LocalMatchesBaseline(
        const LocalPersistenceDocument& local,
        const std::optional<CloudSaveSnapshot>& baseline) noexcept
    {
        if (!baseline || baseline->deleted)
        {
            return local.state == LocalPersistenceDocumentState::Missing;
        }
        return local.state == LocalPersistenceDocumentState::Loaded
            && local.bytes == baseline->content;
    }

    bool ManifestMatchesBaseline(
        const CloudSaveManifestItem* remote,
        const std::optional<CloudSaveSnapshot>& baseline) noexcept
    {
        if (!remote || !baseline)
        {
            return remote == nullptr && !baseline;
        }
        return remote->etag == baseline->etag
            && remote->deleted == baseline->deleted
            && remote->byteLength == baseline->content.size()
            && remote->sha256 == baseline->sha256;
    }

    void ValidateFullSnapshotDocument(
        const CloudSaveSnapshot& snapshot)
    {
        if (!IsValidResource(snapshot.resource))
        {
            throw std::invalid_argument("Invalid cloud snapshot resource.");
        }
        if (snapshot.deleted)
        {
            if (!snapshot.content.empty() || !snapshot.sha256.empty())
            {
                throw std::invalid_argument("Invalid cloud tombstone.");
            }
            return;
        }
        const std::string_view bytes(
            reinterpret_cast<const char*>(snapshot.content.data()),
            snapshot.content.size());
        if (snapshot.resource.kind == CloudSaveResourceKind::Preferences)
        {
            LamaPon::Detail::ValidatePlayerPrefsFullDocument(bytes);
        }
        else
        {
            LamaPon::Detail::ValidateSaveDataFullDocument(
                snapshot.resource.slot,
                bytes);
        }
    }

    void ValidatePendingDocument(
        const CloudSavePendingMutation& pending)
    {
        if (!IsValidResource(pending.resource))
        {
            throw std::invalid_argument("Invalid journal resource.");
        }
        if (pending.kind == CloudSavePendingKind::Delete)
        {
            if (!pending.content.empty())
            {
                throw std::invalid_argument("Invalid journal delete.");
            }
            return;
        }
        CloudSaveSnapshot snapshot;
        snapshot.resource = pending.resource;
        snapshot.content = pending.content;
        snapshot.sha256 = pending.sha256;
        ValidateFullSnapshotDocument(snapshot);
    }

    void AddResource(
        std::vector<CloudSaveResource>& resources,
        const CloudSaveResource& resource)
    {
        if (!IsValidResource(resource))
        {
            throw std::runtime_error("Invalid cloud save resource.");
        }
        if (std::none_of(
                resources.begin(),
                resources.end(),
                [&resource](const CloudSaveResource& existing)
                {
                    return SameResource(existing, resource);
                }))
        {
            resources.push_back(resource);
        }
    }

    bool ContainsResource(
        const std::vector<CloudSaveResource>& resources,
        const CloudSaveResource& resource) noexcept
    {
        return std::any_of(
            resources.begin(),
            resources.end(),
            [&resource](const CloudSaveResource& existing)
            {
                return SameResource(existing, resource);
            });
    }

    std::uint64_t SaturatingAdd(
        const std::uint64_t left,
        const std::uint64_t right) noexcept
    {
        if (right > std::numeric_limits<std::uint64_t>::max() - left)
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return left + right;
    }
}

namespace LamaPon::Detail
{
    struct PreparedCloudSaveAttachment::State final
    {
        const void* owner{};
        CloudSaveJournal* journal{};
        std::string accountStorageKey;
        std::shared_ptr<SecretString> token;
        LocalInventory initialInventory;
        std::size_t conflictCount{};
    };

    PreparedCloudSaveAttachment::PreparedCloudSaveAttachment() noexcept =
        default;
    PreparedCloudSaveAttachment::~PreparedCloudSaveAttachment() = default;
    PreparedCloudSaveAttachment::PreparedCloudSaveAttachment(
        PreparedCloudSaveAttachment&&) noexcept = default;
    PreparedCloudSaveAttachment&
        PreparedCloudSaveAttachment::operator=(
        PreparedCloudSaveAttachment&&) noexcept = default;

    PreparedCloudSaveAttachment::PreparedCloudSaveAttachment(
        std::unique_ptr<State> state) noexcept
        : m_state(std::move(state))
    {
    }

    bool PreparedCloudSaveAttachment::IsValid() const noexcept
    {
        return m_state != nullptr;
    }

    struct CloudSaveSynchronizer::Implementation final
    {
        Implementation(
            PlayerPrefs& preferencesValue,
            SaveDataStore& savesValue,
            std::shared_ptr<const CloudSaveClient> clientValue,
            CloudSaveMutationIdGenerator generatorValue)
            : preferences(preferencesValue),
              saves(savesValue),
              client(std::move(clientValue)),
              mutationIdGenerator(std::move(generatorValue)),
              ownerThread(std::this_thread::get_id())
        {
            if (!client)
            {
                throw std::invalid_argument(
                    "Cloud save client is required.");
            }
        }

        ~Implementation()
        {
            DetachNoThrow();
            // retired workerはshared client/mailbox/secretだけを所有します。
            retiredMailbox.reset();
        }

        void RequireOwner() const
        {
            if (std::this_thread::get_id() != ownerThread)
            {
                throw std::logic_error(
                    "Cloud save synchronizer is main-thread only.");
            }
        }

        void RetireActiveMailbox() noexcept
        {
            if (activeMailbox)
            {
                // 物理的な同時wire呼出しを1件に保つため、完了までは保持します。
                retiredMailbox = std::move(activeMailbox);
            }
            activeOperation.reset();
            activeFence.reset();
            activeReadInvalidated = false;
        }

        void ReapRetiredMailbox() noexcept
        {
            if (!retiredMailbox)
            {
                return;
            }
            const auto mailbox = retiredMailbox;
            bool completed{};
            {
                std::scoped_lock lock(mailbox->mutex);
                completed = mailbox->completed;
            }
            if (completed && retiredMailbox == mailbox)
            {
                retiredMailbox.reset();
            }
        }

        void ClearAccountStorageKey() noexcept
        {
            EraseSecret(accountStorageKey);
        }

        void DetachNoThrow() noexcept
        {
            ++sessionSerial;
            RetireActiveMailbox();
            journal = nullptr;
            profileEpoch = 0u;
            ClearAccountStorageKey();
            token.reset();
            manifest.reset();
            processed.clear();
            lastObservedInventory.resources.clear();
            hasLastObservedInventory = false;
            validatedJournalGeneration.reset();
            reconcileRequested = false;
            deleteIntentCapturePending = false;
            retryAttempt = 0u;
            authorizedWireSuccessPending = false;
            status = {};
            status.state = CloudSaveSynchronizerState::Detached;
        }

        [[nodiscard]] std::string NewMutationId()
        {
            return mutationIdGenerator
                ? mutationIdGenerator()
                : GenerateMutationId();
        }

        [[nodiscard]] RequestFence MakeFence(
            const CloudSaveResource& resource = {},
            std::string mutationId = {},
            std::string expectedEtag = {},
            const bool localWasLoadedAtReadStart = false) const
        {
            return {
                sessionSerial,
                profileEpoch,
                journal ? journal->Generation() : 0u,
                accountStorageKey,
                resource,
                std::move(mutationId),
                std::move(expectedEtag),
                localWasLoadedAtReadStart
            };
        }

        [[nodiscard]] bool FenceMatches(
            const RequestFence& fence,
            const bool checkMutation) const
        {
            if (!journal
                || fence.sessionSerial != sessionSerial
                || fence.profileEpoch != profileEpoch
                || fence.accountStorageKey != accountStorageKey
                || fence.journalGeneration != journal->Generation())
            {
                return false;
            }
            if (!checkMutation)
            {
                return true;
            }
            const auto pending = journal->Pending(fence.resource);
            return pending
                && pending->mutationId == fence.mutationId;
        }

        [[nodiscard]] std::optional<WireResult> PollActive()
        {
            if (!activeMailbox)
            {
                return std::nullopt;
            }
            const auto mailbox = activeMailbox;
            std::optional<WireResult> result;
            bool completed{};
            bool failed{};
            {
                std::scoped_lock lock(mailbox->mutex);
                completed = mailbox->completed;
                failed = mailbox->resultLost;
                if (completed && mailbox->result)
                {
                    result = std::move(mailbox->result);
                }
            }
            if (!completed)
            {
                return std::nullopt;
            }
            if (activeMailbox == mailbox)
            {
                activeMailbox.reset();
            }
            activeOperation.reset();
            activeFence.reset();
            if (failed || !result)
            {
                throw std::runtime_error(
                    "Cloud save worker mailbox failed.");
            }
            return result;
        }

        void LaunchManifest()
        {
            auto mailbox = std::make_shared<WireMailbox>();
            const auto fence = MakeFence();
            activeMailbox = mailbox;
            activeOperation = WireOperation::Manifest;
            activeFence = fence;
            try
            {
                LaunchDetached(
                    client,
                    token,
                    std::move(mailbox),
                    WireOperation::Manifest,
                    fence,
                    [](const CloudSaveClient& cloud,
                       const std::string_view accessToken,
                       WireResult& result)
                    {
                        result.manifest = cloud.FetchManifest(accessToken);
                    });
            }
            catch (...)
            {
                activeMailbox.reset();
                activeOperation.reset();
                activeFence.reset();
                throw;
            }
            status.state = CloudSaveSynchronizerState::Synchronizing;
        }

        void LaunchRead(
            const CloudSaveResource& resource,
            const std::string_view expectedEtag,
            const LocalPersistenceDocumentState localState)
        {
            auto mailbox = std::make_shared<WireMailbox>();
            auto fence = MakeFence(
                resource,
                {},
                std::string(expectedEtag),
                localState == LocalPersistenceDocumentState::Loaded);
            activeMailbox = mailbox;
            activeOperation = WireOperation::Read;
            activeFence = fence;
            activeReadInvalidated = false;
            try
            {
                LaunchDetached(
                    client,
                    token,
                    std::move(mailbox),
                    WireOperation::Read,
                    std::move(fence),
                    [resource](const CloudSaveClient& cloud,
                               const std::string_view accessToken,
                               WireResult& result)
                    {
                        result.item = cloud.Read(accessToken, resource);
                    });
            }
            catch (...)
            {
                activeMailbox.reset();
                activeOperation.reset();
                activeFence.reset();
                throw;
            }
            status.state = CloudSaveSynchronizerState::Synchronizing;
        }

        void LaunchMutation(const CloudSavePendingMutation& pending)
        {
            auto mailbox = std::make_shared<WireMailbox>();
            auto fence = MakeFence(
                pending.resource,
                pending.mutationId);
            const auto operation = pending.kind == CloudSavePendingKind::Put
                ? WireOperation::Put
                : WireOperation::Delete;
            activeMailbox = mailbox;
            activeOperation = operation;
            activeFence = fence;
            try
            {
                LaunchDetached(
                    client,
                    token,
                    std::move(mailbox),
                    operation,
                    std::move(fence),
                    [pending](const CloudSaveClient& cloud,
                              const std::string_view accessToken,
                              WireResult& result)
                    {
                        if (pending.kind == CloudSavePendingKind::Put)
                        {
                            result.item = cloud.Put(
                                accessToken,
                                pending.resource,
                                pending.content,
                                pending.mutationId,
                                pending.baseEtag);
                        }
                        else
                        {
                            result.item = cloud.Delete(
                                accessToken,
                                pending.resource,
                                pending.mutationId,
                                *pending.baseEtag);
                        }
                    });
            }
            catch (...)
            {
                activeMailbox.reset();
                activeOperation.reset();
                activeFence.reset();
                throw;
            }
            status.state = CloudSaveSynchronizerState::Synchronizing;
        }

        [[nodiscard]] LocalInventory ReadLocalInventory();
        void Halt(CloudSaveSynchronizerStopReason reason) noexcept;
        void RefreshConflictStatus() noexcept;
        void ResetReconcileSession() noexcept;
        void EnterBackoff(
            const CloudSaveWireOutcome& outcome,
            std::uint64_t nowMilliseconds) noexcept;
        [[nodiscard]] bool HandleWireFailure(
            const CloudSaveWireOutcome& outcome,
            WireOperation operation,
            std::uint64_t nowMilliseconds);
        [[nodiscard]] bool ApplyRemoteSnapshot(
            const CloudSaveSnapshot& snapshot,
            const LocalPersistenceDocument& observed,
            const CloudSaveResource* localIdentity = nullptr);
        void PrepareLocalDelete(const CloudSaveResource& resource);
        void NoteRemoteDeletion(const CloudSaveResource& resource) noexcept;
        void CaptureDurableLocalDeleteIntents();
        void ValidateJournalDocuments();
        void QueueLocalMutation(
            const CloudSaveResource& resource,
            const LocalPersistenceDocument& local,
            const std::optional<CloudSaveSnapshot>& baseline);
        void QueueOverlayAfterSuccess(
            const CloudSaveResource& resource,
            const CloudSaveSnapshot& snapshot);
        void HandleManifestResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        void HandleReadResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        void HandleMutationResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        void HandleWireResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        [[nodiscard]] bool StartPendingMutation(
            const LocalInventory& inventory);
        [[nodiscard]] bool ReconcileOne(
            const LocalInventory& inventory);
        void CheckpointLocalStateForDetach();
        void Tick(std::uint64_t nowMilliseconds);

        PlayerPrefs& preferences;
        SaveDataStore& saves;
        std::shared_ptr<const CloudSaveClient> client;
        CloudSaveMutationIdGenerator mutationIdGenerator;
        std::thread::id ownerThread;

        CloudSaveJournal* journal{};
        std::uint64_t profileEpoch{};
        std::uint64_t sessionSerial{};
        std::string accountStorageKey;
        std::shared_ptr<SecretString> token;

        std::shared_ptr<WireMailbox> activeMailbox;
        std::shared_ptr<WireMailbox> retiredMailbox;
        std::optional<WireOperation> activeOperation;
        std::optional<RequestFence> activeFence;
        bool activeReadInvalidated{};
        std::optional<std::vector<CloudSaveManifestItem>> manifest;
        std::vector<CloudSaveResource> processed;
        std::optional<std::uint64_t> validatedJournalGeneration;
        bool reconcileRequested{};
        bool deleteIntentCapturePending{};
        std::uint32_t retryAttempt{};
        std::uint64_t lastTickMilliseconds{};
        CloudSaveSynchronizerStatus status;
        LocalInventory lastObservedInventory;
        bool hasLastObservedInventory{};
        bool authorizedWireSuccessPending{};
    };

    LocalInventory
        CloudSaveSynchronizer::Implementation::ReadLocalInventory()
    {
        LocalInventory inventory;
        std::uint64_t totalBytes{};

        // dirty memoryやload-failure中のPlayerPrefsをdisk snapshotだけで
        // 同期すると、remote applyで未保存値を失うため明示Save/repair待ちです。
        if (preferences.IsDirty() || preferences.HasLoadFailure())
        {
            Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
            return inventory;
        }

        auto preferencesDocument =
            LocalPersistenceDocuments::ReadPlayerPrefs(preferences);
        if (preferencesDocument.state
            == LocalPersistenceDocumentState::Unavailable)
        {
            Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
            return inventory;
        }
        if (preferencesDocument.state
            == LocalPersistenceDocumentState::Corrupt)
        {
            Halt(CloudSaveSynchronizerStopReason::LocalCorrupt);
            return inventory;
        }
        if (preferencesDocument.state
            == LocalPersistenceDocumentState::Loaded)
        {
            totalBytes = preferencesDocument.bytes.size();
        }
        inventory.resources.push_back({
            CloudSaveResource::Preferences(),
            std::move(preferencesDocument)
        });

        const auto listing = LocalPersistenceDocuments::ListSaveData(saves);
        if (listing.state == LocalPersistenceDocumentState::Unavailable)
        {
            Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
            return {};
        }
        if (listing.state == LocalPersistenceDocumentState::Corrupt)
        {
            Halt(CloudSaveSynchronizerStopReason::LocalCorrupt);
            return {};
        }
        if (listing.state == LocalPersistenceDocumentState::Missing)
        {
            return inventory;
        }

        for (const auto& slot : listing.slots)
        {
            auto document =
                LocalPersistenceDocuments::ReadSaveData(saves, slot);
            // 列挙後に消えた場合も外部processとの競合なので、削除として
            // 推測せずUnavailableで停止します。
            if (document.state == LocalPersistenceDocumentState::Unavailable
                || document.state == LocalPersistenceDocumentState::Missing)
            {
                Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
                return {};
            }
            if (document.state == LocalPersistenceDocumentState::Corrupt)
            {
                Halt(CloudSaveSynchronizerStopReason::LocalCorrupt);
                return {};
            }
            if (document.bytes.size()
                > CloudSaveAccountMaxBytes - totalBytes)
            {
                Halt(CloudSaveSynchronizerStopReason::LocalCorrupt);
                return {};
            }
            totalBytes += document.bytes.size();
            inventory.resources.push_back({
                CloudSaveResource::SaveSlot(slot),
                std::move(document)
            });
        }
        return inventory;
    }

    void CloudSaveSynchronizer::Implementation::Halt(
        const CloudSaveSynchronizerStopReason reason) noexcept
    {
        reconcileRequested = false;
        manifest.reset();
        processed.clear();
        status.state = CloudSaveSynchronizerState::Halted;
        status.stopReason = reason;
        status.retryAtMilliseconds = 0u;
    }

    void CloudSaveSynchronizer::Implementation::RefreshConflictStatus() noexcept
    {
        std::size_t count{};
        try
        {
            if (journal)
            {
                for (const auto& resource : journal->Resources())
                {
                    if (journal->Conflict(resource))
                    {
                        ++count;
                    }
                }
            }
        }
        catch (...)
        {
            Halt(CloudSaveSynchronizerStopReason::InternalFailure);
            return;
        }
        status.conflictCount = count;
        if (!activeMailbox
            && status.state != CloudSaveSynchronizerState::BackingOff
            && status.state != CloudSaveSynchronizerState::Unauthorized
            && status.state != CloudSaveSynchronizerState::Halted
            && status.state != CloudSaveSynchronizerState::Detached)
        {
            status.state = count != 0u
                ? CloudSaveSynchronizerState::Conflict
                : (reconcileRequested
                    ? CloudSaveSynchronizerState::Synchronizing
                    : CloudSaveSynchronizerState::Idle);
        }
    }

    void CloudSaveSynchronizer::Implementation::ResetReconcileSession() noexcept
    {
        manifest.reset();
        processed.clear();
        reconcileRequested = true;
    }

    void CloudSaveSynchronizer::Implementation::EnterBackoff(
        const CloudSaveWireOutcome& outcome,
        const std::uint64_t nowMilliseconds) noexcept
    {
        std::uint64_t delay{};
        if (outcome.status == CloudSaveWireStatus::RateLimited)
        {
            const auto seconds = (std::max)(
                std::uint64_t{ 1u },
                static_cast<std::uint64_t>(outcome.retryAfterSeconds));
            delay = seconds > MaximumRateLimitDelayMilliseconds / 1000u
                ? MaximumRateLimitDelayMilliseconds
                : seconds * 1000u;
        }
        else
        {
            const auto shift = (std::min)(retryAttempt, 6u);
            delay = InitialRetryDelayMilliseconds << shift;
            delay = (std::min)(delay, MaximumRetryDelayMilliseconds);
            if (retryAttempt != std::numeric_limits<std::uint32_t>::max())
            {
                ++retryAttempt;
            }
        }
        status.state = CloudSaveSynchronizerState::BackingOff;
        status.stopReason = CloudSaveSynchronizerStopReason::None;
        status.retryAtMilliseconds = SaturatingAdd(nowMilliseconds, delay);
    }

    bool CloudSaveSynchronizer::Implementation::HandleWireFailure(
        const CloudSaveWireOutcome& outcome,
        const WireOperation operation,
        const std::uint64_t nowMilliseconds)
    {
        if (outcome.status == CloudSaveWireStatus::Succeeded)
        {
            retryAttempt = 0u;
            status.retryAtMilliseconds = 0u;
            return false;
        }
        if (outcome.status == CloudSaveWireStatus::Unauthorized)
        {
            status.state = CloudSaveSynchronizerState::Unauthorized;
            status.stopReason = CloudSaveSynchronizerStopReason::None;
            status.retryAtMilliseconds = 0u;
            return true;
        }
        if (outcome.status == CloudSaveWireStatus::RateLimited
            || outcome.status == CloudSaveWireStatus::RetryableServiceError
            || outcome.status == CloudSaveWireStatus::TransportError
            || (operation == WireOperation::Read
                && outcome.status == CloudSaveWireStatus::NotFound))
        {
            if (operation == WireOperation::Manifest
                || operation == WireOperation::Read)
            {
                ResetReconcileSession();
            }
            EnterBackoff(outcome, nowMilliseconds);
            return true;
        }

        if (outcome.status == CloudSaveWireStatus::InvalidResponse)
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
        }
        else if (outcome.status == CloudSaveWireStatus::InvalidRequest)
        {
            Halt(CloudSaveSynchronizerStopReason::InternalFailure);
        }
        else
        {
            Halt(CloudSaveSynchronizerStopReason::RemoteRejected);
        }
        return true;
    }

    bool CloudSaveSynchronizer::Implementation::ApplyRemoteSnapshot(
        const CloudSaveSnapshot& snapshot,
        const LocalPersistenceDocument& observed,
        const CloudSaveResource* const localIdentity)
    {
        ValidateFullSnapshotDocument(snapshot);
        LocalPersistenceConditionalApplyResult result{};
        if (snapshot.resource.kind == CloudSaveResourceKind::Preferences)
        {
            if (snapshot.deleted)
            {
                result = LocalPersistenceDocuments::
                    DeletePlayerPrefsIfUnchanged(preferences, observed);
            }
            else
            {
                result = LocalPersistenceDocuments::
                    ApplyPlayerPrefsIfUnchanged(
                    preferences,
                    observed,
                    snapshot.content);
            }
        }
        else
        {
            // Windows ordinal-ignore-caseで同一なlocal slotが既にある場合、
            // remote側のcaseではなく検証済みlocal documentの綴りを使います。
            // `Save`/`save`が同一fileなのにslot identity不一致でCorrupt扱い
            // したり、case-sensitive volumeで別fileを作ることを防ぎます。
            const std::string_view slot = localIdentity
                    && localIdentity->kind
                        == CloudSaveResourceKind::SaveSlot
                    && SameResource(*localIdentity, snapshot.resource)
                ? std::string_view(localIdentity->slot)
                : std::string_view(snapshot.resource.slot);
            if (snapshot.deleted)
            {
                result = LocalPersistenceDocuments::DeleteSaveDataIfUnchanged(
                    saves,
                    slot,
                    observed);
            }
            else
            {
                result = LocalPersistenceDocuments::ApplySaveDataIfUnchanged(
                    saves,
                    slot,
                    observed,
                    snapshot.content);
            }
        }
        const bool applied = result
            == LocalPersistenceConditionalApplyResult::Applied;
        if (applied && snapshot.deleted)
        {
            NoteRemoteDeletion(snapshot.resource);
        }
        return applied;
    }

    void CloudSaveSynchronizer::Implementation::NoteRemoteDeletion(
        const CloudSaveResource& resource) noexcept
    {
        if (!hasLastObservedInventory)
        {
            return;
        }
        for (auto& local : lastObservedInventory.resources)
        {
            if (SameResource(local.resource, resource))
            {
                // allocationを伴わない更新だけをdisk commit後に行います。
                local.document.bytes.clear();
                local.document.state =
                    LocalPersistenceDocumentState::Missing;
                return;
            }
        }
    }

    void CloudSaveSynchronizer::Implementation::PrepareLocalDelete(
        const CloudSaveResource& resource)
    {
        RequireOwner();
        if (!journal || !IsValidResource(resource))
        {
            throw std::logic_error(
                "Cloud save delete checkpoint is not active.");
        }
        const auto local = resource.kind
                == CloudSaveResourceKind::Preferences
            ? LocalPersistenceDocuments::ReadPlayerPrefs(preferences)
            : LocalPersistenceDocuments::ReadSaveData(
                saves,
                resource.slot);
        if (local.state == LocalPersistenceDocumentState::Missing)
        {
            return;
        }
        if (local.state != LocalPersistenceDocumentState::Loaded)
        {
            throw std::runtime_error(
                "Local persistence cannot be checkpointed for deletion.");
        }

        // baselineのstrong ETagは同じjournal entryに残るため、intent処理時に
        // remote最新版ではなくdelete開始時の既知revisionへCASできます。
        journal->RecordLocalDeleteIntent(resource);
        if (activeOperation == WireOperation::Read
            && activeFence
            && SameResource(activeFence->resource, resource))
        {
            activeReadInvalidated = true;
        }
        reconcileRequested = true;
    }

    void CloudSaveSynchronizer::Implementation::
        CaptureDurableLocalDeleteIntents()
    {
        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        if (hasLastObservedInventory)
        {
            for (const auto& previous : lastObservedInventory.resources)
            {
                if (previous.document.state
                    != LocalPersistenceDocumentState::Loaded)
                {
                    continue;
                }
                const auto* current = FindLocal(inventory, previous.resource);
                if (!current
                    || current->state == LocalPersistenceDocumentState::Missing)
                {
                    // ETag取得済みのimmutable pending Deleteが既にある場合、
                    // ETag未取得期間用のintentを重ねて復活させません。
                    if (!journal->Pending(previous.resource))
                    {
                        journal->RecordLocalDeleteIntent(previous.resource);
                    }
                }
            }
        }
        for (const auto& current : inventory.resources)
        {
            if (current.document.state == LocalPersistenceDocumentState::Loaded
                && journal->HasLocalDeleteIntent(current.resource))
            {
                journal->ClearLocalDeleteIntent(current.resource);
            }
        }
        lastObservedInventory = inventory;
        hasLastObservedInventory = true;
    }

    void CloudSaveSynchronizer::Implementation::ValidateJournalDocuments()
    {
        const auto generation = journal->Generation();
        if (validatedJournalGeneration == generation)
        {
            return;
        }
        for (const auto& resource : journal->Resources())
        {
            if (const auto baseline = journal->Baseline(resource))
            {
                ValidateFullSnapshotDocument(*baseline);
            }
            if (const auto conflict = journal->Conflict(resource))
            {
                ValidateFullSnapshotDocument(*conflict);
            }
            if (const auto pending = journal->Pending(resource))
            {
                ValidatePendingDocument(*pending);
            }
        }
        validatedJournalGeneration = generation;
    }

    void CloudSaveSynchronizer::Implementation::QueueLocalMutation(
        const CloudSaveResource& resource,
        const LocalPersistenceDocument& local,
        const std::optional<CloudSaveSnapshot>& baseline)
    {
        const auto mutationId = NewMutationId();
        if (local.state == LocalPersistenceDocumentState::Loaded)
        {
            journal->QueuePut(
                resource,
                local.bytes,
                mutationId,
                baseline
                    ? std::optional<std::string>{ baseline->etag }
                    : std::nullopt);
        }
        else if (local.state == LocalPersistenceDocumentState::Missing
            && baseline && !baseline->deleted)
        {
            journal->QueueDelete(resource, mutationId, baseline->etag);
        }
    }

    void CloudSaveSynchronizer::Implementation::QueueOverlayAfterSuccess(
        const CloudSaveResource& resource,
        const CloudSaveSnapshot& snapshot)
    {
        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        LocalPersistenceDocument missing;
        missing.state = LocalPersistenceDocumentState::Missing;
        const auto* found = FindLocal(inventory, resource);
        const auto& local = found ? *found : missing;
        const std::optional<CloudSaveSnapshot> baseline{ snapshot };
        if (!LocalMatchesBaseline(local, baseline))
        {
            QueueLocalMutation(resource, local, baseline);
        }
    }

    void CloudSaveSynchronizer::Implementation::HandleManifestResult(
        WireResult result,
        const std::uint64_t nowMilliseconds)
    {
        if (!FenceMatches(result.fence, false))
        {
            ResetReconcileSession();
            return;
        }
        if (HandleWireFailure(
                result.manifest.outcome,
                WireOperation::Manifest,
                nowMilliseconds))
        {
            return;
        }
        authorizedWireSuccessPending = true;

        std::size_t saveSlots{};
        std::uint64_t totalBytes{};
        std::vector<CloudSaveResource> identities;
        for (const auto& item : result.manifest.items)
        {
            if (!IsValidResource(item.resource)
                || ContainsResource(identities, item.resource))
            {
                Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                return;
            }
            AddResource(identities, item.resource);
            if (item.resource.kind == CloudSaveResourceKind::SaveSlot
                && ++saveSlots > CloudSaveMaxSlots)
            {
                Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                return;
            }
            if (item.byteLength > CloudSaveAccountMaxBytes - totalBytes)
            {
                Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                return;
            }
            totalBytes += item.byteLength;
        }
        manifest = std::move(result.manifest.items);
        processed.clear();
        reconcileRequested = true;
        retryAttempt = 0u;
        status.state = CloudSaveSynchronizerState::Synchronizing;
    }

    void CloudSaveSynchronizer::Implementation::HandleReadResult(
        WireResult result,
        const std::uint64_t nowMilliseconds)
    {
        const bool invalidated = std::exchange(
            activeReadInvalidated,
            false);
        if (!FenceMatches(result.fence, false))
        {
            ResetReconcileSession();
            return;
        }
        if (HandleWireFailure(
                result.item.outcome,
                WireOperation::Read,
                nowMilliseconds))
        {
            return;
        }
        authorizedWireSuccessPending = true;
        if (!result.item.snapshot
            || !SameResource(
                result.item.snapshot->resource,
                result.fence.resource))
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
            return;
        }
        try
        {
            ValidateFullSnapshotDocument(*result.item.snapshot);
        }
        catch (...)
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
            return;
        }

        const auto* expected = manifest
            ? FindManifest(*manifest, result.fence.resource)
            : nullptr;
        if (!expected)
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
            return;
        }
        if (expected->deleted
            || result.item.snapshot->deleted
            || expected->etag != result.item.snapshot->etag
            || expected->byteLength != result.item.snapshot->content.size()
            || expected->sha256 != result.item.snapshot->sha256)
        {
            // manifest取得後の正当なremote更新競合です。異なるrevisionを
            // 古い三者比較へ適用せず、bounded backoff後にmanifestから再開します。
            ResetReconcileSession();
            CloudSaveWireOutcome race;
            race.status = CloudSaveWireStatus::RetryableServiceError;
            EnterBackoff(race, nowMilliseconds);
            return;
        }

        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        LocalPersistenceDocument missing;
        missing.state = LocalPersistenceDocumentState::Missing;
        const auto* localResource = FindLocalResource(
            inventory,
            result.fence.resource);
        const auto& local = localResource
            ? localResource->document
            : missing;
        const auto baseline = journal->Baseline(result.fence.resource);
        const std::optional<CloudSaveSnapshot> remote{
            *result.item.snapshot
        };

        if (invalidated)
        {
            if (journal->HasLocalDeleteIntent(result.fence.resource)
                && local.state == LocalPersistenceDocumentState::Missing)
            {
                // pre-delete WALを検証済みRead revisionでpendingへ昇格します。
                // 既存baselineがあればdelete開始時に観測したETagを優先し、
                // Read中のremote更新は412 conflictとして保持します。
                journal->QueueDelete(
                    result.fence.resource,
                    NewMutationId(),
                    baseline ? baseline->etag : remote->etag);
            }
            else if (!baseline
                && result.fence.localWasLoadedAtReadStart
                && local.state == LocalPersistenceDocumentState::Missing)
            {
                // baseline未作成のRead中に発生した明示deleteを「新端末の
                // 初期Missing」と混同しません。検証済みremote ETagへの
                // DeleteをWALへ記録してからwireへ進めます。
                QueueLocalMutation(result.fence.resource, local, remote);
            }
            // それ以外のlocal commitも旧判断では適用せず再manifestします。
            ResetReconcileSession();
            return;
        }

        if (LocalMatchesBaseline(local, remote))
        {
            journal->RecordBaseline(*result.item.snapshot);
        }
        else if (LocalMatchesBaseline(local, baseline))
        {
            if (!FitsAccountQuotaAfterApply(
                    inventory,
                    *result.item.snapshot))
            {
                Halt(CloudSaveSynchronizerStopReason::LocalCorrupt);
                return;
            }
            try
            {
                if (!ApplyRemoteSnapshot(
                        *result.item.snapshot,
                        local,
                        localResource ? &localResource->resource : nullptr))
                {
                    ResetReconcileSession();
                    return;
                }
            }
            catch (...)
            {
                Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
                return;
            }
            // disk+memoryのatomic applyが完了した後だけbaselineを進めます。
            journal->RecordBaseline(*result.item.snapshot);
        }
        else if (!baseline)
        {
            if (result.fence.localWasLoadedAtReadStart
                && local.state == LocalPersistenceDocumentState::Missing)
            {
                QueueLocalMutation(result.fence.resource, local, remote);
                ResetReconcileSession();
                return;
            }
            // 初回にlocal/remote双方が存在する場合は勝手に上書きしません。
            QueueLocalMutation(result.fence.resource, local, std::nullopt);
            const auto pending = journal->Pending(result.fence.resource);
            if (!pending)
            {
                throw std::runtime_error("Initial conflict queue failed.");
            }
            journal->RecordConflict(
                result.fence.resource,
                pending->mutationId,
                *result.item.snapshot);
        }
        else
        {
            // Read中にlocal overlayが進んだ場合も古いremoteを適用せず、
            // baseline ETagに対するCASとしてjournalへ先に記録します。
            QueueLocalMutation(result.fence.resource, local, baseline);
        }
        ResetReconcileSession();
        RefreshConflictStatus();
    }

    void CloudSaveSynchronizer::Implementation::HandleMutationResult(
        WireResult result,
        const std::uint64_t nowMilliseconds)
    {
        if (!FenceMatches(result.fence, true))
        {
            ResetReconcileSession();
            return;
        }
        if (result.item.outcome.status == CloudSaveWireStatus::Conflict)
        {
            authorizedWireSuccessPending = true;
            if (!result.item.conflictSnapshot
                || !SameResource(
                    result.item.conflictSnapshot->resource,
                    result.fence.resource))
            {
                Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                return;
            }
            try
            {
                ValidateFullSnapshotDocument(*result.item.conflictSnapshot);
            }
            catch (...)
            {
                Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                return;
            }
            journal->RecordConflict(
                result.fence.resource,
                result.fence.mutationId,
                *result.item.conflictSnapshot);
            ResetReconcileSession();
            RefreshConflictStatus();
            return;
        }
        if (HandleWireFailure(
                result.item.outcome,
                result.operation,
                nowMilliseconds))
        {
            return;
        }
        authorizedWireSuccessPending = true;
        if (!result.item.snapshot
            || !SameResource(
                result.item.snapshot->resource,
                result.fence.resource)
            || (result.operation == WireOperation::Put
                && result.item.snapshot->deleted)
            || (result.operation == WireOperation::Delete
                && !result.item.snapshot->deleted))
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
            return;
        }

        const auto pending = journal->Pending(result.fence.resource);
        try
        {
            ValidateFullSnapshotDocument(*result.item.snapshot);
        }
        catch (...)
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
            return;
        }
        if (!pending
            || (result.operation == WireOperation::Put
                && (pending->kind != CloudSavePendingKind::Put
                    || pending->content != result.item.snapshot->content
                    || pending->sha256 != result.item.snapshot->sha256))
            || (result.operation == WireOperation::Delete
                && pending->kind != CloudSavePendingKind::Delete))
        {
            Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
            return;
        }

        journal->RecordSuccess(
            result.fence.resource,
            result.fence.mutationId,
            *result.item.snapshot);
        // P1 ack後にdiskを再読し、P2 overlayだけを新UUID/P1 ETagでqueueします。
        QueueOverlayAfterSuccess(
            result.fence.resource,
            *result.item.snapshot);
        ResetReconcileSession();
        RefreshConflictStatus();
    }

    void CloudSaveSynchronizer::Implementation::HandleWireResult(
        WireResult result,
        const std::uint64_t nowMilliseconds)
    {
        switch (result.operation)
        {
        case WireOperation::Manifest:
            HandleManifestResult(std::move(result), nowMilliseconds);
            return;
        case WireOperation::Read:
            HandleReadResult(std::move(result), nowMilliseconds);
            return;
        case WireOperation::Put:
        case WireOperation::Delete:
            HandleMutationResult(std::move(result), nowMilliseconds);
            return;
        default:
            Halt(CloudSaveSynchronizerStopReason::InternalFailure);
            return;
        }
    }

    bool CloudSaveSynchronizer::Implementation::StartPendingMutation(
        const LocalInventory& inventory)
    {
        (void)inventory;
        auto pending = journal->Dispatchable();
        if (pending.empty())
        {
            return false;
        }
        const auto priority = [this](
            const CloudSavePendingMutation& mutation)
        {
            if (mutation.kind == CloudSavePendingKind::Delete)
            {
                return 0;
            }
            const auto baseline = journal->Baseline(mutation.resource);
            const auto before = baseline && !baseline->deleted
                ? baseline->content.size()
                : 0u;
            const auto after = mutation.content.size();
            if (after < before)
            {
                return 0;
            }
            return after > before ? 2 : 1;
        };
        std::stable_sort(
            pending.begin(),
            pending.end(),
            [&priority](const auto& left, const auto& right)
            {
                return priority(left) < priority(right);
            });
        if (!IsValidResource(pending.front().resource))
        {
            Halt(CloudSaveSynchronizerStopReason::JournalFailure);
            return false;
        }
        LaunchMutation(pending.front());
        return true;
    }

    bool CloudSaveSynchronizer::Implementation::ReconcileOne(
        const LocalInventory& inventory)
    {
        if (!manifest)
        {
            LaunchManifest();
            return true;
        }

        std::vector<CloudSaveResource> resources;
        AddResource(resources, CloudSaveResource::Preferences());
        for (const auto& local : inventory.resources)
        {
            AddResource(resources, local.resource);
        }
        for (const auto& remote : *manifest)
        {
            AddResource(resources, remote.resource);
        }
        for (const auto& persisted : journal->Resources())
        {
            AddResource(resources, persisted);
        }

        // remote削除/縮小をgrowthより先に適用します。remote/local双方の
        // 最終総量がquota内でも、名前順でgrowthを先に適用すると一時的に
        // quotaを越えて停止するためです。各適用後はmanifestから再開します。
        const auto quotaTransitionPriority =
            [&inventory, this](const CloudSaveResource& resource)
            {
                const auto* remote = FindManifest(*manifest, resource);
                if (journal->HasLocalDeleteIntent(resource)
                    && remote && !remote->deleted)
                {
                    return 0;
                }
                const auto* local = FindLocal(inventory, resource);
                const auto localBytes = local
                        && local->state
                            == LocalPersistenceDocumentState::Loaded
                    ? local->bytes.size()
                    : 0u;
                const auto remoteBytes = remote && !remote->deleted
                    ? remote->byteLength
                    : 0u;
                LocalPersistenceDocument missing;
                missing.state = LocalPersistenceDocumentState::Missing;
                const auto& effectiveLocal = local ? *local : missing;
                const auto baseline = journal->Baseline(resource);
                const bool localMatches =
                    LocalMatchesBaseline(effectiveLocal, baseline);
                const bool remoteMatches =
                    ManifestMatchesBaseline(remote, baseline);

                std::uint64_t before{};
                std::uint64_t after{};
                if (localMatches && !remoteMatches)
                {
                    // remote revisionをlocalへ適用します。
                    before = localBytes;
                    after = remoteBytes;
                }
                else if (!localMatches && remoteMatches)
                {
                    // local revisionをremoteへPUT/DELETEします。
                    before = remoteBytes;
                    after = localBytes;
                }
                else if (baseline && !localMatches && !remoteMatches)
                {
                    // 両側変更はfull read後、旧baselineへのlocal CASです。
                    before = remoteBytes;
                    after = localBytes;
                }
                else
                {
                    return 1;
                }
                if (after < before)
                {
                    return 0;
                }
                return after > before ? 2 : 1;
            };
        std::stable_sort(
            resources.begin(),
            resources.end(),
            [&quotaTransitionPriority](
                const CloudSaveResource& left,
                const CloudSaveResource& right)
            {
                return quotaTransitionPriority(left)
                    < quotaTransitionPriority(right);
            });

        for (const auto& resource : resources)
        {
            if (ContainsResource(processed, resource))
            {
                continue;
            }
            if (journal->Conflict(resource))
            {
                AddResource(processed, resource);
                continue;
            }
            if (journal->Pending(resource))
            {
                // 非conflict pendingはTick先頭でdispatchされるべきです。
                Halt(CloudSaveSynchronizerStopReason::JournalFailure);
                return false;
            }

            LocalPersistenceDocument missing;
            missing.state = LocalPersistenceDocumentState::Missing;
            const auto* localResource = FindLocalResource(
                inventory,
                resource);
            const auto& local = localResource
                ? localResource->document
                : missing;
            const auto baseline = journal->Baseline(resource);
            const auto* remote = FindManifest(*manifest, resource);
            if (journal->HasLocalDeleteIntent(resource))
            {
                if (local.state == LocalPersistenceDocumentState::Loaded)
                {
                    // delete後の再作成が既にdurableなら古いintentを破棄し、
                    // 通常の三者比較（必要なら初回conflict）へ戻します。
                    journal->ClearLocalDeleteIntent(resource);
                    ResetReconcileSession();
                    return true;
                }
                if (!remote)
                {
                    if (baseline)
                    {
                        Halt(
                            CloudSaveSynchronizerStopReason::
                                InvalidRemoteResponse);
                        return false;
                    }
                    journal->ClearLocalDeleteIntent(resource);
                    AddResource(processed, resource);
                    continue;
                }
                if (!remote->deleted)
                {
                    // ETag取得前に記録したintentを同じjournal publishで
                    // immutable pending Deleteへ昇格します。
                    journal->QueueDelete(
                        resource,
                        NewMutationId(),
                        baseline
                            ? baseline->etag
                            : remote->etag);
                    ResetReconcileSession();
                    return true;
                }
                const CloudSaveSnapshot tombstone{
                    resource,
                    remote->etag,
                    true,
                    {},
                    {}
                };
                journal->RecordBaseline(tombstone);
                ResetReconcileSession();
                return true;
            }
            if (baseline && !remote)
            {
                // 既知resourceの削除は必ずtombstoneとして残るwire契約です。
                Halt(CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                return false;
            }
            if (remote
                && remote->deleted
                && local.state == LocalPersistenceDocumentState::Missing
                && !ManifestMatchesBaseline(remote, baseline))
            {
                // remote tombstoneのlocal適用後、baseline publish前にcrashしても
                // Missing同士は収束済みです。旧baseline ETagでDELETEを再送せず、
                // 検証済みremote revisionへidempotentにbaselineを進めます。
                const CloudSaveSnapshot tombstone{
                    resource,
                    remote->etag,
                    true,
                    {},
                    {}
                };
                journal->RecordBaseline(tombstone);
                ResetReconcileSession();
                return true;
            }
            const bool localMatches = LocalMatchesBaseline(local, baseline);
            const bool remoteMatches =
                ManifestMatchesBaseline(remote, baseline);

            if (localMatches && remoteMatches)
            {
                AddResource(processed, resource);
                continue;
            }

            if (localMatches && !remoteMatches)
            {
                if (!remote)
                {
                    // serverは削除をstrong ETag付きtombstoneで返す契約です。
                    // 既知baselineをmanifestから無言で消す応答は適用しません。
                    Halt(
                        CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                    return false;
                }
                if (!remote->deleted)
                {
                    LaunchRead(resource, remote->etag, local.state);
                    return true;
                }
                CloudSaveSnapshot tombstone{
                    resource,
                    remote->etag,
                    true,
                    {},
                    {}
                };
                try
                {
                    if (!ApplyRemoteSnapshot(
                            tombstone,
                            local,
                            localResource
                                ? &localResource->resource
                                : nullptr))
                    {
                        ResetReconcileSession();
                        return true;
                    }
                }
                catch (...)
                {
                    Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
                    return false;
                }
                journal->RecordBaseline(tombstone);
                ResetReconcileSession();
                return true;
            }

            if (!baseline && remote)
            {
                // 初回localとremoteの双方が変化済みです。live remoteはfull
                // snapshotをReadしてから、tombstoneは今ここでconflict化します。
                if (!remote->deleted)
                {
                    LaunchRead(resource, remote->etag, local.state);
                    return true;
                }
                QueueLocalMutation(resource, local, std::nullopt);
                const auto pending = journal->Pending(resource);
                if (!pending)
                {
                    throw std::runtime_error("Initial conflict queue failed.");
                }
                const CloudSaveSnapshot tombstone{
                    resource,
                    remote->etag,
                    true,
                    {},
                    {}
                };
                journal->RecordConflict(
                    resource,
                    pending->mutationId,
                    tombstone);
                ResetReconcileSession();
                RefreshConflictStatus();
                return true;
            }

            if (!remoteMatches && remote && !remote->deleted)
            {
                // 両側変更でもfull snapshotを検証します。localが既にremoteと
                // 同一なら、直前のlocal apply後journal publish失敗から安全に
                // baselineだけを回復できます。異なればRead完了後にCASします。
                LaunchRead(resource, remote->etag, local.state);
                return true;
            }

            // remote==baselineならlocalだけの変更、双方変更なら古いbaseline
            // ETagへのCASです。いずれもnetworkより先にWALをpublishします。
            QueueLocalMutation(resource, local, baseline);
            if (!journal->Pending(resource))
            {
                // tombstone baselineに対するlocal missing等、実質変更なしです。
                AddResource(processed, resource);
                continue;
            }
            ResetReconcileSession();
            return true;
        }

        reconcileRequested = false;
        manifest.reset();
        processed.clear();
        RefreshConflictStatus();
        return false;
    }

    void CloudSaveSynchronizer::Implementation::
        CheckpointLocalStateForDetach()
    {
        RequireOwner();
        if (!journal)
        {
            return;
        }

        // 完了待ちせず世代を進め、以降のmailbox結果がjournal/localへ
        // 触れないようにします。workerはshared client/mailboxだけで完走します。
        ++sessionSerial;
        RetireActiveMailbox();

        // remote由来の停止状態でもsign-out時のlocal deleteは失えません。
        // strict scanで新たなlocal異常を検出した場合だけ下で失敗させます。
        status.state = CloudSaveSynchronizerState::Synchronizing;
        status.stopReason = CloudSaveSynchronizerStopReason::None;
        status.retryAtMilliseconds = 0u;
        CaptureDurableLocalDeleteIntents();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            throw std::runtime_error(
                "Local persistence could not be checkpointed.");
        }
        deleteIntentCapturePending = false;
        reconcileRequested = false;
    }

    void CloudSaveSynchronizer::Implementation::Tick(
        const std::uint64_t nowMilliseconds)
    {
        RequireOwner();
        lastTickMilliseconds = nowMilliseconds;
        ReapRetiredMailbox();
        if (!journal)
        {
            status.state = CloudSaveSynchronizerState::Detached;
            return;
        }
        if (retiredMailbox)
        {
            status.state = CloudSaveSynchronizerState::Synchronizing;
            return;
        }
        if (status.state == CloudSaveSynchronizerState::Unauthorized
            || status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        if (status.state == CloudSaveSynchronizerState::BackingOff)
        {
            if (nowMilliseconds < status.retryAtMilliseconds)
            {
                return;
            }
            status.state = CloudSaveSynchronizerState::Synchronizing;
            status.retryAtMilliseconds = 0u;
        }

        try
        {
            if (const auto result = PollActive())
            {
                HandleWireResult(
                    std::move(*result),
                    nowMilliseconds);
            }
            if (activeMailbox
                || status.state == CloudSaveSynchronizerState::BackingOff
                || status.state == CloudSaveSynchronizerState::Unauthorized
                || status.state == CloudSaveSynchronizerState::Halted)
            {
                return;
            }
            if (!reconcileRequested)
            {
                return;
            }

            if (deleteIntentCapturePending)
            {
                CaptureDurableLocalDeleteIntents();
                if (status.state == CloudSaveSynchronizerState::Halted)
                {
                    return;
                }
                deleteIntentCapturePending = false;
            }

            // 書込み系wireを始める前にprefs・全slot・account quotaを
            // strict scanします。Unavailable/Corrupt時はupload/deleteしません。
            const auto inventory = ReadLocalInventory();
            if (status.state == CloudSaveSynchronizerState::Halted)
            {
                return;
            }
            lastObservedInventory = inventory;
            hasLastObservedInventory = true;
            try
            {
                ValidateJournalDocuments();
            }
            catch (...)
            {
                Halt(CloudSaveSynchronizerStopReason::JournalFailure);
                return;
            }
            if (StartPendingMutation(inventory))
            {
                return;
            }
            (void)ReconcileOne(inventory);
        }
        catch (const CloudSaveJournalBusyError&)
        {
            deleteIntentCapturePending = true;
            ResetReconcileSession();
            CloudSaveWireOutcome busy;
            busy.status = CloudSaveWireStatus::RetryableServiceError;
            EnterBackoff(busy, nowMilliseconds);
        }
        catch (const std::invalid_argument&)
        {
            Halt(CloudSaveSynchronizerStopReason::InternalFailure);
        }
        catch (const std::logic_error&)
        {
            Halt(CloudSaveSynchronizerStopReason::JournalFailure);
        }
        catch (...)
        {
            Halt(CloudSaveSynchronizerStopReason::JournalFailure);
        }
    }

    CloudSaveSynchronizer::CloudSaveSynchronizer(
        PlayerPrefs& preferences,
        SaveDataStore& saves,
        std::shared_ptr<const CloudSaveClient> client,
        CloudSaveMutationIdGenerator mutationIdGenerator)
        : m_implementation(std::make_unique<Implementation>(
            preferences,
            saves,
            std::move(client),
            std::move(mutationIdGenerator)))
    {
    }

    CloudSaveSynchronizer::~CloudSaveSynchronizer() = default;

    void CloudSaveSynchronizer::Attach(
        CloudSaveJournal& journal,
        const std::uint64_t profileEpoch,
        std::string accountStorageKey,
        std::string accessToken)
    {
        auto prepared = PrepareAttachment(
            journal,
            m_implementation->preferences.FilePath(),
            m_implementation->saves.Directory(),
            std::move(accountStorageKey),
            std::move(accessToken));
        CommitPreparedAttachment(std::move(prepared), profileEpoch);
    }

    PreparedCloudSaveAttachment CloudSaveSynchronizer::PrepareAttachment(
        CloudSaveJournal& journal,
        const std::filesystem::path& accountPreferencesPath,
        const std::filesystem::path& accountSaveDirectory,
        std::string accountStorageKey,
        std::string accessToken)
    {
        auto& implementation = *m_implementation;
        implementation.RequireOwner();
        if (!IsAccountStorageKey(accountStorageKey)
            || !IsSafeOnlineBearerToken(accessToken))
        {
            EraseSecret(accessToken);
            EraseSecret(accountStorageKey);
            throw std::invalid_argument(
                "Invalid cloud save synchronizer binding.");
        }
        bool bindingMatches{};
        try
        {
            const auto journalPath =
                journal.FilePath().lexically_normal();
            const auto keyDirectory = journalPath.parent_path();
            const auto stateRoot = keyDirectory.parent_path();
            const auto engineRoot = stateRoot.parent_path();
            const auto accountRoot = (
                engineRoot
                / L"OnlineProfiles"
                / std::filesystem::path(accountStorageKey)).lexically_normal();
            bindingMatches = journalPath.is_absolute()
                && journalPath.filename() == L"CloudSaveJournal.json"
                && keyDirectory.filename()
                    == std::filesystem::path(accountStorageKey)
                && stateRoot.filename() == L"OnlineState"
                && accountPreferencesPath.lexically_normal()
                    == accountRoot / L"PlayerPrefs.json"
                && accountSaveDirectory.lexically_normal()
                    == accountRoot / L"Saves";
        }
        catch (...)
        {
            EraseSecret(accessToken);
            EraseSecret(accountStorageKey);
            throw std::invalid_argument(
                "Invalid cloud save synchronizer binding.");
        }
        if (!bindingMatches)
        {
            EraseSecret(accessToken);
            EraseSecret(accountStorageKey);
            throw std::invalid_argument(
                "Invalid cloud save synchronizer binding.");
        }

        std::shared_ptr<SecretString> preparedToken;
        try
        {
            preparedToken = std::make_shared<SecretString>(accessToken);
        }
        catch (...)
        {
            EraseSecret(accessToken);
            EraseSecret(accountStorageKey);
            throw;
        }
        EraseSecret(accessToken);
        // publish前にtarget journalと初期local snapshotを完全検証します。
        std::size_t conflictCount{};
        LocalInventory initialInventory;
        try
        {
            const auto generation = journal.Generation();
            (void)generation;
            for (const auto& resource : journal.Resources())
            {
                if (const auto baseline = journal.Baseline(resource))
                {
                    ValidateFullSnapshotDocument(*baseline);
                }
                if (const auto pending = journal.Pending(resource))
                {
                    ValidatePendingDocument(*pending);
                }
                if (journal.Conflict(resource))
                {
                    ValidateFullSnapshotDocument(
                        *journal.Conflict(resource));
                    ++conflictCount;
                }
            }

            PlayerPrefs preparedPreferences(accountPreferencesPath);
            auto preferencesDocument =
                LocalPersistenceDocuments::ReadPlayerPrefs(
                    preparedPreferences);
            if (preferencesDocument.state
                    == LocalPersistenceDocumentState::Unavailable
                || preferencesDocument.state
                    == LocalPersistenceDocumentState::Corrupt)
            {
                throw std::runtime_error(
                    "Cloud save local persistence is unavailable.");
            }
            std::uint64_t totalBytes =
                preferencesDocument.state
                        == LocalPersistenceDocumentState::Loaded
                    ? preferencesDocument.bytes.size()
                    : 0u;
            initialInventory.resources.push_back({
                CloudSaveResource::Preferences(),
                std::move(preferencesDocument)
            });

            SaveDataStore preparedSaves(accountSaveDirectory);
            const auto listing =
                LocalPersistenceDocuments::ListSaveData(preparedSaves);
            if (listing.state == LocalPersistenceDocumentState::Unavailable
                || listing.state == LocalPersistenceDocumentState::Corrupt)
            {
                throw std::runtime_error(
                    "Cloud save local persistence is unavailable.");
            }
            if (listing.state == LocalPersistenceDocumentState::Loaded)
            {
                for (const auto& slot : listing.slots)
                {
                    auto document = LocalPersistenceDocuments::ReadSaveData(
                        preparedSaves,
                        slot);
                    if (document.state
                        != LocalPersistenceDocumentState::Loaded)
                    {
                        throw std::runtime_error(
                            "Cloud save local persistence changed while preparing.");
                    }
                    if (document.bytes.size()
                        > CloudSaveAccountMaxBytes - totalBytes)
                    {
                        throw std::runtime_error(
                            "Cloud save local persistence exceeds quota.");
                    }
                    totalBytes += document.bytes.size();
                    initialInventory.resources.push_back({
                        CloudSaveResource::SaveSlot(slot),
                        std::move(document)
                    });
                }
            }
        }
        catch (...)
        {
            EraseSecret(accountStorageKey);
            throw;
        }

        auto state = std::make_unique<PreparedCloudSaveAttachment::State>();
        state->owner = &implementation;
        state->journal = &journal;
        state->accountStorageKey = std::move(accountStorageKey);
        state->token = std::move(preparedToken);
        state->initialInventory = std::move(initialInventory);
        state->conflictCount = conflictCount;
        return PreparedCloudSaveAttachment(std::move(state));
    }

    bool CloudSaveSynchronizer::CanCommitPreparedAttachment(
        const PreparedCloudSaveAttachment& prepared) const noexcept
    {
        return prepared.m_state
            && prepared.m_state->owner == m_implementation.get()
            && prepared.m_state->journal != nullptr
            && prepared.m_state->token != nullptr;
    }

    void CloudSaveSynchronizer::CommitPreparedAttachment(
        PreparedCloudSaveAttachment&& prepared,
        const std::uint64_t profileEpoch) noexcept
    {
        auto& implementation = *m_implementation;
        if (!CanCommitPreparedAttachment(prepared) || profileEpoch == 0u)
        {
            implementation.DetachNoThrow();
            return;
        }
        auto state = std::move(prepared.m_state);
        ++implementation.sessionSerial;
        implementation.RetireActiveMailbox();
        implementation.journal = state->journal;
        implementation.profileEpoch = profileEpoch;
        implementation.ClearAccountStorageKey();
        implementation.accountStorageKey =
            std::move(state->accountStorageKey);
        implementation.token = std::move(state->token);
        implementation.manifest.reset();
        implementation.processed.clear();
        implementation.lastObservedInventory =
            std::move(state->initialInventory);
        implementation.hasLastObservedInventory = true;
        implementation.validatedJournalGeneration.reset();
        implementation.reconcileRequested = true;
        implementation.deleteIntentCapturePending = false;
        implementation.retryAttempt = 0u;
        implementation.authorizedWireSuccessPending = false;
        implementation.status = {};
        implementation.status.conflictCount = state->conflictCount;
        implementation.status.state =
            CloudSaveSynchronizerState::Synchronizing;
    }

    void CloudSaveSynchronizer::Detach() noexcept
    {
        m_implementation->DetachNoThrow();
    }

    void CloudSaveSynchronizer::CheckpointLocalStateForDetach()
    {
        m_implementation->CheckpointLocalStateForDetach();
    }

    void CloudSaveSynchronizer::UpdateAccessToken(std::string accessToken)
    {
        auto& implementation = *m_implementation;
        implementation.RequireOwner();
        if (!implementation.journal
            || !IsSafeOnlineBearerToken(accessToken))
        {
            EraseSecret(accessToken);
            throw std::invalid_argument(
                "Invalid cloud save access token update.");
        }
        std::shared_ptr<SecretString> preparedToken;
        try
        {
            preparedToken = std::make_shared<SecretString>(accessToken);
        }
        catch (...)
        {
            EraseSecret(accessToken);
            throw;
        }
        EraseSecret(accessToken);

        ++implementation.sessionSerial;
        implementation.RetireActiveMailbox();
        implementation.token = std::move(preparedToken);
        implementation.ResetReconcileSession();
        implementation.retryAttempt = 0u;
        implementation.authorizedWireSuccessPending = false;
        implementation.status.stopReason =
            CloudSaveSynchronizerStopReason::None;
        implementation.status.retryAtMilliseconds = 0u;
        implementation.status.state =
            CloudSaveSynchronizerState::Synchronizing;
    }

    void CloudSaveSynchronizer::RequestReconcile()
    {
        auto& implementation = *m_implementation;
        implementation.RequireOwner();
        if (!implementation.journal)
        {
            return;
        }
        if (implementation.status.state
                == CloudSaveSynchronizerState::Halted
            && (implementation.status.stopReason
                    == CloudSaveSynchronizerStopReason::LocalUnavailable
                || implementation.status.stopReason
                    == CloudSaveSynchronizerStopReason::LocalCorrupt))
        {
            implementation.status.stopReason =
                CloudSaveSynchronizerStopReason::None;
            implementation.status.state =
                CloudSaveSynchronizerState::Synchronizing;
        }
        implementation.deleteIntentCapturePending = true;
        try
        {
            implementation.CaptureDurableLocalDeleteIntents();
            implementation.deleteIntentCapturePending = false;
        }
        catch (const CloudSaveJournalBusyError&)
        {
            if (implementation.activeMailbox)
            {
                if (implementation.activeOperation
                    == WireOperation::Read)
                {
                    implementation.activeReadInvalidated = true;
                }
                implementation.reconcileRequested = true;
            }
            else
            {
                implementation.ResetReconcileSession();
            }
            CloudSaveWireOutcome busy;
            busy.status = CloudSaveWireStatus::RetryableServiceError;
            implementation.EnterBackoff(
                busy,
                implementation.lastTickMilliseconds);
            return;
        }
        catch (...)
        {
            implementation.Halt(
                CloudSaveSynchronizerStopReason::JournalFailure);
            return;
        }
        if (implementation.status.state
            == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        if (implementation.activeMailbox)
        {
            implementation.reconcileRequested = true;
            if (implementation.activeOperation
                == WireOperation::Read)
            {
                implementation.activeReadInvalidated = true;
                // baseline無しのRead中にLoaded→Missingとなったdeleteは、
                // 次回起動で「初期Missing」と区別できません。manifestで既に
                // 得たstrong ETagを使い、このsignal処理中にWAL化します。
                try
                {
                    if (implementation.activeFence
                        && implementation.activeFence
                            ->localWasLoadedAtReadStart
                        && !implementation.journal->Baseline(
                            implementation.activeFence->resource))
                    {
                        const auto& resource =
                            implementation.activeFence->resource;
                        const auto local = resource.kind
                                == CloudSaveResourceKind::Preferences
                            ? LocalPersistenceDocuments::ReadPlayerPrefs(
                                implementation.preferences)
                            : LocalPersistenceDocuments::ReadSaveData(
                                implementation.saves,
                                resource.slot);
                        if (local.state
                            == LocalPersistenceDocumentState::Missing)
                        {
                            implementation.journal->QueueDelete(
                                resource,
                                implementation.NewMutationId(),
                                implementation.activeFence->expectedEtag);
                            implementation.ResetReconcileSession();
                        }
                    }
                }
                catch (...)
                {
                    // active Read完了後のmain-thread再評価でfail-closedにします。
                    // local commit自体は既にdurableなのでここから例外を出しません。
                }
            }
        }
        else
        {
            implementation.ResetReconcileSession();
        }
        if (implementation.status.state
            != CloudSaveSynchronizerState::Unauthorized
            && implementation.status.state
                != CloudSaveSynchronizerState::Halted
            && implementation.status.state
                != CloudSaveSynchronizerState::BackingOff)
        {
            implementation.status.state =
                CloudSaveSynchronizerState::Synchronizing;
        }
    }

    void CloudSaveSynchronizer::PrepareLocalDelete(
        const CloudSaveResource& resource)
    {
        m_implementation->PrepareLocalDelete(resource);
    }

    void CloudSaveSynchronizer::Tick(
        const std::uint64_t nowMilliseconds)
    {
        m_implementation->Tick(nowMilliseconds);
    }

    void CloudSaveSynchronizer::ResolveConflict(
        const CloudSaveResource& resource,
        const std::string_view expectedMutationId,
        const CloudSaveConflictResolution resolution)
    {
        auto& implementation = *m_implementation;
        implementation.RequireOwner();
        if (!implementation.journal || !IsValidResource(resource))
        {
            throw std::logic_error(
                "Cloud save synchronizer is not attached.");
        }
        if (implementation.activeMailbox || implementation.retiredMailbox)
        {
            throw std::logic_error(
                "Cloud save conflict resolution requires an idle wire lane.");
        }

        const auto inventory = implementation.ReadLocalInventory();
        if (implementation.status.state
                == CloudSaveSynchronizerState::Halted
            && (implementation.status.stopReason
                    == CloudSaveSynchronizerStopReason::LocalUnavailable
                || implementation.status.stopReason
                    == CloudSaveSynchronizerStopReason::LocalCorrupt))
        {
            throw std::runtime_error(
                "Local persistence is unavailable for conflict resolution.");
        }
        const auto pending = implementation.journal->Pending(resource);
        const auto conflict = implementation.journal->Conflict(resource);
        if (!pending || !conflict
            || pending->mutationId != expectedMutationId)
        {
            throw std::logic_error("Cloud save conflict is stale.");
        }

        if (resolution == CloudSaveConflictResolution::UseRemote)
        {
            try
            {
                ValidateFullSnapshotDocument(*conflict);
            }
            catch (...)
            {
                implementation.Halt(
                    CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                throw;
            }
            if (!FitsAccountQuotaAfterApply(inventory, *conflict))
            {
                implementation.Halt(
                    CloudSaveSynchronizerStopReason::LocalCorrupt);
                throw std::runtime_error(
                    "Remote conflict exceeds the local account quota.");
            }
            bool applied{};
            try
            {
                LocalPersistenceDocument missing;
                missing.state = LocalPersistenceDocumentState::Missing;
                const auto* found = FindLocalResource(inventory, resource);
                applied = implementation.ApplyRemoteSnapshot(
                    *conflict,
                    found ? found->document : missing,
                    found ? &found->resource : nullptr);
            }
            catch (...)
            {
                implementation.Halt(
                    CloudSaveSynchronizerStopReason::LocalUnavailable);
                throw;
            }
            if (!applied)
            {
                implementation.ResetReconcileSession();
                throw std::runtime_error(
                    "Local persistence changed during conflict resolution.");
            }
            // crash時に再実行可能な順序: local remote適用 -> journal finalize。
            try
            {
                implementation.journal->ResolveConflict(
                    resource,
                    expectedMutationId,
                    CloudSaveConflictResolution::UseRemote);
            }
            catch (...)
            {
                implementation.Halt(
                    CloudSaveSynchronizerStopReason::JournalFailure);
                throw;
            }
        }
        else if (resolution == CloudSaveConflictResolution::RetryLocal)
        {
            implementation.journal->ResolveConflict(
                resource,
                expectedMutationId,
                CloudSaveConflictResolution::RetryLocal,
                implementation.NewMutationId());
        }
        else
        {
            throw std::invalid_argument(
                "Invalid cloud save conflict resolution.");
        }

        implementation.ResetReconcileSession();
        implementation.status.stopReason =
            CloudSaveSynchronizerStopReason::None;
        implementation.status.state =
            CloudSaveSynchronizerState::Synchronizing;
        implementation.RefreshConflictStatus();
    }

    CloudSaveSynchronizerStatus CloudSaveSynchronizer::Status() const noexcept
    {
        return m_implementation->status;
    }

    bool CloudSaveSynchronizer::IsAttached() const noexcept
    {
        return m_implementation->journal != nullptr;
    }

    bool CloudSaveSynchronizer::HasInFlightRequest() const noexcept
    {
        return m_implementation->activeMailbox != nullptr
            || m_implementation->retiredMailbox != nullptr;
    }

    bool CloudSaveSynchronizer::
        ConsumeAuthorizedWireSuccessSignal() noexcept
    {
        return std::exchange(
            m_implementation->authorizedWireSuccessPending,
            false);
    }
}
