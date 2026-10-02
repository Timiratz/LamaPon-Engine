#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace LamaPon
{
    // プロジェクト設定のスクリプト欄で選ぶインストール済みエディターの情報。
    struct ScriptEditorOption final
    {
        // 設定欄に表示するエディター名
        std::string label;
        // 起動する実行ファイルのパス
        std::filesystem::path executablePath;
    };

    // VS Code・InsidersとMSBuild付きVisual Studioを検出し不在なら空を返す。
    [[nodiscard]] std::vector<ScriptEditorOption>
        DetectScriptEditors();

    // エディターの種類に応じてファイル・行・列を開く引数を作る(editor: 起動する実行ファイル, source: 開くsourceファイルのパス, line: 1始まりの行・0なら位置なし, column: 1始まりの列・VS Codeのみ)。
    [[nodiscard]] std::wstring BuildScriptEditorArguments(
        const std::filesystem::path& editor,
        const std::filesystem::path& source,
        std::uint32_t line = 0,
        std::uint32_t column = 0);
}
