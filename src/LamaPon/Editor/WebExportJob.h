#pragma once

#include <filesystem>
#include <string>

namespace LamaPon
{
    struct WebExportTools final
    {
        std::filesystem::path python;
        std::filesystem::path emsdk;
    };

    [[nodiscard]] WebExportTools LoadWebExportTools();
    void SaveWebExportTools(const WebExportTools& tools);

    // 子プロセスと結果だけを所有します。SceneやUIには触れません。
    // 終了時は所有するJob全体を閉じ、コンパイラの子孫も残しません。
    class WebExportJob final
    {
    public:
        WebExportJob() = default;
        ~WebExportJob();
        WebExportJob(const WebExportJob&) = delete;
        WebExportJob& operator=(const WebExportJob&) = delete;

        void Start(const std::filesystem::path& engineRoot,
            const std::filesystem::path& projectFile,
            const std::filesystem::path& output, const WebExportTools& tools);
        // 完了を一度だけ通知します。待機せず、毎フレーム呼べます。
        bool Poll();
        [[nodiscard]] bool Running() const noexcept { return m_process != nullptr; }
        [[nodiscard]] bool Succeeded() const noexcept { return m_succeeded; }
        [[nodiscard]] const std::string& Message() const noexcept { return m_message; }
        [[nodiscard]] const std::filesystem::path& LogPath() const noexcept { return m_logPath; }
        [[nodiscard]] const std::filesystem::path& HtmlPath() const noexcept { return m_htmlPath; }

    private:
        void Close() noexcept;
        void* m_process{};
        void* m_job{};
        bool m_succeeded{};
        std::string m_message;
        std::filesystem::path m_logPath;
        std::filesystem::path m_resultPath;
        std::filesystem::path m_htmlPath;
    };
}
