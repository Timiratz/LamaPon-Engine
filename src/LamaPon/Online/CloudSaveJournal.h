#pragma once

#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Online/CloudSave.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon::Detail
{
    enum class CloudSaveJournalTestFailPoint : std::uint8_t
    {
        None,
        BeforeNextFlush,
        AfterNextFlush
    };

    // internal durability testだけが使うone-shot failpointです。
    void SetCloudSaveJournalTestFailPoint(
        CloudSaveJournalTestFailPoint failPoint) noexcept;

    // productionと同じlock取得を二重に行うWindows排他回帰専用です。
    bool IsCloudSaveJournalLockExclusiveForTesting(
        const std::filesystem::path& lockPath);

    enum class CloudSavePendingKind : std::uint8_t
    {
        Put,
        Delete
    };

    struct CloudSavePendingMutation final
    {
        CloudSaveResource resource;
        CloudSavePendingKind kind{ CloudSavePendingKind::Put };
        std::string mutationId;
        // Putの新規作成時だけnulloptです。Deleteでは必須です。
        std::optional<std::string> baseEtag;
        // Deleteでは空です。Putでは送信するJSON byte列をそのまま保持します。
        std::vector<std::uint8_t> content;
        std::string sha256;
    };

    enum class CloudSaveConflictResolution : std::uint8_t
    {
        UseRemote,
        RetryLocal
    };

    // Cloud通信より先にpendingを永続化する内部write-ahead journalです。
    // trustedUserDataDirectoryはPersistenceProfiles::UserDataDirectory()を
    // 渡します。そこから導出した .../OnlineProfiles/<64 lower-hex key> と
    // accountProfileが厳密に一致しなければ拒否します。journal自身は
    // account directoryを作らず、siblingの OnlineState/<key>だけを使います。
    // 同一userの別processが検証後にancestor directory自体を差し替える攻撃は
    // 脅威モデル外です。各journal file/lockはopen後にもreparse/linkを検証します。
    //
    // game/environment/normalized backend base URL/account rootはSHA-256 bindingへだけ
    // 入り、生のplayerId、Discord ID、token、namespaceは保存しません。
    // 各変更はWindowsのprocess間lock下でgeneration CASを行います。
    // 全method/getterは作成した同一main threadからだけ呼びます。workerは
    // このobjectへ触れず、通信結果をmailbox経由でmain threadへ返します。
    class CloudSaveJournal final
    {
    public:
        CloudSaveJournal(
            std::filesystem::path trustedUserDataDirectory,
            PersistenceProfilePaths accountProfile,
            std::string gameId,
            std::string environmentId,
            std::string backendBaseUrl,
            bool allowInsecureLoopback = false);
        ~CloudSaveJournal();

        CloudSaveJournal(const CloudSaveJournal&) = delete;
        CloudSaveJournal& operator=(const CloudSaveJournal&) = delete;
        CloudSaveJournal(CloudSaveJournal&&) = delete;
        CloudSaveJournal& operator=(CloudSaveJournal&&) = delete;

        // 成功応答やmanifest/readで得たfull snapshotをbaselineへ記録します。
        // 同じresourceにpending/conflictがある間は上書きできません。
        void RecordBaseline(const CloudSaveSnapshot& snapshot);

        // return前にFlushFileBuffers済みのwrite-ahead publishを完了します。
        // 同じresourceにpending/conflictがある場合はlogic_errorです。
        // その間の新しいローカル保存はローカル側のdurable overlayとして
        // 残し、旧pendingのack後に上位同期層が差分を新mutationにします。
        void QueuePut(
            const CloudSaveResource& resource,
            const std::vector<std::uint8_t>& content,
            std::string_view mutationId,
            std::optional<std::string> baseEtag = std::nullopt);
        void QueueDelete(
            const CloudSaveResource& resource,
            std::string_view mutationId,
            std::string_view baseEtag);

        // conflict待ちを除くpendingだけを返します。自動retry時もこの値を
        // 変更せずCloudSaveClientへ渡します。
        [[nodiscard]] std::vector<CloudSavePendingMutation>
            Dispatchable() const;

        void RecordSuccess(
            const CloudSaveResource& resource,
            std::string_view mutationId,
            const CloudSaveSnapshot& snapshot);
        void RecordConflict(
            const CloudSaveResource& resource,
            std::string_view mutationId,
            const CloudSaveSnapshot& remoteSnapshot);

        // UseRemoteはcallerがConflict()のfull snapshotをローカルへatomicに
        // 適用した後だけ呼びます。journalを先にfinalizeしてはいけません。
        // crash復旧時にlocal==remoteなら同じexpectedMutationIdで再実行できます。
        // RetryLocalはpending内容を維持し、conflict ETagと新mutationIdにします。
        void ResolveConflict(
            const CloudSaveResource& resource,
            std::string_view expectedMutationId,
            CloudSaveConflictResolution resolution,
            std::string_view replacementMutationId = {});

        [[nodiscard]] std::optional<CloudSaveSnapshot> Baseline(
            const CloudSaveResource& resource) const;
        [[nodiscard]] std::optional<CloudSaveSnapshot> Conflict(
            const CloudSaveResource& resource) const;
        // conflict中も旧mutationをそのまま返します。
        [[nodiscard]] std::optional<CloudSavePendingMutation> Pending(
            const CloudSaveResource& resource) const;
        [[nodiscard]] std::vector<CloudSaveResource> Resources() const;
        [[nodiscard]] bool HasPending(
            const CloudSaveResource& resource) const;
        [[nodiscard]] std::uint64_t Generation() const;
        [[nodiscard]] const std::filesystem::path& FilePath() const;

    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_implementation;
    };
}
