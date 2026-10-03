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

    // 3種類の内容を保持する上限B
    constexpr std::size_t MaximumJournalPayloadBytes =
        LamaPon::CloudSaveAccountMaxBytes * 3u;
    // base64url変換後の最大B
    constexpr std::size_t MaximumEncodedPayloadBytes =
        (MaximumJournalPayloadBytes * 4u + 2u) / 3u;
    // 3種類の内容を各16MiB保持し、base64url変換分に管理情報用の2MiBを加えます。
    // 本文と管理情報の最大B
    constexpr std::size_t MaximumJournalBytes =
        MaximumEncodedPayloadBytes + 2u * 1024u * 1024u;
    // 許容するJSON階層の上限
    constexpr std::size_t MaximumJsonDepth = 64u;
    // 許容するJSON要素数の上限
    constexpr std::size_t MaximumJsonElements = 65536u;
    // journal形式の識別名
    constexpr std::string_view JournalFormat =
        "LamaPonCloudSaveJournal";
    // 保存先bindingの形式識別子
    constexpr std::string_view BindingDomain =
        "LamaPon.CloudSave.Journal.Binding.1";

    // 次の該当処理だけを失敗させる指定
    std::atomic<LamaPon::Detail::CloudSaveJournalTestFailPoint>
        JournalFailPoint{};

    // 指定段階の失敗注入を一度だけ消費する(expected: 今回到達した処理段階)。
    bool ConsumeFailPoint(
        const LamaPon::Detail::CloudSaveJournalTestFailPoint expected) noexcept
    {
        // 失敗指定を消費する比較交換の値
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
        // CNGプロバイダーを解放する。
        ~AlgorithmHandle()
        {
            if (value != nullptr)
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        }

        // CNGプロバイダーの所有ハンドル
        BCRYPT_ALG_HANDLE value{};
    };

    class FileHandle final
    {
    public:
        // 無効なファイルハンドルで初期化する。
        FileHandle() = default;
        // ハンドルの所有権を受け取る(handle: 解放責任を引き取るファイル)。
        explicit FileHandle(const HANDLE handle) noexcept
            : value(handle)
        {
        }

        // 所有する有効なファイルハンドルを解放する。
        ~FileHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        // 所有ハンドルの複製を禁止する。
        FileHandle(const FileHandle&) = delete;
        // 所有ハンドルのコピー代入を禁止する。
        FileHandle& operator=(const FileHandle&) = delete;

        // ファイルハンドルの所有権を移す(other: ハンドルを渡す移動元)。
        FileHandle(FileHandle&& other) noexcept
            : value(std::exchange(other.value, INVALID_HANDLE_VALUE))
        {
        }

        // 現在のハンドルを解放し所有権を移す(other: ハンドルを渡す移動元)。
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

        // ファイルの所有ハンドル
        HANDLE value{ INVALID_HANDLE_VALUE };
    };

    struct RestrictedSecurity final
    {
        // 現プロセスtokenの所有先
        FileHandle processToken;
        // ユーザーSIDを保持する領域
        std::vector<std::uint8_t> tokenUser;
        // 解放するSYSTEMのSID
        PSID systemSid{};
        // LocalFreeするアクセス許可一覧
        PACL acl{};
        // 作成するセキュリティ記述子
        SECURITY_DESCRIPTOR descriptor{};
        // 作成時のACLと継承条件
        SECURITY_ATTRIBUTES attributes{};

        // ACLとSYSTEM SIDの確保領域を解放する。
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

        // 現ユーザーとSYSTEMだけを許可するACLを作る(inheritance: 子オブジェクトへの継承フラグ)。
        bool Initialize(const DWORD inheritance)
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
            // 取得したWindowsユーザー情報
            const auto* currentUser =
                reinterpret_cast<const TOKEN_USER*>(tokenUser.data());
            // アクセス許可または保存先の一覧
            EXPLICIT_ACCESSW entries[2]{};
            // 検証または更新する保存先の記録
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

        // 保持するTokenUser領域から現ユーザーのSIDを借用する。
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
        // 記録する保存先
        CloudSaveResource resource;
        // 同期済みの基準状態
        std::optional<CloudSaveSnapshot> baseline;
        // 再送条件を保持する未送信更新
        std::optional<CloudSavePendingMutation> pending;
        // CAS競合時点のリモート状態
        std::optional<CloudSaveSnapshot> conflict;
        // ETag取得前のローカル削除意思
        bool localDeleteIntent{};
    };

    struct JournalState final
    {
        // journalの形式版
        std::uint64_t schemaVersion{ 2u };
        // 現在の世代・未保存なら0
        std::uint64_t generation{};
        // 直前の世代
        std::uint64_t parentGeneration{};
        // 親世代の照合ハッシュ
        std::string parentChecksum;
        // 現在の世代の照合ハッシュ
        std::string checksum;
        // 保存先ごとの同期記録
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
        // 本体・次世代・退避の種別
        CandidateSource source{ CandidateSource::Final };
        // 候補のファイルパス
        std::filesystem::path path;
        // 検証済みの復旧状態
        JournalState state;
        // 検証済み状態の正規JSON
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
        // 候補を読み取った結果
        CandidateStatus status{ CandidateStatus::Missing };
        // 検証を通った復旧候補
        std::optional<Candidate> candidate;
    };

    class UnsupportedJournalVersion final : public std::runtime_error
    {
    public:
        // 未対応のjournal形式を表す例外を作る。
        UnsupportedJournalVersion()
            : std::runtime_error("Cloud save journal version is unsupported.")
        {
        }
    };

    class JsonSyntaxError final : public std::runtime_error
    {
    public:
        // JSON構文の破損を表す例外を作る。
        JsonSyntaxError()
            : std::runtime_error("Cloud save journal JSON syntax is invalid.")
        {
        }
    };

    class RecoverableJournalCorruption final : public std::runtime_error
    {
    public:
        // 他の候補から復旧可能な書き込み破損を表す例外を作る。
        RecoverableJournalCorruption()
            : std::runtime_error("Cloud save journal write is incomplete.")
        {
        }
    };

    // 保存内容を含まない固定の操作失敗を送出する。
    [[noreturn]] void ThrowJournalFailure()
    {
        throw std::runtime_error("Cloud save journal operation failed.");
    }

    // 復旧を継続できないjournal破損を送出する。
    [[noreturn]] void ThrowCorruptJournal()
    {
        throw std::runtime_error("Cloud save journal is corrupt.");
    }

    // ファイルまたは親パスの不在かを返す(error: Windows APIの失敗コード)。
    bool IsMissingError(const DWORD error) noexcept
    {
        return error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND;
    }

    // ファイル名の末尾に復旧用の接尾辞を付ける(path: 元のファイルパス, suffix: 追加する接尾辞)。
    std::filesystem::path WithSuffix(
        const std::filesystem::path& path,
        const std::wstring_view suffix)
    {
        // 接尾辞を追加するファイルパス
        auto result = path;
        result += suffix;
        return result;
    }

    // 長さと小文字の16進形式を検証する(value: 検証する文字列, length: 必要な文字数)。
    bool IsLowerHex(
        const std::string_view value,
        const std::size_t length) noexcept
    {
        // 小文字の16進数字だけかを検証する(character: 検証する1文字)。
        return value.size() == length
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                });
    }

    // CNGで32バイトのハッシュを計算する(data: 読み取るバイト列, size: 入力のバイト数)。
    std::array<std::uint8_t, 32> Sha256(
        const std::uint8_t* data,
        const std::size_t size)
    {
        if (size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument("Cloud save journal data is too large.");
        }
        // SHA-256プロバイダーの所有先
        AlgorithmHandle algorithm;
        if (BCryptOpenAlgorithmProvider(
                &algorithm.value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0)
        {
            ThrowJournalFailure();
        }
        // 32バイトのSHA-256
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

    // ハッシュを小文字の16進表記へ変換する(digest: 32バイトのSHA-256)。
    std::string LowerHex(const std::array<std::uint8_t, 32>& digest)
    {
        // 小文字の16進数字一覧
        constexpr char Hex[] = "0123456789abcdef";
        // ハッシュの小文字16進表記
        std::string result;
        result.reserve(digest.size() * 2u);
        // 16進変換するハッシュの1バイト
        for (const auto byte : digest)
        {
            result.push_back(Hex[byte >> 4u]);
            result.push_back(Hex[byte & 0x0fu]);
        }
        return result;
    }

    // 文字列のSHA-256を小文字の16進表記で返す(value: ハッシュするバイト列)。
    std::string Sha256LowerHex(const std::string_view value)
    {
        return LowerHex(Sha256(
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size()));
    }

    // base64urlの符号化文字一覧
    constexpr char Base64UrlAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    // バイト列をパディングなしのbase64urlへ変換する(data: 読み取るバイト列, size: 入力のバイト数)。
    std::string EncodeBase64Url(
        const std::uint8_t* data,
        const std::size_t size)
    {
        // base64urlの出力文字列
        std::string result;
        result.reserve((size * 4u + 2u) / 3u);
        // 入力バイト列の処理位置
        std::size_t index{};
        while (index + 3u <= size)
        {
            // 3バイトを束ねた24ビット値
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
        // 末尾に残る入力バイト数
        const auto remaining = size - index;
        if (remaining == 1u)
        {
            // 3バイトを束ねた24ビット値
            const auto value = static_cast<std::uint32_t>(data[index]) << 16u;
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
        }
        else if (remaining == 2u)
        {
            // 3バイトを束ねた24ビット値
            const auto value =
                (static_cast<std::uint32_t>(data[index]) << 16u)
                | (static_cast<std::uint32_t>(data[index + 1u]) << 8u);
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
        }
        return result;
    }

    // base64urlの1文字を6ビット値にする(character: 変換する1文字)。
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

    // 未使用ビットも検証しbase64urlを復元する(encoded: パディングなしの文字列, maximumBytes: 復元後の上限B, decoded: 開始時に空にする出力)。
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
        // 検証する1文字
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
        // 復元されるバイト数
        const auto decodedSize = encoded.size() / 4u * 3u
            + (encoded.size() % 4u == 2u ? 1u : 0u)
            + (encoded.size() % 4u == 3u ? 2u : 0u);
        if (decodedSize > maximumBytes)
        {
            return false;
        }
        decoded.reserve(decodedSize);
        // base64urlの処理文字位置
        std::size_t index{};
        while (index + 4u <= encoded.size())
        {
            // 4文字から復元した24ビット値
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
        // 末尾に残るbase64url文字数
        const auto remaining = encoded.size() - index;
        if (remaining == 2u)
        {
            // 4文字から復元した24ビット値
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1u])) << 12u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
        }
        else if (remaining == 3u)
        {
            // 4文字から復元した24ビット値
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1u])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2u])) << 6u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
            decoded.push_back(static_cast<std::uint8_t>(value >> 8u));
        }
        return decoded.size() == decodedSize;
    }

    // 内容のSHA-256をbase64urlで返す(content: ハッシュするバイト列)。
    std::string ContentHash(const std::vector<std::uint8_t>& content)
    {
        // 32バイトのSHA-256
        const auto digest = Sha256(content.data(), content.size());
        return EncodeBase64Url(digest.data(), digest.size());
    }

    // SHA-256が正規のbase64url形式かを検証する(value: 検証するハッシュ表記)。
    bool IsCanonicalSha256(const std::string_view value)
    {
        // SHA-256形式の確認用バイト列
        std::vector<std::uint8_t> decoded;
        return value.size() == 43u
            && DecodeBase64Url(value, 32u, decoded)
            && decoded.size() == 32u;
    }

    // 空・弱い値・引用符内の不正文字を拒否する(value: 引用符付きETag)。
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
        // 引用符内の可視ASCII文字を検証する(character: 検証する1文字)。
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

    // 小文字UUIDv4の形式とvariantを検証する(value: 再送識別子)。
    bool IsCanonicalMutationId(const std::string_view value) noexcept
    {
        if (value.size() != 36u)
        {
            return false;
        }
        // UUID文字列の検証位置
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
            // 検証する1文字
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

    // 文字列内を除いてJSON階層の上限を検証する(text: JSON本文)。
    bool JsonNestingIsSafe(const std::string_view text) noexcept
    {
        // 文字列外のJSON階層深度
        std::size_t depth{};
        // JSON文字列の内部か
        bool inString{};
        // 直前のバックスラッシュ状態
        bool escaped{};
        // 検証する1文字
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

    // 残りの上限から子要素を再帰的に数える(value: 検証するJSON, remaining: 残りの許容要素数)。
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
            // 要素数を数えるJSONの子
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

    // UTF-8・階層・重複キー・要素数を検証してJSONを解析する(text: JSONのバイト列)。
    Json ParseJsonStrict(const std::string_view text)
    {
        if (text.empty()
            || LamaPon::Utf8ToWide(text).empty()
            || !JsonNestingIsSafe(text))
        {
            throw JsonSyntaxError();
        }

        // 同一objectでキーが重複したか
        bool duplicateKey{};
        // JSON階層ごとの確認済みキー
        std::array<std::unordered_set<std::string>, MaximumJsonDepth + 1u>
            keysByDepth;
        // 重複キーを検出する(depth: JSONの階層深度, event: 解析イベント, parsed: 解析中のJSON)。
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
                    // 現在のobjectのキー記録深度
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
                    // 現在のobjectで確認済みのキー
                    auto& keys = keysByDepth[static_cast<std::size_t>(depth)];
                    if (!keys.insert(parsed.get<std::string>()).second)
                    {
                        duplicateKey = true;
                    }
                }
                return true;
            };
        // 厳密に解析したJSON
        Json parsed;
        try
        {
            parsed = Json::parse(text, callback, true, false);
        }
        catch (const Json::parse_error&)
        {
            throw JsonSyntaxError();
        }
        // 許容するJSON要素数の残量
        std::size_t remainingElements = MaximumJsonElements;
        if (duplicateKey
            || !JsonElementCountIsSafe(parsed, remainingElements))
        {
            throw std::runtime_error("Cloud save journal JSON is invalid.");
        }
        return parsed;
    }

    // 過不足なく指定した項目を持つobjectかを返す(object: 検証するJSON, expected: 必要な項目名一覧)。
    bool HasExactKeys(
        const Json& object,
        const std::initializer_list<std::string_view> expected)
    {
        if (!object.is_object() || object.size() != expected.size())
        {
            return false;
        }
        // 必要な項目の存在を確認する(key: 必要なJSON項目名)。
        return std::ranges::all_of(
            expected,
            [&object](const std::string_view key)
            {
                return object.contains(std::string(key));
            });
    }

    // 保存先を検証してjournal用JSONへ変換する(resource: 設定または保存スロット)。
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

    // 種別と項目集合を検証して保存先を復元する(json: 保存先を表すJSON)。
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
            // 検証する保存スロット名
            auto slot = json.at("slot").get<std::string>();
            if (LamaPon::Detail::IsValidSaveSlotName(slot))
            {
                return CloudSaveResource::SaveSlot(std::move(slot));
            }
        }
        throw std::runtime_error("Cloud save journal resource is invalid.");
    }

    // 共通のスロット名規則で同じ保存先かを判定する(left: 比較元, right: 比較先)。
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

    // 保存先を検証し内容の最大バイト数を返す(resource: 設定または保存スロット)。
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

    // 保存先の容量上限と厳密なJSON形式を検証する(resource: 保存先, content: JSONのバイト列)。
    void ValidateContent(
        const CloudSaveResource& resource,
        const std::vector<std::uint8_t>& content)
    {
        if (content.empty() || content.size() > MaximumContentBytes(resource))
        {
            throw std::invalid_argument("Cloud save content is invalid.");
        }
        // JSON形式を検証する内容の借用
        const std::string_view text(
            reinterpret_cast<const char*>(content.data()),
            content.size());
        (void)ParseJsonStrict(text);
    }

    // 本文とハッシュを検証し保存状態をJSONにする(snapshot: 基準または競合の状態)。
    Json SnapshotJson(const CloudSaveSnapshot& snapshot)
    {
        (void)MaximumContentBytes(snapshot.resource);
        if (!IsStrongEtag(snapshot.etag))
        {
            throw std::invalid_argument("Cloud save snapshot ETag is invalid.");
        }
        // 保存状態を記録するJSON
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
        // 内容のbase64url SHA-256
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

    // 削除状態・容量・本文・ハッシュを検証して復元する(json: 保存状態のJSON, expectedResource: 対応すべき保存先)。
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
        // 記録した削除済みの印
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
        // JSONから復元した保存先
        auto resource = ParseResource(json.at("resource"));
        if (!EquivalentResources(resource, expectedResource))
        {
            throw std::runtime_error("Cloud save journal resource mismatch.");
        }
        // 復元する基準または競合の状態
        CloudSaveSnapshot snapshot;
        snapshot.resource = std::move(resource);
        snapshot.etag = json.at("etag").get<std::string>();
        snapshot.deleted = deleted;
        if (!IsStrongEtag(snapshot.etag))
        {
            throw std::runtime_error("Cloud save journal ETag is invalid.");
        }
        // 記録した内容のバイト数
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
        // 記録したbase64urlの内容
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

    // 更新ID・CAS条件・本文を検証してJSONにする(pending: 再送条件を保持する更新)。
    Json PendingJson(const CloudSavePendingMutation& pending)
    {
        (void)MaximumContentBytes(pending.resource);
        if (!IsCanonicalMutationId(pending.mutationId))
        {
            throw std::invalid_argument("Cloud save mutation id is invalid.");
        }
        // 未送信更新を記録するJSON
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
        // 内容のbase64url SHA-256
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

    // 種別と再送条件と内容の整合性を検証して復元する(json: 未送信更新のJSON, resource: 対応する保存先)。
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
        // 記録した更新の種別文字列
        const auto kind = json.at("kind").get<std::string>();
        // 更新が内容の保存か
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
        // 復元する未送信の更新
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
        // 記録したbase64urlの内容
        const auto encoded = json.at("content").get<std::string>();
        // 記録した内容のバイト数
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

    // 形式版に合わせて保存先の記録をJSONにする(entry: 保存先の同期記録, version: 出力する形式版)。
    Json EntryJson(const Entry& entry, const std::uint64_t version)
    {
        // 保存先の同期記録JSON
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

    // 同じ保存先の添字を返し不在なら一覧の末尾を返す(entries: 記録一覧, resource: 探す保存先)。
    std::size_t FindEntry(
        const std::vector<Entry>& entries,
        const CloudSaveResource& resource) noexcept
    {
        // 同じ保存先の記録を探す(entry: 照合する同期記録)。
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

    // 他の未送信更新が同じ識別子を使うかを返す(entries: 記録一覧, mutationId: 確認する更新ID, excluded: 検査から除外する記録)。
    bool MutationIdInUse(
        const std::vector<Entry>& entries,
        const std::string_view mutationId,
        const Entry* excluded = nullptr) noexcept
    {
        // 除外対象以外の更新IDを照合する(entry: 照合する同期記録)。
        return std::ranges::any_of(
            entries,
            [mutationId, excluded](const Entry& entry)
            {
                return &entry != excluded
                    && entry.pending
                    && entry.pending->mutationId == mutationId;
            });
    }

    // 保存先と更新IDの重複・内容・種別ごとの総容量を検証する(entries: 記録一覧)。
    void ValidateEntries(const std::vector<Entry>& entries)
    {
        // 設定データの記録数
        std::size_t preferences{};
        // 削除済みを含むスロット数
        std::size_t slots{};
        // 基準・未送信・競合の内容合計B
        std::array<std::uint64_t, 3> totals{};
        // 検証または変換する要素の位置
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            // 検証または更新する保存先の記録
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
            // 重複を調べる前の記録位置
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
            // 種別ごとに容量上限を検証して加算する(category: 基準・未送信・競合の添字, bytes: 加算する内容バイト数)。
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

    // 記録一覧を検証しチェックサム以外のJSONを作る(state: 出力するjournal状態, binding: 保存先と名前空間の照合値)。
    Json PayloadJson(
        const JournalState& state,
        const std::string_view binding)
    {
        ValidateEntries(state.entries);
        // アクセス許可または保存先の一覧
        Json entries = Json::array();
        // 検証または更新する保存先の記録
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

    // 状態にチェックサムを設定し容量内の正規JSONを返す(state: チェックサムを更新する状態, binding: 保存先と名前空間の照合値)。
    std::string SerializeState(
        JournalState& state,
        const std::string_view binding)
    {
        // チェックサムを除く保存データ
        auto payload = PayloadJson(state, binding);
        // ハッシュ計算用の正規JSON
        const auto canonicalPayload = payload.dump();
        state.checksum = Sha256LowerHex(canonicalPayload);
        payload["checksum"] = state.checksum;
        // journal全体のJSON
        const auto document = payload.dump();
        if (document.size() > MaximumJournalBytes)
        {
            throw std::runtime_error("Cloud save journal is too large.");
        }
        return document;
    }

    // 形式版・保存先・世代連鎖・内容を照合して復元する(text: journal全体のJSON, expectedBinding: この保存先の照合値)。
    JournalState ParseState(
        const std::string_view text,
        const std::string_view expectedBinding)
    {
        // journal全体のJSON
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
        // 保存先と名前空間の照合ハッシュ
        const auto binding = document.at("binding").get<std::string>();
        if (binding != expectedBinding)
        {
            throw std::domain_error("Cloud save journal binding does not match.");
        }
        // 復元するjournalの形式版
        const auto version = document.at("version").get<std::uint64_t>();
        // 検証・復元するjournal状態
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
        // チェックサムを除く保存データ
        auto payload = document;
        payload.erase("checksum");
        if (Sha256LowerHex(payload.dump()) != state.checksum)
        {
            throw RecoverableJournalCorruption();
        }
        // 復元する保存先のJSON記録
        for (const auto& item : document.at("entries"))
        {
            // 版に対応する項目集合か
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
            // 検証または更新する保存先の記録
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

    // 非空のパスを正規化した絶対パスにする(path: 正規化するパス)。
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

    // 大小文字も含め正規化後の綴りが一致するかを返す(left: 比較元のパス, right: 比較先のパス)。
    bool EquivalentPaths(
        const std::filesystem::path& left,
        const std::filesystem::path& right) noexcept
    {
        try
        {
            // 正規化した比較元のパス
            const auto a = NormalizedAbsolute(left).native();
            // 正規化した比較先のパス
            const auto b = NormalizedAbsolute(right).native();
            if (a.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
                || b.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }
            // 大小文字を区別する保存先でも別の名前空間へ紐付かないよう綴りを照合します。
            return a == b;
        }
        catch (...)
        {
            return false;
        }
    }

    // 正規化した絶対パスをUTF-8にする(path: 変換するパス)。
    std::string NormalizedPathUtf8(const std::filesystem::path& path)
    {
        return LamaPon::PathToUtf8(NormalizedAbsolute(path));
    }

    // 許可した方式と接続先を検証してURLを正規化する(baseUrl: サービスの基点URL, allowInsecureLoopback: ローカルHTTPを許可するか)。
    std::string NormalizedBackendBaseUrl(
        std::string baseUrl,
        const bool allowInsecureLoopback)
    {
        return LamaPon::Detail::NormalizeOnlineServiceBaseUrl(
            std::move(baseUrl),
            allowInsecureLoopback);
    }

    // 長さの8バイト表記を先行させて値を結合する(output: 追記先, value: 結合する文字列)。
    void AppendLengthTagged(std::string& output, const std::string_view value)
    {
        // 結合する文字列のバイト数
        const auto length = static_cast<std::uint64_t>(value.size());
        // 長さの上位からのビット位置
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            output.push_back(static_cast<char>((length >> shift) & 0xffu));
        }
        output.append(value);
    }

    // 保存先と名前空間を長さ付きで結合してハッシュにする(accountRoot: 保存先のルート, accountStorageKey: アカウントの保存キー, gameId: ゲームID, environmentId: 環境ID, normalizedBackendBaseUrl: 正規化した基点URL)。
    std::string MakeBinding(
        const std::filesystem::path& accountRoot,
        const std::string_view accountStorageKey,
        const std::string_view gameId,
        const std::string_view environmentId,
        const std::string_view normalizedBackendBaseUrl)
    {
        // 長さ付きで結合するbindingの入力
        std::string input(BindingDomain);
        AppendLengthTagged(input, NormalizedPathUtf8(accountRoot));
        AppendLengthTagged(input, accountStorageKey);
        AppendLengthTagged(input, gameId);
        AppendLengthTagged(input, environmentId);
        AppendLengthTagged(input, normalizedBackendBaseUrl);
        return Sha256LowerHex(input);
    }

    // 既存パスがreparse pointでないディレクトリかを検証する(path: 検証するパス)。
    void ValidateExistingDirectory(const std::filesystem::path& path)
    {
        // パスまたはハンドルの属性情報
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
        {
            ThrowJournalFailure();
        }
    }

    // 現ユーザー・SYSTEM・所属グループの所有を許可する(owner: 所有SID, currentUserSid: 現ユーザーのSID, systemSid: SYSTEMのSID)。
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
        // 所有者のグループに所属するか
        BOOL isMember = FALSE;
        return CheckTokenMembership(nullptr, owner, &isMember) != FALSE
            && isMember != FALSE;
    }

    // 継承を遮断し現ユーザーとSYSTEMだけに全権限があるかを返す(path: 検証するパス, directory: 子への継承が必要か, currentUserSid: 現ユーザーのSID, systemSid: SYSTEMのSID)。
    bool VerifyRestrictedAcl(
        const std::filesystem::path& path,
        const bool directory,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        // Windowsオブジェクトの所有SID
        PSID owner{};
        // 検証するアクセス許可一覧
        PACL acl{};
        // 取得したセキュリティ記述子
        PSECURITY_DESCRIPTOR descriptor{};
        // 変換または検証の結果
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
        // DACLの継承制御フラグ
        SECURITY_DESCRIPTOR_CONTROL control{};
        // セキュリティ記述子の版
        DWORD revision{};
        // アクセス許可一覧のサイズ情報
        ACL_SIZE_INFORMATION information{};
        // 許可一覧が必要な条件を満たすか
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
            && OwnerSidIsTrusted(owner, currentUserSid, systemSid);
        // 現ユーザーの許可を確認済みか
        bool currentUserSeen{};
        // SYSTEMの許可を確認済みか
        bool systemSeen{};
        // 要求するACEの継承フラグ
        const auto expectedInheritance = static_cast<BYTE>(directory
            ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
            : 0u);
        // 検証または変換する要素の位置
        for (DWORD index = 0u; valid && index < information.AceCount; ++index)
        {
            // 取得したアクセス許可の領域
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE || rawAce == nullptr)
            {
                valid = false;
                break;
            }
            // 検証する許可ACE
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            // 検証するACEの継承フラグ
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
            // 許可ACEに記録したSID
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

    // パスの所有者が許可対象かを返す(path: 対象パス, currentUserSid: 現ユーザーのSID, systemSid: SYSTEMのSID)。
    bool PathOwnerIsAllowed(
        const std::filesystem::path& path,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        // Windowsオブジェクトの所有SID
        PSID owner{};
        // 取得したセキュリティ記述子
        PSECURITY_DESCRIPTOR descriptor{};
        // 変換または検証の結果
        const auto result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        // 所有SIDが許可対象か
        const bool allowed = result == ERROR_SUCCESS
            && descriptor != nullptr
            && OwnerSidIsTrusted(owner, currentUserSid, systemSid);
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return allowed;
    }

    // 開いたファイルの所有者が許可対象かを返す(file: 対象ハンドル, currentUserSid: 現ユーザーのSID, systemSid: SYSTEMのSID)。
    bool HandleOwnerIsAllowed(
        const HANDLE file,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        // Windowsオブジェクトの所有SID
        PSID owner{};
        // 取得したセキュリティ記述子
        PSECURITY_DESCRIPTOR descriptor{};
        // 変換または検証の結果
        const auto result = GetSecurityInfo(
            file,
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        // 所有SIDが許可対象か
        const bool allowed = result == ERROR_SUCCESS
            && descriptor != nullptr
            && OwnerSidIsTrusted(owner, currentUserSid, systemSid);
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return allowed;
    }

    // 許可した所有者のパスへ制限ACLを適用して検証する(path: 対象パス, directory: 子への継承が必要か)。
    bool ApplyRestrictedAcl(
        const std::filesystem::path& path,
        const bool directory)
    {
        // 現ユーザーとSYSTEM限定のACL
        RestrictedSecurity security;
        // ディレクトリ用の継承設定
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

    // 開いた対象が継承遮断と二者限定のACLを満たすかを返す(file: 対象ハンドル, directory: 子への継承が必要か, currentUserSid: 現ユーザーのSID, systemSid: SYSTEMのSID)。
    bool VerifyRestrictedAclHandle(
        const HANDLE file,
        const bool directory,
        const PSID currentUserSid,
        const PSID systemSid)
    {
        // Windowsオブジェクトの所有SID
        PSID owner{};
        // 検証するアクセス許可一覧
        PACL acl{};
        // 取得したセキュリティ記述子
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
        // DACLの継承制御フラグ
        SECURITY_DESCRIPTOR_CONTROL control{};
        // セキュリティ記述子の版
        DWORD revision{};
        // アクセス許可一覧のサイズ情報
        ACL_SIZE_INFORMATION information{};
        // 許可一覧が必要な条件を満たすか
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
            && OwnerSidIsTrusted(owner, currentUserSid, systemSid);
        // 現ユーザーの許可を確認済みか
        bool currentUserSeen{};
        // SYSTEMの許可を確認済みか
        bool systemSeen{};
        // 要求するACEの継承フラグ
        const auto expectedInheritance = static_cast<BYTE>(directory
            ? CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE
            : 0u);
        // 検証または変換する要素の位置
        for (DWORD index = 0u; valid && index < information.AceCount; ++index)
        {
            // 取得したアクセス許可の領域
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE || rawAce == nullptr)
            {
                valid = false;
                break;
            }
            // 検証する許可ACE
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            // 検証するACEの継承フラグ
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
            // 許可ACEに記録したSID
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

    // 存在する祖先をたどりディレクトリ以外とreparse pointを拒否する(path: 信頼する保存先)。
    void ValidateExistingPathComponents(
        const std::filesystem::path& path)
    {
        // 正規化した絶対パス
        const auto normalized = NormalizedAbsolute(path);
        // 検証中の親ディレクトリ
        auto current = normalized.root_path();
        // 順に検証するパスの構成要素
        for (const auto& component : normalized.relative_path())
        {
            current /= component;
            // パスまたはハンドルの属性情報
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

    // 通常ディレクトリを用意し必要なら制限ACLを適用する(path: 作成または検証する保存先, restrictAccess: 現ユーザーとSYSTEMに制限するか)。
    void EnsurePlainDirectory(
        const std::filesystem::path& path,
        const bool restrictAccess)
    {
        // 現ユーザーとSYSTEM限定のACL
        RestrictedSecurity security;
        // パスまたはハンドルの属性情報
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
            // Windows APIの失敗コード
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

    // リンクとACLを検証して共有なしのプロセス間ロックを取得する(path: ロックファイル)。
    FileHandle AcquireLock(const std::filesystem::path& path)
    {
        // 現ユーザーとSYSTEM限定のACL
        RestrictedSecurity security;
        if (!security.Initialize(NO_INHERITANCE))
        {
            ThrowJournalFailure();
        }
        // 検証と読み書き用の所有ハンドル
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
            // Windows APIの失敗コード
            const auto error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION
                || error == ERROR_LOCK_VIOLATION)
            {
                throw CloudSaveJournalBusyError();
            }
            ThrowJournalFailure();
        }
        // パスまたはハンドルの属性情報
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        // ファイル長とリンク数の情報
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

    // 開いた候補の属性・ACL・形式を検証し失敗種別を返す(path: 候補ファイル, source: 候補の種別, binding: 保存先と名前空間の照合値)。
    CandidateRead ReadCandidate(
        const std::filesystem::path& path,
        const CandidateSource source,
        const std::string_view binding)
    {
        // 現ユーザーとSYSTEM限定のACL
        RestrictedSecurity security;
        if (!security.Initialize(NO_INHERITANCE))
        {
            return { CandidateStatus::Unavailable, std::nullopt };
        }
        // 検証と読み書き用の所有ハンドル
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
        // パスまたはハンドルの属性情報
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        // ファイル長とリンク数の情報
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
        // 読み取るjournalのバイト数
        const auto size = static_cast<std::size_t>(standard.EndOfFile.QuadPart);
        // journalファイル全体の読み取り領域
        std::string text(size, '\0');
        // 読み書き済みのバイト位置
        std::size_t offset{};
        while (offset < size)
        {
            // 今回読む残りバイト数
            const auto remaining = std::min<std::size_t>(
                size - offset,
                std::numeric_limits<DWORD>::max());
            // 今回読み取ったバイト数
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
            // 検証・復元するjournal状態
            auto state = ParseState(text, binding);
            // 検証済み状態の正規JSON
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

    // 同世代の一致と親子の連鎖を検証して復旧候補を選ぶ(finalPath: journal本体, binding: 保存先と名前空間の照合値)。
    std::optional<Candidate> SelectCandidate(
        const std::filesystem::path& finalPath,
        const std::string_view binding)
    {
        // 検証を通った復旧候補の一覧
        std::vector<Candidate> candidates;
        // いずれかの候補が存在するか
        bool anyExisting{};
        // 異なる名前空間の候補があるか
        bool bindingMismatch{};
        // 復旧候補のパスと本体・次世代・退避の種別
        for (const auto& [path, source] : std::array{
                std::pair{ finalPath, CandidateSource::Final },
                std::pair{ WithSuffix(finalPath, L".next"), CandidateSource::Next },
                std::pair{ WithSuffix(finalPath, L".bak"), CandidateSource::Backup } })
        {
            // 候補を読み取った結果
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
        // 候補一覧の添字を探す(source: 本体・次世代・バックアップの種別)。
        const auto sourceIndex = [&candidates](const CandidateSource source)
            -> std::size_t
        {
            // 指定した候補種別を探す(candidate: 照合する復旧候補)。
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
        // 本体の候補添字・不在なら末尾
        const auto finalIndex = sourceIndex(CandidateSource::Final);
        // 次世代の候補添字・不在なら末尾
        const auto nextIndex = sourceIndex(CandidateSource::Next);
        // バックアップ添字・不在なら末尾
        const auto backupIndex = sourceIndex(CandidateSource::Backup);
        // 世代を比較する候補の添字
        for (std::size_t left = 0u; left < candidates.size(); ++left)
        {
            // 照合相手の候補の添字
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
        // 世代番号とハッシュで直接の親子を照合する(parent: 親世代の候補, child: 子世代の候補)。
        const auto directParent = [](const Candidate& parent,
                                     const Candidate& child)
        {
            return child.state.generation == parent.state.generation + 1u
                && child.state.parentGeneration == parent.state.generation
                && child.state.parentChecksum == parent.state.checksum;
        };
        if (nextIndex != candidates.size())
        {
            // 次世代ファイルの復旧候補
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
                // 本体が次世代候補の直接の親か
                const bool finalIsParent = finalIndex != candidates.size()
                    && directParent(candidates[finalIndex], next);
                // バックアップが直接の親か
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
            // 本体の復旧候補
            const auto& final = candidates[finalIndex];
            if (backupIndex != candidates.size())
            {
                // バックアップの復旧候補
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

    // 開いた対象を検証して全内容を書き込みflushする(path: 書き込み先, bytes: 完全なJSON, injectFlushFailure: flush直前の失敗注入を有効にするか)。
    void DurableWrite(
        const std::filesystem::path& path,
        const std::string_view bytes,
        const bool injectFlushFailure)
    {
        // 現ユーザーとSYSTEM限定のACL
        RestrictedSecurity security;
        if (!security.Initialize(NO_INHERITANCE))
        {
            ThrowJournalFailure();
        }
        // 検証と読み書き用の所有ハンドル
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
        // パスまたはハンドルの属性情報
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        // ファイル長とリンク数の情報
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
        // 切り詰めるファイル先頭の位置
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
        // 読み書き済みのバイト位置
        std::size_t offset{};
        while (offset < bytes.size())
        {
            // 今回書く残りバイト数
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max());
            // 今回書き込んだバイト数
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

    // 不在だけをfalseとし他の取得失敗は送出する(path: 確認するパス)。
    bool PathExists(const std::filesystem::path& path)
    {
        // パスまたはハンドルの属性情報
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

    // 書き込み完了を待つ方式でファイルを置き換える(source: 移動元, destination: 置換先)。
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

    // flush済みの書き込みファイルを次世代候補へ公開する(finalPath: journal本体, document: 次世代のJSON, nextPublished: 候補公開後にtrueとする出力)。
    void StageDurableNext(
        const std::filesystem::path& finalPath,
        const std::string_view document,
        bool& nextPublished)
    {
        nextPublished = false;
        // flush前の書き込み先
        const auto writingPath = WithSuffix(finalPath, L".writing");
        // flush済みの次世代候補のパス
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

    // 親世代を照合し復旧候補を本体へ昇格する(selected: 検証済みの候補, finalPath: journal本体, binding: 保存先と名前空間の照合値)。
    void PromoteCandidate(
        const Candidate& selected,
        const std::filesystem::path& finalPath,
        const std::string_view binding)
    {
        if (selected.source == CandidateSource::Final)
        {
            return;
        }
        // flush済みの次世代候補のパス
        const auto nextPath = WithSuffix(finalPath, L".next");
        // 旧本体の退避先パス
        const auto backupPath = WithSuffix(finalPath, L".bak");
        if (selected.source == CandidateSource::Next)
        {
            // 本体の復旧候補
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
        // flush済み候補を公開済みか
        bool nextPublished{};
        StageDurableNext(
            finalPath,
            selected.canonicalDocument,
            nextPublished);
        (void)nextPublished;
        MoveReplace(nextPath, finalPath);
    }

    // flush済み次世代を公開し旧本体を退避して置き換える(finalPath: journal本体, document: 次世代のJSON, nextPublished: 候補公開後にtrueとする出力)。
    void Publish(
        const std::filesystem::path& finalPath,
        const std::string_view document,
        bool& nextPublished)
    {
        // flush済みの次世代候補のパス
        const auto nextPath = WithSuffix(finalPath, L".next");
        // 旧本体の退避先パス
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
        // 寿命中に保持する排他ロック
        auto held = AcquireLock(lockPath);
        try
        {
            // 二重取得を試みる排他ロック
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
        // アカウントの公開ロックを解放する。
        ~Implementation()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
        }

        // アカウント公開ロックの所有ハンドル
        HANDLE handle{ INVALID_HANDLE_VALUE };
    };

    CloudSaveProfileSessionLease::CloudSaveProfileSessionLease(
        CloudSaveJournal& journal)
    {
        // アカウントのjournal保存先
        const auto stateDirectory = journal.FilePath().parent_path();
        ValidateExistingPathComponents(stateDirectory);
        // 寿命中に保持する排他ロック
        auto held = AcquireLock(stateDirectory / L"profile.session.lock");
        // 公開ロックの所有先
        auto implementation = std::make_unique<Implementation>();
        implementation->handle =
            std::exchange(held.value, INVALID_HANDLE_VALUE);
        m_implementation = std::move(implementation);
    }

    CloudSaveProfileSessionLease::~CloudSaveProfileSessionLease() = default;

    struct CloudSaveJournal::Implementation final
    {
        // 導出済みアカウントの保存先
        PersistenceProfilePaths accountProfile;
        // journal本体のパス
        std::filesystem::path filePath;
        // 世代更新を排他するロックのパス
        std::filesystem::path lockPath;
        // 保存先と名前空間の照合ハッシュ
        std::string binding;
        // 検証・復元するjournal状態
        JournalState state;
        // 復旧不能後の操作禁止状態
        bool blocked{};

        // 復旧不能後の操作を拒否する。
        void EnsureAvailable() const
        {
            if (blocked)
            {
                throw std::runtime_error(
                    "Cloud save journal is unavailable after a write failure.");
            }
        }

        // 排他ロック下で世代をCAS更新し失敗時は公開段階に従って復旧する(nextState: 永続化する次の状態)。
        void Persist(JournalState nextState)
        {
            EnsureAvailable();
            // 更新中に保持する排他ロック
            FileHandle lock;
            try
            {
                lock = AcquireLock(lockPath);
            }
            // ディスク変更前の排他競合は同じinstanceから再試行できます。
            catch (const CloudSaveJournalBusyError&)
            {
                throw;
            }
            catch (...)
            {
                blocked = true;
                throw;
            }
            // 次世代のJSONを生成済みか
            bool newStatePrepared{};
            // flush済みの次世代を公開済みか
            bool newStatePublished{};
            try
            {
                // 復旧に採用する検証済み候補
                auto selected = SelectCandidate(filePath, binding);
                // ディスク上の現在の世代
                const auto diskGeneration = selected
                    ? selected->state.generation
                    : 0u;
                // ディスク上の世代ハッシュ
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
                // journal全体のJSON
                auto document = SerializeState(nextState, binding);
                newStatePrepared = true;
                Publish(filePath, document, newStatePublished);
                state = std::move(nextState);
            }
            catch (...)
            {
                // 今回の更新を復旧して確定したか
                bool committedStateRecovered{};
                try
                {
                    // 障害後に読み直した復旧候補
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
        // 信頼するUserDataの絶対パス
        const auto trustedUserData =
            NormalizedAbsolute(trustedUserDataDirectory);
        // 保存先のローカルドライブ名
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
        // UserDataの親のエンジン保存先
        const auto engineRoot = trustedUserData.parent_path();
        // アカウントキーから導出した保存先
        const auto expectedRoot = engineRoot
            / L"OnlineProfiles"
            / LamaPon::PathFromUtf8(accountProfile.accountStorageKey);
        // 検証するアカウント保存先
        const auto root = NormalizedAbsolute(accountProfile.rootDirectory);
        ValidateExistingPathComponents(trustedUserData);
        ValidateExistingPathComponents(expectedRoot);
        // 保存先末尾のアカウントキー
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

        // 正規化したサービスの基点URL
        const auto normalizedBackendBaseUrl = NormalizedBackendBaseUrl(
            std::move(backendBaseUrl),
            allowInsecureLoopback);
        EnsurePlainDirectory(engineRoot, false);
        // journal群の保存ディレクトリ
        const auto stateRoot = engineRoot / L"OnlineState";
        EnsurePlainDirectory(stateRoot, true);
        // 対象アカウントのjournal保存先
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

        // 更新中に保持する排他ロック
        auto lock = AcquireLock(m_implementation->lockPath);
        // 復旧に採用する検証済み候補
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
        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 検証または変換する要素の位置
        const auto index = FindEntry(next.entries, snapshot.resource);
        if (index == next.entries.size())
        {
            // 検証または更新する保存先の記録
            Entry entry;
            entry.resource = snapshot.resource;
            entry.baseline = snapshot;
            next.entries.push_back(std::move(entry));
        }
        else
        {
            // 検証または更新する保存先の記録
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
        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 検証または変換する要素の位置
        auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size())
        {
            // 検証または更新する保存先の記録
            Entry entry;
            entry.resource = resource;
            next.entries.push_back(std::move(entry));
            index = next.entries.size() - 1u;
        }
        // 検証または更新する保存先の記録
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
        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 検証または変換する要素の位置
        auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size())
        {
            // 検証または更新する保存先の記録
            Entry entry;
            entry.resource = resource;
            next.entries.push_back(std::move(entry));
            index = next.entries.size() - 1u;
        }
        // 検証または更新する保存先の記録
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
        ApplyLocalDeleteIntentOperations({ {
            resource,
            CloudSaveDeleteIntentOperationKind::Record
        } });
    }

    void CloudSaveJournal::ClearLocalDeleteIntent(
        const CloudSaveResource& resource)
    {
        ApplyLocalDeleteIntentOperations({ {
            resource,
            CloudSaveDeleteIntentOperationKind::Clear
        } });
    }

    void CloudSaveJournal::ApplyLocalDeleteIntentOperations(
        const std::vector<CloudSaveDeleteIntentOperation>& operations)
    {
        m_implementation->EnsureAvailable();
        if (operations.empty())
        {
            return;
        }

        // 操作が重複していない保存先
        std::vector<CloudSaveResource> resources;
        resources.reserve(operations.size());
        // 適用する削除意思の操作
        for (const auto& operation : operations)
        {
            (void)MaximumContentBytes(operation.resource);
            if (operation.kind
                    != CloudSaveDeleteIntentOperationKind::Record
                && operation.kind
                    != CloudSaveDeleteIntentOperationKind::Clear)
            {
                throw std::invalid_argument(
                    "Cloud save delete intent operation is invalid.");
            }
            // 同じ保存先への操作の重複を検証する(existing: 確認済みの保存先)。
            if (std::ranges::any_of(
                    resources,
                    [&operation](const CloudSaveResource& existing)
                    {
                        return EquivalentResources(
                            existing,
                            operation.resource);
                    }))
            {
                throw std::invalid_argument(
                    "Cloud save delete intent operations are ambiguous.");
            }
            resources.push_back(operation.resource);
        }

        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 削除意思の状態を変更したか
        bool changed{};
        // 適用する削除意思の操作
        for (const auto& operation : operations)
        {
            // 検証または変換する要素の位置
            auto index = FindEntry(next.entries, operation.resource);
            if (operation.kind
                == CloudSaveDeleteIntentOperationKind::Record)
            {
                if (index == next.entries.size())
                {
                    // 検証または更新する保存先の記録
                    Entry entry;
                    entry.resource = operation.resource;
                    next.entries.push_back(std::move(entry));
                    index = next.entries.size() - 1u;
                }
                if (!next.entries[index].localDeleteIntent)
                {
                    next.entries[index].localDeleteIntent = true;
                    changed = true;
                }
                continue;
            }

            if (index == next.entries.size()
                || !next.entries[index].localDeleteIntent)
            {
                continue;
            }
            // 検証または更新する保存先の記録
            auto& entry = next.entries[index];
            entry.localDeleteIntent = false;
            changed = true;
            if (!entry.baseline && !entry.pending && !entry.conflict)
            {
                next.entries.erase(next.entries.begin()
                    + static_cast<std::ptrdiff_t>(index));
            }
        }
        if (!changed)
        {
            return;
        }
        m_implementation->Persist(std::move(next));
    }

    bool CloudSaveJournal::HasLocalDeleteIntent(
        const CloudSaveResource& resource) const
    {
        m_implementation->EnsureAvailable();
        (void)MaximumContentBytes(resource);
        // 検証または変換する要素の位置
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
        // 競合待ちを除く未送信更新
        std::vector<CloudSavePendingMutation> result;
        // 検証または更新する保存先の記録
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
        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 検証または変換する要素の位置
        const auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size()
            || !next.entries[index].pending
            || next.entries[index].pending->mutationId != mutationId
            || next.entries[index].conflict)
        {
            throw std::logic_error("Cloud save mutation is not pending.");
        }
        // 検証または更新する保存先の記録
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
        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 検証または変換する要素の位置
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
        // 永続化前の次の状態または候補
        auto next = m_implementation->state;
        // 検証または変換する要素の位置
        const auto index = FindEntry(next.entries, resource);
        if (index == next.entries.size()
            || !next.entries[index].pending
            || !next.entries[index].conflict
            || next.entries[index].pending->mutationId
                != expectedMutationId)
        {
            throw std::logic_error("Cloud save conflict does not exist.");
        }
        // 検証または更新する保存先の記録
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
        // 検証または変換する要素の位置
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
        // 検証または変換する要素の位置
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
        // 検証または変換する要素の位置
        const auto index = FindEntry(
            m_implementation->state.entries,
            resource);
        return index == m_implementation->state.entries.size()
            ? std::nullopt
            : m_implementation->state.entries[index].pending;
    }

    std::vector<CloudSaveConflictSummary>
        CloudSaveJournal::ConflictSummaries() const
    {
        m_implementation->EnsureAvailable();
        // 本文を含まない競合一覧
        std::vector<CloudSaveConflictSummary> result;
        result.reserve(m_implementation->state.entries.size());
        // 検証または更新する保存先の記録
        for (const auto& entry : m_implementation->state.entries)
        {
            if (!entry.pending || !entry.conflict)
            {
                continue;
            }
            result.push_back({
                entry.resource,
                entry.pending->mutationId,
                entry.pending->kind == CloudSavePendingKind::Delete,
                entry.pending->kind == CloudSavePendingKind::Delete
                    ? 0u
                    : entry.pending->content.size(),
                entry.conflict->deleted,
                entry.conflict->deleted
                    ? 0u
                    : entry.conflict->content.size()
            });
        }
        return result;
    }

    std::vector<CloudSaveResource> CloudSaveJournal::Resources() const
    {
        m_implementation->EnsureAvailable();
        // 記録した保存先の一覧
        std::vector<CloudSaveResource> result;
        result.reserve(m_implementation->state.entries.size());
        // 検証または更新する保存先の記録
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
        // 検証または変換する要素の位置
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
