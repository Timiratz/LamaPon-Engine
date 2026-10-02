#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace LamaPon::Detail
{
    enum class PersistenceConfirmationKind : std::uint8_t
    {
        None,
        UseRemote,
        RetryLocal,
        Restore,
        Discard
    };

    // 公開オブジェクトのアドレスは切替で変わらないため、パスの組を追跡し別profileへ編集中の値を持ち越さない。
    class PersistencePanelState final
    {
    public:
        // 編集対象の設定と保存領域を追跡する(playerPrefsPath: 設定ファイルのパス, saveDataDirectory: 保存領域のフォルダー)。
        PersistencePanelState(
            std::filesystem::path playerPrefsPath,
            std::filesystem::path saveDataDirectory)
            : m_playerPrefsPath(std::move(playerPrefsPath))
            , m_saveDataDirectory(std::move(saveDataDirectory))
        {
        }

        // 保存先の変更で編集値と確認対象を失効させ変更したかを返す(playerPrefsPath: 現在の設定ファイルのパス, saveDataDirectory: 現在の保存領域フォルダー)。
        [[nodiscard]] bool SynchronizeBinding(
            const std::filesystem::path& playerPrefsPath,
            const std::filesystem::path& saveDataDirectory) noexcept
        {
            if (playerPrefsPath == m_playerPrefsPath
                && saveDataDirectory == m_saveDataDirectory)
            {
                return false;
            }

            // 両パスの複製を先に完了し、割当失敗時も古い編集値を必ず破棄する。
            try
            {
                // 先に複製する次の設定パス
                auto nextPlayerPrefsPath = playerPrefsPath;
                // 先に複製する次の保存フォルダー
                auto nextSaveDataDirectory = saveDataDirectory;
                m_playerPrefsPath.swap(nextPlayerPrefsPath);
                m_saveDataDirectory.swap(nextSaveDataDirectory);
            }
            catch (...)
            {
                // パス追跡の更新は次のフレームで再試行する。
            }
            ResetDrafts();
            m_closeDeleteAllPopup = true;
            InvalidateOnlineConfirmation();
            ++m_bindingRevision;
            if (m_bindingRevision == 0)
            {
                ++m_bindingRevision;
            }
            return true;
        }

        // 編集中の設定と保存JSONを初期値へ戻す。
        void ResetDrafts() noexcept
        {
            playerPrefKey = {};
            playerPrefValue = {};
            saveSlot = {};
            saveJson = { '{', '}', '\0' };
            selectedSaveSlot.clear();
            playerPrefType = 0;
            playerPrefBoolean = false;
        }

        // 全削除の確認画面を閉じる要求があるかを返す。
        [[nodiscard]] bool CloseDeleteAllPopupRequested() const noexcept
        {
            return m_closeDeleteAllPopup;
        }

        // 全削除の確認画面を閉じる要求を消費する。
        void AcknowledgeCloseDeleteAllPopup() noexcept
        {
            m_closeDeleteAllPopup = false;
        }

        // 編集対象の保存先の非0の世代を返す。
        [[nodiscard]] std::uint64_t BindingRevision() const noexcept
        {
            return m_bindingRevision;
        }

        // 有効な競合解決要求を現在の保存先へ結び付ける(kind: リモート採用・ローカル再試行, conflictId: process内の競合IDの所有先)。
        void BeginConflictConfirmation(
            const PersistenceConfirmationKind kind,
            std::string conflictId)
        {
            if ((kind != PersistenceConfirmationKind::UseRemote
                    && kind != PersistenceConfirmationKind::RetryLocal)
                || conflictId.empty())
            {
                InvalidateOnlineConfirmation();
                return;
            }
            m_confirmationConflictId = std::move(conflictId);
            m_confirmationKind = kind;
            m_confirmationRecoveryRevision = 0;
            m_confirmationBindingRevision = m_bindingRevision;
            m_closeOnlineConfirmationPopup = false;
        }

        // 有効な復旧要求を現在の保存先へ結び付ける(kind: RestoreまたはDiscard, recoveryRevision: 確認する復旧の非0の世代)。
        void BeginRecoveryConfirmation(
            const PersistenceConfirmationKind kind,
            const std::uint64_t recoveryRevision) noexcept
        {
            if ((kind != PersistenceConfirmationKind::Restore
                    && kind != PersistenceConfirmationKind::Discard)
                || recoveryRevision == 0)
            {
                InvalidateOnlineConfirmation();
                return;
            }
            m_confirmationConflictId.clear();
            m_confirmationKind = kind;
            m_confirmationRecoveryRevision = recoveryRevision;
            m_confirmationBindingRevision = m_bindingRevision;
            m_closeOnlineConfirmationPopup = false;
        }

        // 競合IDと復旧世代は古い操作を拒否するためだけに保持し画面のlabelやstatusへ表示しない。
        // 確認対象の存続を照合し失効したときだけtrueを返す(onlineAccountActive: account保存先を使用中か, conflictStillExists: 確認中の競合が存続するか, recoveryRevision: 現在の復旧操作の世代)。
        [[nodiscard]] bool SynchronizeOnlineConfirmation(
            const bool onlineAccountActive,
            const bool conflictStillExists,
            const std::uint64_t recoveryRevision) noexcept
        {
            // 現在の対象で確認を続行できるか
            bool valid = m_confirmationBindingRevision
                == m_bindingRevision;
            // 操作の種類ごとにaccount・競合の存続または復旧世代の一致を確認する。
            switch (m_confirmationKind)
            {
            case PersistenceConfirmationKind::None:
                return false;
            case PersistenceConfirmationKind::UseRemote:
            case PersistenceConfirmationKind::RetryLocal:
                valid = valid
                    && onlineAccountActive
                    && !m_confirmationConflictId.empty()
                    && conflictStillExists;
                break;
            case PersistenceConfirmationKind::Restore:
            case PersistenceConfirmationKind::Discard:
                valid = valid
                    && m_confirmationRecoveryRevision != 0
                    && m_confirmationRecoveryRevision
                        == recoveryRevision;
                break;
            }
            if (valid)
            {
                return false;
            }
            InvalidateOnlineConfirmation();
            return true;
        }

        // 確認画面の操作種別を返す。
        [[nodiscard]] PersistenceConfirmationKind
            OnlineConfirmationKind() const noexcept
        {
            return m_confirmationKind;
        }

        // 操作照合用の競合IDを借用する。
        [[nodiscard]] const std::string&
            ConfirmationConflictId() const noexcept
        {
            return m_confirmationConflictId;
        }

        // 確認中の復旧操作の世代を返す。
        [[nodiscard]] std::uint64_t
            ConfirmationRecoveryRevision() const noexcept
        {
            return m_confirmationRecoveryRevision;
        }

        // 同期・復旧の確認画面を閉じる要求があるかを返す。
        [[nodiscard]] bool
            CloseOnlineConfirmationPopupRequested() const noexcept
        {
            return m_closeOnlineConfirmationPopup;
        }

        // 同期・復旧の確認画面を閉じる要求を消費する。
        void AcknowledgeCloseOnlineConfirmationPopup() noexcept
        {
            m_closeOnlineConfirmationPopup = false;
        }

        // 確認対象と画面を閉じる要求を消去する。
        void DismissOnlineConfirmation() noexcept
        {
            ClearOnlineConfirmation();
            m_closeOnlineConfirmationPopup = false;
        }

        // 入力中の設定キーのUTF8バッファ
        std::array<char, 128> playerPrefKey{};
        // 入力中の設定値のUTF8バッファ
        std::array<char, 512> playerPrefValue{};
        // 入力中の保存スロット名
        std::array<char, 128> saveSlot{};
        // 入力中の保存JSONのバッファ
        std::array<char, 4096> saveJson{ '{', '}', '\0' };
        // 一覧で選択中の保存スロット名
        std::string selectedSaveSlot;
        // 入力中の設定値の型番号
        int playerPrefType{};
        // 入力中の真偽値設定
        bool playerPrefBoolean{};

    private:
        // 確認対象のID・種類・世代を初期化する。
        void ClearOnlineConfirmation() noexcept
        {
            m_confirmationConflictId.clear();
            m_confirmationKind = PersistenceConfirmationKind::None;
            m_confirmationRecoveryRevision = 0;
            m_confirmationBindingRevision = 0;
        }

        // 確認対象があれば失効させて画面を閉じるよう要求する。
        void InvalidateOnlineConfirmation() noexcept
        {
            if (m_confirmationKind != PersistenceConfirmationKind::None)
            {
                ClearOnlineConfirmation();
                m_closeOnlineConfirmationPopup = true;
            }
        }

        // 現在追跡している設定ファイル
        std::filesystem::path m_playerPrefsPath;
        // 現在追跡している保存フォルダー
        std::filesystem::path m_saveDataDirectory;
        // 編集対象の切替を識別する非0世代
        std::uint64_t m_bindingRevision{ 1 };
        // 全削除の確認画面を閉じる要求
        bool m_closeDeleteAllPopup{};
        // 確認画面の操作種別
        PersistenceConfirmationKind m_confirmationKind{
            PersistenceConfirmationKind::None
        };
        // 確認中のprocess内の競合ID
        std::string m_confirmationConflictId;
        // 確認中の復旧操作の世代
        std::uint64_t m_confirmationRecoveryRevision{};
        // 確認開始時の編集対象の世代
        std::uint64_t m_confirmationBindingRevision{};
        // 同期・復旧の確認画面を閉じる要求
        bool m_closeOnlineConfirmationPopup{};
    };
}
