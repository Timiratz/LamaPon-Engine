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

    // 通常再試行の初期待機ms
    constexpr std::uint64_t InitialRetryDelayMilliseconds = 1000u;
    // 通常再試行の最大待機ms
    constexpr std::uint64_t MaximumRetryDelayMilliseconds = 60000u;
    // レート制限時の最大待機ms
    constexpr std::uint64_t MaximumRateLimitDelayMilliseconds = 300000u;
    // アカウントキーの必要文字数
    constexpr std::size_t AccountStorageKeyBytes = 64u;

    // 共通のスロット名規則で同じ保存先かを返す(left: 比較元, right: 比較先)。
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

    // 種別とスロット名が共通の保存先規則を満たすかを返す(resource: 検証する保存先)。
    bool IsValidResource(const CloudSaveResource& resource) noexcept
    {
        if (resource.kind == CloudSaveResourceKind::Preferences)
        {
            return resource.slot.empty();
        }
        return resource.kind == CloudSaveResourceKind::SaveSlot
            && LamaPon::Detail::IsValidSaveSlotName(resource.slot);
    }

    // 64桁の小文字の16進アカウントキーかを返す(value: 検証する保存キー)。
    bool IsAccountStorageKey(const std::string_view value) noexcept
    {
        if (value.size() != AccountStorageKeyBytes)
        {
            return false;
        }
        // 検証するアカウントキーの1文字
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

    // 文字列の使用領域をゼロ化して空にする(value: 消去する秘密値)。
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
        // 消去責任を持つ短命tokenを複製する(source: 入力tokenの借用)。
        explicit SecretString(const std::string_view source)
            : value(source)
        {
        }

        // 保持するtokenをゼロ化して破棄する。
        ~SecretString()
        {
            EraseSecret(value);
        }

        // 秘密値の複製を禁止する。
        SecretString(const SecretString&) = delete;
        // 秘密値のコピー代入を禁止する。
        SecretString& operator=(const SecretString&) = delete;

        // 消去責任を持つ短命token
        std::string value;
    };

    // CNG乱数から小文字UUIDv4の更新IDを作る。
    std::string GenerateMutationId()
    {
        // UUID生成用の16バイト乱数
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

        // 小文字の16進数字一覧
        constexpr char Digits[] = "0123456789abcdef";
        // 小文字UUIDv4の出力文字列
        std::string result;
        result.reserve(36u);
        // UUIDの処理バイト位置
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
        // 接続・token変更ごとの識別世代
        std::uint64_t sessionSerial{};
        // 公開するアカウントの世代
        std::uint64_t profileEpoch{};
        // 通信開始時のjournal世代
        std::uint64_t journalGeneration{};
        // 通信先の小文字アカウントキー
        std::string accountStorageKey;
        // 結果が対応する保存先
        CloudSaveResource resource;
        // 応答が対応する未送信更新ID
        std::string mutationId;
        // Read開始時の一覧のETag
        std::string expectedEtag;
        // Read開始時にlocal内容があったか
        bool localWasLoadedAtReadStart{};
    };

    struct WireResult final
    {
        // 実行したHTTP通信の種別
        WireOperation operation{ WireOperation::Manifest };
        // 結果適用前の照合条件
        RequestFence fence;
        // 保存一覧の取得結果
        CloudSaveManifestResult manifest;
        // 単一保存先の通信結果
        CloudSaveItemResult item;
    };

    struct WireMailbox final
    {
        // 結果の受け渡し用排他ロック
        std::mutex mutex;
        // workerから受け取る通信結果
        std::optional<WireResult> result;
        // 通信workerが完了したか
        bool completed{};
        // 結果を格納できなかったか
        bool resultLost{};
    };

    // 例外内容を含まない固定の通信失敗を返す。
    CloudSaveWireOutcome FixedTransportFailure()
    {
        return {
            CloudSaveWireStatus::TransportError,
            0u,
            {},
            {}
        };
    }

    // 借用先を捕捉せず通信だけをworkerで実行して結果を渡す(client: 共有する同期HTTP処理, token: 消去責任を共有するtoken, mailbox: 結果の受け渡し先, operation: 通信種別, fence: 結果適用前の照合条件, callback: HTTP処理と結果の格納)。
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
                // workerから渡す通信結果
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

                // 結果を受け渡す間の排他ロック
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
        // 検査したローカル保存先
        CloudSaveResource resource;
        // ローカルの内容と読み取り状態
        LocalPersistenceDocument document;
    };

    struct LocalInventory final
    {
        // 検査したローカル保存の一覧
        std::vector<LocalResource> resources;
    };

    // 同じ保存先のローカル記録を借用し不在ならnullを返す(inventory: 検査済み一覧, resource: 探す保存先)。
    const LocalResource* FindLocalResource(
        const LocalInventory& inventory,
        const CloudSaveResource& resource) noexcept
    {
        // 同じローカル保存先を探す(item: 照合する保存記録)。
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

    // 同じ保存先のローカル文書を借用し不在ならnullを返す(inventory: 検査済み一覧, resource: 探す保存先)。
    const LocalPersistenceDocument* FindLocal(
        const LocalInventory& inventory,
        const CloudSaveResource& resource) noexcept
    {
        // 一致するローカル保存の検索結果
        const auto* found = FindLocalResource(inventory, resource);
        return found ? &found->document : nullptr;
    }

    // リモート適用後の内容合計が上限内かを返す(inventory: 検査済みローカル一覧, snapshot: 適用する保存状態)。
    bool FitsAccountQuotaAfterApply(
        const LocalInventory& inventory,
        const CloudSaveSnapshot& snapshot) noexcept
    {
        // 適用前後の保存内容の合計B
        std::uint64_t total{};
        // ローカル保存の文書または記録
        for (const auto& local : inventory.resources)
        {
            if (local.document.state
                == LocalPersistenceDocumentState::Loaded)
            {
                total += local.document.bytes.size();
            }
        }
        // 現在のローカル保存状態
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

    // 同じ保存先のリモート管理情報を借用する(manifest: リモート一覧, resource: 探す保存先)。
    const CloudSaveManifestItem* FindManifest(
        const std::vector<CloudSaveManifestItem>& manifest,
        const CloudSaveResource& resource) noexcept
    {
        // 同じリモート保存先を探す(item: 照合する一覧の管理情報)。
        const auto iterator = std::find_if(
            manifest.begin(),
            manifest.end(),
            [&resource](const CloudSaveManifestItem& item)
            {
                return SameResource(item.resource, resource);
            });
        return iterator == manifest.end() ? nullptr : &*iterator;
    }

    // 不在と削除も含めローカル内容が基準と同じかを返す(local: ローカルの文書, baseline: 同期済みの基準状態)。
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

    // ETag・削除・長さ・ハッシュが基準と同じかを返す(remote: リモート管理情報・不在ならnull, baseline: 同期済みの基準状態)。
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

    // 設定または保存スロットの完全な文書形式を検証する(snapshot: 検証する保存状態)。
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
        // JSON文書を検証するバイト列の借用
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

    // 未送信更新が完全なローカル文書形式を持つかを検証する(pending: 保存または削除の更新)。
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
        // 検証する未送信の保存状態
        CloudSaveSnapshot snapshot;
        snapshot.resource = pending.resource;
        snapshot.content = pending.content;
        snapshot.sha256 = pending.sha256;
        ValidateFullSnapshotDocument(snapshot);
    }

    // 保存先を検証し一覧に未登録の場合だけ追加する(resources: 追加先の一覧, resource: 追加する保存先)。
    void AddResource(
        std::vector<CloudSaveResource>& resources,
        const CloudSaveResource& resource)
    {
        if (!IsValidResource(resource))
        {
            throw std::runtime_error("Invalid cloud save resource.");
        }
        // 未登録の保存先かを照合する(existing: 確認済みの保存先)。
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

    // 同じ保存先が一覧に含まれるかを返す(resources: 検索する一覧, resource: 探す保存先)。
    bool ContainsResource(
        const std::vector<CloudSaveResource>& resources,
        const CloudSaveResource& resource) noexcept
    {
        // 同じ保存先を照合する(existing: 一覧に登録した保存先)。
        return std::any_of(
            resources.begin(),
            resources.end(),
            [&resource](const CloudSaveResource& existing)
            {
                return SameResource(existing, resource);
            });
    }

    // 上限を超える場合は最大値で加算する(left: 加算元, right: 加算する値)。
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
        // 準備状態を作った同期層の識別子
        const void* owner{};
        // 切断まで借用する同期記録
        CloudSaveJournal* journal{};
        // 公開する小文字アカウントキー
        std::string accountStorageKey;
        // workerと共有する短命token
        std::shared_ptr<SecretString> token;
        // 接続前に検証したローカル一覧
        LocalInventory initialInventory;
        // 準備時に検証した競合数
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
        // 借用先と通信処理を保持し操作スレッドを記録する(preferencesValue: 設定データの借用, savesValue: 保存スロットの借用, clientValue: 同期HTTP処理の共有所有先, generatorValue: 更新IDの生成処理)。
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

        // workerを待たず借用先を切り離して共有状態の所有を解放する。
        ~Implementation()
        {
            DetachNoThrow();
            // 失効したworkerはshared client・token・mailboxだけで完走します。
            retiredMailbox.reset();
        }

        // 構築時と異なるスレッドからの操作を拒否する。
        void RequireOwner() const
        {
            if (std::this_thread::get_id() != ownerThread)
            {
                throw std::logic_error(
                    "Cloud save synchronizer is main-thread only.");
            }
        }

        // 古い結果を失効させ物理的な同時通信を1件に保つ。
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

        // 失効したworkerが完了した場合だけ結果の所有を解放する。
        void ReapRetiredMailbox() noexcept
        {
            if (!retiredMailbox)
            {
                return;
            }
            // workerと共有する結果の受け渡し先
            const auto mailbox = retiredMailbox;
            // 通信workerが完了したか
            bool completed{};
            {
                // 結果を受け渡す間の排他ロック
                std::scoped_lock lock(mailbox->mutex);
                completed = mailbox->completed;
            }
            // 完了を待たず失効させた通信の結果
            if (completed && retiredMailbox == mailbox)
            {
                retiredMailbox.reset();
            }
        }

        // 保持するアカウントキーをゼロ化して空にする。
        void ClearAccountStorageKey() noexcept
        {
            EraseSecret(accountStorageKey);
        }

        // 公開先や世代に依存する競合一覧のキャッシュを失効させる。
        void InvalidateConflictDescriptors() noexcept
        {
            conflictDescriptors.clear();
            conflictDescriptorGeneration =
                (std::numeric_limits<std::uint64_t>::max)();
            conflictDescriptorProfileEpoch = 0u;
        }

        // 接続世代を進め借用先とアカウントの同期状態を解除する。
        void DetachNoThrow() noexcept
        {
            ++sessionSerial;
            RetireActiveMailbox();
            InvalidateConflictDescriptors();
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
            failedDetachCheckpointRecovery = {};
            retryAttempt = 0u;
            authorizedWireSuccessPending = false;
            status = {};
            status.state = CloudSaveSynchronizerState::Detached;
        }

        // 注入した生成処理またはCNGで新しい更新IDを作る。
        [[nodiscard]] std::string NewMutationId()
        {
            return mutationIdGenerator
                ? mutationIdGenerator()
                : GenerateMutationId();
        }

        // 現在の接続とjournal世代に結果の照合条件を結び付ける(resource: 通信する保存先, mutationId: 更新ID・一覧とReadは空, expectedEtag: Read時の一覧のETag, localWasLoadedAtReadStart: Read開始時に内容があったか)。
        [[nodiscard]] RequestFence MakeFence(
            // 同期・検証する保存先
            const CloudSaveResource& resource = {},
            // この更新へ割り当てる新UUID
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

        // 接続・保存先・世代と必要なら更新IDを照合する(fence: 通信開始時の条件, checkMutation: 未送信更新IDも照合するか)。
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
            // 再送条件を保持した未完了更新
            const auto pending = journal->Pending(fence.resource);
            return pending
                && pending->mutationId == fence.mutationId;
        }

        // 完了した通信結果を引き取り受け渡し失敗は送出する。
        [[nodiscard]] std::optional<WireResult> PollActive()
        {
            if (!activeMailbox)
            {
                return std::nullopt;
            }
            // workerと共有する結果の受け渡し先
            const auto mailbox = activeMailbox;
            // 完了したworkerの通信結果
            std::optional<WireResult> result;
            // 通信workerが完了したか
            bool completed{};
            // 結果の受け渡しに失敗したか
            bool failed{};
            {
                // 結果を受け渡す間の排他ロック
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

        // 現在のtokenと世代に結び付けて一覧取得を開始する。
        void LaunchManifest()
        {
            // workerと共有する結果の受け渡し先
            auto mailbox = std::make_shared<WireMailbox>();
            // 通信結果を照合する世代と保存先
            const auto fence = MakeFence();
            activeMailbox = mailbox;
            activeOperation = WireOperation::Manifest;
            activeFence = fence;
            try
            {
                // 保存一覧を取得する(cloud: 同期HTTP処理, accessToken: 短命tokenの借用, result: 通信結果の出力)。
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

        // 一覧のETagとローカル有無を記録して内容取得を開始する(resource: 取得する保存先, expectedEtag: 一覧で観測したETag, localState: 取得開始時のローカル状態)。
        void LaunchRead(
            const CloudSaveResource& resource,
            const std::string_view expectedEtag,
            const LocalPersistenceDocumentState localState)
        {
            // workerと共有する結果の受け渡し先
            auto mailbox = std::make_shared<WireMailbox>();
            // 通信結果を照合する世代と保存先
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
                // 内容を取得する(cloud: 同期HTTP処理, accessToken: 短命tokenの借用, result: 通信結果の出力)。
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

        // 永続化済みの再送条件をそのまま共有し更新通信を開始する(pending: 未送信の保存または削除)。
        void LaunchMutation(const CloudSavePendingMutation& pending)
        {
            // workerと共有する結果の受け渡し先
            auto mailbox = std::make_shared<WireMailbox>();
            // 通信結果を照合する世代と保存先
            auto fence = MakeFence(
                pending.resource,
                pending.mutationId);
            // 保存または削除の通信種別
            const auto operation = pending.kind == CloudSavePendingKind::Put
                ? WireOperation::Put
                : WireOperation::Delete;
            activeMailbox = mailbox;
            activeOperation = operation;
            activeFence = fence;
            try
            {
                // 記録済み条件で更新する(cloud: 同期HTTP処理, accessToken: 短命tokenの借用, result: 通信結果の出力)。
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

        // 設定と全slotを厳密に検査し未保存値・破損・不確かな読取で停止する。
        [[nodiscard]] LocalInventory ReadLocalInventory();
        // 再照合を止め理由を含む停止状態へ移る(reason: 停止理由)。
        void Halt(CloudSaveSynchronizerStopReason reason) noexcept;
        // journalの競合件数を反映し他の停止・待機状態を保つ。
        void RefreshConflictStatus() noexcept;
        // 古いリモート一覧を破棄して一覧からの再照合を求める。
        void ResetReconcileSession() noexcept;
        // 上限付きの指数待機またはレート制限待機を設定する(outcome: 通信失敗の種別と待機指示, nowMilliseconds: 単調時計の現在時刻ms)。
        void EnterBackoff(
            const CloudSaveWireOutcome& outcome,
            std::uint64_t nowMilliseconds) noexcept;
        // 認証・再試行・不正応答を分類し失敗処理済みかを返す(outcome: 通信結果, operation: 通信種別, nowMilliseconds: 単調時計の現在時刻ms)。
        [[nodiscard]] bool HandleWireFailure(
            const CloudSaveWireOutcome& outcome,
            WireOperation operation,
            std::uint64_t nowMilliseconds);
        // ローカルが観測後に変わっていない場合だけatomicに適用する(snapshot: リモート状態, observed: 比較するローカル文書, localIdentity: 既存slotの綴り・不在ならnull)。
        [[nodiscard]] bool ApplyRemoteSnapshot(
            const CloudSaveSnapshot& snapshot,
            const LocalPersistenceDocument& observed,
            const CloudSaveResource* localIdentity = nullptr);
        // ローカル削除前に削除意思を記録し対応するReadを失効させる(resource: 削除する保存先)。
        void PrepareLocalDelete(const CloudSaveResource& resource);
        // リモート削除の適用を観測一覧へ確保なしで反映する(resource: 削除した保存先)。
        void NoteRemoteDeletion(const CloudSaveResource& resource) noexcept;
        // 観測済み状態から消えた保存先と再作成を操作列にする(inventory: 現在の検査済みローカル一覧)。
        [[nodiscard]] std::vector<CloudSaveDeleteIntentOperation>
            DetermineLocalDeleteIntentOperations(
                const LocalInventory& inventory);
        // 確定した操作列をjournalの単一世代で公開する(operations: 保存先ごとの記録・解除)。
        void ApplyLocalDeleteIntentOperations(
            const std::vector<CloudSaveDeleteIntentOperation>& operations);
        // ローカル状態の検査と削除意思の一括公開後に観測一覧を進める。
        void CaptureDurableLocalDeleteIntents();
        // 未検証の世代の基準・競合・未送信内容を完全な文書形式で検証する。
        void ValidateJournalDocuments();
        // ローカル内容と基準ETagから新しい更新を通信前に永続化する(resource: 保存先, local: 検査済みローカル文書, baseline: 同期済みの基準)。
        void QueueLocalMutation(
            const CloudSaveResource& resource,
            const LocalPersistenceDocument& local,
            const std::optional<CloudSaveSnapshot>& baseline);
        // ACK後にローカルを読み直し成功状態との差分を新しい更新にする(resource: 保存先, snapshot: ACKで確定した状態)。
        void QueueOverlayAfterSuccess(
            const CloudSaveResource& resource,
            const CloudSaveSnapshot& snapshot);
        // 結果の世代・保存先・容量を検証して照合用の一覧を更新する(result: 一覧取得の結果, nowMilliseconds: 単調時計の現在時刻ms)。
        void HandleManifestResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        // 結果と三者の変更状態を照合して適用・CAS・競合を選ぶ(result: 保存内容取得の結果, nowMilliseconds: 単調時計の現在時刻ms)。
        void HandleReadResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        // 更新IDとACK内容を検証し競合記録または成功後の差分更新を行う(result: 保存・削除の通信結果, nowMilliseconds: 単調時計の現在時刻ms)。
        void HandleMutationResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        // 通信種別に対応する結果処理へ引き渡す(result: workerの通信結果, nowMilliseconds: 単調時計の現在時刻ms)。
        void HandleWireResult(
            WireResult result,
            std::uint64_t nowMilliseconds);
        // 削除・縮小を優先して永続化済みの更新を1件送信する(inventory: 事前検査済み一覧・この処理では未使用)。
        [[nodiscard]] bool StartPendingMutation(
            const LocalInventory& inventory);
        // 削除・縮小を優先して三者比較による同期を一段階進める(inventory: 検査済みのローカル一覧)。
        [[nodiscard]] bool ReconcileOne(
            const LocalInventory& inventory);
        // 古い通信結果を失効させ切断前の削除意思を単一世代で公開する。
        void CheckpointLocalStateForDetach();
        // 結果反映と厳密な保存検査を行い最大1件の通信を開始する(nowMilliseconds: 単調時計の現在時刻ms)。
        void Tick(std::uint64_t nowMilliseconds);

        // 同じ存続期間の設定データの借用
        PlayerPrefs& preferences;
        // 同じ存続期間の保存スロットの借用
        SaveDataStore& saves;
        // 同期HTTP処理の共有所有先
        std::shared_ptr<const CloudSaveClient> client;
        // 新しい更新IDの生成処理
        CloudSaveMutationIdGenerator mutationIdGenerator;
        // 操作を許可する構築時のスレッド
        std::thread::id ownerThread;

        // 切断まで借用する同期記録
        CloudSaveJournal* journal{};
        // 公開するアカウントの世代
        std::uint64_t profileEpoch{};
        // 接続・token変更ごとの識別世代
        std::uint64_t sessionSerial{};
        // 公開する小文字アカウントキー
        std::string accountStorageKey;
        // workerと共有する短命token
        std::shared_ptr<SecretString> token;

        // 現在の通信結果の受け渡し先
        std::shared_ptr<WireMailbox> activeMailbox;
        // 完了を待たず失効させた通信の結果
        std::shared_ptr<WireMailbox> retiredMailbox;
        // 現在実行中の通信種別
        std::optional<WireOperation> activeOperation;
        // 現在の通信結果の照合条件
        std::optional<RequestFence> activeFence;
        // ローカル変更でReadを失効したか
        bool activeReadInvalidated{};
        // 照合に使うリモート保存一覧
        std::optional<std::vector<CloudSaveManifestItem>> manifest;
        // 今回の一覧で照合済みの保存先
        std::vector<CloudSaveResource> processed;
        // 全文書を検証済みのjournal世代
        std::optional<std::uint64_t> validatedJournalGeneration;
        // 再照合を要求されたか
        bool reconcileRequested{};
        // ローカル削除意思を再検査するか
        bool deleteIntentCapturePending{};
        // 連続する通常再試行の回数
        std::uint32_t retryAttempt{};
        // 最後の単調時計時刻ms
        std::uint64_t lastTickMilliseconds{};
        // 停止理由を含む現在の同期状態
        CloudSaveSynchronizerStatus status;
        // 最後に検査したローカル保存一覧
        LocalInventory lastObservedInventory;
        // ローカル一覧を観測済みか
        bool hasLastObservedInventory{};
        // 認可応答の検証完了を通知するか
        bool authorizedWireSuccessPending{};
        // 切断前の永続化失敗の復旧情報
        CloudSaveDetachCheckpointRecovery failedDetachCheckpointRecovery;
        // 本文の複製を避け、同じ世代では競合の管理情報を再利用します。
        mutable std::uint64_t conflictDescriptorGeneration{
            (std::numeric_limits<std::uint64_t>::max)()
        };
        // 競合一覧を作ったアカウントの世代
        mutable std::uint64_t conflictDescriptorProfileEpoch{};
        // 同じ世代で再利用する競合の管理情報
        mutable std::vector<CloudSaveConflictDescriptor>
            conflictDescriptors;
    };

    // 設定と全slotを厳密に検査し未保存値・破損・不確かな読取で停止する。
    LocalInventory
        CloudSaveSynchronizer::Implementation::ReadLocalInventory()
    {
        // 検査したローカル保存の一覧
        LocalInventory inventory;
        // 設定と保存スロットの合計B
        std::uint64_t totalBytes{};

        // 未保存値や読取失敗をdiskだけで上書きしないよう、明示Saveまたは修復を待ちます。
        if (preferences.IsDirty() || preferences.HasLoadFailure())
        {
            Halt(CloudSaveSynchronizerStopReason::LocalUnavailable);
            return inventory;
        }

        // 検査した設定データの文書
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

        // ローカル保存スロットの列挙結果
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

        // 検証済みの保存スロット名
        for (const auto& slot : listing.slots)
        {
            // 検査した保存スロットの文書
            auto document =
                LocalPersistenceDocuments::ReadSaveData(saves, slot);
            // 列挙後に消えた場合も外部processとの競合なので、削除として推測せずUnavailableで停止します。
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

    // 再照合を止め理由を含む停止状態へ移る(reason: 停止理由)。
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

    // journalの競合件数を反映し他の停止・待機状態を保つ。
    void CloudSaveSynchronizer::Implementation::RefreshConflictStatus() noexcept
    {
        // 現在の競合数
        std::size_t count{};
        try
        {
            if (journal)
            {
                // 同期・検証する保存先
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

    // 古いリモート一覧を破棄して一覧からの再照合を求める。
    void CloudSaveSynchronizer::Implementation::ResetReconcileSession() noexcept
    {
        manifest.reset();
        processed.clear();
        reconcileRequested = true;
    }

    // 上限付きの指数待機またはレート制限待機を設定する(outcome: 通信失敗の種別と待機指示, nowMilliseconds: 単調時計の現在時刻ms)。
    void CloudSaveSynchronizer::Implementation::EnterBackoff(
        const CloudSaveWireOutcome& outcome,
        const std::uint64_t nowMilliseconds) noexcept
    {
        // 再試行までの待機ms
        std::uint64_t delay{};
        if (outcome.status == CloudSaveWireStatus::RateLimited)
        {
            // レート制限で要求された待機秒数
            const auto seconds = (std::max)(
                std::uint64_t{ 1u },
                static_cast<std::uint64_t>(outcome.retryAfterSeconds));
            delay = seconds > MaximumRateLimitDelayMilliseconds / 1000u
                ? MaximumRateLimitDelayMilliseconds
                : seconds * 1000u;
        }
        else
        {
            // 指数待機に使う試行回数
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

    // 認証・再試行・不正応答を分類し失敗処理済みかを返す(outcome: 通信結果, operation: 通信種別, nowMilliseconds: 単調時計の現在時刻ms)。
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

    // ローカルが観測後に変わっていない場合だけatomicに適用する(snapshot: リモート状態, observed: 比較するローカル文書, localIdentity: 既存slotの綴り・不在ならnull)。
    bool CloudSaveSynchronizer::Implementation::ApplyRemoteSnapshot(
        const CloudSaveSnapshot& snapshot,
        const LocalPersistenceDocument& observed,
        const CloudSaveResource* const localIdentity)
    {
        ValidateFullSnapshotDocument(snapshot);
        // ローカル条件付き適用の結果
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
            // 大小文字だけ異なる同一slotを別ファイルにしないため、検証済みのローカル文書の綴りを使います。
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
        // 観測したローカル状態へ適用済みか
        const bool applied = result
            == LocalPersistenceConditionalApplyResult::Applied;
        if (applied && snapshot.deleted)
        {
            NoteRemoteDeletion(snapshot.resource);
        }
        return applied;
    }

    // リモート削除の適用を観測一覧へ確保なしで反映する(resource: 削除した保存先)。
    void CloudSaveSynchronizer::Implementation::NoteRemoteDeletion(
        const CloudSaveResource& resource) noexcept
    {
        if (!hasLastObservedInventory)
        {
            return;
        }
        // ローカル保存の文書または記録
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

    // ローカル削除前に削除意思を記録し対応するReadを失効させる(resource: 削除する保存先)。
    void CloudSaveSynchronizer::Implementation::PrepareLocalDelete(
        const CloudSaveResource& resource)
    {
        RequireOwner();
        if (!journal || !IsValidResource(resource))
        {
            throw std::logic_error(
                "Cloud save delete checkpoint is not active.");
        }
        // ローカル保存の文書または記録
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

        // baselineのstrong ETagは同じjournal entryに残るため、intent処理時にremote最新版ではなくdelete開始時の既知revisionへCASできます。
        journal->RecordLocalDeleteIntent(resource);
        if (activeOperation == WireOperation::Read
            && activeFence
            && SameResource(activeFence->resource, resource))
        {
            activeReadInvalidated = true;
        }
        reconcileRequested = true;
    }

    // 観測済み状態から消えた保存先と再作成を操作列にする(inventory: 現在の検査済みローカル一覧)。
    std::vector<CloudSaveDeleteIntentOperation>
        CloudSaveSynchronizer::Implementation::
        DetermineLocalDeleteIntentOperations(
            const LocalInventory& inventory)
    {
        // 一括記録する削除意思の操作列
        std::vector<CloudSaveDeleteIntentOperation> operations;
        operations.reserve(
            lastObservedInventory.resources.size()
            + inventory.resources.size());
        if (hasLastObservedInventory)
        {
            // 前回観測したローカル保存状態
            for (const auto& previous : lastObservedInventory.resources)
            {
                if (previous.document.state
                    != LocalPersistenceDocumentState::Loaded)
                {
                    continue;
                }
                // 現在のローカル保存状態
                const auto* current = FindLocal(inventory, previous.resource);
                if (!current
                    || current->state == LocalPersistenceDocumentState::Missing)
                {
                    // ETag取得済みのimmutable pending Deleteが既にある場合、ETag未取得期間用のintentを重ねて復活させません。
                    if (!journal->Pending(previous.resource))
                    {
                        operations.push_back({
                            previous.resource,
                            CloudSaveDeleteIntentOperationKind::Record
                        });
                    }
                }
            }
        }
        // 現在のローカル保存状態
        for (const auto& current : inventory.resources)
        {
            if (current.document.state == LocalPersistenceDocumentState::Loaded
                && journal->HasLocalDeleteIntent(current.resource))
            {
                operations.push_back({
                    current.resource,
                    CloudSaveDeleteIntentOperationKind::Clear
                });
            }
        }
        return operations;
    }

    // 確定した操作列をjournalの単一世代で公開する(operations: 保存先ごとの記録・解除)。
    void CloudSaveSynchronizer::Implementation::
        ApplyLocalDeleteIntentOperations(
            const std::vector<CloudSaveDeleteIntentOperation>& operations)
    {
        journal->ApplyLocalDeleteIntentOperations(operations);
    }

    // ローカル状態の検査と削除意思の一括公開後に観測一覧を進める。
    void CloudSaveSynchronizer::Implementation::
        CaptureDurableLocalDeleteIntents()
    {
        // 検査したローカル保存の一覧
        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        // 一括記録する削除意思の操作列
        const auto operations =
            DetermineLocalDeleteIntentOperations(inventory);
        ApplyLocalDeleteIntentOperations(operations);
        lastObservedInventory = inventory;
        hasLastObservedInventory = true;
    }

    // 未検証の世代の基準・競合・未送信内容を完全な文書形式で検証する。
    void CloudSaveSynchronizer::Implementation::ValidateJournalDocuments()
    {
        // 検証または一覧照会時の世代
        const auto generation = journal->Generation();
        if (validatedJournalGeneration == generation)
        {
            return;
        }
        // 同期・検証する保存先
        for (const auto& resource : journal->Resources())
        {
            // 同期済みの基準状態
            if (const auto baseline = journal->Baseline(resource))
            {
                ValidateFullSnapshotDocument(*baseline);
            }
            // 競合時点のリモート保存状態
            if (const auto conflict = journal->Conflict(resource))
            {
                ValidateFullSnapshotDocument(*conflict);
            }
            // 再送条件を保持した未完了更新
            if (const auto pending = journal->Pending(resource))
            {
                ValidatePendingDocument(*pending);
            }
        }
        validatedJournalGeneration = generation;
    }

    // ローカル内容と基準ETagから新しい更新を通信前に永続化する(resource: 保存先, local: 検査済みローカル文書, baseline: 同期済みの基準)。
    void CloudSaveSynchronizer::Implementation::QueueLocalMutation(
        const CloudSaveResource& resource,
        const LocalPersistenceDocument& local,
        const std::optional<CloudSaveSnapshot>& baseline)
    {
        // この更新へ割り当てる新UUID
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

    // ACK後にローカルを読み直し成功状態との差分を新しい更新にする(resource: 保存先, snapshot: ACKで確定した状態)。
    void CloudSaveSynchronizer::Implementation::QueueOverlayAfterSuccess(
        const CloudSaveResource& resource,
        const CloudSaveSnapshot& snapshot)
    {
        // 検査したローカル保存の一覧
        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        // 不在時のローカル文書の代替値
        LocalPersistenceDocument missing;
        missing.state = LocalPersistenceDocumentState::Missing;
        // 一致するローカル保存の検索結果
        const auto* found = FindLocal(inventory, resource);
        // ローカル保存の文書または記録
        const auto& local = found ? *found : missing;
        // 同期済みの基準状態
        const std::optional<CloudSaveSnapshot> baseline{ snapshot };
        if (!LocalMatchesBaseline(local, baseline))
        {
            QueueLocalMutation(resource, local, baseline);
        }
    }

    // 結果の世代・保存先・容量を検証して照合用の一覧を更新する(result: 一覧取得の結果, nowMilliseconds: 単調時計の現在時刻ms)。
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

        // 削除済みも含むリモートslot数
        std::size_t saveSlots{};
        // 設定と保存スロットの合計B
        std::uint64_t totalBytes{};
        // 確認済みのリモート保存先一覧
        std::vector<CloudSaveResource> identities;
        // 検証するリモートの一覧項目
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

    // 結果と三者の変更状態を照合して適用・CAS・競合を選ぶ(result: 保存内容取得の結果, nowMilliseconds: 単調時計の現在時刻ms)。
    void CloudSaveSynchronizer::Implementation::HandleReadResult(
        WireResult result,
        const std::uint64_t nowMilliseconds)
    {
        // 通信中のローカル変更で失効したか
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

        // 一覧で観測したリモート状態
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
            // 一覧取得後にリモートが更新されたため、異なる世代を適用せず待機後に一覧から再開します。
            ResetReconcileSession();
            // 一覧取得後の更新競合の再試行結果
            CloudSaveWireOutcome race;
            race.status = CloudSaveWireStatus::RetryableServiceError;
            EnterBackoff(race, nowMilliseconds);
            return;
        }

        // 検査したローカル保存の一覧
        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            return;
        }
        // 不在時のローカル文書の代替値
        LocalPersistenceDocument missing;
        missing.state = LocalPersistenceDocumentState::Missing;
        // 実際のslot表記を持つlocal記録
        const auto* localResource = FindLocalResource(
            inventory,
            result.fence.resource);
        // ローカル保存の文書または記録
        const auto& local = localResource
            ? localResource->document
            : missing;
        // 同期済みの基準状態
        const auto baseline = journal->Baseline(result.fence.resource);
        // 一覧またはReadで得た保存状態
        const std::optional<CloudSaveSnapshot> remote{
            *result.item.snapshot
        };

        if (invalidated)
        {
            if (journal->HasLocalDeleteIntent(result.fence.resource)
                && local.state == LocalPersistenceDocumentState::Missing)
            {
                // 既存の基準ETagを優先して削除意思をpendingへ昇格し、Read中の更新は412競合として保持します。
                journal->QueueDelete(
                    result.fence.resource,
                    NewMutationId(),
                    baseline ? baseline->etag : remote->etag);
            }
            else if (!baseline
                && result.fence.localWasLoadedAtReadStart
                && local.state == LocalPersistenceDocumentState::Missing)
            {
                // Read中の明示削除を初期Missingと区別し、検証済みETagへの削除を通信前に永続化します。
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
            // 再送条件を保持した未完了更新
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
            // Read中にlocal overlayが進んだ場合も古いremoteを適用せず、baseline ETagに対するCASとしてjournalへ先に記録します。
            QueueLocalMutation(result.fence.resource, local, baseline);
        }
        ResetReconcileSession();
        RefreshConflictStatus();
    }

    // 更新IDとACK内容を検証し競合記録または成功後の差分更新を行う(result: 保存・削除の通信結果, nowMilliseconds: 単調時計の現在時刻ms)。
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

        // 再送条件を保持した未完了更新
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
        // ACK後にローカルを再読し、次の差分を新しいUUIDとACKのETagで記録します。
        QueueOverlayAfterSuccess(
            result.fence.resource,
            *result.item.snapshot);
        ResetReconcileSession();
        RefreshConflictStatus();
    }

    // 通信種別に対応する結果処理へ引き渡す(result: workerの通信結果, nowMilliseconds: 単調時計の現在時刻ms)。
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

    // 削除・縮小を優先して永続化済みの更新を1件送信する(inventory: 事前検査済み一覧・この処理では未使用)。
    bool CloudSaveSynchronizer::Implementation::StartPendingMutation(
        const LocalInventory& inventory)
    {
        (void)inventory;
        // 再送条件を保持した未完了更新
        auto pending = journal->Dispatchable();
        if (pending.empty())
        {
            return false;
        }
        // 削除・縮小を先に送信する順位を返す(mutation: 未送信更新)。
        const auto priority = [this](
            const CloudSavePendingMutation& mutation)
        {
            if (mutation.kind == CloudSavePendingKind::Delete)
            {
                return 0;
            }
            // 同期済みの基準状態
            const auto baseline = journal->Baseline(mutation.resource);
            // 変更先の適用前の内容バイト数
            const auto before = baseline && !baseline->deleted
                ? baseline->content.size()
                : 0u;
            // 変更先の適用後の内容バイト数
            const auto after = mutation.content.size();
            if (after < before)
            {
                return 0;
            }
            return after > before ? 2 : 1;
        };
        // 容量を減らす順に比較する(left: 比較元の更新, right: 比較先の更新)。
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

    // 削除・縮小を優先して三者比較による同期を一段階進める(inventory: 検査済みのローカル一覧)。
    bool CloudSaveSynchronizer::Implementation::ReconcileOne(
        const LocalInventory& inventory)
    {
        if (!manifest)
        {
            LaunchManifest();
            return true;
        }

        // local・remote・journalの保存先
        std::vector<CloudSaveResource> resources;
        AddResource(resources, CloudSaveResource::Preferences());
        // ローカル保存の文書または記録
        for (const auto& local : inventory.resources)
        {
            AddResource(resources, local.resource);
        }
        // 一覧またはReadで得た保存状態
        for (const auto& remote : *manifest)
        {
            AddResource(resources, remote.resource);
        }
        // journalに記録した保存先
        for (const auto& persisted : journal->Resources())
        {
            AddResource(resources, persisted);
        }

        // 一時的な容量超過を避け削除・縮小を優先する(resource: 比較する保存先)。
        const auto quotaTransitionPriority =
            [&inventory, this](const CloudSaveResource& resource)
            {
                // 一覧またはReadで得た保存状態
                const auto* remote = FindManifest(*manifest, resource);
                if (journal->HasLocalDeleteIntent(resource)
                    && remote && !remote->deleted)
                {
                    return 0;
                }
                // ローカル保存の文書または記録
                const auto* local = FindLocal(inventory, resource);
                // ローカル内容のバイト数
                const auto localBytes = local
                        && local->state
                            == LocalPersistenceDocumentState::Loaded
                    ? local->bytes.size()
                    : 0u;
                // リモート内容のバイト数
                const auto remoteBytes = remote && !remote->deleted
                    ? remote->byteLength
                    : 0u;
                // 不在時のローカル文書の代替値
                LocalPersistenceDocument missing;
                missing.state = LocalPersistenceDocumentState::Missing;
                // 不在時の代替を含むローカル文書
                const auto& effectiveLocal = local ? *local : missing;
                // 同期済みの基準状態
                const auto baseline = journal->Baseline(resource);
                // ローカルが同期済み基準と同じか
                const bool localMatches =
                    LocalMatchesBaseline(effectiveLocal, baseline);
                // リモートが同期済み基準と同じか
                const bool remoteMatches =
                    ManifestMatchesBaseline(remote, baseline);

                // 変更先の適用前の内容バイト数
                std::uint64_t before{};
                // 変更先の適用後の内容バイト数
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
        // 容量を減らす順に比較する(left: 比較元の保存先, right: 比較先の保存先)。
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

        // 同期・検証する保存先
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

            // 不在時のローカル文書の代替値
            LocalPersistenceDocument missing;
            missing.state = LocalPersistenceDocumentState::Missing;
            // 実際のslot表記を持つlocal記録
            const auto* localResource = FindLocalResource(
                inventory,
                resource);
            // ローカル保存の文書または記録
            const auto& local = localResource
                ? localResource->document
                : missing;
            // 同期済みの基準状態
            const auto baseline = journal->Baseline(resource);
            // 一覧またはReadで得た保存状態
            const auto* remote = FindManifest(*manifest, resource);
            if (journal->HasLocalDeleteIntent(resource))
            {
                if (local.state == LocalPersistenceDocumentState::Loaded)
                {
                    // delete後の再作成が既にdurableなら古いintentを破棄し、通常の三者比較（必要なら初回conflict）へ戻します。
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
                    // ETag取得前に記録したintentを同じjournal publishでimmutable pending Deleteへ昇格します。
                    journal->QueueDelete(
                        resource,
                        NewMutationId(),
                        baseline
                            ? baseline->etag
                            : remote->etag);
                    ResetReconcileSession();
                    return true;
                }
                // リモートのETag付き削除状態
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
                // 削除のローカル適用後にcrashした場合も削除状態同士は収束済みとし、基準ETagだけを冪等に進めます。
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
            // ローカルが同期済み基準と同じか
            const bool localMatches = LocalMatchesBaseline(local, baseline);
            // リモートが同期済み基準と同じか
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
                    // 既知の保存先は強いETag付きの削除状態で残るため、一覧から消えた応答を拒否します。
                    Halt(
                        CloudSaveSynchronizerStopReason::InvalidRemoteResponse);
                    return false;
                }
                if (!remote->deleted)
                {
                    LaunchRead(resource, remote->etag, local.state);
                    return true;
                }
                // リモートのETag付き削除状態
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
                // 初回に両側の内容が存在した場合も、全文書のReadまたは削除状態の検証後に競合として記録します。
                if (!remote->deleted)
                {
                    LaunchRead(resource, remote->etag, local.state);
                    return true;
                }
                QueueLocalMutation(resource, local, std::nullopt);
                // 再送条件を保持した未完了更新
                const auto pending = journal->Pending(resource);
                if (!pending)
                {
                    throw std::runtime_error("Initial conflict queue failed.");
                }
                // リモートのETag付き削除状態
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
                // 両側変更は全文書をReadし、適用後の復旧で内容が一致すれば基準だけを進め、異なればCASします。
                LaunchRead(resource, remote->etag, local.state);
                return true;
            }

            // ローカル変更を旧基準ETagへのCASとして、通信前に永続化します。
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

    // 古い通信結果を失効させ切断前の削除意思を単一世代で公開する。
    void CloudSaveSynchronizer::Implementation::
        CheckpointLocalStateForDetach()
    {
        RequireOwner();
        failedDetachCheckpointRecovery = {};
        if (!journal)
        {
            return;
        }

        // 接続世代を進め、以降のworker結果をローカル保存とjournalへ適用しません。
        ++sessionSerial;
        RetireActiveMailbox();

        // 通信由来の停止状態も解除して厳密なローカル検査を行い、切断時の削除意思を残します。
        status.state = CloudSaveSynchronizerState::Synchronizing;
        status.stopReason = CloudSaveSynchronizerStopReason::None;
        status.retryAtMilliseconds = 0u;
        // 検査したローカル保存の一覧
        const auto inventory = ReadLocalInventory();
        if (status.state == CloudSaveSynchronizerState::Halted)
        {
            throw std::runtime_error(
                "Local persistence could not be checkpointed.");
        }
        // 一括記録する削除意思の操作列
        auto operations =
            DetermineLocalDeleteIntentOperations(inventory);
        // 操作列を単一世代で公開し、失敗時は呼出し側へ移して新しいjournalに冪等に再適用します。
        failedDetachCheckpointRecovery.operations =
            std::move(operations);
        failedDetachCheckpointRecovery.operationsDetermined = true;
        ApplyLocalDeleteIntentOperations(
            failedDetachCheckpointRecovery.operations);
        lastObservedInventory = inventory;
        hasLastObservedInventory = true;
        deleteIntentCapturePending = false;
        reconcileRequested = false;
        failedDetachCheckpointRecovery = {};
    }

    // 結果反映と厳密な保存検査を行い最大1件の通信を開始する(nowMilliseconds: 単調時計の現在時刻ms)。
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
            // 通信結果または適用の結果
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

            // 設定・全slot・合計容量を検査し、不確かな読取や破損があればアップロードと削除を開始しません。
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
            // journalの排他競合の再試行結果
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
        // 切替前に検証した接続準備
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
        // main threadの同期状態の借用
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
        // journalとアカウント保存先が一致か
        bool bindingMatches{};
        try
        {
            // 正規化したjournal本体のパス
            const auto journalPath =
                journal.FilePath().lexically_normal();
            // アカウントキーのjournal保存先
            const auto keyDirectory = journalPath.parent_path();
            // 全アカウントのjournal保存先
            const auto stateRoot = keyDirectory.parent_path();
            // エンジンの利用者データ保存先
            const auto engineRoot = stateRoot.parent_path();
            // キーから導出したアカウント保存先
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

        // 消去責任を共有する準備済みtoken
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
        // 公開前に対象journalと初期ローカル保存を完全に検証します。
        std::size_t conflictCount{};
        // 接続前に検証したローカル一覧
        LocalInventory initialInventory;
        try
        {
            // 検証または一覧照会時の世代
            const auto generation = journal.Generation();
            (void)generation;
            // 同期・検証する保存先
            for (const auto& resource : journal.Resources())
            {
                // 同期済みの基準状態
                if (const auto baseline = journal.Baseline(resource))
                {
                    ValidateFullSnapshotDocument(*baseline);
                }
                // 再送条件を保持した未完了更新
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

            // 切替前の設定データ検証用の読取先
            PlayerPrefs preparedPreferences(accountPreferencesPath);
            // 検査した設定データの文書
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
            // 設定と保存スロットの合計B
            std::uint64_t totalBytes =
                preferencesDocument.state
                        == LocalPersistenceDocumentState::Loaded
                    ? preferencesDocument.bytes.size()
                    : 0u;
            initialInventory.resources.push_back({
                CloudSaveResource::Preferences(),
                std::move(preferencesDocument)
            });

            // 切替前の保存スロットの検証用読取先
            SaveDataStore preparedSaves(accountSaveDirectory);
            // ローカル保存スロットの列挙結果
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
                // 検証済みの保存スロット名
                for (const auto& slot : listing.slots)
                {
                    // 検査した保存スロットの文書
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

        // 公開前に保持する接続準備状態
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
        // main threadの同期状態の借用
        auto& implementation = *m_implementation;
        if (!CanCommitPreparedAttachment(prepared) || profileEpoch == 0u)
        {
            implementation.DetachNoThrow();
            return;
        }
        // 公開前に保持する接続準備状態
        auto state = std::move(prepared.m_state);
        ++implementation.sessionSerial;
        implementation.RetireActiveMailbox();
        implementation.InvalidateConflictDescriptors();
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
        implementation.failedDetachCheckpointRecovery = {};
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

    CloudSaveDetachCheckpointRecovery
        CloudSaveSynchronizer::TakeFailedDetachCheckpointRecovery() noexcept
    {
        return std::exchange(
            m_implementation->failedDetachCheckpointRecovery,
            CloudSaveDetachCheckpointRecovery{});
    }

    void CloudSaveSynchronizer::CheckpointLocalStateForDetach()
    {
        m_implementation->CheckpointLocalStateForDetach();
    }

    void CloudSaveSynchronizer::UpdateAccessToken(std::string accessToken)
    {
        // main threadの同期状態の借用
        auto& implementation = *m_implementation;
        implementation.RequireOwner();
        if (!implementation.journal
            || !IsSafeOnlineBearerToken(accessToken))
        {
            EraseSecret(accessToken);
            throw std::invalid_argument(
                "Invalid cloud save access token update.");
        }
        // 消去責任を共有する準備済みtoken
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
        // main threadの同期状態の借用
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
            // journalの排他競合の再試行結果
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
                // 初回Read中の明示削除を初期Missingと区別するため、一覧のETagでこの通知処理中に永続化します。
                try
                {
                    if (implementation.activeFence
                        && implementation.activeFence
                            ->localWasLoadedAtReadStart
                        && !implementation.journal->Baseline(
                            implementation.activeFence->resource))
                    {
                        // 同期・検証する保存先
                        const auto& resource =
                            implementation.activeFence->resource;
                        // ローカル保存の文書または記録
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
                    // ローカルcommitは完了済みなので例外を出さず、Read完了後に再評価して不確かなら停止します。
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
        // main threadの同期状態の借用
        auto& implementation = *m_implementation;
        implementation.RequireOwner();
        if (!implementation.journal || !IsValidResource(resource))
        {
            throw std::logic_error(
                "Cloud save synchronizer is not attached.");
        }
        // 再送条件を保持した未完了更新
        const auto pending = implementation.journal->Pending(resource);
        // 競合時点のリモート保存状態
        const auto conflict = implementation.journal->Conflict(resource);
        if (!pending || !conflict
            || pending->mutationId != expectedMutationId)
        {
            // 古いUIの更新IDは通信中かの判定より先に拒否し、待機すれば有効になる操作として扱いません。
            throw std::logic_error("Cloud save conflict is stale.");
        }
        if (implementation.activeMailbox || implementation.retiredMailbox)
        {
            throw std::logic_error(
                "Cloud save conflict resolution requires an idle wire lane.");
        }

        // 検査したローカル保存の一覧
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
            // 観測したローカル状態へ適用済みか
            bool applied{};
            try
            {
                // 不在時のローカル文書の代替値
                LocalPersistenceDocument missing;
                missing.state = LocalPersistenceDocumentState::Missing;
                // 一致するローカル保存の検索結果
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
            // crash後も再実行できるよう、ローカルへ適用してからjournalを確定します。
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

    std::vector<CloudSaveConflictDescriptor>
        CloudSaveSynchronizer::Conflicts() const
    {
        // main threadの同期状態の借用
        const auto& implementation = *m_implementation;
        implementation.RequireOwner();
        // 本文を含まない公開用の競合一覧
        std::vector<CloudSaveConflictDescriptor> result;
        if (!implementation.journal)
        {
            return result;
        }
        // 検証または一覧照会時の世代
        const auto generation = implementation.journal->Generation();
        if (implementation.conflictDescriptorGeneration == generation
            && implementation.conflictDescriptorProfileEpoch
                == implementation.profileEpoch)
        {
            return implementation.conflictDescriptors;
        }
        // 本文を持たないjournalの競合一覧
        const auto summaries =
            implementation.journal->ConflictSummaries();
        result.reserve(summaries.size());
        // 公開用に変換する競合の管理情報
        for (const auto& summary : summaries)
        {
            result.push_back({
                summary.resource,
                summary.expectedMutationId,
                summary.localDeleted,
                summary.localByteLength,
                summary.remoteDeleted,
                summary.remoteByteLength
            });
        }
        implementation.conflictDescriptors = result;
        implementation.conflictDescriptorGeneration = generation;
        implementation.conflictDescriptorProfileEpoch =
            implementation.profileEpoch;
        return result;
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
