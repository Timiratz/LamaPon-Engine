#include "LamaPon/Online/OnlineHttpValidation.h"

#include <algorithm>
#include <stdexcept>

namespace
{
    // スキーム直後のホスト部を借用する(value: 検査URL, scheme: 区切りを含むスキーム)。
    std::string_view UrlAuthority(
        const std::string_view value,
        const std::string_view scheme)
    {
        if (!value.starts_with(scheme))
        {
            return {};
        }
        // スキーム直後の開始位置
        const auto begin = scheme.size();
        // ホスト部の終端位置
        const auto end = value.find_first_of("/?#", begin);
        return value.substr(
            begin,
            end == std::string_view::npos
                ? value.size() - begin
                : end - begin);
    }

    // 空でないASCII数字列かを判定する(value: 検査文字列)。
    bool IsDigits(const std::string_view value)
    {
        // 数字1文字かを判定(character: 検査するバイト)。
        return !value.empty()
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return character >= '0' && character <= '9';
                });
    }

    // 指定の3ホストのHTTPかを判定する(value: 検査URL)。
    bool IsLoopbackBaseUrl(const std::string_view value)
    {
        // スキーム直後のホスト部
        const auto authority = UrlAuthority(value, "http://");
        if (authority == "127.0.0.1"
            || authority == "localhost"
            || authority == "[::1]")
        {
            return true;
        }
        // 許可するローカルホスト名
        for (const auto host : {
                std::string_view("127.0.0.1"),
                std::string_view("localhost"),
                std::string_view("[::1]") })
        {
            if (authority.starts_with(host)
                && authority.size() > host.size()
                && authority[host.size()] == ':'
                && IsDigits(authority.substr(host.size() + 1)))
            {
                return true;
            }
        }
        return false;
    }

    // 空白・制御文字・DELの有無を調べる(value: 検査文字列)。
    bool ContainsUnsafeUrlCharacter(const std::string_view value)
    {
        // 空白・制御文字かを判定(character: 検査するバイト)。
        return std::ranges::any_of(
            value,
            [](const unsigned char character)
            {
                return character <= 0x20 || character == 0x7f;
            });
    }
}

namespace LamaPon::Detail
{
    std::string NormalizeOnlineServiceBaseUrl(
        std::string value,
        const bool allowInsecureLoopback)
    {
        while (!value.empty() && value.back() == '/')
        {
            value.pop_back();
        }
        // HTTPSスキームか
        const bool secure = value.starts_with("https://");
        // ローカルHTTPの許可状態
        const bool allowedLoopback = allowInsecureLoopback
            && IsLoopbackBaseUrl(value);
        // スキーム直後のホスト部
        const auto authority = secure
            ? UrlAuthority(value, "https://")
            : UrlAuthority(value, "http://");
        if ((!secure && !allowedLoopback)
            || authority.empty()
            || authority.find('@') != std::string_view::npos
            || value.size() > 2048
            || value.find_first_of("?#") != std::string::npos
            || ContainsUnsafeUrlCharacter(value))
        {
            throw std::invalid_argument(
                "Online service URL must be an HTTPS base URL. "
                "Only an explicitly enabled loopback URL may use HTTP.");
        }
        return value;
    }

    bool IsSafeOnlineOpaqueValue(
        const std::string_view value,
        const std::size_t maxBytes)
    {
        return !value.empty()
            && value.size() <= maxBytes
            && !ContainsUnsafeUrlCharacter(value);
    }

    bool IsSafeOnlineBearerToken(
        const std::string_view value,
        const std::size_t maxBytes)
    {
        // visible ASCIIかを判定(character: 検査するバイト)。
        return !value.empty()
            && value.size() <= maxBytes
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return character >= 0x21 && character <= 0x7e;
                });
    }

    bool IsSafeOnlineNamespaceId(
        const std::string_view value,
        const std::size_t maxBytes)
    {
        // 識別子用ASCIIかを判定(character: 検査するバイト)。
        return !value.empty()
            && value.size() <= maxBytes
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return (character >= 'A' && character <= 'Z')
                        || (character >= 'a' && character <= 'z')
                        || (character >= '0' && character <= '9')
                        || character == '.'
                        || character == '_'
                        || character == '-';
                });
    }

    bool IsSafeOnlineBrowserUrl(
        const std::string_view value,
        const bool allowInsecureLoopback)
    {
        return !value.empty()
            && value.size() <= 2048
            && !ContainsUnsafeUrlCharacter(value)
            && ((!UrlAuthority(value, "https://").empty()
                    && UrlAuthority(value, "https://").find('@')
                        == std::string_view::npos)
                || (allowInsecureLoopback
                    && IsLoopbackBaseUrl(value)));
    }
}
