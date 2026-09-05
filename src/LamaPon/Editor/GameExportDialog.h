#pragma once

#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Editor/WebExportJob.h"
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace LamaPon
{
    enum class GameExportTarget { Windows, Web };

    // ダイアログへ渡すパスと設定のスナップショット、および
    // UIスレッドで実行するコールバックです。
    struct GameExportDialogContext final
    {
        std::filesystem::path engineRoot;
        std::filesystem::path runtimeDirectory;
        std::filesystem::path assetDirectory;
        std::filesystem::path projectFile;
        ProjectSettings settings;
        std::function<void()> prepareScene;
        std::function<void(std::string, bool)> setStatus;
        std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)> browse;
    };

    // 入力値、診断結果、実行中のWeb出力ジョブを内部で管理します。
    // EditorLayerの状態は保持しません。
    class GameExportDialog final
    {
    public:
        void Open(const std::filesystem::path& projectRoot);
        void Draw(const GameExportDialogContext& context);
        void SelectTarget(GameExportTarget target);
        [[nodiscard]] GameExportTarget Target() const noexcept { return m_target; }
        [[nodiscard]] std::filesystem::path OutputDirectory() const;

    private:
        void Start(const GameExportDialogContext& context);
        void SetPath(const std::filesystem::path& path);
        GameExportTarget m_target{GameExportTarget::Windows};
        std::filesystem::path m_projectRoot;
        std::array<char, 4096> m_path{};
        std::array<char, 4096> m_emsdk{};
        std::array<char, 4096> m_python{};
        bool m_requested{};
        bool m_createZip{};
        std::string m_error;
        std::string m_success;
        std::filesystem::path m_completedOutput;
        WebExportJob m_web;
    };
}
