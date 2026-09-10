#pragma once

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
        // Loadedのときだけ、検証済みfull documentのbyte列を保持します。
        std::vector<std::uint8_t> bytes;
    };

    struct LocalPersistenceSlotListing final
    {
        LocalPersistenceDocumentState state{
            LocalPersistenceDocumentState::Unavailable
        };
        // Loadedのときだけ、Windows ordinal-ignore-caseで一意なslotです。
        std::vector<std::string> slots;
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

    using LocalPersistenceObserverToken = std::uint64_t;

    [[nodiscard]] LocalPersistenceObserverToken
        AttachLocalPersistenceCommitObserver(
        LocalPersistenceCommitCallback callback,
        void* context,
        std::uint64_t profileEpoch) noexcept;
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
        // profile activation用。observerはPlayerPrefs pimpl外なので移動しません。
        static void SwapPlayerPrefsLoadedState(
            PlayerPrefs& target,
            PlayerPrefs& prepared) noexcept;
        static void ApplySaveData(
            SaveDataStore& saveData,
            std::string_view slot,
            std::span<const std::uint8_t> fullDocument);
        static void DeleteSaveData(
            SaveDataStore& saveData,
            std::string_view slot);
    };

    // PlayerPrefs/SaveData writerとremote applyが共有する内部primitiveです。
    // `.writing`へのFlushFileBuffers成功後だけWRITE_THROUGH renameします。
    void DurablePublishLocalDocument(
        const std::filesystem::path& targetPath,
        std::string_view bytes);
    [[nodiscard]] bool DurableDeleteLocalDocument(
        const std::filesystem::path& targetPath);
    void NotifyLocalPersistenceCommit(
        const LocalPersistenceCommitEvent& event) noexcept;

    // PlayerPrefs内部のreplacement準備とread seamが同一strict contractを
    // 使うための検証関数です。成功以外は例外です。
    void ValidatePlayerPrefsFullDocument(std::string_view bytes);
    void ValidateSaveDataFullDocument(
        std::string_view slot,
        std::string_view bytes);
}
