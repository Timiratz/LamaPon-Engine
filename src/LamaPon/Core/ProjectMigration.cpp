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

    // ファイル内容を読み取り、開けなければ空を返します(path: 読み込み元)。
    [[nodiscard]] std::string ReadFileText(
        const std::filesystem::path& path)
    {
        // 資源の内容を読む入力ファイル
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // 設定文書のエンジン版を返し、失敗時は空を返します(settingsPath: 設定文書のパス)。
    [[nodiscard]] std::string ReadRecordedEngineVersion(
        const std::filesystem::path& settingsPath)
    {
        try
        {
            // プロジェクト設定の入力ファイル
            std::ifstream input(
                settingsPath,
                std::ios::binary);
            if (!input)
            {
                return {};
            }
            // 版番号を読み書きする設定文書
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

    // 設定文書のエンジン版だけを書き換えます(settingsPath: 設定文書のパス, version: 記録する版番号)。
    void WriteRecordedEngineVersion(
        const std::filesystem::path& settingsPath,
        const std::string_view version)
    {
        try
        {
            // 版番号を読み書きする設定文書
            nlohmann::json document;
            {
                // 他の項目を保持する設定文書入力
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
            // 設定文書を書き戻すファイル
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
        // 配布時に同期する組み込み資源
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
        // 版番号を数値列へ分解します(text: 版番号文字列, parts: 分解結果の出力先)。
        const auto split =
            [](const std::string_view text,
                std::vector<long long>& parts)
        {
            parts.clear();
            // 読み取る数値成分の先頭位置
            std::size_t begin = 0;
            while (begin <= text.size())
            {
                // 次のドット区切りの位置
                const auto found = text.find('.', begin);
                // 現在の数値成分の末尾位置
                const auto end = found == std::string_view::npos
                    ? text.size()
                    : found;
                // 数値へ変換する文字列成分
                const auto piece =
                    text.substr(begin, end - begin);
                if (piece.empty())
                {
                    return false;
                }
                // 現在読み取り中の成分値
                long long value = 0;
                // 数字として照合する各文字
                for (const char character : piece)
                {
                    if (character < '0' || character > '9')
                    {
                        return false;
                    }
                    value = value * 10
                        + (character - '0');
                    // 次の10倍計算で桁あふれしないよう、成分を10億までに制限します。
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

        // 比較元の版番号の数値成分
        std::vector<long long> leftParts;
        // 比較先の版番号の数値成分
        std::vector<long long> rightParts;
        if (!split(left, leftParts)
            || !split(right, rightParts))
        {
            return std::nullopt;
        }
        // 比較する数値成分の個数
        const auto count = std::max(
            leftParts.size(),
            rightParts.size());
        // 比較する数値成分の位置
        for (std::size_t index = 0; index < count; ++index)
        {
            // 比較元の成分値、不足時は0
            const long long leftValue =
                index < leftParts.size()
                ? leftParts[index]
                : 0;
            // 比較先の成分値、不足時は0
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
        // 記録済みの版と現行版の比較結果
        ProjectVersionInfo info{};
        // エンジン版を保持する設定パス
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
        // 記録済みの版と現行版の大小関係
        const auto comparison = CompareEngineVersions(
            info.recordedVersion,
            currentEngineVersion);
        if (!comparison)
        {
            // 解釈不能な版番号は未記録扱いとして更新対象にします。
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
        // 更新と退避の実行結果
        ProjectMigrationResult result;
        try
        {
            if (!std::filesystem::is_directory(projectRoot)
                || !std::filesystem::is_directory(
                    engineAssetRoot))
            {
                return result;
            }

            // 更新後の版を記録する設定パス
            const auto settingsPath = projectRoot
                / L".lamapon" / L"project.json";
            result.previousEngineVersion =
                ReadRecordedEngineVersion(settingsPath);

            // 更新する組み込み資源の相対パス
            for (const auto& relative :
                BuiltInProjectAssets())
            {
                // 現行エンジンの資源ファイル
                const auto source =
                    engineAssetRoot / relative;
                if (!std::filesystem::is_regular_file(
                    source))
                {
                    continue;
                }
                // プロジェクト内の資源の更新先
                const auto destination = projectRoot
                    / L"assets" / relative;

                // 現行エンジンから読んだ内容
                const auto latest = ReadFileText(source);
                if (latest.empty())
                {
                    continue;
                }
                if (std::filesystem::is_regular_file(
                    destination))
                {
                    // プロジェクトに保存済みの内容
                    const auto existing =
                        ReadFileText(destination);
                    if (existing == latest)
                    {
                        continue;
                    }
                    // 比較用にCRを除去した内容を返します(text: ファイル内容の複製)。
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
                    // 更新前の資源を退避するパス
                    auto backup = destination;
                    backup += L".bak";
                    // 既存資源の退避エラー
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

                // 更新先フォルダーの作成エラー
                std::error_code createError;
                std::filesystem::create_directories(
                    destination.parent_path(),
                    createError);
                // 資源の更新内容を書き込むファイル
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
