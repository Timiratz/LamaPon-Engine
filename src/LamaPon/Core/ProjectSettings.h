#pragma once

#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Physics/PhysicsSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    enum class ViewportNavigationPreset
    {
        Fly,
        Orbit
    };

    struct ViewportSettings final
    {
        ViewportNavigationPreset navigationPreset{
            ViewportNavigationPreset::Fly
        };
        float orbitSensitivity{ 1.0f };
        float panSensitivity{ 1.0f };
        float zoomSensitivity{ 1.0f };
        bool invertY{};
    };

    [[nodiscard]] std::string_view ViewportNavigationPresetName(
        ViewportNavigationPreset preset) noexcept;
    [[nodiscard]] ViewportNavigationPreset
        ViewportNavigationPresetFromName(
            std::string_view name) noexcept;

    enum class ProjectSettingsFileType
    {
        Project,
        GamePackage
    };

    struct ProjectSettings final
    {
        std::string gameName{ "LamaPon Game" };
        std::uint32_t windowWidth{ 1280 };
        std::uint32_t windowHeight{ 720 };
        std::filesystem::path startupScene{
            L"scenes/sandbox.scene.json"
        };
        // Export時に実行ファイルへ埋め込むアイコン画像
        // （assetsからの相対パス、.png/.jpg/.ico等）。
        // 空ならLamaPon標準アイコンのままにします。
        std::filesystem::path gameIcon;
        // スクリプト（.cpp）をアセットブラウザーから開く際に使う
        // 外部エディターの実行ファイル（このPC上の絶対パス）。
        // 空ならWindowsのファイル関連付け（システムの既定）を使います。
        std::filesystem::path scriptEditorPath;
        // trueなら、assets内の.cpp/.hを保存した数秒後にGame Moduleを
        // 自動ビルドします。保存後に変更を反映する設定です。ビルド済みDLLの
        // 差し替えはGameModuleHostが
        // 自動で拾います。手動ビルドだけにしたい場合はfalseにします。
        bool autoBuildGameModuleOnSave{ true };
        // Inspectorで数値を表示する小数点以下の桁数。
        // 既定の1は「0.0」表示で、位置や角度をざっと確認するのに
        // 読みやすい桁数です。細かく詰めたいときだけ増やします
        // （0〜6）。編集中は桁数に関係なく入力した値がそのまま入り、
        // 表示だけが丸められます。エディターの表示設定なので、
        // 配布用のLamaPonGame.jsonには含めません。
        std::uint32_t inspectorDecimals{ 1 };
        GraphicsSettings graphics;
        ViewportSettings viewport;
        std::vector<InputActionDefinition> inputActions{
            DefaultInputActions()
        };
        // プロジェクトで使用するGameObjectタグの一覧。
        // Inspectorのドロップダウン候補になり、Scene読み込み時に
        // 未登録タグへ警告を出します。空なら検査しません。
        std::vector<std::string> tags;
        // 書き出したゲームからHLSLソースを外し、配布物にはバイトコードだけを
        // 入れます。末尾に
        // 足しています（途中へ入れると構造体のレイアウトがずれ、
        // 作り直していないGame Module DLLが壊れます）。
        //
        // 入れると、事前コンパイルは**全バリアント**を焼きます。
        // shader_featureのストリップと同時にやると、取りこぼした
        // 組み合わせを実行時に作り直せず（ソースが無いので）
        // 標準Litへ落ちてしまうためです。
        //
        // 既定はオンです。配布物へHLSLを平文で置く理由が無く、
        // 外しても全バリアントを焼くので実行時の挙動は変わりません
        // （既に保存済みのプロジェクトは、保存された値のままです）。
        bool stripShaderSourceOnExport{ true };
        // 物理・当たり判定の調整値。**末尾へ足しています**
        // （途中へ入れると構造体のレイアウトがずれ、作り直して
        // いないGame Module DLLが壊れます）。
        PhysicsSettings physics;
        // Show the LamaPon logo while the initial scene is loading.
        bool splashScreenEnabled{ true };
    };

    void ValidateProjectSettings(
        const ProjectSettings& settings);

    [[nodiscard]] ProjectSettings LoadProjectSettings(
        const std::filesystem::path& path);

    void SaveProjectSettings(
        const std::filesystem::path& path,
        const ProjectSettings& settings,
        ProjectSettingsFileType fileType);
}
