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
#include <source_location>
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

    // JSONの最大入れ子階層数
    constexpr std::size_t MaximumJsonDepth = 64u;
    // JSONの最大要素数
    constexpr std::size_t MaximumJsonElements = 65536u;

    // 次に注入する保存失敗の段階
    std::atomic<LamaPon::Detail::LocalPersistenceTestFailPoint>
        PersistenceFailPoint{};
    // 通知コールバックの失敗有無
    std::atomic_bool ObserverFailure{};
    // このスレッドの通知抑制深度
    thread_local std::size_t ObserverSuppressionDepth{};

    struct ObserverRegistration final
    {
        // コミット後の通知先
        LamaPon::Detail::LocalPersistenceCommitCallback callback{};
        // 削除前の許可通知先
        LamaPon::Detail::LocalPersistencePreDeleteCallback preDeleteCallback{};
        // 通知先の非所有文脈
        void* context{};
        // 登録したプロファイルの世代
        std::uint64_t profileEpoch{};
        // 通知登録の識別番号
        LamaPon::Detail::LocalPersistenceObserverToken token{};
    };

    // 主スレッド限定の現在の通知登録
    ObserverRegistration Observer{};
    // 次に発行する通知登録番号
    std::uint64_t NextObserverToken{};

    // 一致する失敗注入を消費します(expected: 今回の保存段階)。
    bool ConsumeFailPoint(
        const LamaPon::Detail::LocalPersistenceTestFailPoint expected) noexcept
    {
        // 比較交換用の失敗段階
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
        // 無効なファイルハンドルで初期化します。
        FileHandle() = default;
        // ファイルハンドルの所有権を引き取ります(value: 所有するハンドル)。
        explicit FileHandle(const HANDLE value) noexcept
            : value(value)
        {
        }

        // 所有するファイルハンドルを閉じます。
        ~FileHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        // ハンドルの二重解放を防ぐためコピーを禁止します。
        FileHandle(const FileHandle&) = delete;
        // ハンドルの二重解放を防ぐためコピー代入を禁止します。
        FileHandle& operator=(const FileHandle&) = delete;

        // ファイルハンドルを移譲します(other: 移譲元)。
        FileHandle(FileHandle&& other) noexcept
            : value(std::exchange(other.value, INVALID_HANDLE_VALUE))
        {
        }

        // 現在のハンドルを閉じて移譲します(other: 移譲元)。
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

        // 所有するWin32ハンドル
        HANDLE value{ INVALID_HANDLE_VALUE };
    };

    class FindHandle final
    {
    public:
        // 検索ハンドルの所有権を引き取ります(value: 所有するハンドル)。
        explicit FindHandle(const HANDLE value) noexcept
            : value(value)
        {
        }

        // 所有するファイル検索を終了します。
        ~FindHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                FindClose(value);
            }
        }

        // 検索の二重終了を防ぐためコピーを禁止します。
        FindHandle(const FindHandle&) = delete;
        // 検索の二重終了を防ぐためコピー代入を禁止します。
        FindHandle& operator=(const FindHandle&) = delete;

        // 所有するWin32ハンドル
        HANDLE value{ INVALID_HANDLE_VALUE };
    };

    struct RestrictedSecurity final
    {
        // 現在のプロセストークン
        FileHandle processToken;
        // ユーザートークン情報の領域
        std::vector<std::uint8_t> tokenUser;
        // 所有するSYSTEMのSID
        PSID systemSid{};
        // 所有する制限アクセス一覧
        // 検証するアクセス許可一覧
        PACL acl{};
        // ACLを持つセキュリティ情報
        SECURITY_DESCRIPTOR descriptor{};
        // 新規作成時のセキュリティ属性
        SECURITY_ATTRIBUTES attributes{};

        // ACLとSYSTEM識別子の確保領域を解放します。
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

        // 現ユーザーとSYSTEMだけを許可するACLを作ります(inheritance: ACEの継承フラグ)。
        bool Initialize(const DWORD inheritance = NO_INHERITANCE)
        {
            if (OpenProcessToken(
                    GetCurrentProcess(),
                    TOKEN_QUERY,
                    &processToken.value) == FALSE)
            {
                return false;
            }
            // ユーザートークン情報のバイト数
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
            // SYSTEM SIDの識別機関
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
            // 非所有の現ユーザーSID
            const auto currentUser = CurrentUserSid();
            // ユーザーとSYSTEMの許可項目
            EXPLICIT_ACCESSW entries[2]{};
            // 初期化するアクセス許可項目
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

        // 取得済みトークンのユーザーSIDを非所有参照で返します。
        PSID CurrentUserSid() const noexcept
        {
            return tokenUser.empty()
                ? nullptr
                : reinterpret_cast<const TOKEN_USER*>(tokenUser.data())
                    ->User.Sid;
        }
    };

    // 呼び出し位置を添えて保存失敗を送出します(location: 失敗の検出位置)。
    // 検証失敗時のWin32エラー番号は、以前のAPI呼び出しの値になる場合があります。
    [[noreturn]] void ThrowPersistenceFailure(
        const std::source_location& location =
            std::source_location::current())
    {
        // 直前のWin32エラー番号
        const DWORD error = GetLastError();
        throw std::runtime_error(
            "Local persistence operation failed at "
            + std::string(location.function_name())
            + ":"
            + std::to_string(location.line())
            + " (Win32 error "
            + std::to_string(error)
            + ").");
    }

    class PersistenceLockBusy final : public std::runtime_error
    {
    public:
        // 保存先ロックの競合を表す例外を初期化します。
        PersistenceLockBusy()
            : std::runtime_error("Local persistence is busy.")
        {
        }
    };

    // 未存在を表すWin32エラーか判定します(error: エラー番号)。
    bool IsMissingError(const DWORD error) noexcept
    {
        return error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND;
    }

    // 保存先に補助ファイルの接尾辞を付けます(path: 保存先, suffix: 接尾辞)。
    std::filesystem::path WithSuffix(
        const std::filesystem::path& path,
        const std::wstring_view suffix)
    {
        // 接尾辞を追加するパス
        auto result = path;
        result += suffix;
        return result;
    }

    // 所有者の信頼性を検査します(owner: 所有者SID, currentUserSid: 現ユーザーSID, systemSid: SYSTEMのSID)。
    // Windowsの既定所有者を許容するため、現ユーザー・SYSTEM・所属グループを信頼します。
    bool OwnerSidIsTrusted(
        const PSID owner,
        const PSID currentUserSid,
        const PSID systemSid) noexcept
    {
        if (owner == nullptr)
        {
            return false;
        }
        if (currentUserSid != nullptr
            && EqualSid(owner, currentUserSid) != FALSE)
        {
            return true;
        }
        if (systemSid != nullptr && EqualSid(owner, systemSid) != FALSE)
        {
            return true;
        }
        // 所有者が所属グループかの結果
        BOOL isMember = FALSE;
        return CheckTokenMembership(nullptr, owner, &isMember) != FALSE
            && isMember != FALSE;
    }

    // ファイルの所有者を検証します(file: 検査するハンドル, security: 許可する主体)。
    bool OwnerIsAllowed(
        const HANDLE file,
        const RestrictedSecurity& security)
    {
        // 照合する所有者SID
        PSID owner{};
        // 取得するセキュリティ情報
        PSECURITY_DESCRIPTOR descriptor{};
        // セキュリティ情報の取得結果
        const auto result = GetSecurityInfo(
            file,
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        // 所有者の信頼検査結果
        const bool allowed = result == ERROR_SUCCESS
            && descriptor != nullptr
            && OwnerSidIsTrusted(
                owner,
                security.CurrentUserSid(),
                security.systemSid);
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return allowed;
    }

    // 通常ファイルの安全性を検査します(file: 検査するハンドル, security: 許可する主体)。
    // ディレクトリ・再解析ポイント・複数ハードリンク・信頼しない所有者を拒否します。
    bool IsSafeFileHandle(
        const HANDLE file,
        const RestrictedSecurity& security)
    {
        // ファイル種別と再解析の情報
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        // リンク数を含む標準情報
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

    // 通常ディレクトリの安全性を検査します(directory: 検査するハンドル, security: 許可する主体)。
    bool IsSafeDirectoryHandle(
        const HANDLE directory,
        const RestrictedSecurity& security)
    {
        // ファイル種別と再解析の情報
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

    // ハンドルから文書の識別情報を取得します(file: 読み取り対象, completeBytes: 内容全体の保持有無)。
    std::optional<LamaPon::Detail::LocalPersistenceDocumentIdentity>
        CaptureDocumentIdentity(
        const HANDLE file,
        const bool completeBytes) noexcept
    {
        // サイズと最終書き込み情報
        BY_HANDLE_FILE_INFORMATION information{};
        // メタデータの変更情報
        FILE_BASIC_INFO basic{};
        // ボリュームとファイルのID
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

        // 照合用の16バイトファイルID
        std::array<std::uint8_t, 16u> identifier{};
        std::copy_n(
            fileId.FileId.Identifier,
            identifier.size(),
            identifier.begin());
        // ファイルサイズのバイト数
        ULARGE_INTEGER byteLength{};
        byteLength.HighPart = information.nFileSizeHigh;
        byteLength.LowPart = information.nFileSizeLow;
        // 最終書き込み時刻の結合値
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

    // 読み取り前後のファイル識別情報を照合します(left: 読み取り前, right: 読み取り後)。
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

    // 許可する二主体だけの継承禁止ACLを適用・検証します(file: 更新するハンドル, security: 制限ACL)。
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
        // 照合する所有者SID
        PSID owner{};
        // 検証するアクセス許可一覧
        PACL acl{};
        // 取得するセキュリティ情報
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
        // ACLの継承制御フラグ
        SECURITY_DESCRIPTOR_CONTROL control{};
        // セキュリティ情報の版番号
        DWORD revision{};
        // ACLの許可項目数
        ACL_SIZE_INFORMATION information{};
        // 制限ACLの検証結果
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
            && OwnerSidIsTrusted(
                owner,
                security.CurrentUserSid(),
                security.systemSid);
        // 現ユーザーの許可を確認済み
        bool currentSeen{};
        // SYSTEMの許可を確認済み
        bool systemSeen{};
        // 検査するアクセス許可の番号
        for (DWORD index = 0u; valid && index < information.AceCount; ++index)
        {
            // 取得するアクセス許可項目
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE || rawAce == nullptr)
            {
                valid = false;
                break;
            }
            // 型を確認するアクセス許可項目
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE
                || ace->Header.AceFlags != 0u
                || ace->Mask != FILE_ALL_ACCESS)
            {
                valid = false;
                break;
            }
            // 許可対象の非所有SID
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

    // 保存先の親の種別と所有者を検査します(path: 親ディレクトリ)。
    void ValidateParentDirectory(const std::filesystem::path& path)
    {
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        // 検証するディレクトリのハンドル
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

    // パス要素が再解析でないディレクトリか検査します(path: 検査するパス)。
    void ValidatePlainDirectoryComponent(const std::filesystem::path& path)
    {
        // 検証するディレクトリのハンドル
        FileHandle directory(CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        // ファイル種別と再解析の情報
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
        // 安全なディレクトリが存在
        Exists,
        // 途中のディレクトリが未存在
        Missing,
        // 安全に利用できないパス
        Unavailable
    };

    // ローカル固定ドライブの全パス要素を検査します(directory: 保存先ディレクトリ)。
    DirectoryChainState InspectExistingDirectoryChain(
        const std::filesystem::path& directory) noexcept
    {
        // 正規化した絶対ディレクトリ
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
        // 固定ドライブを判定するルート名
        const auto drive = normalized.root_name().native();
        if (drive.size() != 2u
            || !((drive[0] >= L'A' && drive[0] <= L'Z')
                || (drive[0] >= L'a' && drive[0] <= L'z'))
            || drive[1] != L':'
            || GetDriveTypeW(normalized.root_path().c_str()) != DRIVE_FIXED)
        {
            return DirectoryChainState::Unavailable;
        }
        // 順に検査するディレクトリ
        auto current = normalized.root_path();
        try
        {
            ValidatePlainDirectoryComponent(current);
            // 検査するディレクトリの要素
            for (const auto& component : normalized.relative_path())
            {
                current /= component;
                // パス要素のファイル属性
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

    // 保存先までの安全な親ディレクトリを用意します(targetPath: 保存先)。
    // ローカル固定ドライブだけを許可し、新規ディレクトリには制限ACLを適用します。
    void EnsureParentDirectory(const std::filesystem::path& targetPath)
    {
        // 保存先の親ディレクトリ
        const auto parent = targetPath.parent_path().empty()
            ? std::filesystem::current_path()
            : targetPath.parent_path();
        // 正規化した絶対ディレクトリ
        std::filesystem::path normalized;
        try
        {
            normalized = std::filesystem::absolute(parent).lexically_normal();
        }
        catch (...)
        {
            ThrowPersistenceFailure();
        }
        // 固定ドライブを判定するルート名
        const auto drive = normalized.root_name().native();
        if (drive.size() != 2u
            || !((drive[0] >= L'A' && drive[0] <= L'Z')
                || (drive[0] >= L'a' && drive[0] <= L'z'))
            || drive[1] != L':'
            || GetDriveTypeW(normalized.root_path().c_str()) != DRIVE_FIXED)
        {
            ThrowPersistenceFailure();
        }
        // 新規ディレクトリの継承ACL
        RestrictedSecurity directorySecurity;
        if (!directorySecurity.Initialize(
                CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE))
        {
            ThrowPersistenceFailure();
        }
        // 順に検査するディレクトリ
        auto current = normalized.root_path();
        ValidatePlainDirectoryComponent(current);
        // 作成・検査するディレクトリ要素
        for (const auto& component : normalized.relative_path())
        {
            current /= component;
            // パス要素のファイル属性
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

    // 保存先の共有禁止ロックを取得します(targetPath: 保存先)。
    // 競合はPersistenceLockBusyで、返すハンドルを閉じるまでロックを保持します。
    FileHandle AcquireTargetLock(const std::filesystem::path& targetPath)
    {
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        // 保存先固有のロックパス
        const auto lockPath = WithSuffix(targetPath, L".lock");
        // 共有を禁止するロックハンドル
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
            // 直前のWin32エラー番号
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

    // 既存の保存先が安全か検査します(targetPath: 保存先)。
    // 未存在ならfalseで、安全でない対象や検査失敗は例外です。
    bool ValidateExistingTarget(const std::filesystem::path& targetPath)
    {
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        // 検証・更新する保存先ハンドル
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

    // 公開前ファイルへ全内容を書いて確定します(stagePath: 公開前パス, bytes: 文書全体)。
    void WriteAndFlushStage(
        const std::filesystem::path& stagePath,
        const std::string_view bytes)
    {
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        // 公開前ファイルのハンドル
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
        // ファイルの先頭オフセット
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
        // 処理済み内容のバイト数
        std::size_t offset{};
        while (offset < bytes.size())
        {
            // 今回処理する最大バイト数
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            // 今回書き込んだバイト数
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

    // 文字列内を除いてJSONの入れ子上限を検査します(text: JSONテキスト)。
    bool JsonNestingIsSafe(const std::string_view text) noexcept
    {
        // JSONの現在の入れ子深度
        std::size_t depth{};
        // JSON文字列の解析中か
        bool inString{};
        // 文字列のエスケープ待機有無
        bool escaped{};
        // 検査するJSONのバイト
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

    // JSON要素の残り予算を消費して上限を検査します(value: 検査する値, remaining: 残り要素数)。
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
            // 要素数を検査する子の値
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

    // UTF-8・入れ子・重複キー・要素数を検証して解析します(text: JSONテキスト)。
    Json ParseJsonStrict(const std::string_view text)
    {
        if (text.empty()
            || LamaPon::Utf8ToWide(text).empty()
            || !JsonNestingIsSafe(text))
        {
            throw std::runtime_error("Local persistence JSON is invalid.");
        }
        // 重複キーまたは階層違反の有無
        bool duplicateKey{};
        // 階層ごとの解析済みキー集合
        std::array<std::unordered_set<std::string>, MaximumJsonDepth + 1u>
            keysByDepth;
        // 重複キーを検出します(depth: 解析階層, event: 解析イベント, parsed: 今回の値)。
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
                    // キーを登録する解析階層
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
                    // 現在階層のキー集合
                    auto& keys = keysByDepth[static_cast<std::size_t>(depth)];
                    if (!keys.insert(parsed.get<std::string>()).second)
                    {
                        duplicateKey = true;
                    }
                }
                return true;
            };
        // 厳密に解析したJSON値
        Json parsed;
        try
        {
            parsed = Json::parse(text, callback, true, false);
        }
        catch (...)
        {
            throw std::runtime_error("Local persistence JSON is invalid.");
        }
        // 解析後の残り要素数予算
        std::size_t remaining = MaximumJsonElements;
        if (duplicateKey || !JsonElementCountIsSafe(parsed, remaining))
        {
            throw std::runtime_error("Local persistence JSON is invalid.");
        }
        return parsed;
    }

    // 必要なキーだけを持つオブジェクトか調べます(object: JSON値, expected: キー一覧)。
    bool HasExactKeys(
        const Json& object,
        const std::initializer_list<std::string_view> expected)
    {
        // 各必要キーの存在を確認します(key: 検査するキー)。
        return object.is_object()
            && object.size() == expected.size()
            && std::ranges::all_of(
                expected,
                [&object](const std::string_view key)
                {
                    return object.contains(std::string(key));
                });
    }

    // 設定キーのUTF-8と1〜128バイトの制約を検査します(key: 設定キー)。
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

    // 符号付き64ビット整数として保持できるか調べます(value: JSON値)。
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

    // 同一ファイルから全内容を読み検証します(file: 対象ハンドル, security: 許可主体, maximumBytes: 読み取り上限, saveSlot: 空なら設定文書)。
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
        // 読み取り前の識別情報
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
        // ファイルの先頭オフセット
        LARGE_INTEGER beginning{};
        if (SetFilePointerEx(
                file,
                beginning,
                nullptr,
                FILE_BEGIN) == FALSE)
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        // 読み込む文書全体のバイト列
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(before->byteLength));
        // 処理済み内容のバイト数
        std::size_t offset{};
        while (offset < bytes.size())
        {
            // 今回処理する最大バイト数
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            // 今回読み込んだバイト数
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
        // 読み取り後の識別情報
        auto after = CaptureDocumentIdentity(file, true);
        if (!after || !SameCapturedFile(*before, *after))
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        // 検証する文書のUTF-8参照
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

    // 安全な保存先から文書を厳密に読みます(path: 保存先, maximumBytes: 読み取り上限, saveSlot: 空なら設定文書)。
    LocalPersistenceDocument ReadDocument(
        const std::filesystem::path& path,
        const std::size_t maximumBytes,
        const std::optional<std::string_view> saveSlot)
    {
        // 親までの安全性と存在状態
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
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            return { LocalPersistenceDocumentState::Unavailable, {} };
        }
        // 読み取る文書のハンドル
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

    // 保存文書の接尾辞を大文字小文字を無視して検査します(name: ファイル名)。
    bool EndsWithSaveSuffix(const std::wstring_view name)
    {
        // 保存文書を判別する接尾辞
        constexpr std::wstring_view suffix = L".save.json";
        if (name.size() < suffix.size())
        {
            return false;
        }
        // 比較するファイル名の末尾
        const auto tail = name.substr(name.size() - suffix.size());
        return CompareStringOrdinal(
            tail.data(),
            static_cast<int>(tail.size()),
            suffix.data(),
            static_cast<int>(suffix.size()),
            TRUE) == CSTR_EQUAL;
    }

    // 状態・識別情報・保持内容を観測と照合します(current: 再読み取り結果, observed: 以前の観測)。
    // 利用不能な観測はinvalid_argumentで、過大な破損ファイルは識別情報だけを比較します。
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

    // 保持中のロックで文書を公開します(targetPath: 保存先, bytes: 文書全体, replaceExisting: 置換許可)。
    // 呼び出し側が保存先ロックを保持し、置換禁止で先行作成があればfalseです。
    bool PublishDocumentWithHeldLock(
        const std::filesystem::path& targetPath,
        const std::string_view bytes,
        const bool replaceExisting = true)
    {
        (void)ValidateExistingTarget(targetPath);
        // 公開前の書き込みファイルパス
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
            // 直前のWin32エラー番号
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

    // 保持中のハンドルで文書を改名して削除します(targetPath: 保存先, target: 対象ハンドル, security: 許可主体)。
    // 呼び出し側が保存先ロックを保持し、検証済みハンドルを削除のコミットまで使います。
    void DeleteOpenDocumentWithHeldLock(
        const std::filesystem::path& targetPath,
        const HANDLE target,
        const RestrictedSecurity& security)
    {
        // 削除対象を退避するパス
        const auto deletingPath = WithSuffix(targetPath, L".deleting");
        {
            // 前回残った削除対象のハンドル
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
                // 前回残った対象の削除指定
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

        // 削除退避先の絶対パス
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
        // 削除退避先のUTF-16パス
        const auto fileName = absoluteDeletingPath.native();
        if (fileName.empty()
            || fileName.size()
                > std::numeric_limits<DWORD>::max() / sizeof(wchar_t))
        {
            ThrowPersistenceFailure();
        }
        // 改名先パスのバイト数
        const auto fileNameBytes = static_cast<DWORD>(
            fileName.size() * sizeof(wchar_t));
        // 改名要求の全体バイト数
        const auto renameBytes = sizeof(FILE_RENAME_INFO)
            + static_cast<std::size_t>(fileNameBytes);
        if (renameBytes > std::numeric_limits<DWORD>::max())
        {
            ThrowPersistenceFailure();
        }
        // 改名要求の所有バッファー
        auto renameStorage = std::make_unique<std::byte[]>(renameBytes);
        // バッファー内の改名情報
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

        // WRITE_THROUGHのないハンドル改名の前後で、同じハンドルをFlushFileBuffersへ渡します。
        // .deletingへの改名が論理コミットで、その後は公開パスがMissingになります。
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
        // 改名後の確定失敗は例外とし、既にMissingの公開パスと残る.deletingを成功扱いしません。
        if (FlushFileBuffers(target) == FALSE)
        {
            ThrowPersistenceFailure();
        }
        // 改名後ファイルの削除指定
        FILE_DISPOSITION_INFO disposition{ TRUE };
        // 後片付けの失敗は、次回に安全に処理する.deletingとして残します。
        (void)SetFileInformationByHandle(
            target,
            FileDispositionInfo,
            &disposition,
            sizeof(disposition));
    }

    // 保持中のロックで既存文書を削除します(targetPath: 保存先)。
    // 呼び出し側が保存先ロックを保持し、未存在ならfalseです。
    bool DeleteDocumentWithHeldLock(
        const std::filesystem::path& targetPath)
    {
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        // 検証・更新する保存先ハンドル
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
        // 検査中に保持する保存先ロック
        auto held = AcquireTargetLock(targetPath);
        try
        {
            // 競合を試す二つ目のロック
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
        // 操作完了まで保持する保存先ロック
        auto lock = AcquireTargetLock(targetPath);
        static_cast<void>(PublishDocumentWithHeldLock(targetPath, bytes));
    }

    bool DurableDeleteLocalDocument(
        const std::filesystem::path& targetPath)
    {
        // 保存先の親ディレクトリ
        const auto parent = targetPath.parent_path().empty()
            ? std::filesystem::current_path()
            : targetPath.parent_path();
        // 親までの安全性と存在状態
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
        // 操作完了まで保持する保存先ロック
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
        // 操作完了まで保持する保存先ロック
        auto lock = AcquireTargetLock(targetPath);
        // 再照合する現在の文書
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
        // 保存先の親ディレクトリ
        const auto parent = targetPath.parent_path().empty()
            ? std::filesystem::current_path()
            : targetPath.parent_path();
        // 親までの安全性と存在状態
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
        // 操作完了まで保持する保存先ロック
        auto lock = AcquireTargetLock(targetPath);
        // 現ユーザーとSYSTEMの制限ACL
        RestrictedSecurity security;
        if (!security.Initialize())
        {
            ThrowPersistenceFailure();
        }
        // 検証・更新する保存先ハンドル
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
        // 同じハンドルの現在の文書
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
        // 厳密に解析した保存文書
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
        // key: 設定値のキー名
        // entry: 型と実値を持つ項目
        for (const auto& [key, entry] : document.at("values").items())
        {
            ValidatePreferenceKey(key);
            if (!HasExactKeys(entry, { "type", "value" })
                || !entry.at("type").is_string())
            {
                throw std::runtime_error("PlayerPrefs value is invalid.");
            }
            // 設定値に記録された型名
            const auto type = entry.at("type").get<std::string>();
            // 型を検証する設定値
            const auto& value = entry.at("value");
            // 設定型と実値の一致有無
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
        // 厳密に解析した保存文書
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
        // 列挙する保存先ディレクトリ
        const auto& directory = saveData.Directory();
        // 親までの安全性と存在状態
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
        // 現在のファイル検索結果
        WIN32_FIND_DATAW data{};
        // 保存先全体の検索パターン
        const auto pattern = directory / L"*";
        // Win32の検索開始結果
        const auto rawSearch = FindFirstFileW(pattern.c_str(), &data);
        if (rawSearch == INVALID_HANDLE_VALUE)
        {
            // 直前のWin32エラー番号
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
        // 所有するファイル検索ハンドル
        FindHandle search(rawSearch);
        // 検証済みスロットの列挙結果
        LocalPersistenceSlotListing result{
            LocalPersistenceDocumentState::Loaded,
            {}
        };
        do
        {
            // 検索で得たUTF-16ファイル名
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
            // 保存文書の接尾辞の文字数
            constexpr std::size_t suffixLength =
                std::wstring_view(L".save.json").size();
            // 接尾辞を除いたUTF-16名
            const auto wideSlot = name.substr(0u, name.size() - suffixLength);
            // 検証するUTF-8スロット名
            const auto slot = WideToUtf8(wideSlot);
            if (!IsValidSaveSlotName(slot))
            {
                return { LocalPersistenceDocumentState::Corrupt, {} };
            }
            // 同じ名前の既存スロットか検査します(existing: 列挙済みのスロット名)。
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
        // リモート文書全体の文字列参照
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
        // リモート文書全体の文字列参照
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
        // リモート文書全体の文字列参照
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
        // リモート文書全体の文字列参照
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
