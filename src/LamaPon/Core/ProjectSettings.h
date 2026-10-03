#pragma once

#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Scene/SceneTransition.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    enum class ViewportNavigationPreset
    {
        // 視点を自由に移動
        Fly,
        // 注視点を中心に回転
        Orbit
    };

    struct ViewportSettings final
    {
        // 視点操作の方式
        ViewportNavigationPreset navigationPreset{
            ViewportNavigationPreset::Fly
        };
        // 回転操作の感度
        float orbitSensitivity{ 1.0f };
        // 平行移動操作の感度
        float panSensitivity{ 1.0f };
        // 拡縮操作の感度
        float zoomSensitivity{ 1.0f };
        // 上下操作の反転有無
        bool invertY{};
    };

    // 視点操作方式の保存名を返します(preset: 視点操作方式)。
    [[nodiscard]] std::string_view ViewportNavigationPresetName(
        ViewportNavigationPreset preset) noexcept;
    // 保存名から視点操作方式を求めます(name: 保存名)。
    // Orbitと旧名Unity以外はFlyになります。
    [[nodiscard]] ViewportNavigationPreset
        ViewportNavigationPresetFromName(
            std::string_view name) noexcept;

    enum class ProjectSettingsFileType
    {
        // 編集用の設定形式
        Project,
        // 配布用の設定形式
        GamePackage
    };

    // Rich Presenceはアカウント連携と独立して有効化でき、秘密鍵やトークンは保存しません。
    struct DiscordPresenceProjectSettings final
    {
        // Rich Presenceの有効化
        bool enabled{};
        // Discordの公開アプリID
        std::string applicationId;
        // 既定の大画像キー
        std::string defaultLargeImageKey;
        // 既定の大画像の説明
        std::string defaultLargeImageText;
    };

    // 認証の秘密鍵とトークンはバックエンドだけが保持し、プロジェクトへ保存しません。
    struct OnlineProjectSettings final
    {
        // アカウント連携の有効化
        bool enabled{};
        // バックエンドの基底URL
        std::string serviceBaseUrl;
        // ゲームの名前空間ID
        std::string gameId;
        // 接続環境の識別子
        std::string environmentId{ "production" };
        // 開発用HTTP接続の許可
        bool allowInsecureLoopback{};
        // 認証ブラウザーの起動有無
        bool openAuthorizationBrowser{ true };
        // Rich Presence設定
        DiscordPresenceProjectSettings discordPresence;
    };

    // Game ModuleのABIを保つため、新しいフィールドは末尾へ追加します。
    // アイコンはassets相対、外部エディターはPC上の絶対パスで、空なら各既定値を使います。
    // inspectorDecimalsは表示だけを丸め、入力値を変更せず配布設定へ保存しません。
    // シェーダーソースを除く配布では、実行時の再コンパイルに頼らず全バリアントを事前生成します。
    // タグ一覧が空なら未登録タグを検査せず、ネットワーク接続は明示操作で開始します。
    struct ProjectSettings final
    {
        // ゲームの表示名
        std::string gameName{ "LamaPon Game" };
        // 初期ウィンドウ幅の画素数
        std::uint32_t windowWidth{ 1280 };
        // 初期ウィンドウ高さの画素数
        std::uint32_t windowHeight{ 720 };
        // 起動シーンのアセットパス
        std::filesystem::path startupScene{
            L"scenes/sandbox.scene.json"
        };
        // 埋め込むアイコンのパス
        std::filesystem::path gameIcon;
        // 外部スクリプト編集器のパス
        std::filesystem::path scriptEditorPath;
        // 保存後の自動ビルド有無
        bool autoBuildGameModuleOnSave{ true };
        // 数値表示の小数桁数
        std::uint32_t inspectorDecimals{ 1 };
        // 描画品質と描画方式
        GraphicsSettings graphics;
        // 編集時の視点操作設定
        ViewportSettings viewport;
        // 入力アクションの割り当て
        std::vector<InputActionDefinition> inputActions{
            DefaultInputActions()
        };
        // 登録済みオブジェクトタグ
        std::vector<std::string> tags;
        // 配布時のHLSL除外有無
        bool stripShaderSourceOnExport{ true };
        // 物理計算と衝突の設定
        PhysicsSettings physics;
        // 起動ロゴの表示有無
        bool splashScreenEnabled{ true };
        // オンライン接続の設定
        OnlineProjectSettings online;
        // 通信セッションの設定
        NetworkConfiguration network;
        // 読み込み画面の設定
        SceneLoadingScreenSettings loadingScreen;
    };

    // 値とパスの制約を検証します(settings: 検証する設定)。
    // 制約違反はinvalid_argumentです。
    void ValidateProjectSettings(
        const ProjectSettings& settings);

    // 保存形式を含めて設定を検証します(settings: 検証する設定, fileType: 保存形式)。
    // 配布用のオンライン有効設定にHTTP loopback許可を含めるとinvalid_argumentです。
    void ValidateProjectSettings(
        const ProjectSettings& settings,
        ProjectSettingsFileType fileType);

    // 設定を読み込み移行・検証します(path: 設定ファイル)。
    // 省略項目は既定値を使い、読み込み・形式・値の不備は例外です。
    [[nodiscard]] ProjectSettings LoadProjectSettings(
        const std::filesystem::path& path);

    // 検証した設定を保存します(path: 保存ファイル, settings: 保存する設定, fileType: 保存形式)。
    // 未知の既存キーを保ちますが旧sceneTransitionは除き、既存JSONが壊れていても保存を続けます。
    // 保存は直接上書きで、失敗時は例外です。
    void SaveProjectSettings(
        const std::filesystem::path& path,
        const ProjectSettings& settings,
        ProjectSettingsFileType fileType);
}
