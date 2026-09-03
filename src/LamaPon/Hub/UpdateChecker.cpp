#include "LamaPon/Hub/UpdateChecker.h"

#include "LamaPon/Core/HttpClient.h"
#include "LamaPon/Core/Version.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <utility>

namespace
{
    // エンジンと同じリポジトリのリリースを確認します。
    // 非公開または未リリースの間は通知せず、公開後に利用できます。
    constexpr wchar_t ApiHost[] = L"api.github.com";
    constexpr wchar_t LatestReleasePath[] =
        L"/repos/Timiratz/LamaPon-Engine/releases/latest";
    constexpr char FallbackReleasesUrl[] =
        "https://github.com/Timiratz/LamaPon-Engine/releases";
}

namespace LamaPon::Hub
{
    UpdateCheckResult ParseLatestRelease(
        const std::string_view responseJson,
        const std::string_view currentVersion)
    {
        UpdateCheckResult result;
        try
        {
            const auto document = nlohmann::json::parse(
                responseJson);
            std::string tag = document.value(
                "tag_name",
                std::string{});
            if (!tag.empty()
                && (tag.front() == 'v' || tag.front() == 'V'))
            {
                tag.erase(tag.begin());
            }
            if (tag.empty()
                || !IsNewerVersion(currentVersion, tag))
            {
                return result;
            }
            result.updateAvailable = true;
            result.latestVersion = std::move(tag);
            result.releaseUrl = document.value(
                "html_url",
                std::string{ FallbackReleasesUrl });
            // 予期しないURLはリリース一覧ページへ置き換えます。
            if (result.releaseUrl.rfind(
                    "https://github.com/", 0) != 0)
            {
                result.releaseUrl = FallbackReleasesUrl;
            }
        }
        catch (const std::exception&)
        {
            result = {};
        }
        return result;
    }

    UpdateCheckResult CheckForEngineUpdate()
    {
        const std::string response = HttpGetText(
            ApiHost,
            LatestReleasePath,
            1024u * 1024u,
            L"Accept: application/vnd.github+json\r\n"
            L"X-GitHub-Api-Version: 2022-11-28\r\n");
        if (response.empty())
        {
            return {};
        }
        return ParseLatestRelease(
            response,
            LamaPon::VersionString);
    }
}
