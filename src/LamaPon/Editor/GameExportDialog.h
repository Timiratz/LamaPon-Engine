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

    // 描画中だけ借用するパス・設定のsnapshotと、UIスレッドで呼ぶ保存・通知・参照処理。
    struct GameExportDialogContext final
    {
        // 出力ツールのあるエンジンルート
        std::filesystem::path engineRoot;
        // 配布用runtimeのフォルダー
        std::filesystem::path runtimeDirectory;
        // 配布するassetのフォルダー
        std::filesystem::path assetDirectory;
        // 出力対象のプロジェクト文書
        std::filesystem::path projectFile;
        // 出力時に使う設定のコピー
        ProjectSettings settings;
        // 出力前にSceneを保存する処理
        std::function<void()> prepareScene;
        // 本文と失敗フラグを通知する処理
        std::function<void(std::string, bool)> setStatus;
        // 初期フォルダーから保存先を選ぶ処理
        std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)> browse;
    };

    // EditorLayerを保持せず入力・診断結果・Web出力jobを所有する。
    class GameExportDialog final
    {
    public:
        // Web実行中は入力を保持し次の描画で出力ダイアログを開く(projectRoot: 出力対象projectのルート)。
        void Open(const std::filesystem::path& projectRoot);
        // 閉じた後もWeb結果を回収し出力設定と結果を表示する(context: 当該描画中の設定と処理の借用)。
        void Draw(const GameExportDialogContext& context);
        // 実行中でなければ形式を切り替え既定出力先だけを追従させる(target: 選択する出力形式)。
        void SelectTarget(GameExportTarget target);
        // 現在の出力形式を返す。
        [[nodiscard]] GameExportTarget Target() const noexcept { return m_target; }
        // 入力したUTF8パスを出力フォルダーとして返す。
        [[nodiscard]] std::filesystem::path OutputDirectory() const;

    private:
        // Sceneを準備してWindows配布物またはWeb出力を開始する(context: 当該操作中の設定と処理の借用)。
        void Start(const GameExportDialogContext& context);
        // 指定パスを出力先入力のUTF8バッファへ移す(path: 設定する出力フォルダー)。
        void SetPath(const std::filesystem::path& path);
        // 選択中のWindows・Web出力形式
        GameExportTarget m_target{GameExportTarget::Windows};
        // 既定出力先を求めるprojectルート
        std::filesystem::path m_projectRoot;
        // 出力フォルダーのUTF8入力
        std::array<char, 4096> m_path{};
        // SDKフォルダーのUTF8入力
        std::array<char, 4096> m_emsdk{};
        // Python実行パスのUTF8入力
        std::array<char, 4096> m_python{};
        // SignTool実行パスのUTF8入力
        std::array<char, 4096> m_signTool{};
        // 署名証明書の40桁SHA1拇印
        std::array<char, 41> m_signingCertificate{};
        // 署名時のtimestamp URL入力
        std::array<char, 512> m_timestampUrl{};
        // 次の描画でダイアログを開くか
        bool m_requested{};
        // 配布用ZIPも作成するか
        bool m_createZip{};
        // WindowsのEXEとDLLへ署名するか
        bool m_signWindowsBinaries{};
        // 直近の出力・参照操作の失敗理由
        std::string m_error;
        // 直近の出力成功の説明
        std::string m_success;
        // 出力完了したフォルダーのパス
        std::filesystem::path m_completedOutput;
        // 非同期Web出力processの所有先
        WebExportJob m_web;
    };
}
