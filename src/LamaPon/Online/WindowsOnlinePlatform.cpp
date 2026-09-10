#include "LamaPon/Online/WindowsOnlinePlatform.h"

#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>

#include <aclapi.h>
#include <KnownFolders.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr std::size_t MaximumRefreshTokenBytes = 8192;
    constexpr std::size_t MaximumProtectedBlobBytes = 64 * 1024;
    constexpr std::size_t EnvelopeHeaderBytes = 16;
    constexpr std::array<std::uint8_t, 8> FileMagic{
        'L', 'P', 'O', 'N', 'A', 'U', 'T', 'H'
    };
    constexpr std::array<std::uint8_t, 8> PlaintextMagic{
        'L', 'P', 'O', 'N', 'R', 'T', 'K', 'N'
    };
    constexpr std::string_view EntropyLabel =
        "LamaPon.Online.RefreshToken/v1";
    // パス名の疑似匿名化とWindows名のalias回避に使う固定鍵です。
    // 秘密鍵ではなく、HMAC入力の用途を将来も固定するための定数です。
    constexpr LamaPon::Crypto::AesKey CredentialPathHashKey{
        'L', 'a', 'm', 'a', 'P', 'o', 'n', '.',
        'O', 'n', 'l', 'i', 'n', 'e', '.', 'P',
        'a', 't', 'h', 'H', 'a', 's', 'h', '/',
        'v', '1'
    };

    struct HandleGuard final
    {
        HANDLE value{ INVALID_HANDLE_VALUE };

        ~HandleGuard()
        {
            Reset();
        }

        void Reset() noexcept
        {
            if (value != nullptr && value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
            value = INVALID_HANDLE_VALUE;
        }
    };

    struct KnownFolderGuard final
    {
        PWSTR value{};

        ~KnownFolderGuard()
        {
            CoTaskMemFree(value);
        }
    };

    struct RestrictedSecurity final
    {
        HandleGuard processToken;
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

        [[nodiscard]] bool Initialize(const DWORD inheritance)
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

            SID_IDENTIFIER_AUTHORITY ntAuthority =
                SECURITY_NT_AUTHORITY;
            if (AllocateAndInitializeSid(
                    &ntAuthority,
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

            auto* currentUser = reinterpret_cast<TOKEN_USER*>(
                tokenUser.data());
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
                    &acl) != ERROR_SUCCESS)
            {
                return false;
            }
            if (InitializeSecurityDescriptor(
                    &descriptor,
                    SECURITY_DESCRIPTOR_REVISION) == FALSE
                || SetSecurityDescriptorDacl(
                    &descriptor,
                    TRUE,
                    acl,
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
    };

    [[nodiscard]] LamaPon::Detail::OnlinePlatformResult Success()
    {
        return { true, {}, {} };
    }

    [[nodiscard]] LamaPon::Detail::OnlinePlatformResult Failure(
        std::string code,
        std::string message)
    {
        return {
            false,
            std::move(code),
            std::move(message)
        };
    }

    [[nodiscard]] LamaPon::Detail::RefreshTokenLoadResult LoadFailure(
        const LamaPon::Detail::RefreshTokenLoadStatus status,
        std::string code,
        std::string message)
    {
        LamaPon::Detail::RefreshTokenLoadResult result;
        result.status = status;
        result.errorCode = std::move(code);
        result.errorMessage = std::move(message);
        return result;
    }

    [[nodiscard]] bool IsSafeIdentifier(
        const std::string_view value,
        const std::size_t maximumBytes)
    {
        return !value.empty()
            && value.size() <= maximumBytes
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return (character >= 'a' && character <= 'z')
                        || (character >= 'A' && character <= 'Z')
                        || (character >= '0' && character <= '9')
                        || character == '-'
                        || character == '_'
                        || character == '.';
                });
    }

    [[nodiscard]] bool IsSafeRefreshToken(
        const std::string_view value)
    {
        return !value.empty()
            && value.size() <= MaximumRefreshTokenBytes
            && std::ranges::none_of(
                value,
                [](const unsigned char character)
                {
                    return character <= 0x20 || character == 0x7f;
                });
    }

    void AppendU16(
        std::vector<std::uint8_t>& bytes,
        const std::uint16_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    }

    void AppendU32(
        std::vector<std::uint8_t>& bytes,
        const std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<std::uint8_t>(
                (value >> shift) & 0xffu));
        }
    }

    [[nodiscard]] std::uint16_t ReadU16(
        const std::uint8_t* bytes) noexcept
    {
        return static_cast<std::uint16_t>(bytes[0])
            | static_cast<std::uint16_t>(bytes[1] << 8u);
    }

    [[nodiscard]] std::uint32_t ReadU32(
        const std::uint8_t* bytes) noexcept
    {
        std::uint32_t value{};
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            value |= static_cast<std::uint32_t>(
                bytes[shift / 8]) << shift;
        }
        return value;
    }

    [[nodiscard]] std::vector<std::uint8_t> BuildEntropy(
        const std::string_view gameId,
        const std::string_view environmentId)
    {
        if (!IsSafeIdentifier(gameId, 128)
            || !IsSafeIdentifier(environmentId, 64))
        {
            throw std::invalid_argument(
                "Online credential identifiers must contain only ASCII letters, digits, '.', '_' or '-'.");
        }
        std::vector<std::uint8_t> entropy;
        entropy.reserve(
            EntropyLabel.size() + gameId.size()
            + environmentId.size() + 8);
        entropy.insert(
            entropy.end(),
            EntropyLabel.begin(),
            EntropyLabel.end());
        AppendU32(
            entropy,
            static_cast<std::uint32_t>(gameId.size()));
        entropy.insert(entropy.end(), gameId.begin(), gameId.end());
        AppendU32(
            entropy,
            static_cast<std::uint32_t>(environmentId.size()));
        entropy.insert(
            entropy.end(),
            environmentId.begin(),
            environmentId.end());
        return entropy;
    }

    [[nodiscard]] std::string CredentialPathHash(
        const std::vector<std::uint8_t>& entropy)
    {
        const auto digest = LamaPon::Crypto::Hmac(
            CredentialPathHashKey,
            entropy.data(),
            entropy.size());
        constexpr char HexDigits[] = "0123456789abcdef";
        std::string result(digest.size() * 2, '0');
        for (std::size_t index = 0; index < digest.size(); ++index)
        {
            result[index * 2] = HexDigits[digest[index] >> 4u];
            result[index * 2 + 1] = HexDigits[digest[index] & 0x0fu];
        }
        return result;
    }

    [[nodiscard]] bool ApplyRestrictedAcl(
        const std::filesystem::path& path,
        const bool directory)
    {
        RestrictedSecurity security;
        const DWORD inheritance = directory
            ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
            : NO_INHERITANCE;
        if (!security.Initialize(inheritance))
        {
            return false;
        }
        return SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION
                | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            security.acl,
            nullptr) == ERROR_SUCCESS;
    }

    [[nodiscard]] bool WriteAll(
        const HANDLE file,
        const std::vector<std::uint8_t>& bytes)
    {
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            DWORD written{};
            if (WriteFile(
                    file,
                    bytes.data() + offset,
                    static_cast<DWORD>(remaining),
                    &written,
                    nullptr) == FALSE
                || written == 0)
            {
                return false;
            }
            offset += written;
        }
        return true;
    }

    [[nodiscard]] bool ReadAll(
        const HANDLE file,
        std::vector<std::uint8_t>& bytes)
    {
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            DWORD read{};
            if (ReadFile(
                    file,
                    bytes.data() + offset,
                    static_cast<DWORD>(remaining),
                    &read,
                    nullptr) == FALSE
                || read == 0)
            {
                return false;
            }
            offset += read;
        }
        return true;
    }

    [[nodiscard]] std::filesystem::path LocalCredentialPath(
        const std::string_view gameId,
        const std::string_view environmentId)
    {
        KnownFolderGuard localAppData;
        if (FAILED(SHGetKnownFolderPath(
                FOLDERID_LocalAppData,
                KF_FLAG_DEFAULT,
                nullptr,
                &localAppData.value))
            || localAppData.value == nullptr
            || localAppData.value[0] == L'\0')
        {
            return {};
        }
        auto entropy = BuildEntropy(gameId, environmentId);
        const auto pathHash = CredentialPathHash(entropy);
        // entropyには生のIDが入るため、パスを組み立てた時点で消します。
        LamaPon::Crypto::SecureErase(entropy);
        return std::filesystem::path(localAppData.value)
            / L"LamaPon"
            / L"Online"
            / LamaPon::PathFromUtf8(pathHash)
            / L"session.bin";
    }

    [[nodiscard]] bool DeleteIfPresent(
        const std::filesystem::path& path) noexcept
    {
        if (DeleteFileW(path.c_str()) != FALSE)
        {
            return true;
        }
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND;
    }

    [[nodiscard]] bool IsLoopbackHost(
        const std::wstring_view host)
    {
        std::wstring lower(host);
        std::ranges::transform(
            lower,
            lower.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(towlower(character));
            });
        return lower == L"localhost"
            || lower == L"127.0.0.1"
            || lower == L"::1"
            || lower == L"[::1]";
    }

    [[nodiscard]] bool IsSafeAuthorizationUrl(
        const std::string_view url,
        const bool allowInsecureLoopback,
        std::wstring& wide)
    {
        if (url.empty()
            || url.size() > 2048
            || std::ranges::any_of(
                url,
                [](const unsigned char character)
                {
                    return character <= 0x20
                        || character == 0x7f
                        || character == '\\'
                        || character == '"';
                })
            || url.find('#') != std::string_view::npos)
        {
            return false;
        }
        const auto schemeMarker = url.find("://");
        if (schemeMarker == std::string_view::npos)
        {
            return false;
        }
        const auto authorityEnd = url.find_first_of(
            "/?#",
            schemeMarker + 3);
        const auto authority = url.substr(
            schemeMarker + 3,
            authorityEnd == std::string_view::npos
                ? std::string_view::npos
                : authorityEnd - schemeMarker - 3);
        if (authority.empty()
            || authority.find('@') != std::string_view::npos)
        {
            return false;
        }

        wide = LamaPon::Utf8ToWide(url);
        if (wide.empty())
        {
            return false;
        }
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUserNameLength = static_cast<DWORD>(-1);
        components.dwPasswordLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (WinHttpCrackUrl(
                wide.c_str(),
                0,
                ICU_REJECT_USERPWD,
                &components) == FALSE
            || components.dwHostNameLength == 0
            || components.dwUserNameLength != 0
            || components.dwPasswordLength != 0)
        {
            return false;
        }
        const std::wstring_view host(
            components.lpszHostName,
            components.dwHostNameLength);
        if (components.nScheme == INTERNET_SCHEME_HTTPS)
        {
            return true;
        }
        return allowInsecureLoopback
            && components.nScheme == INTERNET_SCHEME_HTTP
            && IsLoopbackHost(host);
    }
}

namespace LamaPon::Detail
{
    WindowsRefreshTokenStore::WindowsRefreshTokenStore(
        std::filesystem::path filePath,
        std::string gameId,
        std::string environmentId)
        : m_filePath(std::move(filePath))
        , m_entropy(BuildEntropy(gameId, environmentId))
        , m_storageAvailable(!m_filePath.empty())
    {
    }

    RefreshTokenLoadResult WindowsRefreshTokenStore::Load()
    {
        if (!m_storageAvailable)
        {
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }

        HandleGuard file;
        file.value = CreateFileW(
            m_filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file.value == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND
                || error == ERROR_PATH_NOT_FOUND)
            {
                RefreshTokenLoadResult result;
                result.status = RefreshTokenLoadStatus::NotFound;
                return result;
            }
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }

        LARGE_INTEGER size{};
        if (GetFileSizeEx(file.value, &size) == FALSE)
        {
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }
        if (size.QuadPart < static_cast<LONGLONG>(EnvelopeHeaderBytes)
            || size.QuadPart > static_cast<LONGLONG>(
                EnvelopeHeaderBytes + MaximumProtectedBlobBytes))
        {
            return LoadFailure(
                RefreshTokenLoadStatus::Corrupt,
                "credential_corrupt",
                "The saved online session is invalid.");
        }

        std::vector<std::uint8_t> fileBytes(
            static_cast<std::size_t>(size.QuadPart));
        if (!ReadAll(file.value, fileBytes))
        {
            Crypto::SecureErase(fileBytes);
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }
        file.Reset();

        const bool validEnvelope = std::equal(
                FileMagic.begin(),
                FileMagic.end(),
                fileBytes.begin())
            && ReadU16(fileBytes.data() + 8) == 1
            && ReadU16(fileBytes.data() + 10) == 0;
        const auto protectedSize = ReadU32(fileBytes.data() + 12);
        if (!validEnvelope
            || protectedSize == 0
            || protectedSize > MaximumProtectedBlobBytes
            || protectedSize != fileBytes.size() - EnvelopeHeaderBytes)
        {
            Crypto::SecureErase(fileBytes);
            return LoadFailure(
                RefreshTokenLoadStatus::Corrupt,
                "credential_corrupt",
                "The saved online session is invalid.");
        }

        auto unprotected = Crypto::UnprotectForCurrentUser(
            fileBytes.data() + EnvelopeHeaderBytes,
            protectedSize,
            m_entropy.data(),
            m_entropy.size());
        Crypto::SecureErase(fileBytes);
        if (!unprotected.Succeeded())
        {
            const auto status = unprotected.status
                    == Crypto::CurrentUserProtectionStatus::InvalidData
                ? RefreshTokenLoadStatus::Corrupt
                : RefreshTokenLoadStatus::Unavailable;
            Crypto::SecureErase(unprotected.data);
            return LoadFailure(
                status,
                status == RefreshTokenLoadStatus::Corrupt
                    ? "credential_corrupt"
                    : "credential_protection_unavailable",
                status == RefreshTokenLoadStatus::Corrupt
                    ? "The saved online session is invalid."
                    : "Secure credential storage is unavailable.");
        }

        auto& plaintext = unprotected.data;
        const bool validPlaintext = plaintext.size()
                >= EnvelopeHeaderBytes
            && std::equal(
                PlaintextMagic.begin(),
                PlaintextMagic.end(),
                plaintext.begin())
            && ReadU16(plaintext.data() + 8) == 1
            && ReadU16(plaintext.data() + 10) == 0;
        const auto tokenSize = validPlaintext
            ? ReadU32(plaintext.data() + 12)
            : 0;
        if (!validPlaintext
            || tokenSize == 0
            || tokenSize > MaximumRefreshTokenBytes
            || tokenSize != plaintext.size() - EnvelopeHeaderBytes)
        {
            Crypto::SecureErase(plaintext);
            return LoadFailure(
                RefreshTokenLoadStatus::Corrupt,
                "credential_corrupt",
                "The saved online session is invalid.");
        }

        std::string refreshToken(
            reinterpret_cast<const char*>(
                plaintext.data() + EnvelopeHeaderBytes),
            tokenSize);
        if (!IsSafeRefreshToken(refreshToken))
        {
            Crypto::SecureErase(refreshToken);
            Crypto::SecureErase(plaintext);
            return LoadFailure(
                RefreshTokenLoadStatus::Corrupt,
                "credential_corrupt",
                "The saved online session is invalid.");
        }
        Crypto::SecureErase(plaintext);

        RefreshTokenLoadResult result;
        result.status = RefreshTokenLoadStatus::Loaded;
        result.refreshToken = std::move(refreshToken);
        return result;
    }

    OnlinePlatformResult WindowsRefreshTokenStore::Save(
        const std::string_view refreshToken)
    {
        if (!IsSafeRefreshToken(refreshToken))
        {
            return Failure(
                "credential_invalid_token",
                "The refresh token is invalid.");
        }
        if (!m_storageAvailable)
        {
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }

        std::vector<std::uint8_t> plaintext;
        plaintext.reserve(EnvelopeHeaderBytes + refreshToken.size());
        plaintext.insert(
            plaintext.end(),
            PlaintextMagic.begin(),
            PlaintextMagic.end());
        AppendU16(plaintext, 1);
        AppendU16(plaintext, 0);
        AppendU32(
            plaintext,
            static_cast<std::uint32_t>(refreshToken.size()));
        plaintext.insert(
            plaintext.end(),
            refreshToken.begin(),
            refreshToken.end());
        auto protectedData = Crypto::ProtectForCurrentUser(
            plaintext.data(),
            plaintext.size(),
            m_entropy.data(),
            m_entropy.size());
        Crypto::SecureErase(plaintext);
        if (!protectedData.Succeeded()
            || protectedData.data.empty()
            || protectedData.data.size() > MaximumProtectedBlobBytes)
        {
            Crypto::SecureErase(protectedData.data);
            return Failure(
                "credential_protection_unavailable",
                "The online session could not be protected.");
        }

        std::vector<std::uint8_t> fileBytes;
        fileBytes.reserve(
            EnvelopeHeaderBytes + protectedData.data.size());
        fileBytes.insert(
            fileBytes.end(),
            FileMagic.begin(),
            FileMagic.end());
        AppendU16(fileBytes, 1);
        AppendU16(fileBytes, 0);
        AppendU32(
            fileBytes,
            static_cast<std::uint32_t>(protectedData.data.size()));
        fileBytes.insert(
            fileBytes.end(),
            protectedData.data.begin(),
            protectedData.data.end());
        Crypto::SecureErase(protectedData.data);

        std::error_code directoryError;
        if (!EnsureDirectoryExists(
                m_filePath.parent_path(),
                directoryError)
            || !ApplyRestrictedAcl(
                m_filePath.parent_path(),
                true))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }

        RestrictedSecurity fileSecurity;
        if (!fileSecurity.Initialize(NO_INHERITANCE))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        auto temporary = m_filePath;
        temporary += L".tmp";
        HandleGuard file;
        file.value = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            &fileSecurity.attributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
            nullptr);
        if (file.value == INVALID_HANDLE_VALUE
            || !WriteAll(file.value, fileBytes)
            || FlushFileBuffers(file.value) == FALSE)
        {
            file.Reset();
            DeleteFileW(temporary.c_str());
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_write_failed",
                "The online session could not be saved.");
        }
        file.Reset();
        Crypto::SecureErase(fileBytes);
        if (!ApplyRestrictedAcl(temporary, false))
        {
            DeleteFileW(temporary.c_str());
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        if (MoveFileExW(
                temporary.c_str(),
                m_filePath.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            DeleteFileW(temporary.c_str());
            return Failure(
                "credential_write_failed",
                "The online session could not be saved.");
        }
        return Success();
    }

    OnlinePlatformResult WindowsRefreshTokenStore::Delete()
    {
        if (!m_storageAvailable)
        {
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        auto temporary = m_filePath;
        temporary += L".tmp";
        // 片方が失敗してももう片方を必ず試します。finalを消せても
        // tmpを消せなかった場合は、残骸があることを隠さず失敗です。
        const bool finalDeleted = DeleteIfPresent(m_filePath);
        const bool temporaryDeleted = DeleteIfPresent(temporary);
        if (finalDeleted && temporaryDeleted)
        {
            return Success();
        }
        return Failure(
            "credential_delete_failed",
            "The saved online session could not be deleted.");
    }

    WindowsAuthorizationLauncher::WindowsAuthorizationLauncher(
        void* ownerWindow,
        ShellOpenFunction shellOpen)
        : m_ownerWindow(ownerWindow)
        , m_shellOpen(std::move(shellOpen))
    {
        if (!m_shellOpen)
        {
            m_shellOpen = [](
                void* owner,
                const wchar_t* operation,
                const wchar_t* file,
                const wchar_t* parameters,
                const wchar_t* directory,
                const int showCommand)
            {
                return reinterpret_cast<std::intptr_t>(
                    ShellExecuteW(
                        static_cast<HWND>(owner),
                        operation,
                        file,
                        parameters,
                        directory,
                        showCommand));
            };
        }
    }

    OnlinePlatformResult WindowsAuthorizationLauncher::Launch(
        const std::string_view authorizationUrl,
        const bool allowInsecureLoopback)
    {
        std::wstring wide;
        if (!IsSafeAuthorizationUrl(
                authorizationUrl,
                allowInsecureLoopback,
                wide))
        {
            return Failure(
                "authorization_url_invalid",
                "The authorization URL is invalid.");
        }
        const auto result = m_shellOpen(
            m_ownerWindow,
            L"open",
            wide.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
        if (result <= 32)
        {
            return Failure(
                "authorization_launch_failed",
                "The authorization page could not be opened (shell error "
                    + std::to_string(result) + ").");
        }
        return Success();
    }

    std::unique_ptr<IRefreshTokenStore> MakeWindowsRefreshTokenStore(
        std::string gameId,
        std::string environmentId)
    {
        const auto filePath = LocalCredentialPath(
            gameId,
            environmentId);
        return std::make_unique<WindowsRefreshTokenStore>(
            filePath,
            std::move(gameId),
            std::move(environmentId));
    }

    std::unique_ptr<IAuthorizationLauncher>
        MakeWindowsAuthorizationLauncher(void* ownerWindow)
    {
        return std::make_unique<WindowsAuthorizationLauncher>(
            ownerWindow);
    }
}
