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
        // 現在の同期状態
        CloudSaveSynchronizerState state{
            CloudSaveSynchronizerState::Detached
        };
        // 同期を停止した理由
        CloudSaveSynchronizerStopReason stopReason{
            CloudSaveSynchronizerStopReason::None
        };
        // 単調時計での再試行時刻ms
        std::uint64_t retryAtMilliseconds{};
        // 明示解決が必要な競合数
        std::size_t conflictCount{};
    };

    struct CloudSaveConflictDescriptor final
    {
        // 競合した保存先
        CloudSaveResource resource;
        // 競合を識別する内部の更新ID
        std::string expectedMutationId;
        // ローカル更新が削除か
        bool localDeleted{};
        // ローカル内容のバイト数
        std::size_t localByteLength{};
        // リモートが削除済みか
        bool remoteDeleted{};
        // リモート内容のバイト数
        std::size_t remoteByteLength{};
    };

    using CloudSaveMutationIdGenerator =
        std::function<std::string()>;

    // 切断前に確定した操作を単一世代で公開し、失敗後は新しいjournalに同じ列を冪等に再適用します。
    struct CloudSaveDetachCheckpointRecovery final
    {
        // 検査を終え操作列を確定したか
        bool operationsDetermined{};
        // 復旧時に再適用する削除意思
        std::vector<CloudSaveDeleteIntentOperation> operations;
    };

    class PreparedCloudSaveAttachment final
    {
    public:
        struct State;

        // 未準備の接続状態を作る。
        PreparedCloudSaveAttachment() noexcept;
        // 準備済み状態の所有を解放する。
        ~PreparedCloudSaveAttachment();
        // 準備済みの接続状態を移動する。
        PreparedCloudSaveAttachment(PreparedCloudSaveAttachment&&) noexcept;
        // 準備済み接続状態の所有権を移す。
        PreparedCloudSaveAttachment& operator=(
            PreparedCloudSaveAttachment&&) noexcept;
        // 接続状態の複製を禁止する。
        PreparedCloudSaveAttachment(
            const PreparedCloudSaveAttachment&) = delete;
        // 接続状態のコピー代入を禁止する。
        PreparedCloudSaveAttachment& operator=(
            const PreparedCloudSaveAttachment&) = delete;

        // 接続の準備状態を保持しているかを返す。
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        // 切替前に検証した状態を保持する(state: 準備済み接続状態の所有先)。
        explicit PreparedCloudSaveAttachment(
            std::unique_ptr<State> state) noexcept;
        friend class CloudSaveSynchronizer;
        // 切替前に検証した接続状態の所有先
        std::unique_ptr<State> m_state;
    };

    // 全操作を構築時と同じmain threadで行い、同期HTTPだけをdetached workerへ渡します。
    // workerはthis・journal・ローカル保存先を参照せず、共有するclient・token・mailboxと不変の要求だけで完走します。
    // 切断・token更新後は接続世代・公開先・アカウントキー・journal世代・更新IDで古い結果を破棄し、通信の完了を待ちません。
    class CloudSaveSynchronizer final
    {
    public:
        // 借用するローカル保存先と通信処理を設定する(preferences: 設定データの借用, saves: 保存スロットの借用, client: 同期HTTP処理の共有所有先, mutationIdGenerator: 新しい更新IDの生成・空なら標準)。
        CloudSaveSynchronizer(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::shared_ptr<const CloudSaveClient> client,
            CloudSaveMutationIdGenerator mutationIdGenerator = {});
        // 通信を待たず借用先から切り離し同期状態を解放する。
        ~CloudSaveSynchronizer();

        // 同期状態の複製を禁止する。
        CloudSaveSynchronizer(const CloudSaveSynchronizer&) = delete;
        // 同期状態のコピー代入を禁止する。
        CloudSaveSynchronizer& operator=(
            const CloudSaveSynchronizer&) = delete;
        // 同期状態の移動を禁止する。
        CloudSaveSynchronizer(CloudSaveSynchronizer&&) = delete;
        // 同期状態の移動代入を禁止する。
        CloudSaveSynchronizer& operator=(CloudSaveSynchronizer&&) = delete;

        // 既存のローカル状態を検証し同期先を接続する(journal: 切断まで借用するjournal, profileEpoch: 公開先を識別する世代, accountStorageKey: 64桁の小文字アカウントキー, accessToken: メモリだけに保持するtoken)。
        // アカウントキーはPersistenceProfilesが導出した値を渡し、tokenを診断・例外・journalに出しません。
        void Attach(
            CloudSaveJournal& journal,
            std::uint64_t profileEpoch,
            std::string accountStorageKey,
            std::string accessToken);
        // 切替前にtoken・journal・ローカル保存の検証と確保を終える(journal: 借用する同期記録, accountPreferencesPath: 設定ファイル, accountSaveDirectory: 保存スロットの場所, accountStorageKey: 64桁の小文字キー, accessToken: 短命token)。
        [[nodiscard]] PreparedCloudSaveAttachment PrepareAttachment(
            CloudSaveJournal& journal,
            const std::filesystem::path& accountPreferencesPath,
            const std::filesystem::path& accountSaveDirectory,
            std::string accountStorageKey,
            std::string accessToken);
        // この同期層が作った有効な準備状態かを返す(prepared: 切替前の準備状態)。
        [[nodiscard]] bool CanCommitPreparedAttachment(
            const PreparedCloudSaveAttachment& prepared) const noexcept;
        // 準備済み状態を例外なく接続し不正なら切断する(prepared: 引き取る準備状態, profileEpoch: 0以外の公開先の世代)。
        void CommitPreparedAttachment(
            PreparedCloudSaveAttachment&& prepared,
            std::uint64_t profileEpoch) noexcept;

        // 通信結果を失効させ切断前の削除意思を検査して永続化する。
        // 失敗時はjournalと公開ロックを隔離して保持し、明示解決まで同じアカウントを再公開してはいけません。
        // 通常のDeleteSlotは削除前WALを使い、これを迂回した削除とbatch公開前のIO失敗中のcrashが重なる場合は意思の復元を保証しません。
        void CheckpointLocalStateForDetach();
        // 切断前の永続化に失敗した操作列を引き取り内部を空にする。
        [[nodiscard]] CloudSaveDetachCheckpointRecovery
            TakeFailedDetachCheckpointRecovery() noexcept;
        // 通信を待たず借用先と接続状態を解除する。
        void Detach() noexcept;

        // 同じアカウントのtokenを差し替え古い結果を失効させる(accessToken: メモリで保持する新しいtoken)。
        void UpdateAccessToken(std::string accessToken);

        // 初回接続またはローカルcommitを受け削除意思を記録して再照合を求める。
        void RequestReconcile();

        // ローカル削除のcommit前に削除意思を永続化する(resource: 削除する保存先)。
        // 永続化に失敗した場合は送出する例外を受け、呼出し側でローカル削除を中止します。
        void PrepareLocalDelete(const CloudSaveResource& resource);

        // 通信結果を反映し最大1件の通信を開始する(nowMilliseconds: 呼出し側の単調時計の時刻ms)。
        void Tick(std::uint64_t nowMilliseconds);

        // 待機中の競合を採用または新しい更新IDで再送する(resource: 保存先, expectedMutationId: 解決する競合の更新ID, resolution: リモート採用またはローカル再送)。
        // UseRemoteは全文書のatomicなローカル適用後にjournalを確定し、RetryLocalは元のpending内容に新UUIDを割り当てます。
        void ResolveConflict(
            const CloudSaveResource& resource,
            std::string_view expectedMutationId,
            CloudSaveConflictResolution resolution);

        // 停止理由と再試行時刻を含む同期状態を値として返す。
        [[nodiscard]] CloudSaveSynchronizerStatus Status() const noexcept;
        // 更新IDもOnlineServicesのopaque ID registryより外へ公開しません。
        // 本文・ETag・ハッシュを含まない競合一覧を値として返す。
        [[nodiscard]] std::vector<CloudSaveConflictDescriptor>
            Conflicts() const;
        // アカウントのjournalを接続中かを返す。
        [[nodiscard]] bool IsAttached() const noexcept;
        // 失効済みも含む通信の実行が残っているかを返す。
        [[nodiscard]] bool HasInFlightRequest() const noexcept;
        // 認可されたwire応答の検証完了を一度だけ消費する。
        // token更新だけでは立たず、200・204・412などの認可応答をmain threadで検証した場合だけ立ちます。
        [[nodiscard]] bool ConsumeAuthorizedWireSuccessSignal() noexcept;

    private:
        struct Implementation;
        // main threadの同期状態の所有先
        std::unique_ptr<Implementation> m_implementation;
    };
}
