#pragma once

#include "LamaPon/Online/CloudSave.h"
#include "LamaPon/Online/CloudSaveJournal.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class PlayerPrefs;
    class SaveDataStore;
}

namespace LamaPon::Detail
{
    class CloudSaveClient;

    enum class CloudSaveSynchronizerState : std::uint8_t
    {
        Detached,
        Idle,
        Synchronizing,
        BackingOff,
        Conflict,
        Unauthorized,
        Halted
    };

    enum class CloudSaveSynchronizerStopReason : std::uint8_t
    {
        None,
        LocalUnavailable,
        LocalCorrupt,
        RemoteRejected,
        InvalidRemoteResponse,
        JournalFailure,
        InternalFailure
    };

    struct CloudSaveSynchronizerStatus final
    {
        CloudSaveSynchronizerState state{
            CloudSaveSynchronizerState::Detached
        };
        CloudSaveSynchronizerStopReason stopReason{
            CloudSaveSynchronizerStopReason::None
        };
        std::uint64_t retryAtMilliseconds{};
        std::size_t conflictCount{};
    };

    struct CloudSaveConflictDescriptor final
    {
        CloudSaveResource resource;
        std::string expectedMutationId;
        bool localDeleted{};
        std::size_t localByteLength{};
        bool remoteDeleted{};
        std::size_t remoteByteLength{};
    };

    using CloudSaveMutationIdGenerator =
        std::function<std::string()>;

    // detach checkpointはjournal publishを始める前に操作列全体を確定します。
    // 操作列は1世代のbatchとしてpublishします。失敗後は新しいjournal
    // instanceへ同じ列を冪等に再適用します。strict local scan自体が
    // 成立しなかった場合はoperationsDetermined=falseです。
    struct CloudSaveDetachCheckpointRecovery final
    {
        bool operationsDetermined{};
        std::vector<CloudSaveDeleteIntentOperation> operations;
    };

    class PreparedCloudSaveAttachment final
    {
    public:
        struct State;

        PreparedCloudSaveAttachment() noexcept;
        ~PreparedCloudSaveAttachment();
        PreparedCloudSaveAttachment(PreparedCloudSaveAttachment&&) noexcept;
        PreparedCloudSaveAttachment& operator=(
            PreparedCloudSaveAttachment&&) noexcept;
        PreparedCloudSaveAttachment(
            const PreparedCloudSaveAttachment&) = delete;
        PreparedCloudSaveAttachment& operator=(
            const PreparedCloudSaveAttachment&) = delete;

        [[nodiscard]] bool IsValid() const noexcept;

    private:
        explicit PreparedCloudSaveAttachment(
            std::unique_ptr<State> state) noexcept;
        friend class CloudSaveSynchronizer;
        std::unique_ptr<State> m_state;
    };

    // CloudSaveClientの同期wire呼出しだけをdetached workerへ渡し、
    // Journalとローカル永続化はmain threadでだけ更新する内部同期層です。
    // 全public methodは構築した同一main threadから呼びます。
    //
    // workerはthis/Journal/PlayerPrefs/SaveDataStoreを参照せず、immutableな
    // requestとshared mailboxだけを所有します。Detach/token更新後に返った結果は
    // sessionSerial/profileEpoch/accountStorageKey/journal generation/mutationId
    // fenceで破棄されるため、SignOutは通信完了を待ちません。
    class CloudSaveSynchronizer final
    {
    public:
        CloudSaveSynchronizer(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::shared_ptr<const CloudSaveClient> client,
            CloudSaveMutationIdGenerator mutationIdGenerator = {});
        ~CloudSaveSynchronizer();

        CloudSaveSynchronizer(const CloudSaveSynchronizer&) = delete;
        CloudSaveSynchronizer& operator=(
            const CloudSaveSynchronizer&) = delete;
        CloudSaveSynchronizer(CloudSaveSynchronizer&&) = delete;
        CloudSaveSynchronizer& operator=(CloudSaveSynchronizer&&) = delete;

        // accountStorageKeyはPersistenceProfilesが作った64文字lower-hex keyです。
        // 生playerId/Discord IDを渡してはいけません。tokenはmemoryだけに保持し、
        // status・例外・journalへ出しません。
        void Attach(
            CloudSaveJournal& journal,
            std::uint64_t profileEpoch,
            std::string accountStorageKey,
            std::string accessToken);
        // profile切替前にtoken・journal・初期local snapshotのI/O/割当を
        // 完了し、切替後はnoexcept moveだけでattachします。
        [[nodiscard]] PreparedCloudSaveAttachment PrepareAttachment(
            CloudSaveJournal& journal,
            const std::filesystem::path& accountPreferencesPath,
            const std::filesystem::path& accountSaveDirectory,
            std::string accountStorageKey,
            std::string accessToken);
        [[nodiscard]] bool CanCommitPreparedAttachment(
            const PreparedCloudSaveAttachment& prepared) const noexcept;
        void CommitPreparedAttachment(
            PreparedCloudSaveAttachment&& prepared,
            std::uint64_t profileEpoch) noexcept;

        // sign-out直前にworker fenceを失効させ、現在のstrict local状態から
        // delete intentだけをjournalへdurable checkpointします。networkは
        // 開始しません。失敗時はcallerがjournal/profile leaseを保持したまま
        // quarantineし、明示解決まで同accountを再公開してはいけません。
        // productionのpre-delete WALを故意に迂回し、さらにbatch publish
        // 開始前のI/O失敗中にprocessがcrashした場合の意図復元は
        // 保証外です。通常のSaveData::DeleteSlotはpre-delete WALを使います。
        void CheckpointLocalStateForDetach();
        [[nodiscard]] CloudSaveDetachCheckpointRecovery
            TakeFailedDetachCheckpointRecovery() noexcept;
        void Detach() noexcept;

        // 同一accountのproactive refresh用です。旧inflightを待たずstale化し、
        // Unauthorized signalを解除して新tokenで再同期します。
        void UpdateAccessToken(std::string accessToken);

        // local commit observer edgeまたは初期activateから呼びます。
        void RequestReconcile();

        // SaveData local deleteのcommit pointより前に呼び、intent WALを
        // durable化します。失敗時はthrowし、callerはlocal deleteを中止します。
        void PrepareLocalDelete(const CloudSaveResource& resource);

        // mailboxを反映し、必要なら最大1件のwire taskを開始します。
        // nowMillisecondsは呼出し側のmonotonic clockです。
        void Tick(std::uint64_t nowMilliseconds);

        // UseRemoteはremote full snapshotをatomic適用してからjournalを解決します。
        // RetryLocalは同じpending内容に新UUIDを割り当てます。
        void ResolveConflict(
            const CloudSaveResource& resource,
            std::string_view expectedMutationId,
            CloudSaveConflictResolution resolution);

        [[nodiscard]] CloudSaveSynchronizerStatus Status() const noexcept;
        // 公開DTOを組み立てるmain-thread bridgeです。ETag/content/hashは
        // 返さず、expectedMutationIdもOnlineServicesのopaque ID registry
        // より外へ出しません。
        [[nodiscard]] std::vector<CloudSaveConflictDescriptor>
            Conflicts() const;
        [[nodiscard]] bool IsAttached() const noexcept;
        [[nodiscard]] bool HasInFlightRequest() const noexcept;
        // 200/204/412等、Bearerが認可されたwire応答をmain threadで
        // 完全検証できたときだけ一度立つedgeです。token更新だけでは立ちません。
        [[nodiscard]] bool ConsumeAuthorizedWireSuccessSignal() noexcept;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_implementation;
    };
}
