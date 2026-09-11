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

    // Editorの永続化panelで編集中の値をprofile bindingと結び付けます。
    // PlayerPrefs/SaveDataの公開object addressはprofile切替でも不変なため、
    // pathの組をrevisionとして扱い、別profileへdraftを持ち越しません。
    class PersistencePanelState final
    {
    public:
        PersistencePanelState(
            std::filesystem::path playerPrefsPath,
            std::filesystem::path saveDataDirectory)
            : m_playerPrefsPath(std::move(playerPrefsPath))
            , m_saveDataDirectory(std::move(saveDataDirectory))
        {
        }

        [[nodiscard]] bool SynchronizeBinding(
            const std::filesystem::path& playerPrefsPath,
            const std::filesystem::path& saveDataDirectory) noexcept
        {
            if (playerPrefsPath == m_playerPrefsPath
                && saveDataDirectory == m_saveDataDirectory)
            {
                return false;
            }

            // 両方のcopyを先に完了し、途中allocation failureでも
            // 少なくとも危険な旧draftは必ず破棄します。
            try
            {
                auto nextPlayerPrefsPath = playerPrefsPath;
                auto nextSaveDataDirectory = saveDataDirectory;
                m_playerPrefsPath.swap(nextPlayerPrefsPath);
                m_saveDataDirectory.swap(nextSaveDataDirectory);
            }
            catch (...)
            {
                // tracking更新を次frameで再試行します。
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

        [[nodiscard]] bool CloseDeleteAllPopupRequested() const noexcept
        {
            return m_closeDeleteAllPopup;
        }

        void AcknowledgeCloseDeleteAllPopup() noexcept
        {
            m_closeDeleteAllPopup = false;
        }

        [[nodiscard]] std::uint64_t BindingRevision() const noexcept
        {
            return m_bindingRevision;
        }

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

        // 確認対象を描画せずに照合します。conflict IDとrecovery revisionは
        // stale操作を拒否するためだけに保持し、labelやstatusへ渡しません。
        [[nodiscard]] bool SynchronizeOnlineConfirmation(
            const bool onlineAccountActive,
            const bool conflictStillExists,
            const std::uint64_t recoveryRevision) noexcept
        {
            bool valid = m_confirmationBindingRevision
                == m_bindingRevision;
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

        [[nodiscard]] PersistenceConfirmationKind
            OnlineConfirmationKind() const noexcept
        {
            return m_confirmationKind;
        }

        [[nodiscard]] const std::string&
            ConfirmationConflictId() const noexcept
        {
            return m_confirmationConflictId;
        }

        [[nodiscard]] std::uint64_t
            ConfirmationRecoveryRevision() const noexcept
        {
            return m_confirmationRecoveryRevision;
        }

        [[nodiscard]] bool
            CloseOnlineConfirmationPopupRequested() const noexcept
        {
            return m_closeOnlineConfirmationPopup;
        }

        void AcknowledgeCloseOnlineConfirmationPopup() noexcept
        {
            m_closeOnlineConfirmationPopup = false;
        }

        void DismissOnlineConfirmation() noexcept
        {
            ClearOnlineConfirmation();
            m_closeOnlineConfirmationPopup = false;
        }

        std::array<char, 128> playerPrefKey{};
        std::array<char, 512> playerPrefValue{};
        std::array<char, 128> saveSlot{};
        std::array<char, 4096> saveJson{ '{', '}', '\0' };
        std::string selectedSaveSlot;
        int playerPrefType{};
        bool playerPrefBoolean{};

    private:
        void ClearOnlineConfirmation() noexcept
        {
            m_confirmationConflictId.clear();
            m_confirmationKind = PersistenceConfirmationKind::None;
            m_confirmationRecoveryRevision = 0;
            m_confirmationBindingRevision = 0;
        }

        void InvalidateOnlineConfirmation() noexcept
        {
            if (m_confirmationKind != PersistenceConfirmationKind::None)
            {
                ClearOnlineConfirmation();
                m_closeOnlineConfirmationPopup = true;
            }
        }

        std::filesystem::path m_playerPrefsPath;
        std::filesystem::path m_saveDataDirectory;
        std::uint64_t m_bindingRevision{ 1 };
        bool m_closeDeleteAllPopup{};
        PersistenceConfirmationKind m_confirmationKind{
            PersistenceConfirmationKind::None
        };
        std::string m_confirmationConflictId;
        std::uint64_t m_confirmationRecoveryRevision{};
        std::uint64_t m_confirmationBindingRevision{};
        bool m_closeOnlineConfirmationPopup{};
    };
}
