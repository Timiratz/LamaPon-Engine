#include "LamaPon/Core/ProjectMigration.h"

#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    // 実体はLamaPon::BuiltInProjectAssets()。定義はこのファイルの
    // 下の方（namespace LamaPon）にあります。

    [[nodiscard]] std::string ReadFileText(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // プロジェクト設定へ記録されたエンジンバージョンを読みます。
    [[nodiscard]] std::string ReadRecordedEngineVersion(
        const std::filesystem::path& settingsPath)
    {
        try
        {
            std::ifstream input(
                settingsPath,
                std::ios::binary);
            if (!input)
            {
                return {};
            }
            nlohmann::json document;
            input >> document;
            return document.value(
                "engineVersion",
                std::string{});
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    // プロジェクト設定へ現在のエンジンバージョンを書き戻します
    // （他のキーは保持します）。
    void WriteRecordedEngineVersion(
        const std::filesystem::path& settingsPath,
        const std::string_view version)
    {
        try
        {
            nlohmann::json document;
            {
                std::ifstream input(
                    settingsPath,
                    std::ios::binary);
                if (!input)
                {
                    return;
                }
                input >> document;
            }
            if (!document.is_object())
            {
                return;
            }
            document["engineVersion"] =
                std::string(version);
            std::ofstream output(
                settingsPath,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output << document.dump(2) << '\n';
        }
        catch (const std::exception&)
        {
        }
    }
}

namespace LamaPon
{
    const std::vector<std::filesystem::path>&
        BuiltInProjectAssets()
    {
        // includeされる.hlsliを落とすと、更新した瞬間に
        // シェーダーがコンパイルできなくなります。足すときは
        // 依存も一緒に。テストが取りこぼしを検査します。
        static const std::vector<std::filesystem::path>
            assets{
                L"shaders/LamaPonScreenDepth.hlsli",
                L"shaders/LamaPonLit.hlsl",
                L"shaders/LamaPonShaderError.hlsl",
                L"shaders/LamaPonSpriteError.hlsl",
                L"shaders/LamaPonCustomMaterial.hlsl",
                L"shaders/LamaPonEnvironment.hlsl",
                L"shaders/LamaPonLightCulling.hlsl",
                L"shaders/LamaPonSpriteLit.hlsl",
                L"shaders/LamaPonSpriteMask.hlsl",
                L"textures/LamaPonEngineLogo.png"
            };
        return assets;
    }

    std::optional<int> CompareEngineVersions(
        const std::string_view left,
        const std::string_view right)
    {
        const auto split =
            [](const std::string_view text,
                std::vector<long long>& parts)
        {
            parts.clear();
            std::size_t begin = 0;
            while (begin <= text.size())
            {
                const auto found = text.find('.', begin);
                const auto end = found == std::string_view::npos
                    ? text.size()
                    : found;
                const auto piece =
                    text.substr(begin, end - begin);
                if (piece.empty())
                {
                    return false;
                }
                long long value = 0;
                for (const char character : piece)
                {
                    if (character < '0' || character > '9')
                    {
                        return false;
                    }
                    value = value * 10
                        + (character - '0');
                    // 桁が異常に多い文字列で溢れさせない。
                    if (value > 1'000'000'000LL)
                    {
                        return false;
                    }
                }
                parts.push_back(value);
                if (found == std::string_view::npos)
                {
                    break;
                }
                begin = end + 1;
            }
            return !parts.empty();
        };

        std::vector<long long> leftParts;
        std::vector<long long> rightParts;
        if (!split(left, leftParts)
            || !split(right, rightParts))
        {
            return std::nullopt;
        }
        // 足りない桁は0。"2026.8" は "2026.8.1" より古い扱いです。
        const auto count = std::max(
            leftParts.size(),
            rightParts.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            const long long leftValue =
                index < leftParts.size()
                ? leftParts[index]
                : 0;
            const long long rightValue =
                index < rightParts.size()
                ? rightParts[index]
                : 0;
            if (leftValue != rightValue)
            {
                return leftValue < rightValue ? -1 : 1;
            }
        }
        return 0;
    }

    void RecordProjectEngineVersion(
        const std::filesystem::path& projectRoot,
        const std::string_view version)
    {
        WriteRecordedEngineVersion(
            projectRoot / L".lamapon" / L"project.json",
            version);
    }

    ProjectVersionInfo InspectProjectVersion(
        const std::filesystem::path& projectRoot,
        const std::string_view currentEngineVersion)
    {
        ProjectVersionInfo info{};
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        info.recordedVersion =
            ReadRecordedEngineVersion(settingsPath);
        if (info.recordedVersion.empty())
        {
            info.status =
                ProjectVersionStatus::Unrecorded;
            return info;
        }
        const auto comparison = CompareEngineVersions(
            info.recordedVersion,
            currentEngineVersion);
        if (!comparison)
        {
            // 解釈できない版番号は未記録として扱い、project.jsonを
            // 手動編集したプロジェクトも開けるようにします。
            info.status =
                ProjectVersionStatus::Unrecorded;
            return info;
        }
        info.status = *comparison < 0
            ? ProjectVersionStatus::Older
            : (*comparison > 0
                ? ProjectVersionStatus::Newer
                : ProjectVersionStatus::Match);
        return info;
    }

    ProjectMigrationResult MigrateProjectAssets(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineAssetRoot,
        const std::string_view currentEngineVersion)
    {
        ProjectMigrationResult result;
        try
        {
            if (!std::filesystem::is_directory(projectRoot)
                || !std::filesystem::is_directory(
                    engineAssetRoot))
            {
                return result;
            }

            const auto settingsPath = projectRoot
                / L".lamapon" / L"project.json";
            result.previousEngineVersion =
                ReadRecordedEngineVersion(settingsPath);

            for (const auto& relative :
                BuiltInProjectAssets())
            {
                const auto source =
                    engineAssetRoot / relative;
                if (!std::filesystem::is_regular_file(
                    source))
                {
                    continue;
                }
                const auto destination = projectRoot
                    / L"assets" / relative;

                const auto latest = ReadFileText(source);
                if (latest.empty())
                {
                    continue;
                }
                if (std::filesystem::is_regular_file(
                    destination))
                {
                    const auto existing =
                        ReadFileText(destination);
                    if (existing == latest)
                    {
                        continue;
                    }
                    // 改行コードだけの差では書き換えない。エンジンの
                    // チェックアウト(CRLF)とプロジェクトのリポジトリ
                    // (LF)の間で、エディターを開くたびシェーダーが
                    // 不要に書き換わるのを防ぎます。
                    const auto normalize =
                        [](std::string text)
                    {
                        std::erase(text, '\r');
                        return text;
                    };
                    if (normalize(existing) == normalize(latest))
                    {
                        continue;
                    }
                    // 改造されている可能性があるため退避します。
                    auto backup = destination;
                    backup += L".bak";
                    std::error_code backupError;
                    std::filesystem::copy_file(
                        destination,
                        backup,
                        std::filesystem::copy_options::
                            overwrite_existing,
                        backupError);
                    if (!backupError)
                    {
                        result.backedUpAssets.push_back(
                            relative);
                    }
                }

                std::error_code createError;
                std::filesystem::create_directories(
                    destination.parent_path(),
                    createError);
                std::ofstream output(
                    destination,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    continue;
                }
                output << latest;
                output.close();
                if (output)
                {
                    result.updatedAssets.push_back(
                        relative);
                    result.changed = true;
                }
            }

            if (result.previousEngineVersion
                != currentEngineVersion)
            {
                WriteRecordedEngineVersion(
                    settingsPath,
                    currentEngineVersion);
                result.changed = true;
            }
        }
        catch (const std::exception&)
        {
        }
        return result;
    }
}
