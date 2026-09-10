#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace LamaPon::Detail
{
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

        std::array<char, 128> playerPrefKey{};
        std::array<char, 512> playerPrefValue{};
        std::array<char, 128> saveSlot{};
        std::array<char, 4096> saveJson{ '{', '}', '\0' };
        std::string selectedSaveSlot;
        int playerPrefType{};
        bool playerPrefBoolean{};

    private:
        std::filesystem::path m_playerPrefsPath;
        std::filesystem::path m_saveDataDirectory;
        std::uint64_t m_bindingRevision{ 1 };
        bool m_closeDeleteAllPopup{};
    };
}
