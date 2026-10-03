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
#include <atomic>
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
    // 更新用tokenの最大バイト数
    constexpr std::size_t MaximumRefreshTokenBytes = 8192;
    // 暗号化blobの最大バイト数
    constexpr std::size_t MaximumProtectedBlobBytes = 64 * 1024;
    // token包絡ヘッダーのバイト数
    constexpr std::size_t EnvelopeHeaderBytes = 16;
    // 暗号化ファイルの識別子
    constexpr std::array<std::uint8_t, 8> FileMagic{
        'L', 'P', 'O', 'N', 'A', 'U', 'T', 'H'
    };
    // 復号したtoken包絡の識別子
    constexpr std::array<std::uint8_t, 8> PlaintextMagic{
        'L', 'P', 'O', 'N', 'R', 'T', 'K', 'N'
    };
    // 暗号化補助値の用途と版
    constexpr std::string_view EntropyLabel =
        "LamaPon.Online.RefreshToken/v1";
    // 次の該当保存処理だけの失敗指定
    std::atomic<LamaPon::Detail::WindowsRefreshTokenSaveTestFailPoint>
        CredentialSaveFailPoint{
            LamaPon::Detail::WindowsRefreshTokenSaveTestFailPoint::None
        };
    // 資格情報の置換直前のテスト処理
    std::atomic<LamaPon::Detail::WindowsRefreshTokenSaveTestHook>
        CredentialSaveBeforeReplaceHook{};
    // 置換直前テストへ渡す借用状態
    std::atomic<void*> CredentialSaveBeforeReplaceContext{};
    // 資格情報パスのHMAC入力を固定する公開定数で、秘匿用の鍵ではありません。
    // パス名用HMACの固定入力鍵
    constexpr LamaPon::Crypto::AesKey CredentialPathHashKey{
        'L', 'a', 'm', 'a', 'P', 'o', 'n', '.',
        'O', 'n', 'l', 'i', 'n', 'e', '.', 'P',
        'a', 't', 'h', 'H', 'a', 's', 'h', '/',
        'v', '1'
    };

    struct HandleGuard final
    {
        // ファイルまたはロックの所有ハンドル
        HANDLE value{ INVALID_HANDLE_VALUE };

        // 所有するファイルまたはロックのハンドルを解放する。
        ~HandleGuard()
        {
            Reset();
        }

        // 有効な所有ハンドルを解放して無効値にする。
        void Reset() noexcept
        {
            if (value != nullptr && value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
            value = INVALID_HANDLE_VALUE;
        }

        // 解放せず所有ハンドルを呼出し側へ渡す。
        [[nodiscard]] HANDLE Release() noexcept
        {
            return std::exchange(value, INVALID_HANDLE_VALUE);
        }
    };

    // 該当する失敗注入を一度だけ消費する(expected: 今回到達した保存の段階)。
    [[nodiscard]] bool ConsumeCredentialSaveFailPoint(
        const LamaPon::Detail::WindowsRefreshTokenSaveTestFailPoint
            expected) noexcept
    {
        // 失敗指定を消費する比較交換の値
        auto current = expected;
        return CredentialSaveFailPoint.compare_exchange_strong(
            current,
            LamaPon::Detail::WindowsRefreshTokenSaveTestFailPoint::None,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    struct KnownFolderGuard final
    {
        // CoTaskMemFreeする保存先の文字列
        PWSTR value{};

        // KnownFolder APIが確保した文字列を解放する。
        ~KnownFolderGuard()
        {
            CoTaskMemFree(value);
        }
    };

    struct RestrictedSecurity final
    {
        // 現プロセスtokenの所有先
        HandleGuard processToken;
        // ユーザーSIDを保持する領域
        std::vector<std::uint8_t> tokenUser;
        // 解放するSYSTEMのSID
        PSID systemSid{};
        // LocalFreeするアクセス許可一覧
        PACL acl{};
        // 作成するセキュリティ記述子
        SECURITY_DESCRIPTOR descriptor{};
        // 作成時ACLまたは取得した属性情報
        SECURITY_ATTRIBUTES attributes{};

        // 確保したACLとSYSTEM SIDを解放する。
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

        // 現ユーザーとSYSTEMだけに全権限を許可するACLを作る(inheritance: 子オブジェクトへの継承フラグ)。
        [[nodiscard]] bool Initialize(const DWORD inheritance)
        {
            if (OpenProcessToken(
                    GetCurrentProcess(),
                    TOKEN_QUERY,
                    &processToken.value) == FALSE)
            {
                return false;
            }

            // TokenUser取得用のバイト数
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

            // SYSTEM SIDのNT権限識別子
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

            // 取得したWindowsユーザー情報
            auto* currentUser = reinterpret_cast<TOKEN_USER*>(
                tokenUser.data());
            // 現ユーザーとSYSTEMの許可ACE
            EXPLICIT_ACCESSW entries[2]{};
            // 継承条件を設定する許可ACE
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

        // 保持するTokenUser領域から現ユーザーのSIDを借用する。
        [[nodiscard]] PSID CurrentUserSid() const noexcept
        {
            return tokenUser.empty()
                ? nullptr
                : reinterpret_cast<const TOKEN_USER*>(
                    tokenUser.data())->User.Sid;
        }
    };

    enum class ParentDirectoryState : std::uint8_t
    {
        Exists,
        Missing,
        Unavailable
    };

    // 全祖先を開き通常ディレクトリ以外を拒否する(filePath: 資格情報ファイル)。
    [[nodiscard]] ParentDirectoryState InspectCredentialParent(
        const std::filesystem::path& filePath) noexcept
    {
        try
        {
            // 正規化した資格情報の親保存先
            const auto parent = std::filesystem::absolute(
                filePath.parent_path().empty()
                    ? std::filesystem::current_path()
                    : filePath.parent_path()).lexically_normal();
            // 順に検証する祖先のパス
            auto current = parent.root_path();
            // 開いた祖先が通常ディレクトリかを返す(path: 検査する保存先)。
            const auto inspectComponent = [](const auto& path)
            {
                // 属性検証用のディレクトリハンドル
                HandleGuard directory;
                directory.value = CreateFileW(
                    path.c_str(),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS
                        | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
                if (directory.value == INVALID_HANDLE_VALUE)
                {
                    // Windows APIの失敗コード
                    const auto error = GetLastError();
                    return error == ERROR_FILE_NOT_FOUND
                            || error == ERROR_PATH_NOT_FOUND
                        ? ParentDirectoryState::Missing
                        : ParentDirectoryState::Unavailable;
                }
                // 作成時ACLまたは取得した属性情報
                FILE_ATTRIBUTE_TAG_INFO attributes{};
                if (GetFileInformationByHandleEx(
                        directory.value,
                        FileAttributeTagInfo,
                        &attributes,
                        sizeof(attributes)) == FALSE
                    || (attributes.FileAttributes
                        & FILE_ATTRIBUTE_DIRECTORY) == 0u
                    || (attributes.FileAttributes
                        & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
                {
                    return ParentDirectoryState::Unavailable;
                }
                return ParentDirectoryState::Exists;
            };
            // 親ディレクトリの検査結果
            auto state = inspectComponent(current);
            if (state != ParentDirectoryState::Exists)
            {
                return state;
            }
            // 検証する祖先のパス構成要素
            for (const auto& component : parent.relative_path())
            {
                current /= component;
                state = inspectComponent(current);
                if (state != ParentDirectoryState::Exists)
                {
                    return state;
                }
            }
            return ParentDirectoryState::Exists;
        }
        catch (...)
        {
            return ParentDirectoryState::Unavailable;
        }
    }

    // 開いた対象がreparse pointでも複数リンクでもない通常ファイルかを返す(file: 検証するハンドル)。
    [[nodiscard]] bool IsPlainSingleLinkFile(
        const HANDLE file) noexcept
    {
        // 作成時ACLまたは取得した属性情報
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        // ファイル長とリンク数の情報
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
            && (attributes.FileAttributes
                & FILE_ATTRIBUTE_DIRECTORY) == 0u
            && (attributes.FileAttributes
                & FILE_ATTRIBUTE_REPARSE_POINT) == 0u
            && standard.NumberOfLinks == 1u;
    }

    // 通常ファイルを検証し継承を遮断したACLを適用する(file: 対象ハンドル, security: 現ユーザーとSYSTEM限定のACL)。
    [[nodiscard]] bool ProtectPlainFileHandle(
        const HANDLE file,
        const RestrictedSecurity& security) noexcept
    {
        return IsPlainSingleLinkFile(file)
            && SetSecurityInfo(
                file,
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION
                    | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                security.acl,
                nullptr) == ERROR_SUCCESS;
    }

    // 開いた書き込み残骸の属性を検証して削除する(path: 書き込み用のパス)。
    [[nodiscard]] bool RemoveSafeCredentialTemporary(
        const std::filesystem::path& path) noexcept
    {
        // 検証と読み書き用の所有ハンドル
        HandleGuard file;
        file.value = CreateFileW(
            path.c_str(),
            DELETE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (file.value == INVALID_HANDLE_VALUE)
        {
            // Windows APIの失敗コード
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND
                || error == ERROR_PATH_NOT_FOUND;
        }
        if (!IsPlainSingleLinkFile(file.value))
        {
            return false;
        }
        // 削除を要求するファイル処理情報
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        return SetFileInformationByHandle(
            file.value,
            FileDispositionInfo,
            &disposition,
            sizeof(disposition)) != FALSE;
    }

    // 共有なしの通常ファイルを開いて制限ACLのロックを得る(filePath: 資格情報ファイル, suffix: ロック用接尾辞, lock: 取得するハンドルの所有先)。
    [[nodiscard]] bool AcquireCredentialExclusiveLock(
        const std::filesystem::path& filePath,
        const wchar_t* const suffix,
        HandleGuard& lock) noexcept
    {
        try
        {
            // 現ユーザーとSYSTEM限定のACL
            RestrictedSecurity security;
            if (!security.Initialize(NO_INHERITANCE))
            {
                return false;
            }
            // 接尾辞付きの資格情報ロック先
            auto lockPath = filePath;
            lockPath += suffix;
            lock.value = CreateFileW(
                lockPath.c_str(),
                GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES
                    | READ_CONTROL | WRITE_DAC,
                0u,
                &security.attributes,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_HIDDEN
                    | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
                    | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (lock.value == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            if (security.CurrentUserSid() == nullptr
                || !ProtectPlainFileHandle(lock.value, security))
            {
                lock.Reset();
                return false;
            }
            return true;
        }
        catch (...)
        {
            lock.Reset();
            return false;
        }
    }

    // 読取・保存・削除の一回の操作を排他する(filePath: 資格情報ファイル, lock: ロックの所有先)。
    [[nodiscard]] bool AcquireCredentialOperationLock(
        const std::filesystem::path& filePath,
        HandleGuard& lock) noexcept
    {
        return AcquireCredentialExclusiveLock(
            filePath,
            L".lock",
            lock);
    }

    // 資格情報の同時利用をセッションの寿命中排他する(filePath: 資格情報ファイル, lock: ロックの所有先)。
    [[nodiscard]] bool AcquireCredentialUsageLock(
        const std::filesystem::path& filePath,
        HandleGuard& lock) noexcept
    {
        return AcquireCredentialExclusiveLock(
            filePath,
            L".session.lock",
            lock);
    }

    // 秘密値を含まない操作成功を返す。
    [[nodiscard]] LamaPon::Detail::OnlinePlatformResult Success()
    {
        return { true, {}, {} };
    }

    // エンジン定義の失敗を返す(code: 固定エラー識別子, message: 秘密値を含まない診断)。
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

    // tokenを含まない読取失敗を作る(status: 不在・利用不可・破損の種別, code: 固定エラー識別子, message: 固定診断)。
    [[nodiscard]] LamaPon::Detail::RefreshTokenLoadResult LoadFailure(
        const LamaPon::Detail::RefreshTokenLoadStatus status,
        std::string code,
        std::string message)
    {
        // 固定診断だけを持つ読取失敗
        LamaPon::Detail::RefreshTokenLoadResult result;
        result.status = status;
        result.errorCode = std::move(code);
        result.errorMessage = std::move(message);
        return result;
    }

    // 長さと名前空間で許可するASCII文字を検証する(value: 識別子, maximumBytes: 最大バイト数)。
    [[nodiscard]] bool IsSafeIdentifier(
        const std::string_view value,
        const std::size_t maximumBytes)
    {
        // 名前空間で許可するASCII文字を検証する(character: 検証する1文字)。
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

    // 更新用tokenの長さと制御文字・空白の不在を検証する(value: 検証するtoken)。
    [[nodiscard]] bool IsSafeRefreshToken(
        const std::string_view value)
    {
        // 制御文字と空白を検出する(character: 検証するtokenの1文字)。
        return !value.empty()
            && value.size() <= MaximumRefreshTokenBytes
            && std::ranges::none_of(
                value,
                [](const unsigned char character)
                {
                    return character <= 0x20 || character == 0x7f;
                });
    }

    // 16ビット値を下位バイトから追記する(bytes: 追記先, value: 追加する整数)。
    void AppendU16(
        std::vector<std::uint8_t>& bytes,
        const std::uint16_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    }

    // 32ビット値を下位バイトから追記する(bytes: 追記先, value: 追加する整数)。
    void AppendU32(
        std::vector<std::uint8_t>& bytes,
        const std::uint32_t value)
    {
        // 整数を処理するビット位置
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<std::uint8_t>(
                (value >> shift) & 0xffu));
        }
    }

    // 2バイトから下位バイト順の整数を復元する(bytes: 2バイト以上の入力の借用)。
    [[nodiscard]] std::uint16_t ReadU16(
        const std::uint8_t* bytes) noexcept
    {
        return static_cast<std::uint16_t>(bytes[0])
            | static_cast<std::uint16_t>(bytes[1] << 8u);
    }

    // 4バイトから下位バイト順の整数を復元する(bytes: 4バイト以上の入力の借用)。
    [[nodiscard]] std::uint32_t ReadU32(
        const std::uint8_t* bytes) noexcept
    {
        // 復元中の32ビット整数
        std::uint32_t value{};
        // 整数を処理するビット位置
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            value |= static_cast<std::uint32_t>(
                bytes[shift / 8]) << shift;
        }
        return value;
    }

    // ゲームと環境を検証し長さ付きの暗号化補助値を作る(gameId: ゲームの名前空間ID, environmentId: 環境の名前空間ID)。
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
        // ゲームと環境を結合した暗号化補助値
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

    // 補助値をHMACでパス名用の小文字16進表記にする(entropy: ゲームと環境の補助値)。
    [[nodiscard]] std::string CredentialPathHash(
        const std::vector<std::uint8_t>& entropy)
    {
        // 資格情報パス用のHMAC値
        const auto digest = LamaPon::Crypto::Hmac(
            CredentialPathHashKey,
            entropy.data(),
            entropy.size());
        // 小文字の16進数字一覧
        constexpr char HexDigits[] = "0123456789abcdef";
        // 資格情報のパス用HMACの16進表記
        std::string result(digest.size() * 2, '0');
        // HMACの変換バイト位置
        for (std::size_t index = 0; index < digest.size(); ++index)
        {
            result[index * 2] = HexDigits[digest[index] >> 4u];
            result[index * 2 + 1] = HexDigits[digest[index] & 0x0fu];
        }
        return result;
    }

    // 継承を遮断し現ユーザーとSYSTEMだけを許可する(path: 対象の保存先, directory: 子への継承が必要か)。
    [[nodiscard]] bool ApplyRestrictedAcl(
        const std::filesystem::path& path,
        const bool directory)
    {
        // 現ユーザーとSYSTEM限定のACL
        RestrictedSecurity security;
        // ディレクトリ用の継承設定
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

    // 全バイトの書き込みを試し途中の失敗をfalseとする(file: 書き込み先ハンドル, bytes: 書き込む内容)。
    [[nodiscard]] bool WriteAll(
        const HANDLE file,
        const std::vector<std::uint8_t>& bytes)
    {
        // 読み書き済みのバイト位置
        std::size_t offset{};
        while (offset < bytes.size())
        {
            // 今回読み書きする残りバイト数
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            // 今回書き込んだバイト数
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

    // 確保済み領域全体を読み込み途中の失敗をfalseとする(file: 読み取り先ハンドル, bytes: 読む長さを確保済みの出力)。
    [[nodiscard]] bool ReadAll(
        const HANDLE file,
        std::vector<std::uint8_t>& bytes)
    {
        // 読み書き済みのバイト位置
        std::size_t offset{};
        while (offset < bytes.size())
        {
            // 今回読み書きする残りバイト数
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            // 今回読み取ったバイト数
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

    // LocalAppDataに名前空間別の保存先を導出する(gameId: ゲームの名前空間ID, environmentId: 環境の名前空間ID)。
    [[nodiscard]] std::filesystem::path LocalCredentialPath(
        const std::string_view gameId,
        const std::string_view environmentId)
    {
        // LocalAppData保存先の所有領域
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
        // ゲームと環境を結合した暗号化補助値
        auto entropy = BuildEntropy(gameId, environmentId);
        // 匿名化した資格情報の保存先キー
        const auto pathHash = CredentialPathHash(entropy);
        // entropyには生のIDが入るため、パスを組み立てた時点で消します。
        LamaPon::Crypto::SecureErase(entropy);
        return std::filesystem::path(localAppData.value)
            / L"LamaPon"
            / L"Online"
            / LamaPon::PathFromUtf8(pathHash)
            / L"session.bin";
    }

    // ファイルを削除し不在も成功とする(path: 削除する保存先)。
    [[nodiscard]] bool DeleteIfPresent(
        const std::filesystem::path& path) noexcept
    {
        if (DeleteFileW(path.c_str()) != FALSE)
        {
            return true;
        }
        // Windows APIの失敗コード
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND;
    }

    // localhostとIPv4・IPv6のループバック表記かを返す(host: 比較するホスト名)。
    [[nodiscard]] bool IsLoopbackHost(
        const std::wstring_view host)
    {
        // 小文字で比較する接続先のホスト名
        std::wstring lower(host);
        // ホスト名を小文字で比較する(character: ホスト名の1文字)。
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

    // URL構文を検証しHTTPSまたは許可したローカルHTTPに限定する(url: 認証URL, allowInsecureLoopback: ローカルHTTPを許可するか, wide: UTF-16の出力)。
    [[nodiscard]] bool IsSafeAuthorizationUrl(
        const std::string_view url,
        const bool allowInsecureLoopback,
        std::wstring& wide)
    {
        // URLをshellへ安全に渡せるか検証する(character: URLの1文字)。
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
        // URLの方式と接続先の区切り位置
        const auto schemeMarker = url.find("://");
        if (schemeMarker == std::string_view::npos)
        {
            return false;
        }
        // URLの接続先が終わる位置
        const auto authorityEnd = url.find_first_of(
            "/?#",
            schemeMarker + 3);
        // URLのホストとポートの借用
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
        // WinHTTPで解析するURLの項目
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
        // WinHTTPで解析したホスト名の借用
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
    void SetWindowsRefreshTokenSaveTestFailPoint(
        const WindowsRefreshTokenSaveTestFailPoint failPoint) noexcept
    {
        CredentialSaveFailPoint.store(
            failPoint,
            std::memory_order_release);
    }

    void SetWindowsRefreshTokenSaveBeforeReplaceHook(
        const WindowsRefreshTokenSaveTestHook hook,
        void* const context) noexcept
    {
        CredentialSaveBeforeReplaceContext.store(
            context,
            std::memory_order_release);
        CredentialSaveBeforeReplaceHook.store(
            hook,
            std::memory_order_release);
    }

    WindowsRefreshTokenStore::WindowsRefreshTokenStore(
        std::filesystem::path filePath,
        std::string gameId,
        std::string environmentId)
        : m_filePath(std::move(filePath))
        , m_entropy(BuildEntropy(gameId, environmentId))
        , m_storageAvailable(!m_filePath.empty())
    {
    }

    WindowsRefreshTokenStore::~WindowsRefreshTokenStore()
    {
        ReleaseUsageLease();
    }

    OnlinePlatformResult WindowsRefreshTokenStore::AcquireUsageLease()
    {
        if (m_usageLeaseHandle != nullptr)
        {
            return Success();
        }
        if (!m_storageAvailable)
        {
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }

        // 利用ロック取得前の親保存先の状態
        const auto initialParentState =
            InspectCredentialParent(m_filePath);
        if (initialParentState == ParentDirectoryState::Unavailable)
        {
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        // 親ディレクトリ作成の失敗理由
        std::error_code directoryError;
        if (!EnsureDirectoryExists(
                m_filePath.parent_path(),
                directoryError)
            || InspectCredentialParent(m_filePath)
                != ParentDirectoryState::Exists
            || !ApplyRestrictedAcl(
                m_filePath.parent_path(),
                true))
        {
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }

        // 同時利用を排他するロックの所有先
        HandleGuard usageLease;
        if (!AcquireCredentialUsageLock(m_filePath, usageLease))
        {
            return Failure(
                "credential_usage_unavailable",
                "The saved online session is already in use or unavailable.");
        }
        m_usageLeaseHandle = usageLease.Release();
        return Success();
    }

    void WindowsRefreshTokenStore::ReleaseUsageLease() noexcept
    {
        // 解放する資格情報利用のハンドル
        const auto lease = std::exchange(
            m_usageLeaseHandle,
            nullptr);
        if (lease != nullptr
            && lease != static_cast<void*>(INVALID_HANDLE_VALUE))
        {
            CloseHandle(static_cast<HANDLE>(lease));
        }
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
        // 資格情報の親保存先の検査結果
        const auto parentState = InspectCredentialParent(m_filePath);
        if (parentState == ParentDirectoryState::Missing)
        {
            // 更新用tokenの読み込み結果
            RefreshTokenLoadResult result;
            result.status = RefreshTokenLoadStatus::NotFound;
            return result;
        }
        if (parentState != ParentDirectoryState::Exists)
        {
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }
        // 資格情報の操作中の排他ロック
        HandleGuard operationLock;
        if (!AcquireCredentialOperationLock(
                m_filePath,
                operationLock))
        {
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }

        // 検証と読み書き用の所有ハンドル
        HandleGuard file;
        file.value = CreateFileW(
            m_filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (file.value == INVALID_HANDLE_VALUE)
        {
            // Windows APIの失敗コード
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND
                || error == ERROR_PATH_NOT_FOUND)
            {
                // 更新用tokenの読み込み結果
                RefreshTokenLoadResult result;
                result.status = RefreshTokenLoadStatus::NotFound;
                return result;
            }
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }
        if (!IsPlainSingleLinkFile(file.value))
        {
            return LoadFailure(
                RefreshTokenLoadStatus::Unavailable,
                "credential_read_unavailable",
                "The saved online session could not be read.");
        }

        // 暗号化ファイルのバイト数
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

        // 暗号化ファイル全体の読み取り領域
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

        // 暗号化ファイルの版と識別子が正しいか
        const bool validEnvelope = std::equal(
                FileMagic.begin(),
                FileMagic.end(),
                fileBytes.begin())
            && ReadU16(fileBytes.data() + 8) == 1
            && ReadU16(fileBytes.data() + 10) == 0;
        // 暗号化blobの記録済みバイト数
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

        // 現Windowsユーザーによる復号結果
        auto unprotected = Crypto::UnprotectForCurrentUser(
            fileBytes.data() + EnvelopeHeaderBytes,
            protectedSize,
            m_entropy.data(),
            m_entropy.size());
        Crypto::SecureErase(fileBytes);
        if (!unprotected.Succeeded())
        {
            // 復号失敗に対応する読み込み結果
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

        // 復号後または暗号化前の包絡内容
        auto& plaintext = unprotected.data;
        // 復号した包絡の版と識別子が正しいか
        const bool validPlaintext = plaintext.size()
                >= EnvelopeHeaderBytes
            && std::equal(
                PlaintextMagic.begin(),
                PlaintextMagic.end(),
                plaintext.begin())
            && ReadU16(plaintext.data() + 8) == 1
            && ReadU16(plaintext.data() + 10) == 0;
        // 復号したtokenの記録済みバイト数
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

        // 検証して呼出し側へ渡す更新用token
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

        // 更新用tokenの読み込み結果
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

        // 復号後または暗号化前の包絡内容
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
        if (ConsumeCredentialSaveFailPoint(
                WindowsRefreshTokenSaveTestFailPoint::Protection))
        {
            Crypto::SecureErase(plaintext);
            return Failure(
                "credential_protection_unavailable",
                "The online session could not be protected.");
        }
        // 現Windowsユーザーによる暗号化結果
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

        // 暗号化blobとファイルヘッダー
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

        // 親ディレクトリ作成の失敗理由
        std::error_code directoryError;
        if (!EnsureDirectoryExists(
                m_filePath.parent_path(),
                directoryError)
            || InspectCredentialParent(m_filePath)
                != ParentDirectoryState::Exists
            || !ApplyRestrictedAcl(
                m_filePath.parent_path(),
                true))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        // 資格情報の操作中の排他ロック
        HandleGuard operationLock;
        if (!AcquireCredentialOperationLock(
                m_filePath,
                operationLock))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }

        // 資格情報ファイルに適用するACL
        RestrictedSecurity fileSecurity;
        if (!fileSecurity.Initialize(NO_INHERITANCE))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        // 確定値を保つ書き込み先のパス
        auto temporary = m_filePath;
        temporary += L".tmp";
        if (ConsumeCredentialSaveFailPoint(
                WindowsRefreshTokenSaveTestFailPoint::TemporaryWrite))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_write_failed",
                "The online session could not be saved.");
        }
        if (!RemoveSafeCredentialTemporary(temporary))
        {
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_write_failed",
                "The online session could not be saved.");
        }
        // 検証と読み書き用の所有ハンドル
        HandleGuard file;
        file.value = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE | FILE_READ_ATTRIBUTES
                | READ_CONTROL | WRITE_DAC,
            0,
            &fileSecurity.attributes,
            CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (file.value == INVALID_HANDLE_VALUE
            || !ProtectPlainFileHandle(file.value, fileSecurity)
            || !WriteAll(file.value, fileBytes)
            || FlushFileBuffers(file.value) == FALSE)
        {
            file.Reset();
            static_cast<void>(
                RemoveSafeCredentialTemporary(temporary));
            Crypto::SecureErase(fileBytes);
            return Failure(
                "credential_write_failed",
                "The online session could not be saved.");
        }
        file.Reset();
        Crypto::SecureErase(fileBytes);
        if (ConsumeCredentialSaveFailPoint(
                WindowsRefreshTokenSaveTestFailPoint::TemporaryAcl))
        {
            static_cast<void>(
                RemoveSafeCredentialTemporary(temporary));
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        if (!ApplyRestrictedAcl(temporary, false))
        {
            static_cast<void>(
                RemoveSafeCredentialTemporary(temporary));
            return Failure(
                "credential_storage_unavailable",
                "Secure credential storage is unavailable.");
        }
        // 置換前に実行するテスト処理
        if (const auto hook =
                CredentialSaveBeforeReplaceHook.load(
                    std::memory_order_acquire))
        {
            hook(CredentialSaveBeforeReplaceContext.load(
                std::memory_order_acquire));
        }
        if (ConsumeCredentialSaveFailPoint(
                WindowsRefreshTokenSaveTestFailPoint::Replace))
        {
            static_cast<void>(
                RemoveSafeCredentialTemporary(temporary));
            return Failure(
                "credential_write_failed",
                "The online session could not be saved.");
        }
        if (MoveFileExW(
                temporary.c_str(),
                m_filePath.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            static_cast<void>(
                RemoveSafeCredentialTemporary(temporary));
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
        // 資格情報の親保存先の検査結果
        const auto parentState = InspectCredentialParent(m_filePath);
        if (parentState == ParentDirectoryState::Missing)
        {
            return Success();
        }
        if (parentState != ParentDirectoryState::Exists)
        {
            return Failure(
                "credential_delete_failed",
                "The saved online session could not be deleted.");
        }
        // 資格情報の操作中の排他ロック
        HandleGuard operationLock;
        if (!AcquireCredentialOperationLock(
                m_filePath,
                operationLock))
        {
            return Failure(
                "credential_delete_failed",
                "The saved online session could not be deleted.");
        }
        // 確定値を保つ書き込み先のパス
        auto temporary = m_filePath;
        temporary += L".tmp";
        // 片方の削除が失敗しても両方を試し、書き込み残骸を消せなければ失敗とします。
        // 確定値の削除または不在を確認したか
        const bool finalDeleted = DeleteIfPresent(m_filePath);
        // 安全な書き込み残骸を削除したか
        const bool temporaryDeleted =
            RemoveSafeCredentialTemporary(temporary);
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
            // shellへ起動を渡す(owner: 所有window, operation: shellの動詞, file: URL, parameters: 起動引数, directory: 作業ディレクトリ, showCommand: 表示方法)。
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
        // 検証した認証URLのUTF-16表記
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
        // shell起動の成否を示す戻り値
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
        // 名前空間別の資格情報保存先
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
