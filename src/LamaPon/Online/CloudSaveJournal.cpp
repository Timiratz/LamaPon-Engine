#include "LamaPon/Online/CloudSaveJournal.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/SaveSlotValidation.h"
#include "LamaPon/Online/OnlineHttpValidation.h"

#include <Windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using LamaPon::CloudSaveResource;
    using LamaPon::CloudSaveResourceKind;
    using LamaPon::CloudSaveSnapshot;
    using LamaPon::Detail::CloudSavePendingKind;
    using LamaPon::Detail::CloudSavePendingMutation;
    using LamaPon::Detail::CloudSaveJournalBusyError;

    constexpr std::size_t MaximumJournalPayloadBytes =
        LamaPon::CloudSaveAccountMaxBytes * 3u;
    constexpr std::size_t MaximumEncodedPayloadBytes =
        (MaximumJournalPayloadBytes * 4u + 2u) / 3u;
    // baseline・pending・remote conflictが各16MiBでもbase64urlと全metadataを
    // 同時に保持できるよう、encoded worst caseへ2MiBのschema余裕を加えます。
    constexpr std::size_t MaximumJournalBytes =
        MaximumEncodedPayloadBytes + 2u * 1024u * 1024u;
    constexpr std::size_t MaximumJsonDepth = 64u;
    constexpr std::size_t MaximumJsonElements = 65536u;
    constexpr std::string_view JournalFormat =
        "LamaPonCloudSaveJournal";
    constexpr std::string_view BindingDomain =
        "LamaPon.CloudSave.Journal.Binding.1";

    std::atomic<LamaPon::Detail::CloudSaveJournalTestFailPoint>
        JournalFailPoint{};

    bool ConsumeFailPoint(
        const LamaPon::Detail::CloudSaveJournalTestFailPoint expected) noexcept
    {
        auto value = expected;
        return JournalFailPoint.compare_exchange_strong(
            value,
            LamaPon::Detail::CloudSaveJournalTestFailPoint::None,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    class AlgorithmHandle final
    {
    public:
        ~AlgorithmHandle()
        {
            if (value != nullptr)
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        }

        BCRYPT_ALG_HANDLE value{};
    };

    class FileHandle final
    {
    public:
        FileHandle() = default;
        explicit FileHandle(const HANDLE handle) noexcept
            : value(handle)
        {
        }

        ~FileHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        FileHandle(FileHandle&& other) noexcept
            : value(std::exchange(other.value, INVALID_HANDLE_VALUE))
        {
        }

        FileHandle& operator=(FileHandle&& other) noexcept
        {
            if (this != &other)
            {
                if (value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(value);
                }
                value = std::exchange(other.value, INVALID_HANDLE_VALUE);
            }
            return *this;
        }

        HANDLE value{ INVALID_HANDLE_VALUE };
    };

    struct RestrictedSecurity final
    {
        FileHandle processToken;
        std::vector<std::uint8_t> tokenUser;
        PSID systemSid{};
        PACL acl{};
        SECURITY_DESCRIPTOR descriptor{};
        SECURITY_ATTRIBUTES attributes{};

        ~RestrictedSecurity()
        {
            if (acl != nullptr)
            {
                LocalFree(acl);
            }
            if (systemSid != nullptr)
            {
                FreeSid(systemSid);
            }
        }

        bool Initialize(const DWORD inheritance)
        {
            if (OpenProcessToken(
                    GetCurrentProcess(),
                    TOKEN_QUERY,
                    &processToken.value) == FALSE)
            {
                return false;
            }
            DWORD tokenUserBytes{};
            GetTokenInformation(
                processToken.value,
                TokenUser,
                nullptr,
                0,
                &tokenUserBytes);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER
                || tokenUserBytes < sizeof(TOKEN_USER))
            {
                return false;
            }
            tokenUser.resize(tokenUserBytes);
            if (GetTokenInformation(
                    processToken.value,
                    TokenUser,
                    tokenUser.data(),
                    tokenUserBytes,
                    &tokenUserBytes) == FALSE)
            {
                return false;
            }
            SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
            if (AllocateAndInitializeSid(
                    &authority,
                    1,
                    SECURITY_LOCAL_SYSTEM_RID,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    &systemSid) == FALSE)
            {
                return false;
            }
            const auto* currentUser =
                reinterpret_cast<const TOKEN_USER*>(tokenUser.data());
            EXPLICIT_ACCESSW entries[2]{};
            for (auto& entry : entries)
            {
                entry.grfAccessPermissions = FILE_ALL_ACCESS;
                entry.grfAccessMode = SET_ACCESS;
                entry.grfInheritance = inheritance;
                entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
            }
            entries[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
            entries[0].Trustee.ptstrName = static_cast<LPWSTR>(
                currentUser->User.Sid);
            entries[1].Trustee.ptstrName = static_cast<LPWSTR>(systemSid);
            if (SetEntriesInAclW(
                    static_cast<ULONG>(std::size(entries)),
                    entries,
                    nullptr,
                    &acl) != ERROR_SUCCESS
                || InitializeSecurityDescriptor(
                    &descriptor,
                    SECURITY_DESCRIPTOR_REVISION) == FALSE
                || SetSecurityDescriptorDacl(
                    &descriptor,
                    TRUE,
                    acl,
                    FALSE) == FALSE
                || SetSecurityDescriptorOwner(
                    &descriptor,
                    currentUser->User.Sid,
                    FALSE) == FALSE
                || SetSecurityDescriptorControl(
                    &descriptor,
                    SE_DACL_PROTECTED,
                    SE_DACL_PROTECTED) == FALSE)
            {
                return false;
            }
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = &descriptor;
            attributes.bInheritHandle = FALSE;
            return true;
        }

        PSID CurrentUserSid() const noexcept
        {
            return tokenUser.empty()
                ? nullptr
                : reinterpret_cast<const TOKEN_USER*>(tokenUser.data())
                    ->User.Sid;
        }
    };

    struct Entry final
    {
        CloudSaveResource resource;
        std::optional<CloudSaveSnapshot> baseline;
        std::optional<CloudSavePendingMutation> pending;
        std::optional<CloudSaveSnapshot> conflict;
        bool localDeleteIntent{};
    };

    struct JournalState final
    {
        std::uint64_t schemaVersion{ 2u };
        std::uint64_t generation{};
        std::uint64_t parentGeneration{};
        std::string parentChecksum;
        std::string checksum;
        std::vector<Entry> entries;
    };

    enum class CandidateSource : std::uint8_t
    {
        Final,
        Next,
        Backup
    };

    struct Candidate final
    {
        CandidateSource source{ CandidateSource::Final };
        std::filesystem::path path;
        JournalState state;
        std::string canonicalDocument;
    };

    enum class CandidateStatus : std::uint8_t
    {
        Missing,
        Valid,
        Invalid,
        BindingMismatch,
        Unavailable,
        UnsupportedVersion,
        FatalCorruption
    };

    struct CandidateRead final
    {
        CandidateStatus status{ CandidateStatus::Missing };
        std::optional<Candidate> candidate;
    };

    class UnsupportedJournalVersion final : public std::runtime_error
    {
    public:
        UnsupportedJournalVersion()
            : std::runtime_error("Cloud save journal version is unsupported.")
        {
        }
    };

    class JsonSyntaxError final : public std::runtime_error
    {
    public:
        JsonSyntaxError()
            : std::runtime_error("Cloud save journal JSON syntax is invalid.")
        {
        }
    };

    class RecoverableJournalCorruption final : public std::runtime_error
    {
    public:
        RecoverableJournalCorruption()
            : std::runtime_error("Cloud save journal write is incomplete.")
        {
        }
    };

    [[noreturn]] void ThrowJournalFailure()
    {
        throw std::runtime_error("Cloud save journal operation failed.");
    }

    [[noreturn]] void ThrowCorruptJournal()
    {
        throw std::runtime_error("Cloud save journal is corrupt.");
    }

    bool IsMissingError(const DWORD error) noexcept
    {
        return error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND;
    }

    std::filesystem::path WithSuffix(
        const std::filesystem::path& path,
        const std::wstring_view suffix)
    {
        auto result = path;
        result += suffix;
        return result;
    }

    bool IsLowerHex(
        const std::string_view value,
        const std::size_t length) noexcept
    {
        return value.size() == length
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                });
    }

    std::array<std::uint8_t, 32> Sha256(
        const std::uint8_t* data,
        const std::size_t size)
    {
        if (size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument("Cloud save journal data is too large.");
        }
        AlgorithmHandle algorithm;
        if (BCryptOpenAlgorithmProvider(
                &algorithm.value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0)
        {
            ThrowJournalFailure();
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
            ThrowJournalFailure();
        }
        return digest;
    }

    std::string LowerHex(const std::array<std::uint8_t, 32>& digest)
    {
        constexpr char Hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2u);
        for (const auto byte : digest)
        {
            result.push_back(Hex[byte >> 4u]);
            result.push_back(Hex[byte & 0x0fu]);
        }
        return result;
    }

    std::string Sha256LowerHex(const std::string_view value)
    {
        return LowerHex(Sha256(
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size()));
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
                | (static_cast<std::uint32_t>(data[index + 1u]) << 8u)
                | static_cast<std::uint32_t>(data[index + 2u]);
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
                | (static_cast<std::uint32_t>(data[index + 1u]) << 8u);
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
            || encoded.size() > (maximumBytes * 4u + 2u) / 3u)
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
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1u])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2u])) << 6u)
                | static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 3u]));
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
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1u])) << 12u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
        }
        else if (remaining == 3u)
        {
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1u])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2u])) << 6u);
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

    bool IsStrongEtag(const std::string_view value) noexcept
    {
        if (value.size() < 3u
            || value.size() > LamaPon::CloudSaveEtagMaxBytes
            || value.front() != '"'
            || value.back() != '"'
            || value.starts_with("W/")
            || value == "*"
            || value == "\"\"")
        {
            return false;
        }
        return std::ranges::all_of(
            value.substr(1u, value.size() - 2u),
            [](const unsigned char character)
            {
                return character >= 0x21
                    && character <= 0x7e
                    && character != '"'
                    && character != '\\'
                    && character != ',';
            });
    }

    bool IsCanonicalMutationId(const std::string_view value) noexcept
    {
        if (value.size() != 36u)
        {
            return false;
        }
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            if (index == 8u || index == 13u
                || index == 18u || index == 23u)
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
            && (value[19] == '8' || value[19] == '9'
                || value[19] == 'a' || value[19] == 'b');
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
                if (depth == 0u)
                {
                    return false;
                }
                --depth;
            }
        }
        return depth == 0u && !inString && !escaped;
    }

    bool JsonElementCountIsSafe(
        const Json& value,
        std::size_t& remaining) noexcept
    {
        if (remaining == 0u)
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
            throw JsonSyntaxError();
        }

        bool duplicateKey{};
        std::array<std::unordered_set<std::string>, MaximumJsonDepth + 1u>
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
                    auto& keys = keysByDepth[static_cast<std::size_t>(depth)];
                    if (!keys.insert(parsed.get<std::string>()).second)
                    {
                        duplicateKey = true;
                    }
                }
                return true;
            };
        Json parsed;
        try
        {
            parsed = Json::parse(text, callback, true, false);
        }
        catch (const Json::parse_error&)
        {
            throw JsonSyntaxError();
        }
        std::size_t remainingElements = MaximumJsonElements;
        if (duplicateKey
            || !JsonElementCountIsSafe(parsed, remainingElements))
        {
            throw std::runtime_error("Cloud save journal JSON is invalid.");
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
        throw std::runtime_error("Cloud save journal resource is invalid.");
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

    std::size_t MaximumContentBytes(const CloudSaveResource& resource)
    {
        switch (resource.kind)
        {
        case CloudSaveResourceKind::Preferences:
            if (resource.slot.empty())
            {
                return LamaPon::CloudPreferencesMaxBytes;
            }
            break;
        case CloudSaveResourceKind::SaveSlot:
            if (LamaPon::Detail::IsValidSaveSlotName(resource.slot))
            {
                return LamaPon::CloudSaveSlotMaxBytes;
            }
            break;
        }
        throw std::invalid_argument("Cloud save resource is invalid.");
    }

    void ValidateContent(
        const CloudSaveResource& resource,
        const std::vector<std::uint8_t>& content)
    {
        if (content.empty() || content.size() > MaximumContentBytes(resource))
        {
            throw std::invalid_argument("Cloud save content is invalid.");
        }
        const std::string_view text(
            reinterpret_cast<const char*>(content.data()),
            content.size());
        (void)ParseJsonStrict(text);
    }

    Json SnapshotJson(const CloudSaveSnapshot& snapshot)
    {
        (void)MaximumContentBytes(snapshot.resource);
        if (!IsStrongEtag(snapshot.etag))
        {
            throw std::invalid_argument("Cloud save snapshot ETag is invalid.");
        }
        Json result{
            { "resource", ResourceJson(snapshot.resource) },
            { "etag", snapshot.etag },
            { "deleted", snapshot.deleted },
            { "byteLength", snapshot.content.size() }
        };
        if (snapshot.deleted)
        {
            if (!snapshot.content.empty() || !snapshot.sha256.empty())
            {
                throw std::invalid_argument("Cloud save tombstone is invalid.");
            }
            return result;
        }
        ValidateContent(snapshot.resource, snapshot.content);
        const auto hash = ContentHash(snapshot.content);
        if (!IsCanonicalSha256(snapshot.sha256) || snapshot.sha256 != hash)
        {
            throw std::invalid_argument("Cloud save snapshot hash is invalid.");
        }
        result["sha256"] = snapshot.sha256;
        result["content"] = EncodeBase64Url(
            snapshot.content.data(),
            snapshot.content.size());
        return result;
    }

    CloudSaveSnapshot ParseSnapshot(
        const Json& json,
        const CloudSaveResource& expectedResource)
    {
        if (!json.is_object()
            || !json.contains("deleted")
            || !json.at("deleted").is_boolean())
        {
            throw std::runtime_error("Cloud save journal snapshot is invalid.");
        }
        const bool deleted = json.at("deleted").get<bool>();
        if (!(deleted
                ? HasExactKeys(
                    json,
                    { "resource", "etag", "deleted", "byteLength" })
                : HasExactKeys(
                    json,
                    { "resource", "etag", "deleted", "byteLength",
                      "sha256", "content" })))
        {
            throw std::runtime_error("Cloud save journal snapshot is invalid.");
        }
        if (!json.at("etag").is_string()
            || !json.at("byteLength").is_number_unsigned())
        {
            throw std::runtime_error("Cloud save journal snapshot is invalid.");
        }
        auto resource = ParseResource(json.at("resource"));
        if (!EquivalentResources(resource, expectedResource))
        {
            throw std::runtime_error("Cloud save journal resource mismatch.");
        }
        CloudSaveSnapshot snapshot;
        snapshot.resource = std::move(resource);
        snapshot.etag = json.at("etag").get<std::string>();
        snapshot.deleted = deleted;
        if (!IsStrongEtag(snapshot.etag))
        {
            throw std::runtime_error("Cloud save journal ETag is invalid.");
        }
        const auto byteLength = json.at("byteLength").get<std::uint64_t>();
        if (deleted)
        {
            if (byteLength != 0u)
            {
                throw std::runtime_error("Cloud save journal tombstone is invalid.");
            }
            return snapshot;
        }
        if (!json.at("sha256").is_string()
            || !json.at("content").is_string())
        {
            throw std::runtime_error("Cloud save journal snapshot is invalid.");
        }
        snapshot.sha256 = json.at("sha256").get<std::string>();
        const auto encoded = json.at("content").get<std::string>();
        if (!DecodeBase64Url(
                encoded,
                MaximumContentBytes(snapshot.resource),
                snapshot.content)
            || snapshot.content.size() != byteLength
            || !IsCanonicalSha256(snapshot.sha256)
            || ContentHash(snapshot.content) != snapshot.sha256)
        {
            throw std::runtime_error("Cloud save journal content is invalid.");
        }
        ValidateContent(snapshot.resource, snapshot.content);
        return snapshot;
    }

    Json PendingJson(const CloudSavePendingMutation& pending)
    {
        (void)MaximumContentBytes(pending.resource);
        if (!IsCanonicalMutationId(pending.mutationId))
        {
            throw std::invalid_argument("Cloud save mutation id is invalid.");
        }
        Json result{
            { "kind", pending.kind == CloudSavePendingKind::Put
                ? "put" : "delete" },
            { "mutationId", pending.mutationId },
            { "baseEtag", pending.baseEtag
                ? Json(*pending.baseEtag) : Json(nullptr) }
        };
        if (pending.baseEtag && !IsStrongEtag(*pending.baseEtag))
        {
            throw std::invalid_argument("Cloud save base ETag is invalid.");
        }
        if (pending.kind == CloudSavePendingKind::Delete)
        {
            if (!pending.baseEtag
                || !pending.content.empty()
                || !pending.sha256.empty())
            {
                throw std::invalid_argument("Cloud save delete is invalid.");
            }
            return result;
        }
        ValidateContent(pending.resource, pending.content);
        const auto hash = ContentHash(pending.content);
        if (!IsCanonicalSha256(pending.sha256) || pending.sha256 != hash)
        {
            throw std::invalid_argument("Cloud save pending hash is invalid.");
        }
        result["byteLength"] = pending.content.size();
        result["sha256"] = pending.sha256;
        result["content"] = EncodeBase64Url(
            pending.content.data(),
            pending.content.size());
        return result;
    }

    CloudSavePendingMutation ParsePending(
        const Json& json,
        const CloudSaveResource& resource)
    {
        if (!json.is_object()
            || !json.contains("kind")
            || !json.at("kind").is_string())
        {
            throw std::runtime_error("Cloud save pending mutation is invalid.");
        }
        const auto kind = json.at("kind").get<std::string>();
        const bool isPut = kind == "put";
        if (!(isPut
                ? HasExactKeys(
                    json,
                    { "kind", "mutationId", "baseEtag", "byteLength",
                      "sha256", "content" })
                : kind == "delete"
                    && HasExactKeys(
                        json,
                        { "kind", "mutationId", "baseEtag" })))
        {
            throw std::runtime_error("Cloud save pending mutation is invalid.");
        }
        if (!json.at("mutationId").is_string()
            || !(json.at("baseEtag").is_null()
                || json.at("baseEtag").is_string()))
        {
            throw std::runtime_error("Cloud save pending mutation is invalid.");
        }
        CloudSavePendingMutation pending;
        pending.resource = resource;
        pending.kind = isPut
            ? CloudSavePendingKind::Put
            : CloudSavePendingKind::Delete;
        pending.mutationId = json.at("mutationId").get<std::string>();
        if (!IsCanonicalMutationId(pending.mutationId))
        {
            throw std::runtime_error("Cloud save mutation id is invalid.");
        }
        if (json.at("baseEtag").is_string())
        {
            pending.baseEtag = json.at("baseEtag").get<std::string>();
            if (!IsStrongEtag(*pending.baseEtag))
            {
                throw std::runtime_error("Cloud save base ETag is invalid.");
            }
        }
        if (!isPut)
        {
            if (!pending.baseEtag)
            {
                throw std::runtime_error("Cloud save delete ETag is missing.");
            }
            return pending;
        }
        if (!json.at("byteLength").is_number_unsigned()
            || !json.at("sha256").is_string()
            || !json.at("content").is_string())
        {
            throw std::runtime_error("Cloud save pending mutation is invalid.");
        }
        pending.sha256 = json.at("sha256").get<std::string>();
        const auto encoded = json.at("content").get<std::string>();
        const auto byteLength = json.at("byteLength").get<std::uint64_t>();
        if (!DecodeBase64Url(
                encoded,
                MaximumContentBytes(resource),
                pending.content)
            || pending.content.size() != byteLength
            || !IsCanonicalSha256(pending.sha256)
            || ContentHash(pending.content) != pending.sha256)
        {
            throw std::runtime_error("Cloud save pending content is invalid.");
        }
        ValidateContent(resource, pending.content);
        return pending;
    }

    Json EntryJson(const Entry& entry, const std::uint64_t version)
    {
        Json result{
            { "resource", ResourceJson(entry.resource) },
            { "baseline", entry.baseline
                ? SnapshotJson(*entry.baseline) : Json(nullptr) },
            { "pending", entry.pending
                ? PendingJson(*entry.pending) : Json(nullptr) },
            { "conflict", entry.conflict
                ? SnapshotJson(*entry.conflict) : Json(nullptr) },
            { "localDeleteIntent", entry.localDeleteIntent }
        };
        if (version == 1u)
        {
            result.erase("localDeleteIntent");
        }
        return result;
    }

    std::size_t FindEntry(
        const std::vector<Entry>& entries,
        const CloudSaveResource& resource) noexcept
    {
        const auto found = std::ranges::find_if(
            entries,
            [&resource](const Entry& entry)
            {
                return EquivalentResources(entry.resource, resource);
            });
        return found == entries.end()
            ? entries.size()
            : static_cast<std::size_t>(found - entries.begin());
    }

    bool MutationIdInUse(
        const std::vector<Entry>& entries,
        const std::string_view mutationId,
        const Entry* excluded = nullptr) noexcept
    {
        return std::ranges::any_of(
            entries,
            [mutationId, excluded](const Entry& entry)
            {
                return &entry != excluded
                    && entry.pending
                    && entry.pending->mutationId == mutationId;
            });
    }

    void ValidateEntries(const std::vector<Entry>& entries)
    {
        std::size_t preferences{};
        std::size_t slots{};
        std::array<std::uint64_t, 3> totals{};
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto& entry = entries[index];
            (void)ResourceJson(entry.resource);
            if (!entry.baseline && !entry.pending && !entry.conflict
                && !entry.localDeleteIntent)
            {
                throw std::runtime_error("Cloud save journal contains an empty entry.");
            }
            if (entry.resource.kind == CloudSaveResourceKind::Preferences)
            {
                ++preferences;
            }
            else
            {
                ++slots;
            }
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (EquivalentResources(
                        entries[previous].resource,
                        entry.resource))
                {
                    throw std::runtime_error("Cloud save journal resource is duplicated.");
                }
                if (entry.pending
                    && entries[previous].pending
                    && entry.pending->mutationId
                        == entries[previous].pending->mutationId)
                {
                    throw std::runtime_error(
                        "Cloud save mutation id is duplicated.");
                }
            }
            const auto add = [&totals](
                const std::size_t category,
                const std::size_t bytes)
            {
                if (bytes > LamaPon::CloudSaveAccountMaxBytes
                    || totals[category]
                        > LamaPon::CloudSaveAccountMaxBytes - bytes)
                {
                    throw std::runtime_error("Cloud save journal quota is exceeded.");
                }
                totals[category] += bytes;
            };
            if (entry.baseline && !entry.baseline->deleted)
            {
                (void)SnapshotJson(*entry.baseline);
                add(0u, entry.baseline->content.size());
            }
            if (entry.pending)
            {
                (void)PendingJson(*entry.pending);
                if (entry.pending->kind == CloudSavePendingKind::Put)
                {
                    add(1u, entry.pending->content.size());
                }
            }
            if (entry.conflict)
            {
                (void)SnapshotJson(*entry.conflict);
                if (!entry.conflict->deleted)
                {
                    add(2u, entry.conflict->content.size());
                }
                if (!entry.pending)
                {
                    throw std::runtime_error("Cloud save conflict has no pending mutation.");
                }
            }
        }
        if (preferences > 1u || slots > LamaPon::CloudSaveMaxSlots)
        {
            throw std::runtime_error("Cloud save journal has too many resources.");
        }
    }

    Json PayloadJson(
        const JournalState& state,
        const std::string_view binding)
    {
        ValidateEntries(state.entries);
        Json entries = Json::array();
        for (const auto& entry : state.entries)
        {
            entries.push_back(EntryJson(entry, state.schemaVersion));
        }
        return Json{
            { "format", JournalFormat },
            { "version", state.schemaVersion },
            { "generation", state.generation },
            { "parentGeneration", state.parentGeneration },
            { "parentChecksum", state.parentChecksum },
            { "binding", binding },
            { "entries", std::move(entries) }
        };
    }

    std::string SerializeState(
        JournalState& state,
        const std::string_view binding)
    {
        auto payload = PayloadJson(state, binding);
        const auto canonicalPayload = payload.dump();
        state.checksum = Sha256LowerHex(canonicalPayload);
        payload["checksum"] = state.checksum;
        const auto document = payload.dump();
        if (document.size() > MaximumJournalBytes)
        {
            throw std::runtime_error("Cloud save journal is too large.");
        }
        return document;
    }

    JournalState ParseState(
        const std::string_view text,
        const std::string_view expectedBinding)
    {
        Json document;
        try
        {
            document = ParseJsonStrict(text);
        }
        catch (const JsonSyntaxError&)
        {
            throw RecoverableJournalCorruption();
        }
        if (document.is_object()
            && document.contains("format")
            && document.at("format").is_string()
            && document.at("format").get<std::string>() == JournalFormat
            && document.contains("version")
            && document.at("version").is_number_unsigned()
            && document.at("version").get<std::uint64_t>() != 1u
            && document.at("version").get<std::uint64_t>() != 2u)
        {
            throw UnsupportedJournalVersion();
        }
        if (!HasExactKeys(
                document,
                { "format", "version", "generation", "parentGeneration",
                  "parentChecksum", "binding", "entries", "checksum" })
            || !document.at("format").is_string()
            || document.at("format").get<std::string>() != JournalFormat
            || !document.at("version").is_number_unsigned()
            || (document.at("version").get<std::uint64_t>() != 1u
                && document.at("version").get<std::uint64_t>() != 2u)
            || !document.at("generation").is_number_unsigned()
            || !document.at("parentGeneration").is_number_unsigned()
            || !document.at("parentChecksum").is_string()
            || !document.at("binding").is_string()
            || !document.at("entries").is_array()
            || !document.at("checksum").is_string())
        {
            throw std::runtime_error("Cloud save journal schema is invalid.");
        }
        const auto binding = document.at("binding").get<std::string>();
        if (binding != expectedBinding)
        {
            throw std::domain_error("Cloud save journal binding does not match.");
        }
        const auto version = document.at("version").get<std::uint64_t>();
        JournalState state;
        state.schemaVersion = version;
        state.generation = document.at("generation").get<std::uint64_t>();
        state.parentGeneration =
            document.at("parentGeneration").get<std::uint64_t>();
        state.parentChecksum =
            document.at("parentChecksum").get<std::string>();
        state.checksum = document.at("checksum").get<std::string>();
        if (state.generation == 0u
            || state.parentGeneration != state.generation - 1u
            || (state.generation == 1u
                ? !state.parentChecksum.empty()
                : !IsLowerHex(state.parentChecksum, 64u))
            || !IsLowerHex(state.checksum, 64u))
        {
            throw std::runtime_error("Cloud save journal generation is invalid.");
        }
        auto payload = document;
        payload.erase("checksum");
        if (Sha256LowerHex(payload.dump()) != state.checksum)
        {
            throw RecoverableJournalCorruption();
        }
        for (const auto& item : document.at("entries"))
        {
            const bool exactEntry = version == 1u
                ? HasExactKeys(
                    item,
                    { "resource", "baseline", "pending", "conflict" })
                : HasExactKeys(
                    item,
                    { "resource", "baseline", "pending", "conflict",
                      "localDeleteIntent" });
            if (!exactEntry
                || (version == 2u
                    && !item.at("localDeleteIntent").is_boolean()))
            {
                throw std::runtime_error("Cloud save journal entry is invalid.");
            }
            Entry entry;
            entry.resource = ParseResource(item.at("resource"));
            if (!item.at("baseline").is_null())
            {
                entry.baseline = ParseSnapshot(
                    item.at("baseline"),
                    entry.resource);
            }
            if (!item.at("pending").is_null())
            {
                entry.pending = ParsePending(
                    item.at("pending"),
                    entry.resource);
            }
            if (!item.at("conflict").is_null())
            {
                entry.conflict = ParseSnapshot(
                    item.at("conflict"),
                    entry.resource);
            }
            if (version == 2u)
            {
                entry.localDeleteIntent =
                    item.at("localDeleteIntent").get<bool>();
            }
            state.entries.push_back(std::move(entry));
        }
        ValidateEntries(state.entries);
        return state;
    }

    std::filesystem::path NormalizedAbsolute(
        const std::filesystem::path& path)
    {
        if (path.empty())
        {
            throw std::invalid_argument("Cloud save account profile is invalid.");
        }
        try
        {
            return std::filesystem::absolute(path).lexically_normal();
        }
        catch (...)
        {
            throw std::invalid_argument(
                "Cloud save account profile path is invalid.");
        }
    }

    bool EquivalentPaths(
        const std::filesystem::path& left,
        const std::filesystem::path& right) noexcept
    {
        try
        {
            const auto a = NormalizedAbsolute(left).native();
            const auto b = NormalizedAbsolute(right).native();
            if (a.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
                || b.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }
            // engineが組み立てたprofileなので、case-sensitive directoryでも
            // 別namespaceへaliasしないよう正規化後の綴りまで一致させます。
            return a == b;
        }
        catch (...)
        {
            return false;
        }
    }

    std::string NormalizedPathUtf8(const std::filesystem::path& path)
    {
        return LamaPon::PathToUtf8(NormalizedAbsolute(path));
    }

    std::string NormalizedBackendBaseUrl(
        std::string baseUrl,
        const bool allowInsecureLoopback)
    {
        return LamaPon::Detail::NormalizeOnlineServiceBaseUrl(
            std::move(baseUrl),
            allowInsecureLoopback);
    }

    void AppendLengthTagged(std::string& output, const std::string_view value)
    {
        const auto length = static_cast<std::uint64_t>(value.size());
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            output.push_back(static_cast<char>((length >> shift) & 0xffu));
        }
        output.append(value);
    }

    std::string MakeBinding(
        const std::filesystem::path& accountRoot,
        const std::string_view accountStorageKey,
        const std::string_view gameId,
        const std::string_view environmentId,
        const std::string_view normalizedBackendBaseUrl)
    {
        std::string input(BindingDomain);
        AppendLengthTagged(input, NormalizedPathUtf8(accountRoot));
        AppendLengthTagged(input, accountStorageKey);
        AppendLengthTagged(input, gameId);
        AppendLengthTagged(input, environmentId);
        AppendLengthTagged(input, normalizedBackendBaseUrl);
        return Sha256LowerHex(input);
    }

    void ValidateExistingDirectory(const std::filesystem::path& path)
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        {
            ThrowJournalFailure();
        }
    }

    bool VerifyRestrictedAcl(
        const std::filesystem::path& path,
        const bool directory,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        PSID owner{};
        PACL acl{};
        PSECURITY_DESCRIPTOR descriptor{};
        const auto result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner,
            nullptr,
            &acl,
            nullptr,
            &descriptor);
        if (result != ERROR_SUCCESS || descriptor == nullptr || acl == nullptr)
        {
            if (descriptor != nullptr)
            {
                LocalFree(descriptor);
            }
            return false;
        }
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        ACL_SIZE_INFORMATION information{};
        bool valid = GetSecurityDescriptorControl(
                descriptor,
                &control,
                &revision) != FALSE
            && (control & SE_DACL_PROTECTED) != 0u
            && GetAclInformation(
                acl,
                &information,
                sizeof(information),
                AclSizeInformation) != FALSE
            && information.AceCount == 2u
            && owner != nullptr
            && (EqualSid(owner, currentUserSid) != FALSE
                || EqualSid(owner, systemSid) != FALSE);
        bool currentUserSeen{};
        bool systemSeen{};
        const auto expectedInheritance = static_cast<BYTE>(directory
            ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
            : 0u);
        for (DWORD index = 0u; valid && index < information.AceCount; ++index)
        {
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE || rawAce == nullptr)
            {
                valid = false;
                break;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            const auto flags = static_cast<BYTE>(ace->Header.AceFlags
                & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
                    | INHERIT_ONLY_ACE | INHERITED_ACE));
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE
                || ace->Mask != FILE_ALL_ACCESS
                || flags != expectedInheritance)
            {
                valid = false;
                break;
            }
            const auto sid = const_cast<DWORD*>(&ace->SidStart);
            if (EqualSid(sid, currentUserSid) != FALSE && !currentUserSeen)
            {
                currentUserSeen = true;
            }
            else if (EqualSid(sid, systemSid) != FALSE && !systemSeen)
            {
                systemSeen = true;
            }
            else
            {
                valid = false;
            }
        }
        LocalFree(descriptor);
        return valid && currentUserSeen && systemSeen;
    }

    bool PathOwnerIsAllowed(
        const std::filesystem::path& path,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        PSID owner{};
        PSECURITY_DESCRIPTOR descriptor{};
        const auto result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        const bool allowed = result == ERROR_SUCCESS
            && descriptor != nullptr
            && owner != nullptr
            && (EqualSid(owner, currentUserSid) != FALSE
                || EqualSid(owner, systemSid) != FALSE);
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return allowed;
    }

    bool HandleOwnerIsAllowed(
        const HANDLE file,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        PSID owner{};
        PSECURITY_DESCRIPTOR descriptor{};
        const auto result = GetSecurityInfo(
            file,
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        const bool allowed = result == ERROR_SUCCESS
            && descriptor != nullptr
            && owner != nullptr
            && (EqualSid(owner, currentUserSid) != FALSE
                || EqualSid(owner, systemSid) != FALSE);
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return allowed;
    }

    bool ApplyRestrictedAcl(
        const std::filesystem::path& path,
        const bool directory)
    {
        RestrictedSecurity security;
        const auto inheritance = directory
            ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
            : NO_INHERITANCE;
        if (!security.Initialize(inheritance)
            || !PathOwnerIsAllowed(
                path,
                security.CurrentUserSid(),
                security.systemSid)
            || SetNamedSecurityInfoW(
                const_cast<LPWSTR>(path.c_str()),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION
                    | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                security.acl,
                nullptr) != ERROR_SUCCESS)
        {
            return false;
        }
        return VerifyRestrictedAcl(
            path,
            directory,
            security.CurrentUserSid(),
            security.systemSid);
    }

    bool VerifyRestrictedAclHandle(
        const HANDLE file,
        const bool directory,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        PSID owner{};
        PACL acl{};
        PSECURITY_DESCRIPTOR descriptor{};
        if (GetSecurityInfo(
                file,
                SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &owner,
                nullptr,
                &acl,
                nullptr,
                &descriptor) != ERROR_SUCCESS
            || descriptor == nullptr
            || acl == nullptr)
        {
            if (descriptor != nullptr)
            {
                LocalFree(descriptor);
            }
            return false;
        }
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        ACL_SIZE_INFORMATION information{};
        bool valid = GetSecurityDescriptorControl(
                descriptor,
                &control,
                &revision) != FALSE
            && (control & SE_DACL_PROTECTED) != 0u
            && GetAclInformation(
                acl,
                &information,
                sizeof(information),
                AclSizeInformation) != FALSE
            && information.AceCount == 2u
            && owner != nullptr
            && (EqualSid(owner, currentUserSid) != FALSE
                || EqualSid(owner, systemSid) != FALSE);
        bool currentUserSeen{};
        bool systemSeen{};
        const auto expectedInheritance = static_cast<BYTE>(directory
            ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
            : 0u);
        for (DWORD index = 0u; valid && index < information.AceCount; ++index)
        {
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE || rawAce == nullptr)
            {
                valid = false;
                break;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            const auto flags = static_cast<BYTE>(ace->Header.AceFlags
                & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
                    | INHERIT_ONLY_ACE | INHERITED_ACE));
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE
                || ace->Mask != FILE_ALL_ACCESS
                || flags != expectedInheritance)
            {
                valid = false;
                break;
            }
            const auto sid = const_cast<DWORD*>(&ace->SidStart);
            if (EqualSid(sid, currentUserSid) != FALSE && !currentUserSeen)
            {
                currentUserSeen = true;
            }
            else if (EqualSid(sid, systemSid) != FALSE && !systemSeen)
            {
                systemSeen = true;
            }
            else
            {
                valid = false;
            }
        }
        LocalFree(descriptor);
        return valid && currentUserSeen && systemSeen;
    }

    void ValidateExistingPathComponents(
        const std::filesystem::path& path)
    {
        const auto normalized = NormalizedAbsolute(path);
        auto current = normalized.root_path();
        for (const auto& component : normalized.relative_path())
        {
            current /= component;
            const auto attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                if (IsMissingError(GetLastError()))
                {
                    return;
                }
                ThrowJournalFailure();
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u
                || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            {
                throw std::invalid_argument(
                    "Cloud save trusted path contains an unsafe component.");
            }
        }
    }

    void EnsurePlainDirectory(
        const std::filesystem::path& path,
        const bool restrictAccess)
    {
        RestrictedSecurity security;
        SECURITY_ATTRIBUTES* attributes{};
        if (restrictAccess)
        {
            if (!security.Initialize(
                    CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE))
            {
                ThrowJournalFailure();
            }
            attributes = &security.attributes;
        }
        if (CreateDirectoryW(path.c_str(), attributes) == FALSE)
        {
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS)
            {
                ThrowJournalFailure();
            }
        }
        ValidateExistingDirectory(path);
        if (restrictAccess && !ApplyRestrictedAcl(path, true))
        {
            ThrowJournalFailure();
        }
    }

    FileHandle AcquireLock(const std::filesystem::path& path)
    {
        RestrictedSecurity security;
        if (!security.Initialize(NO_INHERITANCE))
        {
            ThrowJournalFailure();
        }
        FileHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
            0,
            &security.attributes,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION
                || error == ERROR_LOCK_VIOLATION)
            {
                throw CloudSaveJournalBusyError();
            }
            ThrowJournalFailure();
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_STANDARD_INFO standard{};
        if (GetFileInformationByHandleEx(
                file.value,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) == FALSE
            || GetFileInformationByHandleEx(
                file.value,
                FileStandardInfo,
                &standard,
                sizeof(standard)) == FALSE
            || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u
            || standard.NumberOfLinks != 1u
            || !HandleOwnerIsAllowed(
                file.value,
                security.CurrentUserSid(),
                security.systemSid)
            || SetSecurityInfo(
                file.value,
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION
                    | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                security.acl,
                nullptr) != ERROR_SUCCESS
            || !VerifyRestrictedAclHandle(
                file.value,
                false,
                security.CurrentUserSid(),
                security.systemSid))
        {
            ThrowJournalFailure();
        }
        return file;
    }

    CandidateRead ReadCandidate(
        const std::filesystem::path& path,
        const CandidateSource source,
        const std::string_view binding)
    {
        RestrictedSecurity security;
        if (!security.Initialize(NO_INHERITANCE))
        {
            return { CandidateStatus::Unavailable, std::nullopt };
        }
        FileHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ | READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
        {
            return { IsMissingError(GetLastError())
                ? CandidateStatus::Missing
                : CandidateStatus::Unavailable, std::nullopt };
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_STANDARD_INFO standard{};
        if (GetFileInformationByHandleEx(
                file.value,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) == FALSE
            || GetFileInformationByHandleEx(
                file.value,
                FileStandardInfo,
                &standard,
                sizeof(standard)) == FALSE
            || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u
            || standard.NumberOfLinks != 1u
            || !HandleOwnerIsAllowed(
                file.value,
                security.CurrentUserSid(),
                security.systemSid)
            || SetSecurityInfo(
                file.value,
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION
                    | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                security.acl,
                nullptr) != ERROR_SUCCESS
            || !VerifyRestrictedAclHandle(
                file.value,
                false,
                security.CurrentUserSid(),
                security.systemSid))
        {
            return { CandidateStatus::Unavailable, std::nullopt };
        }
        if (standard.EndOfFile.QuadPart <= 0)
        {
            return { CandidateStatus::Invalid, std::nullopt };
        }
        if (standard.EndOfFile.QuadPart
            > static_cast<LONGLONG>(MaximumJournalBytes))
        {
            return { CandidateStatus::FatalCorruption, std::nullopt };
        }
        const auto size = static_cast<std::size_t>(standard.EndOfFile.QuadPart);
        std::string text(size, '\0');
        std::size_t offset{};
        while (offset < size)
        {
            const auto remaining = std::min<std::size_t>(
                size - offset,
                std::numeric_limits<DWORD>::max());
            DWORD read{};
            if (ReadFile(
                    file.value,
                    text.data() + offset,
                    static_cast<DWORD>(remaining),
                    &read,
                    nullptr) == FALSE
                || read == 0u)
            {
                return { CandidateStatus::Unavailable, std::nullopt };
            }
            offset += read;
        }
        try
        {
            auto state = ParseState(text, binding);
            auto canonical = SerializeState(state, binding);
            return {
                CandidateStatus::Valid,
                Candidate{ source, path, std::move(state), std::move(canonical) }
            };
        }
        catch (const std::domain_error&)
        {
            return { CandidateStatus::BindingMismatch, std::nullopt };
        }
        catch (const UnsupportedJournalVersion&)
        {
            return { CandidateStatus::UnsupportedVersion, std::nullopt };
        }
        catch (const RecoverableJournalCorruption&)
        {
            return { CandidateStatus::Invalid, std::nullopt };
        }
        catch (...)
        {
            return { CandidateStatus::FatalCorruption, std::nullopt };
        }
    }

    std::optional<Candidate> SelectCandidate(
        const std::filesystem::path& finalPath,
        const std::string_view binding)
    {
        std::vector<Candidate> candidates;
        bool anyExisting{};
        bool bindingMismatch{};
        for (const auto& [path, source] : std::array{
                std::pair{ finalPath, CandidateSource::Final },
                std::pair{ WithSuffix(finalPath, L".next"), CandidateSource::Next },
                std::pair{ WithSuffix(finalPath, L".bak"), CandidateSource::Backup } })
        {
            auto result = ReadCandidate(path, source, binding);
            anyExisting = anyExisting
                || result.status != CandidateStatus::Missing;
            bindingMismatch = bindingMismatch
                || result.status == CandidateStatus::BindingMismatch;
            if (result.status == CandidateStatus::Unavailable)
            {
                ThrowJournalFailure();
            }
            if (result.status == CandidateStatus::UnsupportedVersion)
            {
                throw std::runtime_error(
                    "Cloud save journal version is unsupported.");
            }
            if (result.status == CandidateStatus::FatalCorruption)
            {
                ThrowCorruptJournal();
            }
            if (result.candidate)
            {
                candidates.push_back(std::move(*result.candidate));
            }
        }
        if (bindingMismatch)
        {
            throw std::runtime_error("Cloud save journal binding mismatch.");
        }
        if (candidates.empty())
        {
            if (anyExisting)
            {
                ThrowCorruptJournal();
            }
            return std::nullopt;
        }
        const auto sourceIndex = [&candidates](const CandidateSource source)
            -> std::size_t
        {
            const auto found = std::ranges::find_if(
                candidates,
                [source](const Candidate& candidate)
                {
                    return candidate.source == source;
                });
            return found == candidates.end()
                ? candidates.size()
                : static_cast<std::size_t>(found - candidates.begin());
        };
        const auto finalIndex = sourceIndex(CandidateSource::Final);
        const auto nextIndex = sourceIndex(CandidateSource::Next);
        const auto backupIndex = sourceIndex(CandidateSource::Backup);
        for (std::size_t left = 0u; left < candidates.size(); ++left)
        {
            for (std::size_t right = left + 1u;
                 right < candidates.size();
                 ++right)
            {
                if (candidates[left].state.generation
                        == candidates[right].state.generation
                    && (candidates[left].state.checksum
                            != candidates[right].state.checksum
                        || candidates[left].canonicalDocument
                            != candidates[right].canonicalDocument))
                {
                    ThrowCorruptJournal();
                }
            }
        }
        const auto directParent = [](const Candidate& parent,
                                     const Candidate& child)
        {
            return child.state.generation == parent.state.generation + 1u
                && child.state.parentGeneration == parent.state.generation
                && child.state.parentChecksum == parent.state.checksum;
        };
        if (nextIndex != candidates.size())
        {
            const auto& next = candidates[nextIndex];
            if ((finalIndex != candidates.size()
                    && candidates[finalIndex].state.generation
                        > next.state.generation)
                || (backupIndex != candidates.size()
                    && candidates[backupIndex].state.generation
                        > next.state.generation))
            {
                ThrowCorruptJournal();
            }
            if (finalIndex != candidates.size()
                && candidates[finalIndex].state.generation
                    == next.state.generation)
            {
                if (backupIndex != candidates.size()
                    && next.state.generation > 1u
                    && candidates[backupIndex].state.generation
                        == next.state.generation - 1u
                    && !directParent(candidates[backupIndex], next))
                {
                    ThrowCorruptJournal();
                }
                return std::move(candidates[finalIndex]);
            }
            if (backupIndex != candidates.size()
                && candidates[backupIndex].state.generation
                    == next.state.generation)
            {
                if (finalIndex != candidates.size()
                    && next.state.generation > 1u
                    && candidates[finalIndex].state.generation
                        == next.state.generation - 1u
                    && !directParent(candidates[finalIndex], next))
                {
                    ThrowCorruptJournal();
                }
                return std::move(candidates[backupIndex]);
            }
            if (next.state.generation > 1u)
            {
                const bool finalIsParent = finalIndex != candidates.size()
                    && directParent(candidates[finalIndex], next);
                const bool backupIsParent = backupIndex != candidates.size()
                    && directParent(candidates[backupIndex], next);
                if (!finalIsParent && !backupIsParent)
                {
                    ThrowCorruptJournal();
                }
                if (backupIsParent
                    && finalIndex != candidates.size()
                    && candidates[finalIndex].state.generation
                        < candidates[backupIndex].state.generation)
                {
                    ThrowCorruptJournal();
                }
            }
            return std::move(candidates[nextIndex]);
        }
        if (finalIndex != candidates.size())
        {
            const auto& final = candidates[finalIndex];
            if (backupIndex != candidates.size())
            {
                const auto& backup = candidates[backupIndex];
                if (backup.state.generation > final.state.generation
                    || (final.state.generation > 0u
                        && backup.state.generation
                            == final.state.generation - 1u
                        && !directParent(backup, final)))
                {
                    ThrowCorruptJournal();
                }
            }
            return std::move(candidates[finalIndex]);
        }
        return std::move(candidates[backupIndex]);
    }

    void DurableWrite(
        const std::filesystem::path& path,
        const std::string_view bytes,
        const bool injectFlushFailure)
    {
        RestrictedSecurity security;
        if (!security.Initialize(NO_INHERITANCE))
        {
            ThrowJournalFailure();
        }
        FileHandle file(CreateFileW(
            path.c_str(),
            GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC,
            0,
            &security.attributes,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
        {
            ThrowJournalFailure();
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_STANDARD_INFO standard{};
        if (GetFileInformationByHandleEx(
                file.value,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) == FALSE
            || GetFileInformationByHandleEx(
                file.value,
                FileStandardInfo,
                &standard,
                sizeof(standard)) == FALSE
            || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u
            || standard.NumberOfLinks != 1u
            || !HandleOwnerIsAllowed(
                file.value,
                security.CurrentUserSid(),
                security.systemSid)
            || SetSecurityInfo(
                file.value,
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION
                    | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                security.acl,
                nullptr) != ERROR_SUCCESS
            || !VerifyRestrictedAclHandle(
                file.value,
                false,
                security.CurrentUserSid(),
                security.systemSid))
        {
            ThrowJournalFailure();
        }
        LARGE_INTEGER beginning{};
        if (SetFilePointerEx(
                file.value,
                beginning,
                nullptr,
                FILE_BEGIN) == FALSE
            || SetEndOfFile(file.value) == FALSE)
        {
            ThrowJournalFailure();
        }
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            DWORD written{};
            if (WriteFile(
                    file.value,
                    bytes.data() + offset,
                    static_cast<DWORD>(remaining),
                    &written,
                    nullptr) == FALSE
                || written == 0u)
            {
                ThrowJournalFailure();
            }
            offset += written;
        }
        if ((injectFlushFailure
                && ConsumeFailPoint(
                    LamaPon::Detail::CloudSaveJournalTestFailPoint::
                        BeforeNextFlush))
            || FlushFileBuffers(file.value) == FALSE)
        {
            ThrowJournalFailure();
        }
    }

    bool PathExists(const std::filesystem::path& path)
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return true;
        }
        if (IsMissingError(GetLastError()))
        {
            return false;
        }
        ThrowJournalFailure();
    }

    void MoveReplace(
        const std::filesystem::path& source,
        const std::filesystem::path& destination)
    {
        if (MoveFileExW(
                source.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            ThrowJournalFailure();
        }
    }

    void StageDurableNext(
        const std::filesystem::path& finalPath,
        const std::string_view document,
        bool& nextPublished)
    {
        nextPublished = false;
        const auto writingPath = WithSuffix(finalPath, L".writing");
        const auto nextPath = WithSuffix(finalPath, L".next");
        DurableWrite(writingPath, document, true);
        MoveReplace(writingPath, nextPath);
        nextPublished = true;
        if (ConsumeFailPoint(
                LamaPon::Detail::CloudSaveJournalTestFailPoint::
                    AfterNextFlush))
        {
            ThrowJournalFailure();
        }
    }

    void PromoteCandidate(
        const Candidate& selected,
        const std::filesystem::path& finalPath,
        const std::string_view binding)
    {
        if (selected.source == CandidateSource::Final)
        {
            return;
        }
        const auto nextPath = WithSuffix(finalPath, L".next");
        const auto backupPath = WithSuffix(finalPath, L".bak");
        if (selected.source == CandidateSource::Next)
        {
            auto final = ReadCandidate(
                finalPath,
                CandidateSource::Final,
                binding);
            if (final.status == CandidateStatus::Unavailable
                || final.status == CandidateStatus::BindingMismatch
                || final.status == CandidateStatus::UnsupportedVersion)
            {
                ThrowJournalFailure();
            }
            if (final.candidate)
            {
                if (final.candidate->state.generation
                        != selected.state.parentGeneration
                    || final.candidate->state.checksum
                        != selected.state.parentChecksum)
                {
                    ThrowCorruptJournal();
                }
                MoveReplace(finalPath, backupPath);
            }
            MoveReplace(nextPath, finalPath);
            return;
        }
        bool nextPublished{};
        StageDurableNext(
            finalPath,
            selected.canonicalDocument,
            nextPublished);
        (void)nextPublished;
        MoveReplace(nextPath, finalPath);
    }

    void Publish(
        const std::filesystem::path& finalPath,
        const std::string_view document,
        bool& nextPublished)
    {
        const auto nextPath = WithSuffix(finalPath, L".next");
        const auto backupPath = WithSuffix(finalPath, L".bak");
        StageDurableNext(finalPath, document, nextPublished);
        if (PathExists(finalPath))
        {
            MoveReplace(finalPath, backupPath);
        }
        MoveReplace(nextPath, finalPath);
    }
}

namespace LamaPon::Detail
{
    void SetCloudSaveJournalTestFailPoint(
        const CloudSaveJournalTestFailPoint failPoint) noexcept
    {
        JournalFailPoint.store(failPoint, std::memory_order_release);
    }

    bool IsCloudSaveJournalLockExclusiveForTesting(
        const std::filesystem::path& lockPath)
    {
        auto held = AcquireLock(lockPath);
        try
        {
            auto peer = AcquireLock(lockPath);
            return false;
        }
        catch (const CloudSaveJournalBusyError&)
        {
            return true;
        }
    }

    struct CloudSaveProfileSessionLease::Implementation final
    {
        ~Implementation()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
        }

        HANDLE handle{ INVALID_HANDLE_VALUE };
    };

    CloudSaveProfileSessionLease::CloudSaveProfileSessionLease(
        CloudSaveJournal& journal)
    {
        const auto stateDirectory = journal.FilePath().parent_path();
        ValidateExistingPathComponents(stateDirectory);
        auto held = AcquireLock(stateDirectory / L"profile.session.lock");
        auto implementation = std::make_unique<Implementation>();
        implementation->handle =
            std::exchange(held.value, INVALID_HANDLE_VALUE);
        m_implementation = std::move(implementation);
    }

    CloudSaveProfileSessionLease::~CloudSaveProfileSessionLease() = default;

    struct CloudSaveJournal::Implementation final
    {
        PersistenceProfilePaths accountProfile;
        std::filesystem::path filePath;
        std::filesystem::path lockPath;
        std::string binding;
        JournalState state;
        bool blocked{};

        void EnsureAvailable() const
        {
            if (blocked)
            {
                throw std::runtime_error(
                    "Cloud save journal is unavailable after a write failure.");
            }
        }

        void Persist(JournalState nextState)
        {
            EnsureAvailable();
            FileHandle lock;
            try
            {
                lock = AcquireLock(lockPath);
            }
            catch (const CloudSaveJournalBusyError&)
            {
                // diskへ触れる前の通常競合なので同instanceから再試行できます。
                throw;
            }
            catch (...)
            {
                blocked = true;
                throw;
            }
            bool newStatePrepared{};
            bool newStatePublished{};
            try
            {
                auto selected = SelectCandidate(filePath, binding);
                const auto diskGeneration = selected
                    ? selected->state.generation
                    : 0u;
                const auto diskChecksum = selected
                    ? selected->state.checksum
                    : std::string{};
                if (diskGeneration != state.generation
                    || diskChecksum != state.checksum)
                {
                    throw std::runtime_error(
                        "Cloud save journal generation changed.");
                }
                if (selected)
                {
                    PromoteCandidate(*selected, filePath, binding);
                }
                if (state.generation
                    == std::numeric_limits<std::uint64_t>::max())
                {
                    throw std::overflow_error(
                        "Cloud save journal generation is exhausted.");
                }
                nextState.parentGeneration = state.generation;
                nextState.parentChecksum = state.checksum;
                nextState.generation = state.generation + 1u;
                nextState.schemaVersion = 2u;
                auto document = SerializeState(nextState, binding);
                newStatePrepared = true;
                Publish(filePath, document, newStatePublished);
                state = std::move(nextState);
            }
            catch (...)
            {
                bool committedStateRecovered{};
                try
                {
                    auto recovered = SelectCandidate(filePath, binding);
                    if (!recovered)
                    {
                        if (state.generation != 0u)
                        {
                            blocked = true;
                        }
                    }
                    else if (newStatePrepared && newStatePublished
                        && recovered->state.generation
                            == nextState.generation
                        && recovered->state.checksum
                            == nextState.checksum)
                    {
                        PromoteCandidate(*recovered, filePath, binding);
                        state = std::move(recovered->state);
                        committedStateRecovered = true;
                    }
                    else if (recovered->state.generation == state.generation
                        && recovered->state.checksum == state.checksum)
                    {
                        PromoteCandidate(*recovered, filePath, binding);
                        state = std::move(recovered->state);
                    }
                    else
                    {
                        blocked = true;
                    }
                }
                catch (...)
                {
                    blocked = true;
                }
                if (committedStateRecovered)
                {
                    return;
                }
                throw;
            }
        }
    };

    CloudSaveJournal::CloudSaveJournal(
        std::filesystem::path trustedUserDataDirectory,
        PersistenceProfilePaths accountProfile,
        std::string gameId,
        std::string environmentId,
        std::string backendBaseUrl,
        const bool allowInsecureLoopback)
        : m_implementation(std::make_unique<Implementation>())
    {
        if (accountProfile.isGuest
            || !IsLowerHex(accountProfile.accountStorageKey, 64u)
            || !IsSafeOnlineNamespaceId(gameId, 128u)
            || !IsSafeOnlineNamespaceId(environmentId, 64u))
        {
            throw std::invalid_argument(
                "Cloud save journal account profile is invalid.");
        }
        if (!trustedUserDataDirectory.is_absolute())
        {
            throw std::invalid_argument(
                "Cloud save trusted user data path must be absolute.");
        }
        const auto trustedUserData =
            NormalizedAbsolute(trustedUserDataDirectory);
        const auto drive = trustedUserData.root_name().native();
        if (drive.size() != 2u
            || !((drive[0] >= L'A' && drive[0] <= L'Z')
                || (drive[0] >= L'a' && drive[0] <= L'z'))
            || drive[1] != L':')
        {
            throw std::invalid_argument(
                "Cloud save trusted user data path must be local.");
        }
        if (GetDriveTypeW(trustedUserData.root_path().c_str())
            != DRIVE_FIXED)
        {
            throw std::invalid_argument(
                "Cloud save trusted user data path must use a local fixed drive.");
        }
        const auto engineRoot = trustedUserData.parent_path();
        const auto expectedRoot = engineRoot
            / L"OnlineProfiles"
            / LamaPon::PathFromUtf8(accountProfile.accountStorageKey);
        const auto root = NormalizedAbsolute(accountProfile.rootDirectory);
        ValidateExistingPathComponents(trustedUserData);
        ValidateExistingPathComponents(expectedRoot);
        const auto keyPath = LamaPon::PathToUtf8(root.filename());
        if (keyPath != accountProfile.accountStorageKey
            || !EquivalentPaths(root, expectedRoot)
            || !EquivalentPaths(
                accountProfile.playerPrefsFile,
                root / L"PlayerPrefs.json")
            || !EquivalentPaths(
                accountProfile.saveDataDirectory,
                root / L"Saves"))
        {
            throw std::invalid_argument(
                "Cloud save journal account profile structure is invalid.");
        }

        const auto normalizedBackendBaseUrl = NormalizedBackendBaseUrl(
            std::move(backendBaseUrl),
            allowInsecureLoopback);
        EnsurePlainDirectory(engineRoot, false);
        const auto stateRoot = engineRoot / L"OnlineState";
        EnsurePlainDirectory(stateRoot, true);
        const auto journalDirectory =
            stateRoot / LamaPon::PathFromUtf8(accountProfile.accountStorageKey);
        EnsurePlainDirectory(journalDirectory, true);

        m_implementation->accountProfile = std::move(accountProfile);
        m_implementation->filePath =
            journalDirectory / L"CloudSaveJournal.json";
        m_implementation->lockPath =
            journalDirectory / L"CloudSaveJournal.lock";
        m_implementation->binding = MakeBinding(
            root,
            m_implementation->accountProfile.accountStorageKey,
            gameId,
            environmentId,
            normalizedBackendBaseUrl);

        auto lock = AcquireLock(m_implementation->lockPath);
        auto selected = SelectCandidate(
            m_implementation->filePath,
            m_implementation->binding);
        if (selected)
        {
            PromoteCandidate(
                *selected,
                m_implementation->filePath,
                m_implementation->binding);
            m_implementation->state = std::move(selected->state);
        }
    }

    CloudSaveJournal::~CloudSaveJournal() = default;

    void CloudSaveJournal::RecordBaseline(
        const CloudSaveSnapshot& snapshot)
    {
        m_implementation->EnsureAvailable();
        (void)SnapshotJson(snapshot);
        auto next = m_implementation->state;
        const auto index = FindEntry(next.entries, snapshot.resource);
        if (index == next.entries.size())
        {
            Entry entry;
            entry.resource = snapshot.resource;
            entry.baseline = snapshot;
            next.entries.push_back(std::move(entry));
        }
        else
        {
            auto& entry = next.entries[index];
            if (entry.pending || entry.conflict)
            {
                throw std::logic_error(
                    "Cloud save resource has a pending mutation.");
            }
            entry.baseline = snapshot;
            entry.localDeleteIntent = false;
        }
        m_implementation->Persist(std::move(next));
    }

    void CloudSaveJournal::QueuePut(
        const CloudSaveResource& resource,
        const std::vector<std::uint8_t>& content,
        const std::string_view mutationId,
        std::optional<std::string> baseEtag)
    {
        m_implementation->EnsureAvailable();
        ValidateContent(resource, content);
        if (!IsCanonicalMutationId(mutationId)
            || (baseEtag && !IsStrongEtag(*baseEtag))
            || MutationIdInUse(
                m_implementation->state.entries,
                mutationId))
        {
            throw std::invalid_argument("Cloud save mutation is invalid.");
        }
        auto next = m_implementation->state;
        auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size())
        {
            Entry entry;
            entry.resource = resource;
            next.entries.push_back(std::move(entry));
            index = next.entries.size() - 1u;
        }
        auto& entry = next.entries[index];
        if (entry.pending || entry.conflict)
        {
            throw std::logic_error(
                "Cloud save resource already has a pending mutation.");
        }
        entry.localDeleteIntent = false;
        entry.pending = CloudSavePendingMutation{
            resource,
            CloudSavePendingKind::Put,
            std::string(mutationId),
            std::move(baseEtag),
            content,
            ContentHash(content)
        };
        m_implementation->Persist(std::move(next));
    }

    void CloudSaveJournal::QueueDelete(
        const CloudSaveResource& resource,
        const std::string_view mutationId,
        const std::string_view baseEtag)
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        if (!IsCanonicalMutationId(mutationId)
            || !IsStrongEtag(baseEtag)
            || MutationIdInUse(
                m_implementation->state.entries,
                mutationId))
        {
            throw std::invalid_argument("Cloud save delete is invalid.");
        }
        auto next = m_implementation->state;
        auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size())
        {
            Entry entry;
            entry.resource = resource;
            next.entries.push_back(std::move(entry));
            index = next.entries.size() - 1u;
        }
        auto& entry = next.entries[index];
        if (entry.pending || entry.conflict)
        {
            throw std::logic_error(
                "Cloud save resource already has a pending mutation.");
        }
        entry.localDeleteIntent = false;
        entry.pending = CloudSavePendingMutation{
            resource,
            CloudSavePendingKind::Delete,
            std::string(mutationId),
            std::string(baseEtag),
            {},
            {}
        };
        m_implementation->Persist(std::move(next));
    }

    void CloudSaveJournal::RecordLocalDeleteIntent(
        const CloudSaveResource& resource)
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        auto next = m_implementation->state;
        auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size())
        {
            Entry entry;
            entry.resource = resource;
            next.entries.push_back(std::move(entry));
            index = next.entries.size() - 1u;
        }
        if (next.entries[index].localDeleteIntent)
        {
            return;
        }
        next.entries[index].localDeleteIntent = true;
        m_implementation->Persist(std::move(next));
    }

    void CloudSaveJournal::ClearLocalDeleteIntent(
        const CloudSaveResource& resource)
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        auto next = m_implementation->state;
        const auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size()
            || !next.entries[index].localDeleteIntent)
        {
            return;
        }
        auto& entry = next.entries[index];
        entry.localDeleteIntent = false;
        if (!entry.baseline && !entry.pending && !entry.conflict)
        {
            next.entries.erase(next.entries.begin()
                + static_cast<std::ptrdiff_t>(index));
        }
        m_implementation->Persist(std::move(next));
    }

    bool CloudSaveJournal::HasLocalDeleteIntent(
        const CloudSaveResource& resource) const
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        const auto index = FindEntry(
            m_implementation->state.entries,
            resource);
        return index != m_implementation->state.entries.size()
            && m_implementation->state.entries[index].localDeleteIntent;
    }

    std::vector<CloudSavePendingMutation>
        CloudSaveJournal::Dispatchable() const
    {
        m_implementation->EnsureAvailable();
        std::vector<CloudSavePendingMutation> result;
        for (const auto& entry : m_implementation->state.entries)
        {
            if (entry.pending && !entry.conflict)
            {
                result.push_back(*entry.pending);
            }
        }
        return result;
    }

    void CloudSaveJournal::RecordSuccess(
        const CloudSaveResource& resource,
        const std::string_view mutationId,
        const CloudSaveSnapshot& snapshot)
    {
        m_implementation->EnsureAvailable();
        (void)SnapshotJson(snapshot);
        if (!EquivalentResources(resource, snapshot.resource))
        {
            throw std::invalid_argument("Cloud save success resource mismatch.");
        }
        auto next = m_implementation->state;
        const auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size()
            || !next.entries[index].pending
            || next.entries[index].pending->mutationId != mutationId
            || next.entries[index].conflict)
        {
            throw std::logic_error("Cloud save mutation is not pending.");
        }
        auto& entry = next.entries[index];
        if (entry.pending->kind == CloudSavePendingKind::Put)
        {
            if (snapshot.deleted
                || snapshot.content != entry.pending->content
                || snapshot.sha256 != entry.pending->sha256)
            {
                throw std::invalid_argument(
                    "Cloud save success snapshot does not match the mutation.");
            }
        }
        else if (!snapshot.deleted)
        {
            throw std::invalid_argument(
                "Cloud save delete did not return a tombstone.");
        }
        entry.baseline = snapshot;
        entry.pending.reset();
        entry.conflict.reset();
        m_implementation->Persist(std::move(next));
    }

    void CloudSaveJournal::RecordConflict(
        const CloudSaveResource& resource,
        const std::string_view mutationId,
        const CloudSaveSnapshot& remoteSnapshot)
    {
        m_implementation->EnsureAvailable();
        (void)SnapshotJson(remoteSnapshot);
        if (!EquivalentResources(resource, remoteSnapshot.resource))
        {
            throw std::invalid_argument("Cloud save conflict resource mismatch.");
        }
        auto next = m_implementation->state;
        const auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size()
            || !next.entries[index].pending
            || next.entries[index].pending->mutationId != mutationId
            || next.entries[index].conflict)
        {
            throw std::logic_error("Cloud save mutation is not pending.");
        }
        next.entries[index].conflict = remoteSnapshot;
        m_implementation->Persist(std::move(next));
    }

    void CloudSaveJournal::ResolveConflict(
        const CloudSaveResource& resource,
        const std::string_view expectedMutationId,
        const CloudSaveConflictResolution resolution,
        const std::string_view replacementMutationId)
    {
        m_implementation->EnsureAvailable();
        auto next = m_implementation->state;
        const auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size()
            || !next.entries[index].pending
            || !next.entries[index].conflict
            || next.entries[index].pending->mutationId
                != expectedMutationId)
        {
            throw std::logic_error("Cloud save conflict does not exist.");
        }
        auto& entry = next.entries[index];
        switch (resolution)
        {
        case CloudSaveConflictResolution::UseRemote:
            if (!replacementMutationId.empty())
            {
                throw std::invalid_argument(
                    "Remote conflict resolution needs no mutation id.");
            }
            entry.baseline = entry.conflict;
            entry.pending.reset();
            entry.conflict.reset();
            break;
        case CloudSaveConflictResolution::RetryLocal:
            if (!IsCanonicalMutationId(replacementMutationId)
                || replacementMutationId == entry.pending->mutationId
                || MutationIdInUse(
                    next.entries,
                    replacementMutationId,
                    &entry))
            {
                throw std::invalid_argument(
                    "Cloud save replacement mutation id is invalid.");
            }
            entry.pending->mutationId = replacementMutationId;
            entry.pending->baseEtag = entry.conflict->etag;
            entry.conflict.reset();
            break;
        default:
            throw std::invalid_argument(
                "Cloud save conflict resolution is invalid.");
        }
        m_implementation->Persist(std::move(next));
    }

    std::optional<CloudSaveSnapshot> CloudSaveJournal::Baseline(
        const CloudSaveResource& resource) const
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        const auto index = FindEntry(
            m_implementation->state.entries,
            resource);
        return index == m_implementation->state.entries.size()
            ? std::nullopt
            : m_implementation->state.entries[index].baseline;
    }

    std::optional<CloudSaveSnapshot> CloudSaveJournal::Conflict(
        const CloudSaveResource& resource) const
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        const auto index = FindEntry(
            m_implementation->state.entries,
            resource);
        return index == m_implementation->state.entries.size()
            ? std::nullopt
            : m_implementation->state.entries[index].conflict;
    }

    std::optional<CloudSavePendingMutation> CloudSaveJournal::Pending(
        const CloudSaveResource& resource) const
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        const auto index = FindEntry(
            m_implementation->state.entries,
            resource);
        return index == m_implementation->state.entries.size()
            ? std::nullopt
            : m_implementation->state.entries[index].pending;
    }

    std::vector<CloudSaveResource> CloudSaveJournal::Resources() const
    {
        m_implementation->EnsureAvailable();
        std::vector<CloudSaveResource> result;
        result.reserve(m_implementation->state.entries.size());
        for (const auto& entry : m_implementation->state.entries)
        {
            result.push_back(entry.resource);
        }
        return result;
    }

    bool CloudSaveJournal::HasPending(
        const CloudSaveResource& resource) const
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        const auto index = FindEntry(
            m_implementation->state.entries,
            resource);
        return index != m_implementation->state.entries.size()
            && m_implementation->state.entries[index].pending.has_value();
    }

    std::uint64_t CloudSaveJournal::Generation() const
    {
        m_implementation->EnsureAvailable();
        return m_implementation->state.generation;
    }

    const std::filesystem::path& CloudSaveJournal::FilePath() const
    {
        m_implementation->EnsureAvailable();
        return m_implementation->filePath;
    }
}
