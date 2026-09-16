#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    class PlayerPrefs;
    class SaveDataStore;

    struct PersistenceProfilePaths final
    {
        std::filesystem::path rootDirectory;
        std::filesystem::path playerPrefsFile;
        std::filesystem::path saveDataDirectory;
        // ゲストでは空、アカウントではゲーム・環境・playerIdを
        // 長さ付きで入力した64文字の小文字SHA-256です。
        std::string accountStorageKey;
        bool isGuest{ true };
    };

    enum class GuestPersistenceImportStatus
    {
        Imported,
        NothingToImport,
        AccountAlreadyHasData,
        Failed
    };

    struct GuestPersistenceImportResult final
    {
        GuestPersistenceImportStatus status{
            GuestPersistenceImportStatus::Failed
        };
        // 失敗理由は固定文言です。playerIdやローカルパスを含みません。
        std::string error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status
                == GuestPersistenceImportStatus::Imported;
        }
    };

    // ゲストとオンラインアカウントのローカル保存先を解決します。
    //
    // ゲストは従来どおり <userData>/PlayerPrefs.json と
    // <userData>/Saves を使うため、既存ゲームのデータを移動しません。
    // アカウントは <userDataの親>/OnlineProfiles/<SHA-256>/... に
    // 分離し、生のgameId/environmentId/backend playerIdをパスへ
    // 一切含めません。ゲーム表示名由来のuserData末尾が変わっても、
    // 同じLamaPon親・gameId・environmentIdなら保存先は安定します。
    class PersistenceProfiles final
    {
    public:
        explicit PersistenceProfiles(
            std::filesystem::path userDataDirectory,
            std::string gameId,
            std::string environmentId = "production");

        [[nodiscard]] const std::filesystem::path&
            UserDataDirectory() const noexcept;
        [[nodiscard]] PersistenceProfilePaths Guest() const;
        [[nodiscard]] PersistenceProfilePaths Account(
            std::string_view playerId) const;

        // PlayerPrefsの読み込みが成功した後にのみSaveDataStoreを
        // 切り替えます。PlayerPrefsに未保存の変更がある場合、または
        // 新しいファイルが壊れている場合は両方とも元のままです。
        void RebindGuest(
            PlayerPrefs& preferences,
            SaveDataStore& saves) const;
        void RebindAccount(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::string_view playerId) const;

        // 現在guestへbindされているPlayerPrefs/SaveDataを、新規
        // アカウントへ明示的にコピーし、成功時は同じオブジェクトを
        // accountへ切り替えます。guest側は削除しません。アカウントの
        // 保存先が既に存在する場合は、空に見えても上書きしません。
        // 別profileへbind済み、またはPlayerPrefsがload failure中なら
        // 何も変更せず拒否します。dirtyなguestはコピー前にSave()し、
        // 保存できなければaccount側を一切作りません。
        //
        // コピーは同じ親に作ったステージングディレクトリを最後に
        // renameして公開します。rename前の通常エラーではステージを
        // best effortで除去し、既存のゲスト/アカウントデータには
        // 触れません。プロセス強制終了まで含む複数ファイルの完全な
        // トランザクションではありませんが、不完全な内容を正式な
        // アカウント保存先として公開しません。
        //
        // この操作は各プロセスのメインスレッドから直列に呼ぶ契約
        // です。stageはCNG乱数を含む呼出し固有名で、失敗時も自分が
        // 作成したstageだけを除去します。他プロセスや以前のstageは
        // 無視し、final renameに成功した1プロセスだけがaccountを
        // 公開します。
        [[nodiscard]] GuestPersistenceImportResult
            ImportGuestToAccount(
                PlayerPrefs& activePreferences,
                SaveDataStore& activeSaves,
                std::string_view playerId) const;

    private:
        [[nodiscard]] std::string AccountStorageKey(
            std::string_view playerId) const;
        void Rebind(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            const PersistenceProfilePaths& profile) const;

        std::filesystem::path m_userDataDirectory;
        std::string m_gameId;
        std::string m_environmentId;
    };
}
