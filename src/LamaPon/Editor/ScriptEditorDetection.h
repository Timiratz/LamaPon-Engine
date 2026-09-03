#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon
{
    // Project Settingsの「スクリプト」カテゴリーで選択肢として提示する、
    // インストール済みスクリプトエディターの情報。
    struct ScriptEditorOption final
    {
        std::string label;
        std::filesystem::path executablePath;
    };

    // このPCにインストールされているVisual Studio Code（Insidersを含む）と、
    // vswhere経由で見つかるVisual Studio（Community/Professional/
    // Enterpriseや2026などのプレリリース版を含む）を検出します。
    // 何も見つからなければ空のvectorを返します
    // （呼び出し側で「システムの既定」と組み合わせて表示してください）。
    [[nodiscard]] std::vector<ScriptEditorOption>
        DetectScriptEditors();
}
