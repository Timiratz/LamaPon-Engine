#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
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
    struct LocalPersistenceDocumentIdentity final
    {
        std::uint64_t volumeSerial{};
        std::array<std::uint8_t, 16u> fileId{};
        std::uint64_t byteLength{};
        std::int64_t lastWriteTime{};
        std::int64_t changeTime{};
        bool valid{};
        // trueならbytesは空fileを含めた全内容です。falseのCorruptは
        // oversizeのため、固定サイズfile identityだけを保持します。
        bool completeBytes{};

        friend bool operator==(
            const LocalPersistenceDocumentIdentity&,
            const LocalPersistenceDocumentIdentity&) = default;
    };

    // Cloud同期が曖昧な既存Load APIを使わず、diskの状態をfail-closedで
    // 判定するための内部状態です。
    enum class LocalPersistenceDocumentState : std::uint8_t
    {
        Loaded,
        Missing,
        Unavailable,
        Corrupt
    };

    struct LocalPersistenceDocument final
    {
        LocalPersistenceDocumentState state{
            LocalPersistenceDocumentState::Unavailable
        };
        // Loadedでは検証済みfull document、Corruptでは上限内で安全に
        // 読み切れたraw bytesを保持します。後者はconditional discardの
        // 同一性確認専用で、JSONとして利用してはいけません。
        std::vector<std::uint8_t> bytes;
        LocalPersistenceDocumentIdentity identity;
    };

    struct LocalPersistenceSlotListing final
    {
        LocalPersistenceDocumentState state{
            LocalPersistenceDocumentState::Unavailable
        };
        // Loadedのときだけ、Windows ordinal-ignore-caseで一意なslotです。
        std::vector<std::string> slots;
    };

    enum class LocalPersistenceConditionalApplyResult : std::uint8_t
    {
        Applied,
        LocalChanged
    };

    enum class LocalPersistenceResourceKind : std::uint8_t
    {
        PlayerPrefs,
        SaveData
    };

    struct LocalPersistenceCommitEvent final
    {
        LocalPersistenceResourceKind kind{
            LocalPersistenceResourceKind::PlayerPrefs
        };
        // callback中だけ有効な非所有参照です。通知経路では割当しません。
        const void* source{};
        const std::filesystem::path* filePath{};
        std::string_view slot;
        bool deleted{};
    };

    // 非所有callback/contextはAttachからDetachまで呼出し側が生存させます。
    // falseはlocal commitを失敗させず、Consume...Failureで後続reconcileへ
    // 渡します。全操作はPlayerPrefs/SaveDataと同じmain thread限定です。
    using LocalPersistenceCommitCallback = bool(*)(
        void* context,
        std::uint64_t profileEpoch,
        const LocalPersistenceCommitEvent& event) noexcept;
    // trueを返した後だけlocal deleteのcommit pointへ進みます。cloud WALを
    // durable化できない場合はfalseで削除を中止し、既存fileを維持します。
    using LocalPersistencePreDeleteCallback = bool(*)(
        void* context,
        std::uint64_t profileEpoch,
        const LocalPersistenceCommitEvent& event) noexcept;

    using LocalPersistenceObserverToken = std::uint64_t;

    [[nodiscard]] LocalPersistenceObserverToken
        AttachLocalPersistenceCommitObserver(
        LocalPersistenceCommitCallback callback,
        void* context,
        std::uint64_t profileEpoch,
        LocalPersistencePreDeleteCallback preDeleteCallback = nullptr) noexcept;
    [[nodiscard]] bool DetachLocalPersistenceCommitObserver(
        LocalPersistenceObserverToken token) noexcept;
    [[nodiscard]] bool ConsumeLocalPersistenceObserverFailure() noexcept;

    class ScopedLocalPersistenceObserverSuppression final
    {
    public:
        ScopedLocalPersistenceObserverSuppression() noexcept;
        ~ScopedLocalPersistenceObserverSuppression();

        ScopedLocalPersistenceObserverSuppression(
            const ScopedLocalPersistenceObserverSuppression&) = delete;
        ScopedLocalPersistenceObserverSuppression& operator=(
            const ScopedLocalPersistenceObserverSuppression&) = delete;
    };

    enum class LocalPersistenceTestFailPoint : std::uint8_t
    {
        None,
        BeforeFlush,
        AfterFlushBeforePublish
    };

    void SetLocalPersistenceTestFailPoint(
        LocalPersistenceTestFailPoint failPoint) noexcept;

    // productionと同じper-target lockを二重取得する回帰専用です。
    [[nodiscard]] bool IsLocalPersistenceLockExclusiveForTesting(
        const std::filesystem::path& targetPath);

    class LocalPersistenceDocuments final
    {
    public:
        [[nodiscard]] static LocalPersistenceDocument ReadPlayerPrefs(
            const PlayerPrefs& playerPrefs);
        [[nodiscard]] static LocalPersistenceDocument ReadSaveData(
            const SaveDataStore& saveData,
            std::string_view slot);
        [[nodiscard]] static LocalPersistenceSlotListing ListSaveData(
            const SaveDataStore& saveData);

        // remote full documentは検証を全て終え、replacement stateも準備して
        // からpublishします。成功後もPlayerPrefs objectのaddressは不変です。
        static void ApplyPlayerPrefs(
            PlayerPrefs& playerPrefs,
            std::span<const std::uint8_t> fullDocument);
        static void DeletePlayerPrefs(PlayerPrefs& playerPrefs);
        // observedは直前のstrict read結果です。同じper-target lock内で
        // diskを再照合し、一致時だけremoteをpublishします。別processの
        // local commitが先行した場合は何も変更せずLocalChangedです。
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            ApplyPlayerPrefsIfUnchanged(
            PlayerPrefs& playerPrefs,
            const LocalPersistenceDocument& observed,
            std::span<const std::uint8_t> fullDocument);
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            DeletePlayerPrefsIfUnchanged(
            PlayerPrefs& playerPrefs,
            const LocalPersistenceDocument& observed);
        // profile activation用。observerはPlayerPrefs pimpl外なので移動しません。
        static void SwapPlayerPrefsLoadedState(
            PlayerPrefs& target,
            PlayerPrefs& prepared) noexcept;
        // ReadPlayerPrefsが返した同一full-byte snapshotから、diskを
        // 再open/変更せずprepared PlayerPrefs memoryを構築します。
        static void LoadPlayerPrefsSnapshot(
            PlayerPrefs& target,
            const LocalPersistenceDocument& snapshot);
        static void ApplySaveData(
            SaveDataStore& saveData,
            std::string_view slot,
            std::span<const std::uint8_t> fullDocument);
        static void DeleteSaveData(
            SaveDataStore& saveData,
            std::string_view slot);
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            ApplySaveDataIfUnchanged(
            SaveDataStore& saveData,
            std::string_view slot,
            const LocalPersistenceDocument& observed,
            std::span<const std::uint8_t> fullDocument);
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            DeleteSaveDataIfUnchanged(
            SaveDataStore& saveData,
            std::string_view slot,
            const LocalPersistenceDocument& observed);
    };

    // PlayerPrefs/SaveData writerとremote applyが共有する内部primitiveです。
    // `.writing`へのFlushFileBuffers成功後だけWRITE_THROUGH renameします。
    void DurablePublishLocalDocument(
        const std::filesystem::path& targetPath,
        std::string_view bytes);
    [[nodiscard]] bool DurableDeleteLocalDocument(
        const std::filesystem::path& targetPath);
    // conditional操作は同じtarget lockを使うLamaPon writer間のCASです。
    // lock protocolを無視して保存先を直接書き換える外部processとの原子的CASは
    // Win32のpath replaceでは提供できません。ただしMissing観測へのpublishは
    // no-replaceとし、後から現れた外部fileを上書きしません。
    [[nodiscard]] LocalPersistenceConditionalApplyResult
        DurablePublishLocalDocumentIfUnchanged(
        const std::filesystem::path& targetPath,
        const LocalPersistenceDocument& observed,
        std::string_view bytes,
        std::size_t maximumBytes,
        std::string_view saveSlot = {});
    [[nodiscard]] LocalPersistenceConditionalApplyResult
        DurableDeleteLocalDocumentIfUnchanged(
        const std::filesystem::path& targetPath,
        const LocalPersistenceDocument& observed,
        std::size_t maximumBytes,
        std::string_view saveSlot = {});
    void NotifyLocalPersistenceCommit(
        const LocalPersistenceCommitEvent& event) noexcept;
    [[nodiscard]] bool PrepareLocalPersistenceDelete(
        const LocalPersistenceCommitEvent& event) noexcept;

    // PlayerPrefs内部のreplacement準備とread seamが同一strict contractを
    // 使うための検証関数です。成功以外は例外です。
    void ValidatePlayerPrefsFullDocument(std::string_view bytes);
    void ValidateSaveDataFullDocument(
        std::string_view slot,
        std::string_view bytes);
}
