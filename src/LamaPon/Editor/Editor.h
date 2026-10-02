#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace LamaPon
{
    class Application;

    // 指定UIを描画してPNGと実行結果を出力するエディターの撮影・リモート操作設定。
    struct EditorScreenshotOptions final
    {
        // 出力PNGのパス・空なら撮影しない
        std::filesystem::path imagePath;
        // 実行結果JSONの出力パス
        std::filesystem::path reportPath;
        // UI指定はproject-settings:設定名・inspector:オブジェクト名・panel:パネルIDのいずれかで、空なら既定レイアウトを使う。
        // 撮影前に開くUIの指定
        std::string show;
        // レイアウト確定を待つ撮影フレーム
        std::uint32_t captureFrame{ 12 };

        // command.jsonの入力・撮影・終了要求を毎フレーム受け付けstate.jsonへ結果を返し、終了はquit要求まで待つ。
        // リモート操作文書を置くフォルダー
        std::filesystem::path remoteDirectory;
    };

    // 編集レイヤーを接続し撮影または操作設定を適用する(application: 接続するApplication, scenePath: 編集対象シーンのパス, engineRoot: エンジンのルートパス, buildConfiguration: モジュールのビルド構成, safeMode: モジュール未読込の警告を出すか, screenshot: 撮影・操作設定の借用・省略可)。
    void EnableEditor(
        Application& application,
        std::filesystem::path scenePath,
        std::filesystem::path engineRoot,
        std::string buildConfiguration,
        bool safeMode = false,
        const EditorScreenshotOptions* screenshot
            = nullptr);

    // 終了後に再起動要求を確認しtrueならプロジェクトロック解放後に通常モードで起動する。
    [[nodiscard]] bool WasNormalModeRestartRequested() noexcept;
}
