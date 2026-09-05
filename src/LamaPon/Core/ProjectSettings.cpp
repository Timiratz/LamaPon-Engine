#include "LamaPon/Core/ProjectSettings.h"

#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    bool IsSafeRelativePath(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
        {
            return false;
        }

        for (const auto& part : path)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
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
        // 1.0より大きい描画スケールは高解像度で描いて縮小する
        // スーパーサンプリング（SSAA）になります。
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
        // 物理計算を停止させる0以下の値を拒否します。固定ステップは、
        // 高速な物体の衝突を安定して検出できる範囲に制限します。
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
        for (std::size_t first = 0;
            first < settings.tags.size();
            ++first)
        {
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
    }

    ProjectSettings LoadProjectSettings(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Project settings were not found: "
                + PathToUtf8(path));
        }

        nlohmann::json document;
        input >> document;
        static_cast<void>(
            MigrateSerializedDocument(
                document,
                SerializedDocumentKind::ProjectSettings));

        ProjectSettings settings;
        settings.gameName = document.value(
            "gameName",
            settings.gameName);
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
        if (const auto graphics = document.find("graphics");
            graphics != document.end()
            && graphics->is_object())
        {
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
            // プリセットの後に読みます。上でGraphicsSettingsForPreset
            // が既定へ戻すので、先に読むと必ず上書きされます。
            settings.graphics.renderingPath =
                RenderingPathFromName(
                    graphics->value(
                        "renderingPath",
                        std::string(
                            RenderingPathName(
                                settings.graphics
                                    .renderingPath))));
        }
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
        if (const auto physics = document.find("physics");
            physics != document.end()
            && physics->is_object())
        {
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
            if (const auto layerNames =
                    physics->find("layerNames");
                layerNames != physics->end()
                && layerNames->is_array())
            {
                std::size_t index = 0;
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
            if (const auto collisionOff =
                    physics->find("collisionOff");
                collisionOff != physics->end()
                && collisionOff->is_array())
            {
                for (const auto& pair : *collisionOff)
                {
                    if (!pair.is_array()
                        || pair.size() != 2
                        || !pair[0].is_number_unsigned()
                        || !pair[1].is_number_unsigned())
                    {
                        continue;
                    }
                    const auto first =
                        pair[0].get<std::uint32_t>();
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
        if (const auto tags = document.find("tags");
            tags != document.end())
        {
            if (!tags->is_array())
            {
                throw std::runtime_error(
                    "Tags must be a JSON array of strings.");
            }
            settings.tags.clear();
            for (const auto& tagValue : *tags)
            {
                settings.tags.push_back(
                    tagValue.get<std::string>());
            }
        }
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
            for (const auto& actionValue : *inputActions)
            {
                InputActionDefinition action;
                action.name =
                    actionValue.at("name").get<std::string>();
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
        ValidateProjectSettings(settings);
        return settings;
    }

    void SaveProjectSettings(
        const std::filesystem::path& path,
        const ProjectSettings& settings,
        const ProjectSettingsFileType fileType)
    {
        ValidateProjectSettings(settings);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }

        // レイヤー名は使用中の項目だけを保存し、末尾の空欄を省きます。
        // マトリクスはビット列ではなく「衝突しないペアの一覧」で
        // 保存します。既定値では空配列となり、JSONだけでも設定内容を
        // 読み取れます。
        nlohmann::json layerNamesJson =
            nlohmann::json::array();
        {
            std::size_t used = CollisionLayerCount;
            while (used > 0
                && settings.physics
                    .layerNames[used - 1].empty())
            {
                --used;
            }
            for (std::size_t index = 0;
                index < used;
                ++index)
            {
                layerNamesJson.push_back(
                    settings.physics.layerNames[index]);
            }
        }
        nlohmann::json collisionOffJson =
            nlohmann::json::array();
        for (std::size_t row = 0;
            row < CollisionLayerCount;
            ++row)
        {
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

        nlohmann::json document{
            {
                "format",
                fileType == ProjectSettingsFileType::Project
                    ? "LamaPonProject"
                    : "LamaPonGame"
            },
            { "version", 1 },
            { "gameName", settings.gameName },
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
            // 物理は書き出したゲームでも同じでなければならないので、
            // ProjectとGamePackageの両方へ書きます（エディター表示
            // 用のinspectorDecimalsとはそこが違います）。
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
                    // 「当たらないペア」の一覧（[i, j]、i <= j）。
                    // 空なら全レイヤーが当たる（既定）。
                    { "collisionOff", collisionOffJson }
                }
            }
        };
        // アイコンはExport時に実行ファイルへ埋め込むため、
        // ゲームパッケージ側の設定には不要です。
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
        document["inputActions"] =
            nlohmann::json::array();
        for (const auto& action : settings.inputActions)
        {
            nlohmann::json actionValue{
                { "name", action.name },
                { "bindings", nlohmann::json::array() }
            };
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

        // ProjectSettingsが知らないキーは、元のファイルから引き継ぎ
        // ます。ここで丸ごと書き直すと、他の仕組みが書いた項目
        // （engineVersionなど）が保存のたびに消えます。
        // 未知のキーを保持することで、別機能が保存した設定を失いません。
        {
            std::ifstream existing(path, std::ios::binary);
            if (existing)
            {
                try
                {
                    nlohmann::json previous;
                    existing >> previous;
                    if (previous.is_object())
                    {
                        for (const auto& entry :
                            previous.items())
                        {
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
                    // 壊れていたら引き継ぎません（保存は続けます）。
                }
            }
        }

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
