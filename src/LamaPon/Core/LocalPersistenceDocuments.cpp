#include "LamaPon/Core/LocalPersistenceDocuments.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Core/SaveSlotValidation.h"
#include "LamaPon/Online/CloudSave.h"

#include <Windows.h>
#include <aclapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using LamaPon::Detail::LocalPersistenceDocument;
    using LamaPon::Detail::LocalPersistenceDocumentIdentity;
    using LamaPon::Detail::LocalPersistenceDocumentState;

    constexpr std::size_t MaximumJsonDepth = 64u;
    constexpr std::size_t MaximumJsonElements = 65536u;

    std::atomic<LamaPon::Detail::LocalPersistenceTestFailPoint>
        PersistenceFailPoint{};
    std::atomic_bool ObserverFailure{};
    thread_local std::size_t ObserverSuppressionDepth{};

    struct ObserverRegistration final
    {
        LamaPon::Detail::LocalPersistenceCommitCallback callback{};
        LamaPon::Detail::LocalPersistencePreDeleteCallback preDeleteCallback{};
        void* context{};
        std::uint64_t profileEpoch{};
        LamaPon::Detail::LocalPersistenceObserverToken token{};
    };

    // PlayerPrefs/SaveDataと同じmain thread限定契約なので、登録のpairを
    // lock-freeに差し替えても途中状態を別threadから観測しません。
    ObserverRegistration Observer{};
    std::uint64_t NextObserverToken{};

    bool ConsumeFailPoint(
        const LamaPon::Detail::LocalPersistenceTestFailPoint expected) noexcept
    {
        auto value = expected;
        return PersistenceFailPoint.compare_exchange_strong(
            value,
            LamaPon::Detail::LocalPersistenceTestFailPoint::None,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    class FileHandle final
    {
    public:
        FileHandle() = default;
        explicit FileHandle(const HANDLE value) noexcept
            : value(value)
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

    class FindHandle final
    {
    public:
        explicit FindHandle(const HANDLE value) noexcept
            : value(value)
        {
        }

        ~FindHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                FindClose(value);
            }
        }

        FindHandle(const FindHandle&) = delete;
        FindHandle& operator=(const FindHandle&) = delete;

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

        bool Initialize(const DWORD inheritance = NO_INHERITANCE)
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
                0u,
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
                    1u,
                    SECURITY_LOCAL_SYSTEM_RID,
                    0u,
                    0u,
                    0u,
                    0u,
                    0u,
                    0u,
                    0u,
                    &systemSid) == FALSE)
            {
                return false;
            }
            const auto currentUser = CurrentUserSid();
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
            entries[0].Trustee.ptstrName = static_cast<LPWSTR>(currentUser);
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
                    currentUser,
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

    [[noreturn]] void ThrowPersistenceFailure()
    {
        throw std::runtime_error("Local persistence operation failed.");
    }

    class PersistenceLockBusy final : public std::runtime_error
    {
    public:
        PersistenceLockBusy()
            : std::runtime_error("Local persistence is busy.")
        {
        }
    };

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

    bool OwnerIsAllowed(
        const HANDLE file,
        const RestrictedSecurity& security)
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
            && (EqualSid(owner, security.CurrentUserSid()) != FALSE
                || EqualSid(owner, security.systemSid) != FALSE);
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return allowed;
    }

    bool IsSafeFileHandle(
        const HANDLE file,
        const RestrictedSecurity& security)
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_STANDARD_INFO standard{};
        return GetFileInformationByHandleEx(
                file,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) != FALSE
            && GetFileInformationByHandleEx(
                file,
                FileStandardInfo,
                &standard,
                sizeof(standard)) != FALSE
            && (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u
            && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u
            && standard.NumberOfLinks == 1u
            && OwnerIsAllowed(file, security);
    }

    bool IsSafeDirectoryHandle(
        const HANDLE directory,
        const RestrictedSecurity& security)
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        return GetFileInformationByHandleEx(
                directory,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) != FALSE
            && (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u
            && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u
            && OwnerIsAllowed(directory, security);
    }

    std::optional<LamaPon::Detail::LocalPersistenceDocumentIdentity>
        CaptureDocumentIdentity(
        const HANDLE file,
        const bool completeBytes) noexcept
    {
        BY_HANDLE_FILE_INFORMATION information{};
        FILE_BASIC_INFO basic{};
        FILE_ID_INFO fileId{};
        if (GetFileInformationByHandle(file, &information) == FALSE
            || GetFileInformationByHandleEx(
                file,
                FileBasicInfo,
                &basic,
                sizeof(basic)) == FALSE
            || GetFileInformationByHandleEx(
                file,
                FileIdInfo,
                &fileId,
                sizeof(fileId)) == FALSE)
        {
            return std::nullopt;
        }

        std::array<std::uint8_t, 16u> identifier{};
        std::copy_n(
            fileId.FileId.Identifier,
            identifier.size(),
            identifier.begin());
        ULARGE_INTEGER byteLength{};
        byteLength.HighPart = information.nFileSizeHigh;
        byteLength.LowPart = information.nFileSizeLow;
        ULARGE_INTEGER lastWrite{};
        lastWrite.HighPart = information.ftLastWriteTime.dwHighDateTime;
        lastWrite.LowPart = information.ftLastWriteTime.dwLowDateTime;
        return LamaPon::Detail::LocalPersistenceDocumentIdentity{
            fileId.VolumeSerialNumber,
            identifier,
            byteLength.QuadPart,
            static_cast<std::int64_t>(lastWrite.QuadPart),
            basic.ChangeTime.QuadPart,
            true,
            completeBytes
        };
    }

    bool SameCapturedFile(
        const LamaPon::Detail::LocalPersistenceDocumentIdentity& left,
        const LamaPon::Detail::LocalPersistenceDocumentIdentity& right)
        noexcept
    {
        return left.valid
            && right.valid
            && left.volumeSerial == right.volumeSerial
            && left.fileId == right.fileId
            && left.byteLength == right.byteLength
            && left.lastWriteTime == right.lastWriteTime
            && left.changeTime == right.changeTime;
    }

    bool ProtectFileHandle(
        const HANDLE file,
        const RestrictedSecurity& security)
    {
        if (SetSecurityInfo(
                file,
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                security.acl,
                nullptr) != ERROR_SUCCESS)
        {
            return false;
        }
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
            || owner == nullptr
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
            && (EqualSid(owner, security.CurrentUserSid()) != FALSE
                || EqualSid(owner, security.systemSid) != FALSE);
        bool currentSeen{};
        bool systemSeen{};
        for (DWORD index = 0u; valid && index < information.AceCount; ++index)
        {
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE || rawAce == nullptr)
            {
                valid = false;
                break;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE
                || ace->Header.AceFlags != 0u
                || ace->Mask != FILE_ALL_ACCESS)
            {
                valid = false;
                break;
            }
            const auto sid = const_cast<DWORD*>(&ace->SidStart);
            if (EqualSid(sid, security.CurrentUserSid()) != FALSE
                && !currentSeen)
            {
                currentSeen = true;
            }
            else if (EqualSid(sid, security.systemSid) != FALSE
                && !systemSeen)
            {
                systemSeen = true;
            }
            else
            {
                valid = false;
            }
        }
        LocalFree(descriptor);
        return valid && currentSeen && systemSeen;
    }

    void ValidateParentDirectory(const std::filesystem::path& path)
    {
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        FileHandle directory(CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (directory.value == INVALID_HANDLE_VALUE
            || !IsSafeDirectoryHandle(directory.value, security))
        {
            ThrowPersistenceFailure();
        }
    }

    void ValidatePlainDirectoryComponent(const std::filesystem::path& path)
    {
        FileHandle directory(CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (directory.value == INVALID_HANDLE_VALUE
            || GetFileInformationByHandleEx(
                directory.value,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes)) == FALSE
            || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        {
            ThrowPersistenceFailure();
        }
    }

    enum class DirectoryChainState : std::uint8_t
    {
        Exists,
        Missing,
        Unavailable
    };

    DirectoryChainState InspectExistingDirectoryChain(
        const std::filesystem::path& directory) noexcept
    {
        std::filesystem::path normalized;
        try
        {
            normalized = std::filesystem::absolute(
                directory.empty()
                    ? std::filesystem::current_path()
                    : directory).lexically_normal();
        }
        catch (...)
        {
            return DirectoryChainState::Unavailable;
        }
        const auto drive = normalized.root_name().native();
        if (drive.size() != 2u
            || !((drive[0] >= L'A' && drive[0] <= L'Z')
                || (drive[0] >= L'a' && drive[0] <= L'z'))
            || drive[1] != L':'
            || GetDriveTypeW(normalized.root_path().c_str()) != DRIVE_FIXED)
        {
            return DirectoryChainState::Unavailable;
        }
        auto current = normalized.root_path();
        try
        {
            ValidatePlainDirectoryComponent(current);
            for (const auto& component : normalized.relative_path())
            {
                current /= component;
                const auto attributes = GetFileAttributesW(current.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    return IsMissingError(GetLastError())
                        ? DirectoryChainState::Missing
                        : DirectoryChainState::Unavailable;
                }
                ValidatePlainDirectoryComponent(current);
            }
        }
        catch (...)
        {
            return DirectoryChainState::Unavailable;
        }
        return DirectoryChainState::Exists;
    }

    void EnsureParentDirectory(const std::filesystem::path& targetPath)
    {
        const auto parent = targetPath.parent_path().empty()
            ? std::filesystem::current_path()
            : targetPath.parent_path();
        std::filesystem::path normalized;
        try
        {
            normalized = std::filesystem::absolute(parent).lexically_normal();
        }
        catch (...)
        {
            ThrowPersistenceFailure();
        }
        const auto drive = normalized.root_name().native();
        if (drive.size() != 2u
            || !((drive[0] >= L'A' && drive[0] <= L'Z')
                || (drive[0] >= L'a' && drive[0] <= L'z'))
            || drive[1] != L':'
            || GetDriveTypeW(normalized.root_path().c_str()) != DRIVE_FIXED)
        {
            ThrowPersistenceFailure();
        }
        RestrictedSecurity directorySecurity;
        if (!directorySecurity.Initialize(
                CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE))
        {
            ThrowPersistenceFailure();
        }
        auto current = normalized.root_path();
        ValidatePlainDirectoryComponent(current);
        for (const auto& component : normalized.relative_path())
        {
            current /= component;
            const auto attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                if (!IsMissingError(GetLastError())
                    || (CreateDirectoryW(
                            current.c_str(),
                            &directorySecurity.attributes) == FALSE
                        && GetLastError() != ERROR_ALREADY_EXISTS))
                {
                    ThrowPersistenceFailure();
                }
            }
            ValidatePlainDirectoryComponent(current);
        }
        ValidateParentDirectory(normalized);
    }

    FileHandle AcquireTargetLock(const std::filesystem::path& targetPath)
    {
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        const auto lockPath = WithSuffix(targetPath, L".lock");
        FileHandle lock(CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES
                | READ_CONTROL | WRITE_DAC,
            0u,
            &security.attributes,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (lock.value == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION
                || error == ERROR_LOCK_VIOLATION)
            {
                throw PersistenceLockBusy();
            }
            ThrowPersistenceFailure();
        }
        if (!IsSafeFileHandle(lock.value, security)
            || !ProtectFileHandle(lock.value, security))
        {
            ThrowPersistenceFailure();
        }
        return lock;
    }

    bool ValidateExistingTarget(const std::filesystem::path& targetPath)
    {
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        FileHandle target(CreateFileW(
            targetPath.c_str(),
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (target.value == INVALID_HANDLE_VALUE)
        {
            if (IsMissingError(GetLastError()))
            {
                return false;
            }
            ThrowPersistenceFailure();
        }
        if (!IsSafeFileHandle(target.value, security))
        {
            ThrowPersistenceFailure();
        }
        return true;
    }

    void WriteAndFlushStage(
        const std::filesystem::path& stagePath,
        const std::string_view bytes)
    {
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        FileHandle stage(CreateFileW(
            stagePath.c_str(),
            GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC,
            0u,
            &security.attributes,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (stage.value == INVALID_HANDLE_VALUE
            || !IsSafeFileHandle(stage.value, security)
            || !ProtectFileHandle(stage.value, security))
        {
            ThrowPersistenceFailure();
        }
        LARGE_INTEGER beginning{};
        if (SetFilePointerEx(
                stage.value,
                beginning,
                nullptr,
                FILE_BEGIN) == FALSE
            || SetEndOfFile(stage.value) == FALSE)
        {
            ThrowPersistenceFailure();
        }
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            DWORD written{};
            if (WriteFile(
                    stage.value,
                    bytes.data() + offset,
                    static_cast<DWORD>(remaining),
                    &written,
                    nullptr) == FALSE
                || written == 0u)
            {
                ThrowPersistenceFailure();
            }
            offset += written;
        }
        if (ConsumeFailPoint(
                LamaPon::Detail::LocalPersistenceTestFailPoint::BeforeFlush)
            || FlushFileBuffers(stage.value) == FALSE)
        {
            ThrowPersistenceFailure();
        }
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
                if (++depth > MaximumJsonDepth)
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
            throw std::runtime_error("Local persistence JSON is invalid.");
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
        catch (...)
        {
            throw std::runtime_error("Local persistence JSON is invalid.");
        }
        std::size_t remaining = MaximumJsonElements;
        if (duplicateKey || !JsonElementCountIsSafe(parsed, remaining))
        {
            throw std::runtime_error("Local persistence JSON is invalid.");
        }
        return parsed;
    }

    bool HasExactKeys(
        const Json& object,
        const std::initializer_list<std::string_view> expected)
    {
        return object.is_object()
            && object.size() == expected.size()
            && std::ranges::all_of(
                expected,
                [&object](const std::string_view key)
                {
                    return object.contains(std::string(key));
                });
    }

    void ValidatePreferenceKey(const std::string_view key)
    {
        if (key.empty()
            || key.size() > 128u
            || key.find_first_not_of(" \t\r\n") == std::string_view::npos
            || LamaPon::Utf8ToWide(key).empty())
        {
            throw std::runtime_error("PlayerPrefs key is invalid.");
        }
    }

    bool IsValidInteger(const Json& value)
    {
        if (value.type() == Json::value_t::number_integer)
        {
            return true;
        }
        return value.is_number_unsigned()
            && value.get<std::uint64_t>()
                <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max());
    }

    LocalPersistenceDocument ReadOpenDocument(
        const HANDLE file,
        const RestrictedSecurity& security,
        const std::size_t maximumBytes,
        const std::optional<std::string_view> saveSlot)
    {
        if (!IsSafeFileHandle(file, security))
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        auto before = CaptureDocumentIdentity(file, false);
        if (!before)
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        if (before->byteLength == 0u)
        {
            return {
                LocalPersistenceDocumentState::Corrupt,
                {},
                LocalPersistenceDocumentIdentity{
                    before->volumeSerial,
                    before->fileId,
                    before->byteLength,
                    before->lastWriteTime,
                    before->changeTime,
                    true,
                    true
                }
            };
        }
        if (before->byteLength > maximumBytes)
        {
            return {
                LocalPersistenceDocumentState::Corrupt,
                {},
                *before
            };
        }
        LARGE_INTEGER beginning{};
        if (SetFilePointerEx(
                file,
                beginning,
                nullptr,
                FILE_BEGIN) == FALSE)
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(before->byteLength));
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
                || read == 0u)
            {
                return { LocalPersistenceDocumentState::Unavailable, {} };
            }
            offset += read;
        }
        auto after = CaptureDocumentIdentity(file, true);
        if (!after || !SameCapturedFile(*before, *after))
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        const std::string_view text(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size());
        try
        {
            if (saveSlot)
            {
                LamaPon::Detail::ValidateSaveDataFullDocument(*saveSlot, text);
            }
            else
            {
                LamaPon::Detail::ValidatePlayerPrefsFullDocument(text);
            }
        }
        catch (...)
        {
            return {
                LocalPersistenceDocumentState::Corrupt,
                std::move(bytes),
                *after
            };
        }
        return {
            LocalPersistenceDocumentState::Loaded,
            std::move(bytes),
            *after
        };
    }

    LocalPersistenceDocument ReadDocument(
        const std::filesystem::path& path,
        const std::size_t maximumBytes,
        const std::optional<std::string_view> saveSlot)
    {
        const auto chain = InspectExistingDirectoryChain(path.parent_path());
        if (chain != DirectoryChainState::Exists)
        {
            return {
                chain == DirectoryChainState::Missing
                    ? LocalPersistenceDocumentState::Missing
                    : LocalPersistenceDocumentState::Unavailable,
                {}
            };
        }
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        FileHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
        {
            return {
                IsMissingError(GetLastError())
                    ? LocalPersistenceDocumentState::Missing
                    : LocalPersistenceDocumentState::Unavailable,
                {}
            };
        }
        return ReadOpenDocument(
            file.value,
            security,
            maximumBytes,
            saveSlot);
    }

    bool EndsWithSaveSuffix(const std::wstring_view name)
    {
        constexpr std::wstring_view suffix = L".save.json";
        if (name.size() < suffix.size())
        {
            return false;
        }
        const auto tail = name.substr(name.size() - suffix.size());
        return CompareStringOrdinal(
            tail.data(),
            static_cast<int>(tail.size()),
            suffix.data(),
            static_cast<int>(suffix.size()),
            TRUE) == CSTR_EQUAL;
    }

    bool MatchesObservedDocument(
        const LamaPon::Detail::LocalPersistenceDocument& current,
        const LamaPon::Detail::LocalPersistenceDocument& observed)
    {
        using LamaPon::Detail::LocalPersistenceDocumentState;
        if (observed.state == LocalPersistenceDocumentState::Loaded)
        {
            return current.state == LocalPersistenceDocumentState::Loaded
                && current.bytes == observed.bytes
                && current.identity.valid
                && observed.identity.valid
                && current.identity == observed.identity;
        }
        if (observed.state == LocalPersistenceDocumentState::Missing)
        {
            return current.state == LocalPersistenceDocumentState::Missing;
        }
        if (observed.state == LocalPersistenceDocumentState::Corrupt
            && observed.identity.valid)
        {
            return current.state == LocalPersistenceDocumentState::Corrupt
                && current.identity == observed.identity
                && (!observed.identity.completeBytes
                    || current.bytes == observed.bytes);
        }
        throw std::invalid_argument(
            "Conditional persistence requires a readable observation.");
    }

    bool PublishDocumentWithHeldLock(
        const std::filesystem::path& targetPath,
        const std::string_view bytes,
        const bool replaceExisting = true)
    {
        (void)ValidateExistingTarget(targetPath);
        const auto stagePath = WithSuffix(targetPath, L".writing");
        WriteAndFlushStage(stagePath, bytes);
        if (ConsumeFailPoint(
                LamaPon::Detail::LocalPersistenceTestFailPoint::
                    AfterFlushBeforePublish))
        {
            ThrowPersistenceFailure();
        }
        if (MoveFileExW(
                stagePath.c_str(),
                targetPath.c_str(),
                MOVEFILE_WRITE_THROUGH
                    | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0u))
            == FALSE)
        {
            const auto error = GetLastError();
            if (!replaceExisting
                && (error == ERROR_ALREADY_EXISTS
                    || error == ERROR_FILE_EXISTS))
            {
                return false;
            }
            ThrowPersistenceFailure();
        }
        return true;
    }

    void DeleteOpenDocumentWithHeldLock(
        const std::filesystem::path& targetPath,
        const HANDLE target,
        const RestrictedSecurity& security)
    {
        const auto deletingPath = WithSuffix(targetPath, L".deleting");
        {
            FileHandle stale(CreateFileW(
                deletingPath.c_str(),
                DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
            if (stale.value != INVALID_HANDLE_VALUE)
            {
                if (!IsSafeFileHandle(stale.value, security))
                {
                    ThrowPersistenceFailure();
                }
                FILE_DISPOSITION_INFO disposition{ TRUE };
                if (SetFileInformationByHandle(
                        stale.value,
                        FileDispositionInfo,
                        &disposition,
                        sizeof(disposition)) == FALSE)
                {
                    ThrowPersistenceFailure();
                }
            }
            else if (!IsMissingError(GetLastError()))
            {
                ThrowPersistenceFailure();
            }
        }

        std::filesystem::path absoluteDeletingPath;
        try
        {
            absoluteDeletingPath = std::filesystem::absolute(
                deletingPath).lexically_normal();
        }
        catch (...)
        {
            ThrowPersistenceFailure();
        }
        const auto fileName = absoluteDeletingPath.native();
        if (fileName.empty()
            || fileName.size()
                > std::numeric_limits<DWORD>::max() / sizeof(wchar_t))
        {
            ThrowPersistenceFailure();
        }
        const auto fileNameBytes = static_cast<DWORD>(
            fileName.size() * sizeof(wchar_t));
        const auto renameBytes = sizeof(FILE_RENAME_INFO)
            + static_cast<std::size_t>(fileNameBytes);
        if (renameBytes > std::numeric_limits<DWORD>::max())
        {
            ThrowPersistenceFailure();
        }
        auto renameStorage = std::make_unique<std::byte[]>(renameBytes);
        auto* const rename = reinterpret_cast<FILE_RENAME_INFO*>(
            renameStorage.get());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = fileNameBytes;
        std::copy(
            reinterpret_cast<const std::byte*>(fileName.data()),
            reinterpret_cast<const std::byte*>(fileName.data())
                + fileNameBytes,
            reinterpret_cast<std::byte*>(rename->FileName));

        // 検証済みの同一handleをcommit pointまで保持するため、path再openに
        // よる差替えTOCTOUを作りません。rename後の`.deleting`はselector外で、
        // crash時にも公開側はMissingとして一貫します。
        // FileRenameInfoにはWRITE_THROUGH flagがないため、内容を
        // rename前にdurability barrierへ通し、rename後も同一handleを
        // flushしてdirectory metadataのpublishを耐久化します。
        if (FlushFileBuffers(target) == FALSE)
        {
            ThrowPersistenceFailure();
        }
        if (SetFileInformationByHandle(
                target,
                FileRenameInfo,
                rename,
                static_cast<DWORD>(renameBytes)) == FALSE)
        {
            ThrowPersistenceFailure();
        }
        // ここで失敗しても公開pathは既にMissingです。曖昧に
        // 成功扱いせずgeneric failureを返し、selector外の
        // `.deleting`を次回の安全なcleanupに残します。
        if (FlushFileBuffers(target) == FALSE)
        {
            ThrowPersistenceFailure();
        }
        FILE_DISPOSITION_INFO disposition{ TRUE };
        // renameが論理commit pointです。cleanup失敗時は安全なstale
        // `.deleting`として次回処理に残します。
        (void)SetFileInformationByHandle(
            target,
            FileDispositionInfo,
            &disposition,
            sizeof(disposition));
    }

    bool DeleteDocumentWithHeldLock(
        const std::filesystem::path& targetPath)
    {
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        FileHandle target(CreateFileW(
            targetPath.c_str(),
            GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (target.value == INVALID_HANDLE_VALUE)
        {
            if (IsMissingError(GetLastError()))
            {
                return false;
            }
            ThrowPersistenceFailure();
        }
        if (!IsSafeFileHandle(target.value, security))
        {
            ThrowPersistenceFailure();
        }
        DeleteOpenDocumentWithHeldLock(
            targetPath,
            target.value,
            security);
        return true;
    }
}

namespace LamaPon::Detail
{
    ScopedLocalPersistenceObserverSuppression::
        ScopedLocalPersistenceObserverSuppression() noexcept
    {
        ++ObserverSuppressionDepth;
    }

    ScopedLocalPersistenceObserverSuppression::
        ~ScopedLocalPersistenceObserverSuppression()
    {
        --ObserverSuppressionDepth;
    }

    LocalPersistenceObserverToken AttachLocalPersistenceCommitObserver(
        const LocalPersistenceCommitCallback callback,
        void* const context,
        const std::uint64_t profileEpoch,
        const LocalPersistencePreDeleteCallback preDeleteCallback) noexcept
    {
        if (callback == nullptr)
        {
            return 0u;
        }
        ++NextObserverToken;
        if (NextObserverToken == 0u)
        {
            ++NextObserverToken;
        }
        Observer = {
            callback,
            preDeleteCallback,
            context,
            profileEpoch,
            NextObserverToken
        };
        ObserverFailure.store(false, std::memory_order_release);
        return NextObserverToken;
    }

    bool DetachLocalPersistenceCommitObserver(
        const LocalPersistenceObserverToken token) noexcept
    {
        if (token == 0u || Observer.token != token)
        {
            return false;
        }
        Observer = {};
        return true;
    }

    bool ConsumeLocalPersistenceObserverFailure() noexcept
    {
        return ObserverFailure.exchange(false, std::memory_order_acq_rel);
    }

    void NotifyLocalPersistenceCommit(
        const LocalPersistenceCommitEvent& event) noexcept
    {
        if (ObserverSuppressionDepth != 0u || Observer.callback == nullptr)
        {
            return;
        }
        if (!Observer.callback(
                Observer.context,
                Observer.profileEpoch,
                event))
        {
            ObserverFailure.store(true, std::memory_order_release);
        }
    }

    bool PrepareLocalPersistenceDelete(
        const LocalPersistenceCommitEvent& event) noexcept
    {
        if (ObserverSuppressionDepth != 0u
            || Observer.preDeleteCallback == nullptr)
        {
            return true;
        }
        return Observer.preDeleteCallback(
            Observer.context,
            Observer.profileEpoch,
            event);
    }

    void SetLocalPersistenceTestFailPoint(
        const LocalPersistenceTestFailPoint failPoint) noexcept
    {
        PersistenceFailPoint.store(failPoint, std::memory_order_release);
    }

    bool IsLocalPersistenceLockExclusiveForTesting(
        const std::filesystem::path& targetPath)
    {
        EnsureParentDirectory(targetPath);
        auto held = AcquireTargetLock(targetPath);
        try
        {
            auto peer = AcquireTargetLock(targetPath);
            return false;
        }
        catch (const PersistenceLockBusy&)
        {
            return true;
        }
    }

    void DurablePublishLocalDocument(
        const std::filesystem::path& targetPath,
        const std::string_view bytes)
    {
        EnsureParentDirectory(targetPath);
        auto lock = AcquireTargetLock(targetPath);
        static_cast<void>(PublishDocumentWithHeldLock(targetPath, bytes));
    }

    bool DurableDeleteLocalDocument(
        const std::filesystem::path& targetPath)
    {
        const auto parent = targetPath.parent_path().empty()
            ? std::filesystem::current_path()
            : targetPath.parent_path();
        const auto chain = InspectExistingDirectoryChain(parent);
        if (chain == DirectoryChainState::Missing)
        {
            return false;
        }
        if (chain != DirectoryChainState::Exists)
        {
            ThrowPersistenceFailure();
        }
        ValidateParentDirectory(parent);
        auto lock = AcquireTargetLock(targetPath);
        return DeleteDocumentWithHeldLock(targetPath);
    }

    LocalPersistenceConditionalApplyResult
        DurablePublishLocalDocumentIfUnchanged(
        const std::filesystem::path& targetPath,
        const LocalPersistenceDocument& observed,
        const std::string_view bytes,
        const std::size_t maximumBytes,
        const std::string_view saveSlot)
    {
        EnsureParentDirectory(targetPath);
        auto lock = AcquireTargetLock(targetPath);
        const auto current = ReadDocument(
            targetPath,
            maximumBytes,
            saveSlot.empty()
                ? std::nullopt
                : std::optional<std::string_view>{ saveSlot });
        if (current.state == LocalPersistenceDocumentState::Unavailable
            || (current.state == LocalPersistenceDocumentState::Corrupt
                && (observed.state
                        != LocalPersistenceDocumentState::Corrupt
                    || current.bytes.empty())))
        {
            ThrowPersistenceFailure();
        }
        if (!MatchesObservedDocument(current, observed))
        {
            return LocalPersistenceConditionalApplyResult::LocalChanged;
        }
        if (!PublishDocumentWithHeldLock(
                targetPath,
                bytes,
                observed.state
                    != LocalPersistenceDocumentState::Missing))
        {
            return LocalPersistenceConditionalApplyResult::LocalChanged;
        }
        return LocalPersistenceConditionalApplyResult::Applied;
    }

    LocalPersistenceConditionalApplyResult
        DurableDeleteLocalDocumentIfUnchanged(
        const std::filesystem::path& targetPath,
        const LocalPersistenceDocument& observed,
        const std::size_t maximumBytes,
        const std::string_view saveSlot)
    {
        const auto parent = targetPath.parent_path().empty()
            ? std::filesystem::current_path()
            : targetPath.parent_path();
        const auto chain = InspectExistingDirectoryChain(parent);
        if (chain == DirectoryChainState::Missing)
        {
            if (observed.state == LocalPersistenceDocumentState::Missing)
            {
                return LocalPersistenceConditionalApplyResult::Applied;
            }
            return LocalPersistenceConditionalApplyResult::LocalChanged;
        }
        if (chain != DirectoryChainState::Exists)
        {
            ThrowPersistenceFailure();
        }
        ValidateParentDirectory(parent);
        auto lock = AcquireTargetLock(targetPath);
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        FileHandle target(CreateFileW(
            targetPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | DELETE
                | FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (target.value == INVALID_HANDLE_VALUE)
        {
            if (IsMissingError(GetLastError()))
            {
                return observed.state == LocalPersistenceDocumentState::Missing
                    ? LocalPersistenceConditionalApplyResult::Applied
                    : LocalPersistenceConditionalApplyResult::LocalChanged;
            }
            ThrowPersistenceFailure();
        }
        const auto current = ReadOpenDocument(
            target.value,
            security,
            maximumBytes,
            saveSlot.empty()
                ? std::nullopt
                : std::optional<std::string_view>{ saveSlot });
        if (current.state == LocalPersistenceDocumentState::Unavailable
            || (current.state == LocalPersistenceDocumentState::Corrupt
                && observed.state
                    != LocalPersistenceDocumentState::Corrupt))
        {
            ThrowPersistenceFailure();
        }
        if (!MatchesObservedDocument(current, observed))
        {
            return LocalPersistenceConditionalApplyResult::LocalChanged;
        }
        DeleteOpenDocumentWithHeldLock(
            targetPath,
            target.value,
            security);
        return LocalPersistenceConditionalApplyResult::Applied;
    }

    void ValidatePlayerPrefsFullDocument(const std::string_view bytes)
    {
        if (bytes.empty() || bytes.size() > CloudPreferencesMaxBytes)
        {
            throw std::runtime_error("PlayerPrefs document is invalid.");
        }
        const auto document = ParseJsonStrict(bytes);
        if (!HasExactKeys(document, { "format", "version", "values" })
            || !document.at("format").is_string()
            || document.at("format").get<std::string>()
                != "LamaPonPlayerPrefs"
            || !document.at("version").is_number_unsigned()
            || document.at("version").get<std::uint64_t>() != 1u
            || !document.at("values").is_object())
        {
            throw std::runtime_error("PlayerPrefs document is invalid.");
        }
        for (const auto& [key, entry] : document.at("values").items())
        {
            ValidatePreferenceKey(key);
            if (!HasExactKeys(entry, { "type", "value" })
                || !entry.at("type").is_string())
            {
                throw std::runtime_error("PlayerPrefs value is invalid.");
            }
            const auto type = entry.at("type").get<std::string>();
            const auto& value = entry.at("value");
            const bool valid = type == "integer"
                ? IsValidInteger(value)
                : type == "number"
                    ? value.is_number_float()
                        && std::isfinite(value.get<double>())
                    : type == "boolean"
                        ? value.is_boolean()
                        : type == "string" && value.is_string();
            if (!valid)
            {
                throw std::runtime_error("PlayerPrefs value is invalid.");
            }
        }
    }

    void ValidateSaveDataFullDocument(
        const std::string_view slot,
        const std::string_view bytes)
    {
        ValidateSaveSlotName(slot);
        if (bytes.empty() || bytes.size() > CloudSaveSlotMaxBytes)
        {
            throw std::runtime_error("SaveData document is invalid.");
        }
        const auto document = ParseJsonStrict(bytes);
        if (!HasExactKeys(document, { "format", "version", "slot", "data" })
            || !document.at("format").is_string()
            || document.at("format").get<std::string>() != "LamaPonSaveData"
            || !document.at("version").is_number_unsigned()
            || document.at("version").get<std::uint64_t>() != 1u
            || !document.at("slot").is_string()
            || !EquivalentSaveSlotNames(
                document.at("slot").get<std::string>(),
                slot))
        {
            throw std::runtime_error("SaveData document is invalid.");
        }
    }

    LocalPersistenceDocument LocalPersistenceDocuments::ReadPlayerPrefs(
        const PlayerPrefs& playerPrefs)
    {
        return ReadDocument(
            playerPrefs.FilePath(),
            CloudPreferencesMaxBytes,
            std::nullopt);
    }

    LocalPersistenceDocument LocalPersistenceDocuments::ReadSaveData(
        const SaveDataStore& saveData,
        const std::string_view slot)
    {
        ValidateSaveSlotName(slot);
        return ReadDocument(
            saveData.SlotPath(slot),
            CloudSaveSlotMaxBytes,
            slot);
    }

    LocalPersistenceSlotListing LocalPersistenceDocuments::ListSaveData(
        const SaveDataStore& saveData)
    {
        const auto& directory = saveData.Directory();
        const auto chain = InspectExistingDirectoryChain(directory);
        if (chain != DirectoryChainState::Exists)
        {
            return {
                chain == DirectoryChainState::Missing
                    ? LocalPersistenceDocumentState::Missing
                    : LocalPersistenceDocumentState::Unavailable,
                {}
            };
        }
        try
        {
            ValidateParentDirectory(directory);
        }
        catch (...)
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        WIN32_FIND_DATAW data{};
        const auto pattern = directory / L"*";
        const auto rawSearch = FindFirstFileW(pattern.c_str(), &data);
        if (rawSearch == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            return {
                error == ERROR_FILE_NOT_FOUND
                    ? LocalPersistenceDocumentState::Loaded
                    : IsMissingError(error)
                        ? LocalPersistenceDocumentState::Missing
                        : LocalPersistenceDocumentState::Unavailable,
                {}
            };
        }
        FindHandle search(rawSearch);
        LocalPersistenceSlotListing result{
            LocalPersistenceDocumentState::Loaded,
            {}
        };
        do
        {
            const std::wstring_view name(data.cFileName);
            if (!EndsWithSaveSuffix(name))
            {
                continue;
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u
                || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            {
                return { LocalPersistenceDocumentState::Unavailable, {} };
            }
            constexpr std::size_t suffixLength =
                std::wstring_view(L".save.json").size();
            const auto wideSlot = name.substr(0u, name.size() - suffixLength);
            const auto slot = WideToUtf8(wideSlot);
            if (!IsValidSaveSlotName(slot))
            {
                return { LocalPersistenceDocumentState::Corrupt, {} };
            }
            if (std::ranges::any_of(
                    result.slots,
                    [&slot](const std::string& existing)
                    {
                        return EquivalentSaveSlotNames(existing, slot);
                    }))
            {
                return { LocalPersistenceDocumentState::Corrupt, {} };
            }
            result.slots.push_back(slot);
            if (result.slots.size() > CloudSaveMaxSlots)
            {
                return { LocalPersistenceDocumentState::Corrupt, {} };
            }
        }
        while (FindNextFileW(search.value, &data) != FALSE);
        if (GetLastError() != ERROR_NO_MORE_FILES)
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        std::ranges::sort(result.slots);
        return result;
    }

    void LocalPersistenceDocuments::ApplyPlayerPrefs(
        PlayerPrefs& playerPrefs,
        const std::span<const std::uint8_t> fullDocument)
    {
        const std::string_view bytes(
            fullDocument.empty()
                ? ""
                : reinterpret_cast<const char*>(fullDocument.data()),
            fullDocument.size());
        playerPrefs.ApplyRemoteDocumentAtomically(bytes);
    }

    void LocalPersistenceDocuments::DeletePlayerPrefs(
        PlayerPrefs& playerPrefs)
    {
        playerPrefs.DeleteRemoteDocumentAtomically();
    }

    LocalPersistenceConditionalApplyResult
        LocalPersistenceDocuments::ApplyPlayerPrefsIfUnchanged(
        PlayerPrefs& playerPrefs,
        const LocalPersistenceDocument& observed,
        const std::span<const std::uint8_t> fullDocument)
    {
        const std::string_view bytes(
            fullDocument.empty()
                ? ""
                : reinterpret_cast<const char*>(fullDocument.data()),
            fullDocument.size());
        return playerPrefs.ApplyRemoteDocumentAtomicallyIfUnchanged(
                bytes,
                observed)
            ? LocalPersistenceConditionalApplyResult::Applied
            : LocalPersistenceConditionalApplyResult::LocalChanged;
    }

    LocalPersistenceConditionalApplyResult
        LocalPersistenceDocuments::DeletePlayerPrefsIfUnchanged(
        PlayerPrefs& playerPrefs,
        const LocalPersistenceDocument& observed)
    {
        return playerPrefs.DeleteRemoteDocumentAtomicallyIfUnchanged(observed)
            ? LocalPersistenceConditionalApplyResult::Applied
            : LocalPersistenceConditionalApplyResult::LocalChanged;
    }

    void LocalPersistenceDocuments::SwapPlayerPrefsLoadedState(
        PlayerPrefs& target,
        PlayerPrefs& prepared) noexcept
    {
        target.SwapLoadedState(prepared);
    }

    void LocalPersistenceDocuments::LoadPlayerPrefsSnapshot(
        PlayerPrefs& target,
        const LocalPersistenceDocument& snapshot)
    {
        switch (snapshot.state)
        {
        case LocalPersistenceDocumentState::Loaded:
            if (snapshot.bytes.empty())
            {
                ThrowPersistenceFailure();
            }
            target.LoadValidatedSnapshot(
                std::string_view(
                    reinterpret_cast<const char*>(snapshot.bytes.data()),
                    snapshot.bytes.size()),
                false);
            return;

        case LocalPersistenceDocumentState::Missing:
            if (!snapshot.bytes.empty())
            {
                ThrowPersistenceFailure();
            }
            target.LoadValidatedSnapshot({}, true);
            return;

        case LocalPersistenceDocumentState::Unavailable:
        case LocalPersistenceDocumentState::Corrupt:
            ThrowPersistenceFailure();
        }
        ThrowPersistenceFailure();
    }

    void LocalPersistenceDocuments::ApplySaveData(
        SaveDataStore& saveData,
        const std::string_view slot,
        const std::span<const std::uint8_t> fullDocument)
    {
        const std::string_view bytes(
            fullDocument.empty()
                ? ""
                : reinterpret_cast<const char*>(fullDocument.data()),
            fullDocument.size());
        ValidateSaveDataFullDocument(slot, bytes);
        DurablePublishLocalDocument(saveData.SlotPath(slot), bytes);
    }

    void LocalPersistenceDocuments::DeleteSaveData(
        SaveDataStore& saveData,
        const std::string_view slot)
    {
        ValidateSaveSlotName(slot);
        (void)DurableDeleteLocalDocument(saveData.SlotPath(slot));
    }

    LocalPersistenceConditionalApplyResult
        LocalPersistenceDocuments::ApplySaveDataIfUnchanged(
        SaveDataStore& saveData,
        const std::string_view slot,
        const LocalPersistenceDocument& observed,
        const std::span<const std::uint8_t> fullDocument)
    {
        const std::string_view bytes(
            fullDocument.empty()
                ? ""
                : reinterpret_cast<const char*>(fullDocument.data()),
            fullDocument.size());
        ValidateSaveDataFullDocument(slot, bytes);
        return DurablePublishLocalDocumentIfUnchanged(
            saveData.SlotPath(slot),
            observed,
            bytes,
            CloudSaveSlotMaxBytes,
            slot);
    }

    LocalPersistenceConditionalApplyResult
        LocalPersistenceDocuments::DeleteSaveDataIfUnchanged(
        SaveDataStore& saveData,
        const std::string_view slot,
        const LocalPersistenceDocument& observed)
    {
        ValidateSaveSlotName(slot);
        return DurableDeleteLocalDocumentIfUnchanged(
            saveData.SlotPath(slot),
            observed,
            CloudSaveSlotMaxBytes,
            slot);
    }
}
