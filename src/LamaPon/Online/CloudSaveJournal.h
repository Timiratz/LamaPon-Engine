#pragma once

#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Online/CloudSave.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon::Detail
{
    class CloudSaveJournal;

    // ディスク変更前の一時的な排他競合だけを表し、同じjournalから安全に再試行できます。
    class CloudSaveJournalBusyError final : public std::runtime_error
    {
    public:
        // ディスク変更前の排他競合を表す例外を作る。
        CloudSaveJournalBusyError()
            : std::runtime_error("Cloud save journal is busy.")
        {
        }
    };

    // 同じアカウントを同時公開しないため、OnlineState/<key>/profile.session.lockを寿命中保持します。
    class CloudSaveProfileSessionLease final
    {
    public:
        // 同じアカウントの同時公開を防ぐロックを取得する(journal: 対象アカウントのjournal)。
        explicit CloudSaveProfileSessionLease(CloudSaveJournal& journal);
        // アカウントの公開ロックを解放する。
        ~CloudSaveProfileSessionLease();

        // 公開ロックの複製を禁止する。
        CloudSaveProfileSessionLease(
            const CloudSaveProfileSessionLease&) = delete;
        // 公開ロックのコピー代入を禁止する。
        CloudSaveProfileSessionLease& operator=(
            const CloudSaveProfileSessionLease&) = delete;
        // 公開ロックの移動を禁止する。
        CloudSaveProfileSessionLease(
            CloudSaveProfileSessionLease&&) = delete;
        // 公開ロックの移動代入を禁止する。
        CloudSaveProfileSessionLease& operator=(
            CloudSaveProfileSessionLease&&) = delete;

    private:
        struct Implementation;
        // 排他ロックまたはjournalの所有先
        std::unique_ptr<Implementation> m_implementation;
    };

    enum class CloudSaveJournalTestFailPoint : std::uint8_t
    {
        None,
        BeforeNextFlush,
        AfterNextFlush
    };

    // 永続化テスト用に次の該当処理だけを失敗させる(failPoint: 失敗させる段階)。
    void SetCloudSaveJournalTestFailPoint(
        CloudSaveJournalTestFailPoint failPoint) noexcept;

    // 実運用のロックを二重取得して排他性を確認する(lockPath: 検証するロックファイル)。
    bool IsCloudSaveJournalLockExclusiveForTesting(
        const std::filesystem::path& lockPath);

    enum class CloudSavePendingKind : std::uint8_t
    {
        Put,
        Delete
    };

    struct CloudSavePendingMutation final
    {
        // 更新する保存先
        CloudSaveResource resource;
        // 保存または削除の種別
        CloudSavePendingKind kind{ CloudSavePendingKind::Put };
        // 再送で共用する小文字UUIDv4
        std::string mutationId;
        // 更新元ETag・新規Putのみ空
        std::optional<std::string> baseEtag;
        // PutのJSON・Deleteでは空
        std::vector<std::uint8_t> content;
        // Putの内容ハッシュ・削除は空
        std::string sha256;
    };

    enum class CloudSaveDeleteIntentOperationKind : std::uint8_t
    {
        Record,
        Clear
    };

    struct CloudSaveDeleteIntentOperation final
    {
        // 削除意思を変更する保存先
        CloudSaveResource resource;
        // 削除意思の記録または解除
        CloudSaveDeleteIntentOperationKind kind{
            CloudSaveDeleteIntentOperationKind::Record
        };
    };

    // 本文・ETag・ハッシュを複製せず、再送識別子もOnlineServicesの外へ公開しません。
    struct CloudSaveConflictSummary final
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

    enum class CloudSaveConflictResolution : std::uint8_t
    {
        UseRemote,
        RetryLocal
    };

    // 通信前に送信内容を永続化し、全操作を作成時と同じmain threadで行います。
    // 保存先はUserDataDirectoryから導出したOnlineProfiles/<64桁の小文字キー>と厳密一致させ、journalは隣のOnlineState/<key>にだけ作成します。
    // ファイルとロックはopen後にもreparse pointとリンク数を検証し、検証後に同一ユーザーが祖先を差し替える攻撃は対象外とします。
    // 保存先と名前空間はSHA-256 bindingにだけ含め、生のplayerId・Discord ID・token・名前空間を保存しません。
    // 各変更はプロセス間ロック下で世代をCAS更新し、workerはこのobjectに触れずmailboxで結果を返します。
    class CloudSaveJournal final
    {
    public:
        // 保存先と名前空間を検証し既存journalを復旧する(trustedUserDataDirectory: 信頼する利用者データ保存先, accountProfile: 導出済みアカウントの保存先, gameId: ゲームの名前空間, environmentId: 環境の名前空間, backendBaseUrl: サービスの基点URL, allowInsecureLoopback: ローカルHTTPを許可するか)。
        CloudSaveJournal(
            std::filesystem::path trustedUserDataDirectory,
            PersistenceProfilePaths accountProfile,
            std::string gameId,
            std::string environmentId,
            std::string backendBaseUrl,
            bool allowInsecureLoopback = false);
        // journalの保持状態を破棄する。
        ~CloudSaveJournal();

        // journalの複製を禁止する。
        CloudSaveJournal(const CloudSaveJournal&) = delete;
        // journalのコピー代入を禁止する。
        CloudSaveJournal& operator=(const CloudSaveJournal&) = delete;
        // journalの移動を禁止する。
        CloudSaveJournal(CloudSaveJournal&&) = delete;
        // journalの移動代入を禁止する。
        CloudSaveJournal& operator=(CloudSaveJournal&&) = delete;

        // 未送信・競合中の更新がない保存先の基準を永続化する(snapshot: 検証済みの保存状態)。
        void RecordBaseline(const CloudSaveSnapshot& snapshot);

        // 本文と再送条件を通信前に永続化する(resource: 保存先, content: JSONのバイト列, mutationId: 再送共通の小文字UUIDv4, baseEtag: 更新元の強いETag・新規作成なら空)。
        // 戻る前にFlushFileBuffers済みの公開を終え、同じ保存先に未送信更新・競合があればlogic_errorとします。
        // 未完了更新の間のローカル保存はdurable overlayへ残し、ACK後に上位同期層が新しい更新IDで差分を送ります。
        void QueuePut(
            const CloudSaveResource& resource,
            const std::vector<std::uint8_t>& content,
            std::string_view mutationId,
            std::optional<std::string> baseEtag = std::nullopt);
        // 削除意思を未送信の更新へ昇格し永続化する(resource: 削除先, mutationId: 再送共通の小文字UUIDv4, baseEtag: 削除元の強いETag)。
        // 同じ保存先に未送信更新・競合があればlogic_errorとし、削除意思の解除とpending化を同じ世代で公開します。
        void QueueDelete(
            const CloudSaveResource& resource,
            std::string_view mutationId,
            std::string_view baseEtag);

        // ETag取得前のローカル削除意思を永続化する(resource: 削除した保存先)。
        // 再起動時の初期Missingと区別し、ETag取得後にQueueDeleteで未送信更新へ昇格します。
        void RecordLocalDeleteIntent(const CloudSaveResource& resource);
        // ローカル削除意思を解除して永続化する(resource: 削除意思を解除する保存先)。
        void ClearLocalDeleteIntent(const CloudSaveResource& resource);
        // 全操作を検証し単一の世代で一括永続化する(operations: 保存先ごとの削除意思の記録・解除)。
        void ApplyLocalDeleteIntentOperations(
            const std::vector<CloudSaveDeleteIntentOperation>& operations);
        // 永続化したローカル削除意思の有無を返す(resource: 確認する保存先)。
        [[nodiscard]] bool HasLocalDeleteIntent(
            const CloudSaveResource& resource) const;

        // 自動再送に渡すID・本文・CAS条件はこの返却値から変更しません。
        // 競合待ちを除く未送信更新を値として返す。
        [[nodiscard]] std::vector<CloudSavePendingMutation>
            Dispatchable() const;

        // 更新IDと本文を照合し成功状態を新しい基準にする(resource: 保存先, mutationId: 応答に対応する更新ID, snapshot: 成功した保存状態)。
        void RecordSuccess(
            const CloudSaveResource& resource,
            std::string_view mutationId,
            const CloudSaveSnapshot& snapshot);
        // 未送信更新に対応するリモート競合状態を永続化する(resource: 保存先, mutationId: 応答に対応する更新ID, remoteSnapshot: CAS競合時点の保存状態)。
        void RecordConflict(
            const CloudSaveResource& resource,
            std::string_view mutationId,
            const CloudSaveSnapshot& remoteSnapshot);

        // 競合を採用または新しいIDで再送する(resource: 保存先, expectedMutationId: 解決する競合の更新ID, resolution: 解決方針, replacementMutationId: 再送時の新しい小文字UUIDv4)。
        // UseRemoteはConflict()の全内容をローカルへatomicに適用した後でだけ確定し、復旧時も同じexpectedMutationIdを使います。
        // RetryLocalは元のpending内容を保ち、競合ETagと新しい更新IDへ差し替えます。
        void ResolveConflict(
            const CloudSaveResource& resource,
            std::string_view expectedMutationId,
            CloudSaveConflictResolution resolution,
            std::string_view replacementMutationId = {});

        // 同期済みの基準状態を値として返す(resource: 取得する保存先)。
        [[nodiscard]] std::optional<CloudSaveSnapshot> Baseline(
            const CloudSaveResource& resource) const;
        // 競合時点のリモート状態を値として返す(resource: 取得する保存先)。
        [[nodiscard]] std::optional<CloudSaveSnapshot> Conflict(
            const CloudSaveResource& resource) const;
        // 競合中も元の未送信更新を値として返す(resource: 取得する保存先)。
        [[nodiscard]] std::optional<CloudSavePendingMutation> Pending(
            const CloudSaveResource& resource) const;
        // 競合の本文を含まない一覧を値として返す。
        [[nodiscard]] std::vector<CloudSaveConflictSummary>
            ConflictSummaries() const;
        // journalに記録した保存先を値として返す。
        [[nodiscard]] std::vector<CloudSaveResource> Resources() const;
        // 競合中も含む未完了更新の有無を返す(resource: 確認する保存先)。
        [[nodiscard]] bool HasPending(
            const CloudSaveResource& resource) const;
        // 永続化済みの現在の世代を返す。
        [[nodiscard]] std::uint64_t Generation() const;
        // journalの存続中有効なファイルパスを借用する。
        [[nodiscard]] const std::filesystem::path& FilePath() const;

    private:
        struct Implementation;
        // 排他ロックまたはjournalの所有先
        std::unique_ptr<Implementation> m_implementation;
    };
}
