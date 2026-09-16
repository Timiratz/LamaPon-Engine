#include "LamaPon/Online/OnlineHttpValidation.h"

#include <algorithm>
#include <stdexcept>

namespace
{
    std::string_view UrlAuthority(
        const std::string_view value,
        const std::string_view scheme)
    {
        if (!value.starts_with(scheme))
        {
            return {};
        }
        const auto begin = scheme.size();
        const auto end = value.find_first_of("/?#", begin);
        return value.substr(
            begin,
            end == std::string_view::npos
                ? value.size() - begin
                : end - begin);
    }

    bool IsDigits(const std::string_view value)
    {
        return !value.empty()
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return character >= '0' && character <= '9';
                });
    }

    bool IsLoopbackBaseUrl(const std::string_view value)
    {
        const auto authority = UrlAuthority(value, "http://");
        if (authority == "127.0.0.1"
            || authority == "localhost"
            || authority == "[::1]")
        {
            return true;
        }
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

    bool ContainsUnsafeUrlCharacter(const std::string_view value)
    {
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
        const bool secure = value.starts_with("https://");
        const bool allowedLoopback = allowInsecureLoopback
            && IsLoopbackBaseUrl(value);
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
