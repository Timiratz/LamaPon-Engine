#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace LamaPon
{
    class Application;

    // エディターのスクリーンショットモード。
    //
    // 指定したUIを開いて数フレーム描画し、バックバッファをPNGへ
    // 保存します。GUIを使わない自動テストから表示を確認できます。
    struct EditorScreenshotOptions final
    {
        // 出力PNG。空ならスクリーンショットモードにしません。
        std::filesystem::path imagePath;
        // 実行結果のJSON（ok/エラー/画像パス）。エディターは
        // コンソールを持たないので、stdoutの代わりにここへ書きます。
        std::filesystem::path reportPath;
        // 撮る前に開いておくUI。空なら既定レイアウトのまま。
        //   "project-settings:<ゲーム|グラフィック|ビューポート設定|物理|タグ|入力|スクリプト>"
        //   "inspector:<GameObject名>"
        std::string show;
        // 撮影するフレーム番号。UIのレイアウトとフォントが落ち着く
        // まで数フレームかかるので、少し待ってから撮ります。
        std::uint32_t captureFrame{ 12 };

        // リモート操作モード（--remote <dir>）。指定すると、この
        // フォルダーの command.json を毎フレーム監視し、マウス・
        // キー入力の注入／スクリーンショット／終了を受け付けます。
        // 実行結果は state.json へ返します。
        //
        // 自動化クライアントがスクリーンショットを確認しながら
        // エディターを操作するための仕組みです。imagePathと違い、
        // こちらは自動終了しません
        // （quitコマンドで閉じます）。
        std::filesystem::path remoteDirectory;
    };

    // safeModeがtrueのときは、C++ Game Moduleを読み込まずに起動した
    // 状態として扱い、エディターへ警告を表示します。
    // screenshotを渡すと、撮影後に自動で終了します。
    void EnableEditor(
        Application& application,
        std::filesystem::path scenePath,
        std::filesystem::path engineRoot,
        std::string buildConfiguration,
        bool safeMode = false,
        const EditorScreenshotOptions* screenshot
            = nullptr);

    // セーフモードから通常モードへ戻る要求があったかを返します。
    // エディター終了後に呼び、trueなら通常モードで起動し直します
    // （プロジェクトのロックが解放されてから再起動するため）。
    [[nodiscard]] bool WasNormalModeRestartRequested() noexcept;
}
