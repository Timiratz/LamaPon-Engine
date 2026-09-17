#include "LamaPon/Online/CloudSaveClient.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/SaveSlotValidation.h"
#include "LamaPon/Online/OnlineHttpValidation.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using LamaPon::CloudSaveManifestItem;
    using LamaPon::CloudSaveResource;
    using LamaPon::CloudSaveResourceKind;
    using LamaPon::CloudSaveSnapshot;
    using LamaPon::Detail::CloudSaveItemResult;
    using LamaPon::Detail::CloudSaveManifestResult;
    using LamaPon::Detail::CloudSaveWireOutcome;
    using LamaPon::Detail::CloudSaveWireStatus;

    constexpr std::size_t MaximumManifestResponseBytes =
        128u * 1024u;
    constexpr std::size_t MaximumItemResponseBytes =
        1536u * 1024u;
    constexpr std::size_t MaximumJsonDepth = 64u;
    constexpr std::size_t MaximumJsonElements = 65536u;
    constexpr std::uint32_t MaximumRetryAfterSeconds = 300u;
    constexpr std::string_view ConflictCode = "revision_conflict";

    struct AlgorithmHandle final
    {
        ~AlgorithmHandle()
        {
            if (value != nullptr)
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        }

        BCRYPT_ALG_HANDLE value{};
    };

    bool HeaderNameEquals(
        const std::wstring_view left,
        const std::wstring_view right) noexcept
    {
        return left.size() == right.size()
            && std::ranges::equal(
                left,
                right,
                [](const wchar_t a, const wchar_t b)
                {
                    const auto fold = [](const wchar_t value)
                    {
                        return value >= L'A' && value <= L'Z'
                            ? value - L'A' + L'a'
                            : value;
                    };
                    return fold(a) == fold(b);
                });
    }

    std::vector<std::wstring_view> HeaderValues(
        const LamaPon::HttpResponse& response,
        const std::wstring_view name)
    {
        std::vector<std::wstring_view> values;
        for (const auto& [headerName, value] : response.headers)
        {
            if (HeaderNameEquals(headerName, name))
            {
                values.emplace_back(value);
            }
        }
        return values;
    }

    std::wstring_view TrimOws(std::wstring_view value) noexcept
    {
        while (!value.empty()
            && (value.front() == L' ' || value.front() == L'\t'))
        {
            value.remove_prefix(1);
        }
        while (!value.empty()
            && (value.back() == L' ' || value.back() == L'\t'))
        {
            value.remove_suffix(1);
        }
        return value;
    }

    std::wstring AsciiLower(std::wstring_view value)
    {
        std::wstring result(value);
        std::ranges::transform(
            result,
            result.begin(),
            [](const wchar_t character)
            {
                return character >= L'A' && character <= L'Z'
                    ? character - L'A' + L'a'
                    : character;
            });
        return result;
    }

    bool HasValidJsonContentType(
        const LamaPon::HttpResponse& response)
    {
        const auto values = HeaderValues(response, L"Content-Type");
        if (values.size() != 1)
        {
            return false;
        }
        const auto lower = AsciiLower(TrimOws(values.front()));
        const std::wstring_view lowerView(lower);
        const auto semicolon = lowerView.find(L';');
        const auto mediaType = TrimOws(lowerView.substr(0, semicolon));
        if (mediaType != L"application/json")
        {
            return false;
        }
        if (semicolon == std::wstring::npos)
        {
            return true;
        }
        const auto parameter = TrimOws(lowerView.substr(semicolon + 1));
        if (parameter.empty()
            || parameter.find(L';') != std::wstring::npos)
        {
            return false;
        }
        const auto equals = parameter.find(L'=');
        return equals != std::wstring::npos
            && TrimOws(parameter.substr(0, equals)) == L"charset"
            && TrimOws(parameter.substr(equals + 1)) == L"utf-8";
    }

    bool HasValidContentLength(
        const LamaPon::HttpResponse& response)
    {
        const auto values = HeaderValues(response, L"Content-Length");
        if (values.empty())
        {
            return true;
        }
        if (values.size() != 1)
        {
            return false;
        }
        const auto text = TrimOws(values.front());
        if (text.empty()
            || !std::ranges::all_of(
                text,
                [](const wchar_t character)
                {
                    return character >= L'0' && character <= L'9';
                }))
        {
            return false;
        }
        std::size_t value{};
        for (const auto character : text)
        {
            const auto digit = static_cast<std::size_t>(character - L'0');
            if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u)
            {
                return false;
            }
            value = value * 10u + digit;
        }
        return value == response.body.size();
    }

    bool ResponseEnvelopeIsValid(
        const LamaPon::HttpResponse& response,
        const std::size_t maximumBytes,
        const bool requireJsonContentType)
    {
        return response.body.size() <= maximumBytes
            && HasValidContentLength(response)
            && (!requireJsonContentType
                || HasValidJsonContentType(response));
    }

    bool IsStrongEtag(const std::string_view value) noexcept
    {
        if (value.size() < 2
            || value.size() > LamaPon::CloudSaveEtagMaxBytes
            || value.front() != '"'
            || value.back() != '"'
            || value == "\"\""
            || value.starts_with("W/")
            || value == "*")
        {
            return false;
        }
        return std::ranges::all_of(
            value.substr(1, value.size() - 2),
            [](const unsigned char character)
            {
                return character >= 0x21
                    && character <= 0x7e
                    && character != '"'
                    && character != '\\'
                    && character != ',';
            });
    }

    std::optional<std::string> SingleStrongEtag(
        const LamaPon::HttpResponse& response)
    {
        const auto values = HeaderValues(response, L"ETag");
        if (values.size() != 1)
        {
            return std::nullopt;
        }
        const auto trimmed = TrimOws(values.front());
        if (!std::ranges::all_of(
                trimmed,
                [](const wchar_t character)
                {
                    return character >= 0x21 && character <= 0x7e;
                }))
        {
            return std::nullopt;
        }
        std::string result;
        result.reserve(trimmed.size());
        for (const auto character : trimmed)
        {
            result.push_back(static_cast<char>(character));
        }
        return IsStrongEtag(result)
            ? std::optional<std::string>(std::move(result))
            : std::nullopt;
    }

    std::uint32_t RetryAfterSeconds(
        const LamaPon::HttpResponse& response) noexcept
    {
        const auto values = HeaderValues(response, L"Retry-After");
        if (values.size() != 1)
        {
            return 1u;
        }
        const auto text = TrimOws(values.front());
        if (text.empty()
            || !std::ranges::all_of(
                text,
                [](const wchar_t character)
                {
                    return character >= L'0' && character <= L'9';
                }))
        {
            return 1u;
        }
        std::uint32_t value{};
        for (const auto character : text)
        {
            const auto digit = static_cast<std::uint32_t>(character - L'0');
            if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10u)
            {
                return 1u;
            }
            value = value * 10u + digit;
        }
        return std::clamp(value, 1u, MaximumRetryAfterSeconds);
    }

    bool JsonNestingIsSafe(const std::string_view text) noexcept
    {
        std::size_t depth{};
        bool inString{};
        bool escaped{};
        for (const unsigned char character : text)
        {
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if (character == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (character == '"')
            {
                inString = true;
            }
            else if (character == '{' || character == '[')
            {
                ++depth;
                if (depth > MaximumJsonDepth)
                {
                    return false;
                }
            }
            else if (character == '}' || character == ']')
            {
                if (depth == 0)
                {
                    return false;
                }
                --depth;
            }
        }
        return depth == 0 && !inString && !escaped;
    }

    bool JsonElementCountIsSafe(
        const Json& value,
        std::size_t& remaining) noexcept
    {
        if (remaining == 0)
        {
            return false;
        }
        --remaining;
        if (value.is_array() || value.is_object())
        {
            for (const auto& child : value)
            {
                if (!JsonElementCountIsSafe(child, remaining))
                {
                    return false;
                }
            }
        }
        return true;
    }

    Json ParseJsonStrict(const std::string_view text)
    {
        if (text.empty()
            || LamaPon::Utf8ToWide(text).empty()
            || !JsonNestingIsSafe(text))
        {
            throw std::runtime_error("JSON envelope is invalid.");
        }

        bool duplicateKey{};
        std::array<std::unordered_set<std::string>, MaximumJsonDepth + 1>
            keysByDepth;
        const auto callback =
            [&duplicateKey, &keysByDepth](
                const int depth,
                const Json::parse_event_t event,
                Json& parsed)
            {
                if (depth < 0
                    || static_cast<std::size_t>(depth) >= keysByDepth.size())
                {
                    duplicateKey = true;
                    return true;
                }
                if (event == Json::parse_event_t::object_start)
                {
                    const auto keyDepth = static_cast<std::size_t>(depth) + 1u;
                    if (keyDepth >= keysByDepth.size())
                    {
                        duplicateKey = true;
                    }
                    else
                    {
                        keysByDepth[keyDepth].clear();
                    }
                }
                else if (event == Json::parse_event_t::key)
                {
                    auto& keys =
                        keysByDepth[static_cast<std::size_t>(depth)];
                    const auto key = parsed.get<std::string>();
                    if (!keys.insert(key).second)
                    {
                        duplicateKey = true;
                    }
                }
                return true;
            };
        auto parsed = Json::parse(text, callback, true, false);
        std::size_t remainingElements = MaximumJsonElements;
        if (duplicateKey
            || !JsonElementCountIsSafe(parsed, remainingElements))
        {
            throw std::runtime_error(
                "JSON contains duplicate keys or too many elements.");
        }
        return parsed;
    }

    bool HasExactKeys(
        const Json& object,
        const std::initializer_list<std::string_view> expected)
    {
        if (!object.is_object() || object.size() != expected.size())
        {
            return false;
        }
        return std::ranges::all_of(
            expected,
            [&object](const std::string_view key)
            {
                return object.contains(std::string(key));
            });
    }

    Json ResourceJson(const CloudSaveResource& resource)
    {
        if (resource.kind == CloudSaveResourceKind::Preferences
            && resource.slot.empty())
        {
            return Json{ { "kind", "preferences" } };
        }
        if (resource.kind == CloudSaveResourceKind::SaveSlot
            && LamaPon::Detail::IsValidSaveSlotName(resource.slot))
        {
            return Json{
                { "kind", "save_slot" },
                { "slot", resource.slot }
            };
        }
        throw std::invalid_argument("Cloud save resource is invalid.");
    }

    CloudSaveResource ParseResource(const Json& json)
    {
        if (!json.is_object())
        {
            throw std::runtime_error("Cloud save resource is invalid.");
        }
        if (HasExactKeys(json, { "kind" })
            && json.at("kind").is_string()
            && json.at("kind").get<std::string>() == "preferences")
        {
            return CloudSaveResource::Preferences();
        }
        if (HasExactKeys(json, { "kind", "slot" })
            && json.at("kind").is_string()
            && json.at("kind").get<std::string>() == "save_slot"
            && json.at("slot").is_string())
        {
            auto slot = json.at("slot").get<std::string>();
            if (LamaPon::Detail::IsValidSaveSlotName(slot))
            {
                return CloudSaveResource::SaveSlot(std::move(slot));
            }
        }
        throw std::runtime_error("Cloud save resource is invalid.");
    }

    bool EquivalentResources(
        const CloudSaveResource& left,
        const CloudSaveResource& right) noexcept
    {
        if (left.kind != right.kind)
        {
            return false;
        }
        if (left.kind == CloudSaveResourceKind::Preferences)
        {
            return left.slot.empty() && right.slot.empty();
        }
        return LamaPon::Detail::EquivalentSaveSlotNames(
            left.slot,
            right.slot);
    }

    std::size_t MaximumContentBytes(
        const CloudSaveResource& resource)
    {
        switch (resource.kind)
        {
        case CloudSaveResourceKind::Preferences:
            if (!resource.slot.empty())
            {
                break;
            }
            return LamaPon::CloudPreferencesMaxBytes;
        case CloudSaveResourceKind::SaveSlot:
            if (LamaPon::Detail::IsValidSaveSlotName(resource.slot))
            {
                return LamaPon::CloudSaveSlotMaxBytes;
            }
            break;
        }
        throw std::invalid_argument("Cloud save resource is invalid.");
    }

    bool IsCanonicalMutationId(const std::string_view value) noexcept
    {
        if (value.size() != 36)
        {
            return false;
        }
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            if (index == 8 || index == 13 || index == 18 || index == 23)
            {
                if (value[index] != '-')
                {
                    return false;
                }
                continue;
            }
            const auto character = static_cast<unsigned char>(value[index]);
            if (!((character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f')))
            {
                return false;
            }
        }
        return value[14] == '4'
            && (value[19] == '8'
                || value[19] == '9'
                || value[19] == 'a'
                || value[19] == 'b');
    }

    std::array<std::uint8_t, 32> Sha256(
        const std::uint8_t* data,
        const std::size_t size)
    {
        if (size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument("Cloud save content is too large.");
        }
        AlgorithmHandle algorithm;
        if (BCryptOpenAlgorithmProvider(
                &algorithm.value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0)
        {
            throw std::runtime_error("SHA-256 is unavailable.");
        }
        std::array<std::uint8_t, 32> digest{};
        if (BCryptHash(
                algorithm.value,
                nullptr,
                0,
                const_cast<PUCHAR>(data),
                static_cast<ULONG>(size),
                digest.data(),
                static_cast<ULONG>(digest.size())) < 0)
        {
            throw std::runtime_error("SHA-256 failed.");
        }
        return digest;
    }

    constexpr char Base64UrlAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string EncodeBase64Url(
        const std::uint8_t* data,
        const std::size_t size)
    {
        std::string result;
        result.reserve((size * 4u + 2u) / 3u);
        std::size_t index{};
        while (index + 3u <= size)
        {
            const auto value =
                (static_cast<std::uint32_t>(data[index]) << 16u)
                | (static_cast<std::uint32_t>(data[index + 1]) << 8u)
                | static_cast<std::uint32_t>(data[index + 2]);
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[value & 0x3fu]);
            index += 3u;
        }
        const auto remaining = size - index;
        if (remaining == 1u)
        {
            const auto value = static_cast<std::uint32_t>(data[index]) << 16u;
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
        }
        else if (remaining == 2u)
        {
            const auto value =
                (static_cast<std::uint32_t>(data[index]) << 16u)
                | (static_cast<std::uint32_t>(data[index + 1]) << 8u);
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
        }
        return result;
    }

    int Base64UrlValue(const unsigned char character) noexcept
    {
        if (character >= 'A' && character <= 'Z')
        {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z')
        {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9')
        {
            return character - '0' + 52;
        }
        if (character == '-')
        {
            return 62;
        }
        if (character == '_')
        {
            return 63;
        }
        return -1;
    }

    bool DecodeBase64Url(
        const std::string_view encoded,
        const std::size_t maximumBytes,
        std::vector<std::uint8_t>& decoded)
    {
        decoded.clear();
        if (encoded.size() % 4u == 1u
            || encoded.size() > ((maximumBytes * 4u + 2u) / 3u))
        {
            return false;
        }
        for (const unsigned char character : encoded)
        {
            if (Base64UrlValue(character) < 0)
            {
                return false;
            }
        }
        if (encoded.size() % 4u == 2u
            && (Base64UrlValue(
                    static_cast<unsigned char>(encoded.back())) & 0x0f) != 0)
        {
            return false;
        }
        if (encoded.size() % 4u == 3u
            && (Base64UrlValue(
                    static_cast<unsigned char>(encoded.back())) & 0x03) != 0)
        {
            return false;
        }

        const auto decodedSize = encoded.size() / 4u * 3u
            + (encoded.size() % 4u == 2u ? 1u : 0u)
            + (encoded.size() % 4u == 3u ? 2u : 0u);
        if (decodedSize > maximumBytes)
        {
            return false;
        }
        decoded.reserve(decodedSize);
        std::size_t index{};
        while (index + 4u <= encoded.size())
        {
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2])) << 6u)
                | static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 3]));
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
            decoded.push_back(static_cast<std::uint8_t>(value >> 8u));
            decoded.push_back(static_cast<std::uint8_t>(value));
            index += 4u;
        }
        const auto remaining = encoded.size() - index;
        if (remaining == 2u)
        {
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1])) << 12u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
        }
        else if (remaining == 3u)
        {
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2])) << 6u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
            decoded.push_back(static_cast<std::uint8_t>(value >> 8u));
        }
        return decoded.size() == decodedSize;
    }

    std::string ContentHash(const std::vector<std::uint8_t>& content)
    {
        const auto digest = Sha256(content.data(), content.size());
        return EncodeBase64Url(digest.data(), digest.size());
    }

    bool IsCanonicalSha256(const std::string_view value)
    {
        std::vector<std::uint8_t> decoded;
        return value.size() == 43u
            && DecodeBase64Url(value, 32u, decoded)
            && decoded.size() == 32u;
    }

    bool IsUnsigned(const Json& value) noexcept
    {
        return value.is_number_unsigned();
    }

    CloudSaveManifestItem ParseManifestItem(const Json& json)
    {
        if (!json.is_object()
            || !json.contains("deleted")
            || !json.at("deleted").is_boolean())
        {
            throw std::runtime_error("Cloud manifest item is invalid.");
        }
        const bool deleted = json.at("deleted").get<bool>();
        if (!(deleted
                ? HasExactKeys(
                    json,
                    { "resource", "etag", "deleted", "byteLength" })
                : HasExactKeys(
                    json,
                    { "resource", "etag", "deleted", "byteLength", "sha256" })))
        {
            throw std::runtime_error("Cloud manifest item has an invalid schema.");
        }
        if (!json.at("etag").is_string()
            || !IsUnsigned(json.at("byteLength")))
        {
            throw std::runtime_error("Cloud manifest metadata is invalid.");
        }
        CloudSaveManifestItem item;
        item.resource = ParseResource(json.at("resource"));
        item.etag = json.at("etag").get<std::string>();
        item.deleted = deleted;
        item.byteLength = json.at("byteLength").get<std::uint64_t>();
        if (!IsStrongEtag(item.etag)
            || item.byteLength
                > static_cast<std::uint64_t>(MaximumContentBytes(item.resource)))
        {
            throw std::runtime_error("Cloud manifest metadata is outside limits.");
        }
        if (deleted)
        {
            if (item.byteLength != 0)
            {
                throw std::runtime_error("Cloud tombstone has content metadata.");
            }
        }
        else
        {
            if (item.byteLength == 0
                || !json.at("sha256").is_string())
            {
                throw std::runtime_error("Cloud manifest hash is invalid.");
            }
            item.sha256 = json.at("sha256").get<std::string>();
            if (!IsCanonicalSha256(item.sha256))
            {
                throw std::runtime_error("Cloud manifest hash is invalid.");
            }
        }
        return item;
    }

    CloudSaveSnapshot ParseSnapshot(
        const Json& json,
        const bool hasProtocolVersion)
    {
        if (!json.is_object()
            || !json.contains("deleted")
            || !json.at("deleted").is_boolean())
        {
            throw std::runtime_error("Cloud snapshot is invalid.");
        }
        if (hasProtocolVersion
            && (!json.contains("protocolVersion")
                || !IsUnsigned(json.at("protocolVersion"))
                || json.at("protocolVersion").get<std::uint64_t>() != 1u))
        {
            throw std::runtime_error("Cloud protocol version is invalid.");
        }
        const bool deleted = json.at("deleted").get<bool>();
        const auto matchesSchema = deleted
            ? (hasProtocolVersion
                ? HasExactKeys(
                    json,
                    { "protocolVersion", "resource", "etag", "deleted", "byteLength" })
                : HasExactKeys(
                    json,
                    { "resource", "etag", "deleted", "byteLength" }))
            : (hasProtocolVersion
                ? HasExactKeys(
                    json,
                    { "protocolVersion", "resource", "etag", "deleted", "byteLength", "sha256", "content" })
                : HasExactKeys(
                    json,
                    { "resource", "etag", "deleted", "byteLength", "sha256", "content" }));
        if (!matchesSchema
            || !json.at("etag").is_string()
            || !IsUnsigned(json.at("byteLength")))
        {
            throw std::runtime_error("Cloud snapshot schema is invalid.");
        }

        CloudSaveSnapshot snapshot;
        snapshot.resource = ParseResource(json.at("resource"));
        snapshot.etag = json.at("etag").get<std::string>();
        snapshot.deleted = deleted;
        const auto byteLength = json.at("byteLength").get<std::uint64_t>();
        if (!IsStrongEtag(snapshot.etag)
            || byteLength
                > static_cast<std::uint64_t>(MaximumContentBytes(snapshot.resource)))
        {
            throw std::runtime_error("Cloud snapshot metadata is outside limits.");
        }
        if (deleted)
        {
            if (byteLength != 0)
            {
                throw std::runtime_error("Cloud tombstone has content.");
            }
            return snapshot;
        }
        if (!json.at("sha256").is_string()
            || !json.at("content").is_string())
        {
            throw std::runtime_error("Cloud snapshot payload is invalid.");
        }
        snapshot.sha256 = json.at("sha256").get<std::string>();
        const auto encoded = json.at("content").get<std::string>();
        if (!IsCanonicalSha256(snapshot.sha256)
            || !DecodeBase64Url(
                encoded,
                MaximumContentBytes(snapshot.resource),
                snapshot.content)
            || snapshot.content.size() != byteLength
            || ContentHash(snapshot.content) != snapshot.sha256)
        {
            throw std::runtime_error("Cloud snapshot integrity check failed.");
        }
        const std::string contentText(
            snapshot.content.begin(),
            snapshot.content.end());
        static_cast<void>(ParseJsonStrict(contentText));
        return snapshot;
    }

    CloudSaveWireOutcome Outcome(
        const CloudSaveWireStatus status,
        std::string message,
        std::string code = {},
        const std::uint32_t retryAfter = 0)
    {
        return {
            status,
            retryAfter,
            std::move(code),
            std::move(message)
        };
    }

    CloudSaveWireOutcome StatusFailure(
        const LamaPon::HttpResponse& response,
        const bool readOperation)
    {
        if (!response.TransportSucceeded())
        {
            return Outcome(
                CloudSaveWireStatus::TransportError,
                "Cloud save transport failed.");
        }
        if (response.statusCode == 401)
        {
            return Outcome(
                CloudSaveWireStatus::Unauthorized,
                "Cloud save authorization expired.",
                "unauthorized");
        }
        if (readOperation && response.statusCode == 404)
        {
            return Outcome(
                CloudSaveWireStatus::NotFound,
                "Cloud save item was not found.",
                "not_found");
        }
        if (response.statusCode == 429)
        {
            return Outcome(
                CloudSaveWireStatus::RateLimited,
                "Cloud save service is rate limited.",
                "rate_limited",
                RetryAfterSeconds(response));
        }
        if (response.statusCode == 408
            || response.statusCode == 425
            || (response.statusCode >= 500
                && response.statusCode < 600))
        {
            return Outcome(
                CloudSaveWireStatus::RetryableServiceError,
                "Cloud save service is temporarily unavailable.",
                "temporarily_unavailable");
        }
        if (response.statusCode < 200
            || response.statusCode >= 600
            || (response.statusCode >= 200 && response.statusCode < 300)
            || (response.statusCode >= 300 && response.statusCode < 400)
            || response.statusCode == 0)
        {
            return Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an unexpected response.");
        }
        return Outcome(
            CloudSaveWireStatus::Rejected,
            "Cloud save request was rejected.",
            "rejected");
    }

    CloudSaveItemResult ParseConflict(
        const LamaPon::HttpResponse& response,
        const CloudSaveResource& requested,
        const std::size_t maximumBytes)
    {
        CloudSaveItemResult result;
        try
        {
            if (!ResponseEnvelopeIsValid(response, maximumBytes, true))
            {
                throw std::runtime_error("Cloud conflict envelope is invalid.");
            }
            const auto json = ParseJsonStrict(response.Text());
            if (!HasExactKeys(
                    json,
                    { "protocolVersion", "error", "current" })
                || !IsUnsigned(json.at("protocolVersion"))
                || json.at("protocolVersion").get<std::uint64_t>() != 1u
                || !HasExactKeys(json.at("error"), { "code" })
                || !json.at("error").at("code").is_string()
                || json.at("error").at("code").get<std::string>()
                    != ConflictCode)
            {
                throw std::runtime_error("Cloud conflict schema is invalid.");
            }
            auto current = ParseSnapshot(json.at("current"), false);
            if (!EquivalentResources(current.resource, requested))
            {
                throw std::runtime_error("Cloud conflict resource mismatched.");
            }
            const auto headerEtag = SingleStrongEtag(response);
            if (!headerEtag || *headerEtag != current.etag)
            {
                throw std::runtime_error("Cloud conflict ETag mismatched.");
            }
            result.outcome = Outcome(
                CloudSaveWireStatus::Conflict,
                "Cloud save revision conflicted.",
                std::string(ConflictCode));
            result.conflictSnapshot = std::move(current);
        }
        catch (...)
        {
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid conflict.");
        }
        return result;
    }

    CloudSaveWireOutcome InvalidRequest(std::string message)
    {
        return Outcome(
            CloudSaveWireStatus::InvalidRequest,
            std::move(message));
    }
}

namespace LamaPon::Detail
{
    CloudSaveClient::CloudSaveClient(
        std::string serviceBaseUrl,
        std::string gameId,
        std::string environmentId,
        const bool allowInsecureLoopback,
        CloudSaveHttpSender sender)
        : m_serviceBaseUrl(NormalizeOnlineServiceBaseUrl(
            std::move(serviceBaseUrl),
            allowInsecureLoopback))
        , m_gameId(std::move(gameId))
        , m_environmentId(std::move(environmentId))
        , m_allowInsecureLoopback(allowInsecureLoopback)
        , m_sender(sender ? std::move(sender) : CloudSaveHttpSender(HttpSend))
    {
        if (!IsSafeOnlineNamespaceId(m_gameId, 128)
            || !IsSafeOnlineNamespaceId(m_environmentId, 64))
        {
            throw std::invalid_argument(
                "Cloud save game and environment IDs are invalid.");
        }
    }

    HttpResponse CloudSaveClient::Send(
        std::wstring method,
        const std::string_view path,
        const std::string_view accessToken,
        const std::string_view body,
        std::vector<std::pair<std::wstring, std::wstring>> headers,
        const std::size_t maxResponseBytes) const
    {
        HttpRequest request;
        request.url = Utf8ToWide(
            m_serviceBaseUrl + std::string(path));
        request.method = std::move(method);
        request.headers.emplace_back(L"Accept", L"application/json");
        request.headers.emplace_back(L"Cache-Control", L"no-store");
        request.headers.emplace_back(
            L"Authorization",
            L"Bearer " + Utf8ToWide(accessToken));
        request.headers.emplace_back(
            L"X-LamaPon-Game-Id",
            Utf8ToWide(m_gameId));
        request.headers.emplace_back(
            L"X-LamaPon-Environment-Id",
            Utf8ToWide(m_environmentId));
        if (!body.empty())
        {
            request.headers.emplace_back(
                L"Content-Type",
                L"application/json; charset=utf-8");
            request.body.assign(body.begin(), body.end());
        }
        request.headers.insert(
            request.headers.end(),
            std::make_move_iterator(headers.begin()),
            std::make_move_iterator(headers.end()));
        request.maxResponseBytes = maxResponseBytes;
        request.followRedirects = false;
        request.allowInsecureLoopback = m_allowInsecureLoopback;
        try
        {
            return m_sender(request);
        }
        catch (...)
        {
            HttpResponse failure;
            failure.transportError = "Cloud save sender failed.";
            return failure;
        }
    }

    CloudSaveManifestResult CloudSaveClient::FetchManifest(
        const std::string_view accessToken) const
    {
        CloudSaveManifestResult result;
        if (!IsSafeOnlineBearerToken(accessToken))
        {
            result.outcome = InvalidRequest(
                "Cloud save access token is invalid.");
            return result;
        }
        const auto response = Send(
            L"GET",
            "/v1/cloud-saves/manifest",
            accessToken,
            {},
            {},
            MaximumManifestResponseBytes);
        if (!response.TransportSucceeded())
        {
            result.outcome = StatusFailure(response, false);
            return result;
        }
        if (!ResponseEnvelopeIsValid(
                response,
                MaximumManifestResponseBytes,
                false))
        {
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid response envelope.");
            return result;
        }
        if (response.statusCode != 200)
        {
            result.outcome = StatusFailure(response, false);
            return result;
        }
        try
        {
            if (!ResponseEnvelopeIsValid(
                    response,
                    MaximumManifestResponseBytes,
                    true))
            {
                throw std::runtime_error("Cloud manifest envelope is invalid.");
            }
            const auto json = ParseJsonStrict(response.Text());
            if (!HasExactKeys(json, { "protocolVersion", "items" })
                || !IsUnsigned(json.at("protocolVersion"))
                || json.at("protocolVersion").get<std::uint64_t>() != 1u
                || !json.at("items").is_array()
                || json.at("items").size()
                    > CloudSaveMaxSlots + 1u)
            {
                throw std::runtime_error("Cloud manifest schema is invalid.");
            }
            std::uint64_t totalBytes{};
            std::size_t saveSlotCount{};
            for (const auto& entry : json.at("items"))
            {
                auto item = ParseManifestItem(entry);
                if (item.resource.kind == CloudSaveResourceKind::SaveSlot)
                {
                    ++saveSlotCount;
                    if (saveSlotCount > CloudSaveMaxSlots)
                    {
                        throw std::runtime_error("Cloud manifest has too many slots.");
                    }
                }
                for (const auto& existing : result.items)
                {
                    if (EquivalentResources(
                            existing.resource,
                            item.resource))
                    {
                        throw std::runtime_error("Cloud manifest has duplicate resources.");
                    }
                }
                if (item.byteLength > CloudSaveAccountMaxBytes
                    || totalBytes
                        > CloudSaveAccountMaxBytes - item.byteLength)
                {
                    throw std::runtime_error("Cloud manifest exceeds account limit.");
                }
                totalBytes += item.byteLength;
                result.items.push_back(std::move(item));
            }
            result.outcome = Outcome(
                CloudSaveWireStatus::Succeeded,
                {});
        }
        catch (...)
        {
            result.items.clear();
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid manifest.");
        }
        return result;
    }

    CloudSaveItemResult CloudSaveClient::Read(
        const std::string_view accessToken,
        const CloudSaveResource& resource) const
    {
        CloudSaveItemResult result;
        Json resourceBody;
        try
        {
            resourceBody = ResourceJson(resource);
        }
        catch (...)
        {
            result.outcome = InvalidRequest(
                "Cloud save resource is invalid.");
            return result;
        }
        if (!IsSafeOnlineBearerToken(accessToken))
        {
            result.outcome = InvalidRequest(
                "Cloud save access token is invalid.");
            return result;
        }
        const Json requestBody{
            { "protocolVersion", 1 },
            { "resource", std::move(resourceBody) }
        };
        const auto response = Send(
            L"POST",
            "/v1/cloud-saves/read",
            accessToken,
            requestBody.dump(),
            {},
            MaximumItemResponseBytes);
        if (!response.TransportSucceeded())
        {
            result.outcome = StatusFailure(response, true);
            return result;
        }
        if (!ResponseEnvelopeIsValid(
                response,
                MaximumItemResponseBytes,
                false))
        {
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid response envelope.");
            return result;
        }
        if (response.statusCode != 200)
        {
            result.outcome = StatusFailure(response, true);
            return result;
        }
        try
        {
            if (!ResponseEnvelopeIsValid(
                    response,
                    MaximumItemResponseBytes,
                    true))
            {
                throw std::runtime_error("Cloud snapshot envelope is invalid.");
            }
            const auto headerEtag = SingleStrongEtag(response);
            auto snapshot = ParseSnapshot(
                ParseJsonStrict(response.Text()),
                true);
            if (!headerEtag
                || *headerEtag != snapshot.etag
                || !EquivalentResources(snapshot.resource, resource))
            {
                throw std::runtime_error("Cloud snapshot identity mismatched.");
            }
            result.snapshot = std::move(snapshot);
            result.outcome = Outcome(
                CloudSaveWireStatus::Succeeded,
                {});
        }
        catch (...)
        {
            result.snapshot.reset();
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid snapshot.");
        }
        return result;
    }

    CloudSaveItemResult CloudSaveClient::Put(
        const std::string_view accessToken,
        const CloudSaveResource& resource,
        const std::vector<std::uint8_t>& content,
        const std::string_view mutationId,
        std::optional<std::string> baseEtag) const
    {
        CloudSaveItemResult result;
        Json resourceBody;
        try
        {
            resourceBody = ResourceJson(resource);
            if (content.empty()
                || content.size() > MaximumContentBytes(resource))
            {
                throw std::invalid_argument("Cloud save content is outside limits.");
            }
            const std::string contentText(content.begin(), content.end());
            static_cast<void>(ParseJsonStrict(contentText));
        }
        catch (...)
        {
            result.outcome = InvalidRequest(
                "Cloud save resource or content is invalid.");
            return result;
        }
        if (!IsSafeOnlineBearerToken(accessToken)
            || !IsCanonicalMutationId(mutationId)
            || (baseEtag && !IsStrongEtag(*baseEtag)))
        {
            result.outcome = InvalidRequest(
                "Cloud save authorization or mutation metadata is invalid.");
            return result;
        }
        const auto hash = ContentHash(content);
        const Json requestBody{
            { "protocolVersion", 1 },
            { "resource", std::move(resourceBody) },
            { "byteLength", content.size() },
            { "sha256", hash },
            { "content", EncodeBase64Url(content.data(), content.size()) }
        };
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        headers.emplace_back(
            L"Idempotency-Key",
            Utf8ToWide(mutationId));
        if (baseEtag)
        {
            headers.emplace_back(
                L"If-Match",
                Utf8ToWide(*baseEtag));
        }
        else
        {
            headers.emplace_back(L"If-None-Match", L"*");
        }
        const auto response = Send(
            L"PUT",
            "/v1/cloud-saves/item",
            accessToken,
            requestBody.dump(),
            std::move(headers),
            MaximumItemResponseBytes);
        if (!response.TransportSucceeded())
        {
            result.outcome = StatusFailure(response, false);
            return result;
        }
        if (!ResponseEnvelopeIsValid(
                response,
                MaximumItemResponseBytes,
                false))
        {
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid response envelope.");
            return result;
        }
        if (response.TransportSucceeded()
            && response.statusCode == 412)
        {
            return ParseConflict(
                response,
                resource,
                MaximumItemResponseBytes);
        }
        if (response.statusCode != 200
            && response.statusCode != 201)
        {
            result.outcome = StatusFailure(response, false);
            return result;
        }
        try
        {
            if (!ResponseEnvelopeIsValid(
                    response,
                    MaximumItemResponseBytes,
                    true))
            {
                throw std::runtime_error("Cloud put envelope is invalid.");
            }
            const auto headerEtag = SingleStrongEtag(response);
            auto snapshot = ParseSnapshot(
                ParseJsonStrict(response.Text()),
                true);
            if (!headerEtag
                || *headerEtag != snapshot.etag
                || snapshot.deleted
                || !EquivalentResources(snapshot.resource, resource)
                || snapshot.content != content
                || snapshot.sha256 != hash)
            {
                throw std::runtime_error("Cloud put snapshot mismatched.");
            }
            result.snapshot = std::move(snapshot);
            result.outcome = Outcome(
                CloudSaveWireStatus::Succeeded,
                {});
        }
        catch (...)
        {
            result.snapshot.reset();
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid mutation result.");
        }
        return result;
    }

    CloudSaveItemResult CloudSaveClient::Delete(
        const std::string_view accessToken,
        const CloudSaveResource& resource,
        const std::string_view mutationId,
        const std::string_view baseEtag) const
    {
        CloudSaveItemResult result;
        Json resourceBody;
        try
        {
            resourceBody = ResourceJson(resource);
        }
        catch (...)
        {
            result.outcome = InvalidRequest(
                "Cloud save resource is invalid.");
            return result;
        }
        if (!IsSafeOnlineBearerToken(accessToken)
            || !IsCanonicalMutationId(mutationId)
            || !IsStrongEtag(baseEtag))
        {
            result.outcome = InvalidRequest(
                "Cloud save authorization or mutation metadata is invalid.");
            return result;
        }
        const Json requestBody{
            { "protocolVersion", 1 },
            { "resource", std::move(resourceBody) }
        };
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        headers.emplace_back(
            L"Idempotency-Key",
            Utf8ToWide(mutationId));
        headers.emplace_back(
            L"If-Match",
            Utf8ToWide(baseEtag));
        const auto response = Send(
            L"DELETE",
            "/v1/cloud-saves/item",
            accessToken,
            requestBody.dump(),
            std::move(headers),
            MaximumItemResponseBytes);
        if (!response.TransportSucceeded())
        {
            result.outcome = StatusFailure(response, false);
            return result;
        }
        if (!ResponseEnvelopeIsValid(
                response,
                MaximumItemResponseBytes,
                false))
        {
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid response envelope.");
            return result;
        }
        if (response.TransportSucceeded()
            && response.statusCode == 412)
        {
            return ParseConflict(
                response,
                resource,
                MaximumItemResponseBytes);
        }
        if (response.statusCode != 200)
        {
            result.outcome = StatusFailure(response, false);
            return result;
        }
        try
        {
            if (!ResponseEnvelopeIsValid(
                    response,
                    MaximumItemResponseBytes,
                    true))
            {
                throw std::runtime_error("Cloud delete envelope is invalid.");
            }
            const auto headerEtag = SingleStrongEtag(response);
            auto snapshot = ParseSnapshot(
                ParseJsonStrict(response.Text()),
                true);
            if (!headerEtag
                || *headerEtag != snapshot.etag
                || !snapshot.deleted
                || !EquivalentResources(snapshot.resource, resource))
            {
                throw std::runtime_error("Cloud delete snapshot mismatched.");
            }
            result.snapshot = std::move(snapshot);
            result.outcome = Outcome(
                CloudSaveWireStatus::Succeeded,
                {});
        }
        catch (...)
        {
            result.snapshot.reset();
            result.outcome = Outcome(
                CloudSaveWireStatus::InvalidResponse,
                "Cloud save service returned an invalid mutation result.");
        }
        return result;
    }
}
