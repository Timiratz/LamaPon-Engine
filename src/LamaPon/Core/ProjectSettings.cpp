#include "LamaPon/Core/ProjectSettings.h"

#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Online/DiscordPresence.h"
#include "LamaPon/Online/OnlineHttpValidation.h"
#include "LamaPon/Online/NetworkSettingsJson.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    // 絶対パスと親への遡行を拒否します(path: アセット相対パス)。
    bool IsSafeRelativePath(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
        {
            return false;
        }

        // 相対パスの構成要素
        for (const auto& part : path)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    // ASCII制御文字の有無を調べます(value: 検証する文字列)。
    [[nodiscard]] bool HasControlCharacter(
        const std::string_view value) noexcept
    {
        // 検査する文字のバイト
        for (const char character : value)
        {
            // 符号なしの文字コード
            const auto code =
                static_cast<unsigned char>(character);
            if (code < 0x20u || code == 0x7Fu)
            {
                return true;
            }
        }
        return false;
    }

    // Rich Presenceの公開値を検証します(presence: 表示設定)。
    // アプリIDはASCII数字に限定し、制約違反はinvalid_argumentです。
    void ValidateDiscordPresenceSettings(
        const LamaPon::DiscordPresenceProjectSettings& presence)
    {
        if (!presence.applicationId.empty())
        {
            if (presence.applicationId.size()
                > LamaPon::DiscordApplicationIdMaxBytes)
            {
                throw std::invalid_argument(
                    "Discord application ID must be 1 to 32 ASCII digits.");
            }
            // アプリIDの検査文字
            for (const char character : presence.applicationId)
            {
                if (character < '0' || character > '9')
                {
                    throw std::invalid_argument(
                        "Discord application ID must be 1 to 32 ASCII digits.");
                }
            }
        }
        if (presence.defaultLargeImageKey.size()
                > LamaPon::DiscordActivityImageKeyMaxBytes
            || HasControlCharacter(
                presence.defaultLargeImageKey))
        {
            throw std::invalid_argument(
                "Discord default large image key must be at most 256 printable bytes.");
        }
        if (!presence.defaultLargeImageText.empty()
            && (presence.defaultLargeImageText.size()
                    < LamaPon::DiscordActivityTextMinBytes
                || presence.defaultLargeImageText.size()
                    > LamaPon::DiscordActivityTextMaxBytes
                || HasControlCharacter(
                    presence.defaultLargeImageText)))
        {
            throw std::invalid_argument(
                "Discord default large image text must be 2 to 128 printable bytes.");
        }
    }
}

namespace LamaPon
{
    std::string_view ViewportNavigationPresetName(
        const ViewportNavigationPreset preset) noexcept
    {
        switch (preset)
        {
        case ViewportNavigationPreset::Orbit:
            return "Orbit";
        case ViewportNavigationPreset::Fly:
        default:
            return "Fly";
        }
    }

    ViewportNavigationPreset ViewportNavigationPresetFromName(
        const std::string_view name) noexcept
    {
        if (name == "Orbit" || name == "Unity")
        {
            return ViewportNavigationPreset::Orbit;
        }
        return ViewportNavigationPreset::Fly;
    }

    void ValidateProjectSettings(
        const ProjectSettings& settings)
    {
        if (settings.gameName.empty()
            || settings.gameName.size() > 128
            || settings.gameName.find_first_not_of(" \t\r\n")
                == std::string::npos
            || Utf8ToWide(settings.gameName).empty())
        {
            throw std::invalid_argument(
                "Game name must be valid UTF-8 text with 1 to 128 bytes.");
        }
        if (settings.windowWidth < 320
            || settings.windowWidth > 7680)
        {
            throw std::invalid_argument(
                "Window width must be between 320 and 7680.");
        }
        if (settings.windowHeight < 200
            || settings.windowHeight > 4320)
        {
            throw std::invalid_argument(
                "Window height must be between 200 and 4320.");
        }
        if (!IsSafeRelativePath(settings.startupScene))
        {
            throw std::invalid_argument(
                "Startup scene must be a safe relative asset path.");
        }
        if (!settings.gameIcon.empty()
            && !IsSafeRelativePath(settings.gameIcon))
        {
            throw std::invalid_argument(
                "Game icon must be a safe relative asset path.");
        }
        if (settings.inspectorDecimals > 6)
        {
            throw std::invalid_argument(
                "Inspector decimals must be between 0 and 6.");
        }
        switch (settings.viewport.navigationPreset)
        {
        case ViewportNavigationPreset::Fly:
        case ViewportNavigationPreset::Orbit:
            break;
        default:
            throw std::invalid_argument(
                "Viewport navigation preset is invalid.");
        }
        if (!std::isfinite(settings.viewport.orbitSensitivity)
            || settings.viewport.orbitSensitivity < 0.1f
            || settings.viewport.orbitSensitivity > 3.0f
            || !std::isfinite(settings.viewport.panSensitivity)
            || settings.viewport.panSensitivity < 0.1f
            || settings.viewport.panSensitivity > 3.0f
            || !std::isfinite(settings.viewport.zoomSensitivity)
            || settings.viewport.zoomSensitivity < 0.1f
            || settings.viewport.zoomSensitivity > 3.0f)
        {
            throw std::invalid_argument(
                "Viewport sensitivities must be between 0.1 and 3.0.");
        }
        if (settings.graphics.renderScale < 0.5f
            || settings.graphics.renderScale > 2.0f
            || settings.graphics.automaticLodQuality < 0.25f
            || settings.graphics.automaticLodQuality > 2.0f
            || settings.graphics.shadowResolution < 256
            || settings.graphics.shadowResolution > 8192
            || settings.graphics.shadowCascadeLimit < 1
            || settings.graphics.shadowCascadeLimit > 4
            || settings.graphics.pointLightLimit > 12
            || settings.graphics.spotLightLimit > 4
            || (settings.graphics.targetFrameRate != 0
                && (settings.graphics.targetFrameRate < 15
                    || settings.graphics.targetFrameRate > 1000)))
        {
            throw std::invalid_argument(
                "Graphics settings are outside their supported range.");
        }
        // 固定ステップの許容範囲は0秒より大きく0.1秒以下です。
        if (!(settings.physics.fixedTimeStep > 0.0f)
            || settings.physics.fixedTimeStep > 0.1f
            || settings.physics.maximumCatchUpSteps < 1
            || settings.physics.maximumCatchUpSteps > 32
            || settings.physics.solverIterations < 1
            || settings.physics.solverIterations > 64
            || settings.physics.sleepLinearVelocity < 0.0f
            || settings.physics.sleepAngularVelocity < 0.0f
            || settings.physics.sleepDelay < 0.0f
            || settings.physics.sleepDelay > 60.0f
            || !(settings.physics.discreteSafeSpeed > 0.0f)
            || settings.physics.discreteSafeSpeed > 100000.0f)
        {
            throw std::invalid_argument(
                "Physics settings are outside their supported range.");
        }
        if (settings.tags.size() > 256)
        {
            throw std::invalid_argument(
                "A project can register up to 256 tags.");
        }
        // 検証する登録タグ
        for (const auto& tag : settings.tags)
        {
            if (tag.empty()
                || tag.size() > 64
                || tag.front() == ' '
                || tag.back() == ' ')
            {
                throw std::invalid_argument(
                    "Tags must be 1 to 64 bytes without leading or trailing spaces.");
            }
        }
        // 重複検査の基準タグ番号
        for (std::size_t first = 0;
            first < settings.tags.size();
            ++first)
        {
            // 重複を比較するタグ番号
            for (std::size_t second = first + 1;
                second < settings.tags.size();
                ++second)
            {
                if (settings.tags[first]
                    == settings.tags[second])
                {
                    throw std::invalid_argument(
                        "Tags must be unique: "
                        + settings.tags[first]);
                }
            }
        }
        ValidateInputActions(settings.inputActions);
        ValidateNetworkConfiguration(settings.network);

        if (!settings.online.serviceBaseUrl.empty())
        {
            static_cast<void>(
                Detail::NormalizeOnlineServiceBaseUrl(
                    settings.online.serviceBaseUrl,
                    settings.online.allowInsecureLoopback));
        }
        if (!settings.online.gameId.empty()
            && !Detail::IsSafeOnlineNamespaceId(
                settings.online.gameId,
                128))
        {
            throw std::invalid_argument(
                "Online game ID must use 1 to 128 ASCII letters, digits, '.', '_', or '-'.");
        }
        if (!settings.online.environmentId.empty()
            && !Detail::IsSafeOnlineNamespaceId(
                settings.online.environmentId,
                64))
        {
            throw std::invalid_argument(
                "Online environment ID must use 1 to 64 ASCII letters, digits, '.', '_', or '-'.");
        }
        if (settings.online.enabled
            && (settings.online.serviceBaseUrl.empty()
                || settings.online.gameId.empty()
                || settings.online.environmentId.empty()))
        {
            throw std::invalid_argument(
                "Enabled online services require a service URL, game ID, and environment ID.");
        }
        ValidateDiscordPresenceSettings(
            settings.online.discordPresence);
    }

    void ValidateProjectSettings(
        const ProjectSettings& settings,
        const ProjectSettingsFileType fileType)
    {
        ValidateProjectSettings(settings);
        if (fileType == ProjectSettingsFileType::GamePackage
            && settings.online.enabled
            && settings.online.allowInsecureLoopback)
        {
            throw std::invalid_argument(
                "An exported game cannot enable online services while allowing insecure loopback HTTP.");
        }
    }

    ProjectSettings LoadProjectSettings(
        const std::filesystem::path& path)
    {
        // 設定ファイルの入力ストリーム
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Project settings were not found: "
                + PathToUtf8(path));
        }

        // 読み込んだ設定文書
        nlohmann::json document;
        input >> document;
        static_cast<void>(
            MigrateSerializedDocument(
                document,
                SerializedDocumentKind::ProjectSettings));

        // 既定値から復元する設定
        ProjectSettings settings;
        settings.gameName = document.value(
            "gameName",
            settings.gameName);
        // ウィンドウ設定のJSON項目
        if (const auto window = document.find("window");
            window != document.end() && window->is_object())
        {
            settings.windowWidth = window->value(
                "width",
                settings.windowWidth);
            settings.windowHeight = window->value(
                "height",
                settings.windowHeight);
        }
        settings.startupScene = PathFromUtf8(
            document.value(
                "startupScene",
                PathToUtf8(settings.startupScene)));
        settings.splashScreenEnabled = document.value(
            "splashScreenEnabled",
            settings.splashScreenEnabled);
        settings.gameIcon = PathFromUtf8(
            document.value(
                "gameIcon",
                PathToUtf8(settings.gameIcon)));
        settings.scriptEditorPath = PathFromUtf8(
            document.value(
                "scriptEditorPath",
                PathToUtf8(settings.scriptEditorPath)));
        settings.stripShaderSourceOnExport =
            document.value(
                "stripShaderSourceOnExport",
                settings.stripShaderSourceOnExport);
        settings.autoBuildGameModuleOnSave =
            document.value(
                "autoBuildGameModuleOnSave",
                settings.autoBuildGameModuleOnSave);
        settings.inspectorDecimals =
            document.value(
                "inspectorDecimals",
                settings.inspectorDecimals);
        // 通信設定のJSON項目
        if (const auto network = document.find("network"); network != document.end())
        {
            settings.network = Detail::NetworkSettingsFromJson(*network);
        }
        // オンライン設定のJSON項目
        if (const auto online = document.find("online");
            online != document.end())
        {
            if (!online->is_object())
            {
                throw std::runtime_error(
                    "Online settings must be a JSON object.");
            }
            settings.online.enabled = online->value(
                "enabled",
                settings.online.enabled);
            settings.online.serviceBaseUrl = online->value(
                "serviceBaseUrl",
                settings.online.serviceBaseUrl);
            settings.online.gameId = online->value(
                "gameId",
                settings.online.gameId);
            settings.online.environmentId = online->value(
                "environmentId",
                settings.online.environmentId);
            settings.online.allowInsecureLoopback = online->value(
                "allowInsecureLoopback",
                settings.online.allowInsecureLoopback);
            settings.online.openAuthorizationBrowser = online->value(
                "openAuthorizationBrowser",
                settings.online.openAuthorizationBrowser);
            // Rich PresenceのJSON項目
            if (const auto presence =
                    online->find("discordPresence");
                presence != online->end())
            {
                if (!presence->is_object())
                {
                    throw std::runtime_error(
                        "Discord presence settings must be a JSON object.");
                }
                // 復元先のPresence設定
                auto& target = settings.online.discordPresence;
                target.enabled = presence->value(
                    "enabled",
                    target.enabled);
                target.applicationId = presence->value(
                    "applicationId",
                    target.applicationId);
                target.defaultLargeImageKey = presence->value(
                    "defaultLargeImageKey",
                    target.defaultLargeImageKey);
                target.defaultLargeImageText = presence->value(
                    "defaultLargeImageText",
                    target.defaultLargeImageText);
            }
        }
        // 描画設定のJSON項目
        if (const auto graphics = document.find("graphics");
            graphics != document.end()
            && graphics->is_object())
        {
            // 復元する描画品質プリセット
            const auto preset =
                GraphicsQualityPresetFromName(
                    graphics->value(
                        "preset",
                        std::string(
                            GraphicsQualityPresetName(
                                settings.graphics.preset))));
            settings.graphics =
                GraphicsSettingsForPreset(preset);
            settings.graphics.renderScale =
                graphics->value(
                    "renderScale",
                    settings.graphics.renderScale);
            settings.graphics.shadowsEnabled =
                graphics->value(
                    "shadowsEnabled",
                    settings.graphics.shadowsEnabled);
            settings.graphics.shadowResolution =
                graphics->value(
                    "shadowResolution",
                    settings.graphics.shadowResolution);
            settings.graphics.shadowCascadeLimit =
                graphics->value(
                    "shadowCascadeLimit",
                    settings.graphics.shadowCascadeLimit);
            settings.graphics.bloomEnabled =
                graphics->value(
                    "bloomEnabled",
                    settings.graphics.bloomEnabled);
            settings.graphics.screenSpaceLensFlareEnabled =
                graphics->value(
                    "screenSpaceLensFlareEnabled",
                    settings.graphics
                        .screenSpaceLensFlareEnabled);
            settings.graphics.depthOfFieldEnabled =
                graphics->value(
                    "depthOfFieldEnabled",
                    settings.graphics
                        .depthOfFieldEnabled);
            settings.graphics.motionBlurEnabled =
                graphics->value(
                    "motionBlurEnabled",
                    settings.graphics
                        .motionBlurEnabled);
            settings.graphics.autoExposureEnabled =
                graphics->value(
                    "autoExposureEnabled",
                    settings.graphics
                        .autoExposureEnabled);
            settings.graphics.ambientOcclusionEnabled =
                graphics->value(
                    "ambientOcclusionEnabled",
                    settings.graphics
                        .ambientOcclusionEnabled);
            settings.graphics.antiAliasingEnabled =
                graphics->value(
                    "antiAliasingEnabled",
                    settings.graphics.antiAliasingEnabled);
            settings.graphics.fogEnabled =
                graphics->value(
                    "fogEnabled",
                    settings.graphics.fogEnabled);
            settings.graphics.vSyncEnabled =
                graphics->value(
                    "vSyncEnabled",
                    settings.graphics.vSyncEnabled);
            settings.graphics.pointLightLimit =
                graphics->value(
                    "pointLightLimit",
                    settings.graphics.pointLightLimit);
            settings.graphics.spotLightLimit =
                graphics->value(
                    "spotLightLimit",
                    settings.graphics.spotLightLimit);
            settings.graphics.targetFrameRate =
                graphics->value(
                    "targetFrameRate",
                    settings.graphics.targetFrameRate);
            settings.graphics.runtimeTextureCompression =
                graphics->value(
                    "runtimeTextureCompression",
                    settings.graphics
                        .runtimeTextureCompression);
            settings.graphics.automaticLodQuality =
                graphics->value(
                    "automaticLodQuality",
                    settings.graphics.automaticLodQuality);
            // 個別の描画方式は、設定全体を初期化するプリセットの適用後に復元します。
            settings.graphics.renderingPath =
                RenderingPathFromName(
                    graphics->value(
                        "renderingPath",
                        std::string(
                            RenderingPathName(
                                settings.graphics
                                    .renderingPath))));
            settings.graphics.renderingApi =
                RenderingApiFromName(
                    graphics->value(
                        "renderingApi",
                        std::string(
                            RenderingApiName(
                                settings.graphics
                                    .renderingApi))));
        }
        // 視点操作設定のJSON項目
        if (const auto viewport = document.find("viewport");
            viewport != document.end()
            && viewport->is_object())
        {
            settings.viewport.navigationPreset =
                ViewportNavigationPresetFromName(
                    viewport->value(
                        "navigationPreset",
                        std::string(
                            ViewportNavigationPresetName(
                                settings.viewport.navigationPreset))));
            settings.viewport.orbitSensitivity =
                viewport->value(
                    "orbitSensitivity",
                    settings.viewport.orbitSensitivity);
            settings.viewport.panSensitivity =
                viewport->value(
                    "panSensitivity",
                    settings.viewport.panSensitivity);
            settings.viewport.zoomSensitivity =
                viewport->value(
                    "zoomSensitivity",
                    settings.viewport.zoomSensitivity);
            settings.viewport.invertY =
                viewport->value(
                    "invertY",
                    settings.viewport.invertY);
        }
        // 物理設定のJSON項目
        if (const auto physics = document.find("physics");
            physics != document.end()
            && physics->is_object())
        {
            // 重力ベクトルのJSON項目
            if (const auto gravity = physics->find("gravity");
                gravity != physics->end()
                && gravity->is_object())
            {
                settings.physics.gravity = {
                    gravity->value(
                        "x",
                        settings.physics.gravity.x),
                    gravity->value(
                        "y",
                        settings.physics.gravity.y),
                    gravity->value(
                        "z",
                        settings.physics.gravity.z)
                };
            }
            settings.physics.fixedTimeStep =
                physics->value(
                    "fixedTimeStep",
                    settings.physics.fixedTimeStep);
            settings.physics.maximumCatchUpSteps =
                physics->value(
                    "maximumCatchUpSteps",
                    settings.physics.maximumCatchUpSteps);
            settings.physics.solverIterations =
                physics->value(
                    "solverIterations",
                    settings.physics.solverIterations);
            settings.physics.sleepLinearVelocity =
                physics->value(
                    "sleepLinearVelocity",
                    settings.physics.sleepLinearVelocity);
            settings.physics.sleepAngularVelocity =
                physics->value(
                    "sleepAngularVelocity",
                    settings.physics.sleepAngularVelocity);
            settings.physics.sleepDelay =
                physics->value(
                    "sleepDelay",
                    settings.physics.sleepDelay);
            settings.physics.discreteSafeSpeed =
                physics->value(
                    "discreteSafeSpeed",
                    settings.physics.discreteSafeSpeed);
            settings.physics.clampDiscreteSpeed =
                physics->value(
                    "clampDiscreteSpeed",
                    settings.physics.clampDiscreteSpeed);
            // 衝突レイヤー名のJSON項目
            if (const auto layerNames =
                    physics->find("layerNames");
                layerNames != physics->end()
                && layerNames->is_array())
            {
                // 復元する衝突レイヤー番号
                std::size_t index = 0;
                // 復元するレイヤー名
                for (const auto& name : *layerNames)
                {
                    if (index >= CollisionLayerCount)
                    {
                        break;
                    }
                    if (name.is_string())
                    {
                        settings.physics
                            .layerNames[index] =
                            name.get<std::string>();
                    }
                    ++index;
                }
            }
            // 衝突を無効にする組のJSON項目
            if (const auto collisionOff =
                    physics->find("collisionOff");
                collisionOff != physics->end()
                && collisionOff->is_array())
            {
                // 衝突を無効にするレイヤー組
                for (const auto& pair : *collisionOff)
                {
                    if (!pair.is_array()
                        || pair.size() != 2
                        || !pair[0].is_number_unsigned()
                        || !pair[1].is_number_unsigned())
                    {
                        continue;
                    }
                    // 組の先頭レイヤー番号
                    const auto first =
                        pair[0].get<std::uint32_t>();
                    // 組の末尾レイヤー番号
                    const auto second =
                        pair[1].get<std::uint32_t>();
                    if (first >= CollisionLayerCount
                        || second >= CollisionLayerCount)
                    {
                        continue;
                    }
                    settings.physics
                        .collisionMatrix[first] &=
                        ~(1u << second);
                    settings.physics
                        .collisionMatrix[second] &=
                        ~(1u << first);
                }
            }
        }
        // 登録タグのJSON項目
        if (const auto tags = document.find("tags");
            tags != document.end())
        {
            if (!tags->is_array())
            {
                throw std::runtime_error(
                    "Tags must be a JSON array of strings.");
            }
            settings.tags.clear();
            // 復元するタグのJSON値
            for (const auto& tagValue : *tags)
            {
                settings.tags.push_back(
                    tagValue.get<std::string>());
            }
        }
        // 入力割り当てのJSON項目
        if (const auto inputActions =
            document.find("inputActions");
            inputActions != document.end())
        {
            if (!inputActions->is_array())
            {
                throw std::runtime_error(
                    "Input actions must be a JSON array.");
            }
            settings.inputActions.clear();
            // 入力アクションのJSON値
            for (const auto& actionValue : *inputActions)
            {
                // 復元する入力アクション
                InputActionDefinition action;
                action.name =
                    actionValue.at("name").get<std::string>();
                // 入力操作の割り当て値
                for (const auto& bindingValue :
                    actionValue.at("bindings"))
                {
                    action.bindings.push_back(
                        InputBinding{
                            InputControlFromName(
                                bindingValue.at("control")
                                    .get<std::string>()),
                            bindingValue.value(
                                "scale",
                                1.0f)
                        });
                }
                settings.inputActions.push_back(
                    std::move(action));
            }
        }
        // 読み込み画面のJSON項目
        if (const auto loadingScreen = document.find("loadingScreen");
            loadingScreen != document.end())
        {
            settings.loadingScreen = SceneLoadingScreenFromJson(
                *loadingScreen,
                settings.loadingScreen);
        }
        // 文書の保存形式
        const auto fileType = document.value(
            "format",
            std::string{}) == "LamaPonGame"
            ? ProjectSettingsFileType::GamePackage
            : ProjectSettingsFileType::Project;
        ValidateProjectSettings(settings, fileType);
        return settings;
    }

    void SaveProjectSettings(
        const std::filesystem::path& path,
        const ProjectSettings& settings,
        const ProjectSettingsFileType fileType)
    {
        ValidateProjectSettings(settings, fileType);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }

        // 保存する衝突レイヤー名配列
        nlohmann::json layerNamesJson =
            nlohmann::json::array();
        {
            // 末尾の空欄を除いたレイヤー数
            std::size_t used = CollisionLayerCount;
            while (used > 0
                && settings.physics
                    .layerNames[used - 1].empty())
            {
                --used;
            }
            // 保存する衝突レイヤー番号
            for (std::size_t index = 0;
                index < used;
                ++index)
            {
                layerNamesJson.push_back(
                    settings.physics.layerNames[index]);
            }
        }
        // 衝突を無効にする組の保存配列
        nlohmann::json collisionOffJson =
            nlohmann::json::array();
        // 衝突検査の基準レイヤー番号
        for (std::size_t row = 0;
            row < CollisionLayerCount;
            ++row)
        {
            // 衝突検査の相手レイヤー番号
            for (std::size_t column = row;
                column < CollisionLayerCount;
                ++column)
            {
                if ((settings.physics.collisionMatrix[row]
                        & (1u << column)) == 0)
                {
                    collisionOffJson.push_back({
                        row,
                        column });
                }
            }
        }

        // 書き出す設定文書
        // 物理設定は配布用にも保存し、衝突の無効化は重複しない番号組で記録します。
        nlohmann::json document{
            {
                "format",
                fileType == ProjectSettingsFileType::Project
                    ? "LamaPonProject"
                    : "LamaPonGame"
            },
            { "version", 1 },
            { "gameName", settings.gameName },
            { "network", Detail::NetworkSettingsToJson(settings.network) },
            {
                "window",
                {
                    { "width", settings.windowWidth },
                    { "height", settings.windowHeight }
                }
            },
            {
                "startupScene",
                PathToUtf8(settings.startupScene)
            },
            {
                "splashScreenEnabled",
                settings.splashScreenEnabled
            },
            {
                "online",
                {
                    { "enabled", settings.online.enabled },
                    {
                        "serviceBaseUrl",
                        settings.online.serviceBaseUrl
                    },
                    { "gameId", settings.online.gameId },
                    {
                        "environmentId",
                        settings.online.environmentId
                    },
                    {
                        "allowInsecureLoopback",
                        settings.online.allowInsecureLoopback
                    },
                    {
                        "openAuthorizationBrowser",
                        settings.online.openAuthorizationBrowser
                    },
                    {
                        "discordPresence",
                        {
                            {
                                "enabled",
                                settings.online.discordPresence
                                    .enabled
                            },
                            {
                                "applicationId",
                                settings.online.discordPresence
                                    .applicationId
                            },
                            {
                                "defaultLargeImageKey",
                                settings.online.discordPresence
                                    .defaultLargeImageKey
                            },
                            {
                                "defaultLargeImageText",
                                settings.online.discordPresence
                                    .defaultLargeImageText
                            }
                        }
                    }
                }
            },
            {
                "graphics",
                {
                    {
                        "preset",
                        GraphicsQualityPresetName(
                            settings.graphics.preset)
                    },
                    {
                        "renderScale",
                        settings.graphics.renderScale
                    },
                    {
                        "shadowsEnabled",
                        settings.graphics.shadowsEnabled
                    },
                    {
                        "shadowResolution",
                        settings.graphics.shadowResolution
                    },
                    {
                        "shadowCascadeLimit",
                        settings.graphics.shadowCascadeLimit
                    },
                    {
                        "ambientOcclusionEnabled",
                        settings.graphics
                            .ambientOcclusionEnabled
                    },
                    {
                        "bloomEnabled",
                        settings.graphics.bloomEnabled
                    },
                    {
                        "screenSpaceLensFlareEnabled",
                        settings.graphics
                            .screenSpaceLensFlareEnabled
                    },
                    {
                        "depthOfFieldEnabled",
                        settings.graphics
                            .depthOfFieldEnabled
                    },
                    {
                        "motionBlurEnabled",
                        settings.graphics
                            .motionBlurEnabled
                    },
                    {
                        "autoExposureEnabled",
                        settings.graphics
                            .autoExposureEnabled
                    },
                    {
                        "antiAliasingEnabled",
                        settings.graphics.antiAliasingEnabled
                    },
                    {
                        "fogEnabled",
                        settings.graphics.fogEnabled
                    },
                    {
                        "vSyncEnabled",
                        settings.graphics.vSyncEnabled
                    },
                    {
                        "pointLightLimit",
                        settings.graphics.pointLightLimit
                    },
                    {
                        "spotLightLimit",
                        settings.graphics.spotLightLimit
                    },
                    {
                        "targetFrameRate",
                        settings.graphics.targetFrameRate
                    },
                    {
                        "renderingPath",
                        RenderingPathName(
                            settings.graphics
                                .renderingPath)
                    },
                    {
                        "renderingApi",
                        RenderingApiName(
                            settings.graphics
                                .renderingApi)
                    },
                    {
                        "runtimeTextureCompression",
                        settings.graphics
                            .runtimeTextureCompression
                    },
                    {
                        "automaticLodQuality",
                        settings.graphics.automaticLodQuality
                    }
                }
            },
            {
                "physics",
                {
                    {
                        "gravity",
                        {
                            { "x", settings.physics.gravity.x },
                            { "y", settings.physics.gravity.y },
                            { "z", settings.physics.gravity.z }
                        }
                    },
                    {
                        "fixedTimeStep",
                        settings.physics.fixedTimeStep
                    },
                    {
                        "maximumCatchUpSteps",
                        settings.physics.maximumCatchUpSteps
                    },
                    {
                        "solverIterations",
                        settings.physics.solverIterations
                    },
                    {
                        "sleepLinearVelocity",
                        settings.physics.sleepLinearVelocity
                    },
                    {
                        "sleepAngularVelocity",
                        settings.physics.sleepAngularVelocity
                    },
                    {
                        "sleepDelay",
                        settings.physics.sleepDelay
                    },
                    {
                        "discreteSafeSpeed",
                        settings.physics.discreteSafeSpeed
                    },
                    {
                        "clampDiscreteSpeed",
                        settings.physics.clampDiscreteSpeed
                    },
                    { "layerNames", layerNamesJson },
                    { "collisionOff", collisionOffJson }
                }
            }
        };
        // 編集用の設定と埋め込み済みアイコンのパスは配布設定へ保存しません。
        if (fileType == ProjectSettingsFileType::Project)
        {
            document["viewport"] = {
                {
                    "navigationPreset",
                    ViewportNavigationPresetName(
                        settings.viewport.navigationPreset)
                },
                {
                    "orbitSensitivity",
                    settings.viewport.orbitSensitivity
                },
                {
                    "panSensitivity",
                    settings.viewport.panSensitivity
                },
                {
                    "zoomSensitivity",
                    settings.viewport.zoomSensitivity
                },
                { "invertY", settings.viewport.invertY }
            };
            document["gameIcon"] =
                PathToUtf8(settings.gameIcon);
            document["scriptEditorPath"] =
                PathToUtf8(settings.scriptEditorPath);
            document["stripShaderSourceOnExport"] =
                settings.stripShaderSourceOnExport;
            document["autoBuildGameModuleOnSave"] =
                settings.autoBuildGameModuleOnSave;
            document["inspectorDecimals"] =
                settings.inspectorDecimals;
        }
        document["tags"] = settings.tags;
        document["loadingScreen"] =
            SceneLoadingScreenToJson(settings.loadingScreen);
        document["inputActions"] =
            nlohmann::json::array();
        // 保存する入力アクション
        for (const auto& action : settings.inputActions)
        {
            // 入力アクションの保存値
            nlohmann::json actionValue{
                { "name", action.name },
                { "bindings", nlohmann::json::array() }
            };
            // 保存する入力操作の割り当て
            for (const auto& binding : action.bindings)
            {
                actionValue["bindings"].push_back(
                    {
                        {
                            "control",
                            InputControlName(
                                binding.control)
                        },
                        { "scale", binding.scale }
                    });
            }
            document["inputActions"].push_back(
                std::move(actionValue));
        }

        {
            // 既存設定の入力ストリーム
            std::ifstream existing(path, std::ios::binary);
            if (existing)
            {
                try
                {
                    // 未知の項目を引き継ぐ既存文書
                    nlohmann::json previous;
                    existing >> previous;
                    if (previous.is_object())
                    {
                        // 引き継ぎを判定する既存項目
                        for (const auto& entry :
                            previous.items())
                        {
                            if (entry.key() == "sceneTransition")
                            {
                                continue;
                            }
                            if (!document.contains(
                                entry.key()))
                            {
                                document[entry.key()] =
                                    entry.value();
                            }
                        }
                    }
                }
                catch (const std::exception&)
                {
                    // 既存文書が壊れている場合は、未知の項目を引き継がず保存を続けます。
                }
            }
        }

        // 設定ファイルの出力ストリーム
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create project settings: "
                + PathToUtf8(path));
        }
        output << document.dump(2) << '\n';
        output.close();
        if (!output)
        {
            throw std::runtime_error(
                "Could not write project settings: "
                + PathToUtf8(path));
        }
    }
}
