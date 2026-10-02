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
        // プロファイルの保存ルート
        std::filesystem::path rootDirectory;
        // 設定ファイルの保存先
        std::filesystem::path playerPrefsFile;
        // スロットファイルの保存先
        std::filesystem::path saveDataDirectory;
        // アカウントの保存先識別キー
        // ゲストは空とし、アカウントはゲーム・環境・プレイヤーIDを長さ付きでハッシュした小文字16進64桁です。
        std::string accountStorageKey;
        // ゲスト用の保存先か
        bool isGuest{ true };
    };

    enum class GuestPersistenceImportStatus
    {
        // ゲストデータを移行できた
        Imported,
        // コピーするゲストデータがない
        NothingToImport,
        // 移行先の保存領域が既に存在する
        AccountAlreadyHasData,
        // ゲストデータの移行に失敗した
        Failed
    };

    struct GuestPersistenceImportResult final
    {
        // ゲストデータ移行の結果
        GuestPersistenceImportStatus status{
            GuestPersistenceImportStatus::Failed
        };
        // 固定文言で返す失敗理由
        std::string error;

        // ゲストデータをアカウントへ移行できたかを返します。
        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status
                == GuestPersistenceImportStatus::Imported;
        }
    };

    // ゲストはuserData内、アカウントはその親のOnlineProfiles内のハッシュ名へ保存先を分離します。
    // 切り替えとインポートは各プロセスのメインスレッドから直列に呼びます。
    class PersistenceProfiles final
    {
    public:
        // 保存先を識別する情報を保持します(userDataDirectory: ゲスト保存領域, gameId: ゲームID, environmentId: 接続環境ID)。
        // 保存領域は空を拒否し、ゲームIDはUTF-8で1～128バイト、環境IDは1～64バイトに限ります。
        explicit PersistenceProfiles(
            std::filesystem::path userDataDirectory,
            std::string gameId,
            std::string environmentId = "production");

        // ゲストの保存領域への参照を返します。
        [[nodiscard]] const std::filesystem::path&
            UserDataDirectory() const noexcept;
        // ゲスト用の設定とスロットの保存先を返します。
        [[nodiscard]] PersistenceProfilePaths Guest() const;
        // アカウント固有の保存先を返します(playerId: UTF-8プレイヤーID)。
        // プレイヤーIDは1〜1024バイトを指定します。
        [[nodiscard]] PersistenceProfilePaths Account(
            std::string_view playerId) const;

        // 設定とスロットの保存先をゲストへ切り替えます(preferences: 設定管理, saves: スロット管理)。
        // 未保存変更・保存先の固定・読み込み失敗では、両方の状態を保って例外を送出します。
        void RebindGuest(
            PlayerPrefs& preferences,
            SaveDataStore& saves) const;
        // 設定とスロットの保存先をアカウントへ切り替えます(preferences: 設定管理, saves: スロット管理, playerId: 切り替え先のプレイヤーID)。
        void RebindAccount(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            std::string_view playerId) const;

        // ゲストデータを新規アカウントへコピーして切り替えます(activePreferences: 現在のゲスト設定, activeSaves: 現在のゲスト保存先, playerId: 移行先のプレイヤーID)。
        // ゲスト側は残し、アカウントの保存先が既にあれば空でも上書きを拒否します。
        // ゲスト以外・読込失敗・固定中の状態を拒否し、未保存の設定はコピー前にSaveします。
        // コピーした文書を再検証してから同じ親でディレクトリ名を変更し、公開後の状態交換は例外を送出しません。
        // ステージ名は呼出し固有の乱数を使い、通常の失敗時は自身が作ったステージだけの除去を試みます。
        // 強制終了を含む複数ファイル全体の完全なトランザクションは保証しません。
        [[nodiscard]] GuestPersistenceImportResult
            ImportGuestToAccount(
                PlayerPrefs& activePreferences,
                SaveDataStore& activeSaves,
                std::string_view playerId) const;

    private:
        // ゲーム・環境・プレイヤーを結合した保存先キーを返します(playerId: プレイヤーID)。
        // 既存保存先との互換性のため、ハッシュのドメインと長さ付き符号化を維持します。
        [[nodiscard]] std::string AccountStorageKey(
            std::string_view playerId) const;
        // 設定の読込成功後に両保存先を切り替えます(preferences: 設定管理, saves: スロット管理, profile: 切り替え先のパス群)。
        void Rebind(
            PlayerPrefs& preferences,
            SaveDataStore& saves,
            const PersistenceProfilePaths& profile) const;

        // ゲストの保存領域
        std::filesystem::path m_userDataDirectory;
        // 保存先の識別に使うゲームID
        std::string m_gameId;
        // 保存先の識別に使う接続環境ID
        std::string m_environmentId;
    };
}
