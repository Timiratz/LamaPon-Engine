#pragma once

#include <filesystem>
#include <string>

namespace LamaPon
{
    struct WebExportTools final
    {
        // 指定するPython実行ファイル
        std::filesystem::path python;
        // Emscripten SDKの配置先
        std::filesystem::path emsdk;
    };

    struct AndroidExportTools final
    {
        std::filesystem::path sdk, javaHome, gradleHome, sdlSource;
        bool allowDependencyDownloads{};
    };

    struct LinuxExportTools final
    {
        std::string distribution;
        std::filesystem::path sdlSource;
    };

    // EMSDK環境変数を初期値に保存済みツール設定を読み込む。
    [[nodiscard]] WebExportTools LoadWebExportTools();
    // ツール設定を作業ファイルへ書いて保存先を置換する(tools: 保存するツールの配置情報)。
    void SaveWebExportTools(const WebExportTools& tools);

    // SceneやUIを参照せず子processと結果を所有し、終了時はjobを閉じてコンパイラの子孫も終了させる。
    class WebExportJob final
    {
    public:
        // 未開始のWeb出力処理を作る。
        WebExportJob() = default;
        // 所有jobを閉じて子孫ごと終了させhandleを解放する。
        ~WebExportJob();
        // 実行中processの共有を禁止する。
        WebExportJob(const WebExportJob&) = delete;
        // 実行中processの共有を禁止する。
        WebExportJob& operator=(const WebExportJob&) = delete;

        // Pythonをjobで管理して非表示のWeb出力を開始する(engineRoot: 出力ツールのあるエンジンルート, projectFile: 出力対象のプロジェクト文書, output: HTMLの出力先フォルダー, tools: PythonとSDKの指定情報)。
        void Start(const std::filesystem::path& engineRoot,
            const std::filesystem::path& projectFile,
            const std::filesystem::path& output, const WebExportTools& tools);
        // Linux／Android向けの診断とビルド設定生成を同じprocess管理で開始する。
        void StartNativeBuildProject(const std::filesystem::path& engineRoot,
            const std::filesystem::path& projectFile,
            const std::filesystem::path& output, const WebExportTools& tools,
            const std::string& platform);
        void StartAndroidApk(const std::filesystem::path& engineRoot,
            const std::filesystem::path& projectFile, const std::filesystem::path& output,
            const WebExportTools& tools, const AndroidExportTools& android);
        void StartLinuxBuild(const std::filesystem::path& engineRoot,
            const std::filesystem::path& projectFile, const std::filesystem::path& output,
            const WebExportTools& tools, const LinuxExportTools& linux);
        // 待機せず終了を確認し結果の検証後に完了を一度だけ通知する。
        bool Poll();
        // 所有するPython processがあるかを返す。
        [[nodiscard]] bool Running() const noexcept { return m_process != nullptr; }
        // 直近の出力が終了コード・結果JSON・HTML確認に成功したかを返す。
        [[nodiscard]] bool Succeeded() const noexcept { return m_succeeded; }
        // 進行状態または失敗理由を次の更新まで借用する。
        [[nodiscard]] const std::string& Message() const noexcept { return m_message; }
        // ビルドログのパスを次の開始まで借用する。
        [[nodiscard]] const std::filesystem::path& LogPath() const noexcept { return m_logPath; }
        // 検証済みHTMLのパスを次の開始まで借用する。
        [[nodiscard]] const std::filesystem::path& HtmlPath() const noexcept { return m_htmlPath; }
        [[nodiscard]] const std::filesystem::path& BuildProjectDirectory() const noexcept { return m_buildProjectDirectory; }
        [[nodiscard]] const std::filesystem::path& NativeArtifactPath() const noexcept { return m_nativeArtifactPath; }

    private:
        void StartProcess(const std::filesystem::path& engineRoot,
            const std::filesystem::path& projectFile,
            const std::filesystem::path& output, const WebExportTools& tools,
            const std::string& platform, const AndroidExportTools* android = nullptr,
            const LinuxExportTools* linux = nullptr);
        // jobを閉じて子孫ごと終了させprocess handleを解放する。
        void Close() noexcept;
        // 終了確認するPython process handle
        void* m_process{};
        // 子孫ごと終了させるjob handle
        void* m_job{};
        // 直近の出力処理が成功したか
        bool m_succeeded{};
        // 進行状態または失敗理由
        std::string m_message;
        // ビルド出力を記録するログパス
        std::filesystem::path m_logPath;
        // 終了結果JSONのパス
        std::filesystem::path m_resultPath;
        // 成功時に確認済みのHTMLパス
        std::filesystem::path m_htmlPath;
        std::filesystem::path m_buildProjectDirectory;
        std::string m_nativePlatform;
        std::filesystem::path m_nativeArtifactPath, m_expectedApk, m_expectedLinuxOutput;
        bool m_androidApk{}, m_linuxBuild{};
    };
}
