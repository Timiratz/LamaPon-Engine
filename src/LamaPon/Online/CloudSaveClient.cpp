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

    // manifest応答の上限バイト数
    constexpr std::size_t MaximumManifestResponseBytes =
        128u * 1024u;
    // 保存内容の応答上限バイト数
    constexpr std::size_t MaximumItemResponseBytes =
        1536u * 1024u;
    // JSONの最大階層深度
    constexpr std::size_t MaximumJsonDepth = 64u;
    // JSON要素数の上限
    constexpr std::size_t MaximumJsonElements = 65536u;
    // 再試行の最大待機秒数
    constexpr std::uint32_t MaximumRetryAfterSeconds = 300u;
    // CAS競合に対応する固定識別子
    constexpr std::string_view ConflictCode = "revision_conflict";

    struct AlgorithmHandle final
    {
        // 所有する暗号プロバイダーを解放する。
        ~AlgorithmHandle()
        {
            if (value != nullptr)
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        }

        // 暗号プロバイダーの所有先
        BCRYPT_ALG_HANDLE value{};
    };

    // ASCIIの大小文字を無視してヘッダー名を比較する(left: 左のヘッダー名, right: 右のヘッダー名)。
    bool HeaderNameEquals(
        const std::wstring_view left,
        const std::wstring_view right) noexcept
    {
        // ASCIIの大小文字を無視して照合する(a: 左の文字, b: 右の文字)。
        return left.size() == right.size()
            && std::ranges::equal(
                left,
                right,
                [](const wchar_t a, const wchar_t b)
                {
                    // ASCIIの小文字へ変換(value: 比較する1文字)。
                    const auto fold = [](const wchar_t value)
                    {
                        return value >= L'A' && value <= L'Z'
                            ? value - L'A' + L'a'
                            : value;
                    };
                    return fold(a) == fold(b);
                });
    }

    // 同名ヘッダーの値をresponse内から借用する(response: 生存が必要なHTTP応答, name: 検索するヘッダー名)。
    std::vector<std::wstring_view> HeaderValues(
        const LamaPon::HttpResponse& response,
        const std::wstring_view name)
    {
        // 同じ名前の応答ヘッダー一覧
        std::vector<std::wstring_view> values;
        // 応答ヘッダー名と値の借用
        for (const auto& [headerName, value] : response.headers)
        {
            if (HeaderNameEquals(headerName, name))
            {
                values.emplace_back(value);
            }
        }
        return values;
    }

    // 前後の空白とタブを除いた範囲を借用する(value: ヘッダー値)。
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

    // ASCIIの大文字だけを小文字へ変換する(value: ヘッダー値)。
    std::wstring AsciiLower(std::wstring_view value)
    {
        // 小文字化するヘッダー値
        std::wstring result(value);
        // ASCIIの大文字を小文字へ変える(character: ヘッダーの1文字)。
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

    // 単一のapplication/jsonと任意のutf-8指定を検証する(response: HTTP応答)。
    bool HasValidJsonContentType(
        const LamaPon::HttpResponse& response)
    {
        // 同じ名前の応答ヘッダー一覧
        const auto values = HeaderValues(response, L"Content-Type");
        if (values.size() != 1)
        {
            return false;
        }
        // 小文字化したContent-Type
        const auto lower = AsciiLower(TrimOws(values.front()));
        // 小文字化した値の借用
        const std::wstring_view lowerView(lower);
        // メディア型と引数の区切り位置
        const auto semicolon = lowerView.find(L';');
        // Content-Typeのメディア型
        const auto mediaType = TrimOws(lowerView.substr(0, semicolon));
        if (mediaType != L"application/json")
        {
            return false;
        }
        if (semicolon == std::wstring::npos)
        {
            return true;
        }
        // Content-Typeのcharset引数
        const auto parameter = TrimOws(lowerView.substr(semicolon + 1));
        if (parameter.empty()
            || parameter.find(L';') != std::wstring::npos)
        {
            return false;
        }
        // charset引数の等号位置
        const auto equals = parameter.find(L'=');
        return equals != std::wstring::npos
            && TrimOws(parameter.substr(0, equals)) == L"charset"
            && TrimOws(parameter.substr(equals + 1)) == L"utf-8";
    }

    // 省略または単一のContent-Lengthが本文の長さと一致するかを調べる(response: HTTP応答)。
    bool HasValidContentLength(
        const LamaPon::HttpResponse& response)
    {
        // 同じ名前の応答ヘッダー一覧
        const auto values = HeaderValues(response, L"Content-Length");
        if (values.empty())
        {
            return true;
        }
        if (values.size() != 1)
        {
            return false;
        }
        // 空白を除いたヘッダーの値
        const auto text = TrimOws(values.front());
        // ASCII数字だけかを判定(character: ヘッダーの1文字)。
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
        // 宣言された本文のバイト数
        std::size_t value{};
        // 検証または変換する1文字
        for (const auto character : text)
        {
            // ヘッダーの数字1桁の数値
            const auto digit = static_cast<std::size_t>(character - L'0');
            if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u)
            {
                return false;
            }
            value = value * 10u + digit;
        }
        return value == response.body.size();
    }

    // 本文サイズと長さ・型のヘッダーを検証する(response: HTTP応答, maximumBytes: 本文の上限バイト数, requireJsonContentType: JSON型を必須にするか)。
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

    // 引用符付きの空でないASCII ETagを検証する(value: 強いETagの候補)。
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
        // 強いETag内の文字を検証する(character: 引用符内の1文字)。
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

    // 単一の強いETagを複写し、不正なら空を返す(response: HTTP応答)。
    std::optional<std::string> SingleStrongEtag(
        const LamaPon::HttpResponse& response)
    {
        // 同じ名前の応答ヘッダー一覧
        const auto values = HeaderValues(response, L"ETag");
        if (values.size() != 1)
        {
            return std::nullopt;
        }
        // 前後の空白を除いたETag
        const auto trimmed = TrimOws(values.front());
        // visible ASCIIかを判定(character: ETagの1文字)。
        if (!std::ranges::all_of(
                trimmed,
                [](const wchar_t character)
                {
                    return character >= 0x21 && character <= 0x7e;
                }))
        {
            return std::nullopt;
        }
        // 引用符を含む強いETag
        std::string result;
        result.reserve(trimmed.size());
        // 検証または変換する1文字
        for (const auto character : trimmed)
        {
            result.push_back(static_cast<char>(character));
        }
        return IsStrongEtag(result)
            ? std::optional<std::string>(std::move(result))
            : std::nullopt;
    }

    // 数値Retry-Afterを1〜300秒へ制限し、不正なら1秒を返す(response: HTTP応答)。
    std::uint32_t RetryAfterSeconds(
        const LamaPon::HttpResponse& response) noexcept
    {
        // 同じ名前の応答ヘッダー一覧
        const auto values = HeaderValues(response, L"Retry-After");
        if (values.size() != 1)
        {
            return 1u;
        }
        // 空白を除いたヘッダーの値
        const auto text = TrimOws(values.front());
        // ASCII数字だけかを判定(character: ヘッダーの1文字)。
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
        // ヘッダーが要求する待機秒数
        std::uint32_t value{};
        // 検証または変換する1文字
        for (const auto character : text)
        {
            // ヘッダーの数字1桁の数値
            const auto digit = static_cast<std::uint32_t>(character - L'0');
            if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10u)
            {
                return 1u;
            }
            value = value * 10u + digit;
        }
        return std::clamp(value, 1u, MaximumRetryAfterSeconds);
    }

    // 文字列を区別してJSONの括弧と最大64階層を確認する(text: JSONのバイト列)。
    bool JsonNestingIsSafe(const std::string_view text) noexcept
    {
        // 文字列外のJSON階層深度
        std::size_t depth{};
        // JSON文字列の内部か
        bool inString{};
        // 直前のバックスラッシュ状態
        bool escaped{};
        // 検証または変換する1文字
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

    // 配列とobjectの子を含む要素数を再帰的に数える(value: 数えるJSON値, remaining: 許容要素数の残量)。
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

    // UTF-8・深度・重複キー・要素数を検証してJSONを読む(text: JSONのバイト列)。
    Json ParseJsonStrict(const std::string_view text)
    {
        if (text.empty()
            || LamaPon::Utf8ToWide(text).empty()
            || !JsonNestingIsSafe(text))
        {
            throw std::runtime_error("JSON envelope is invalid.");
        }

        // 同一objectでキーが重複したか
        bool duplicateKey{};
        // 各objectの深度別に記録したキー
        std::array<std::unordered_set<std::string>, MaximumJsonDepth + 1>
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
                    auto& keys =
                        keysByDepth[static_cast<std::size_t>(depth)];
                    // 読み取ったJSON項目名
                    const auto key = parsed.get<std::string>();
                    if (!keys.insert(key).second)
                    {
                        duplicateKey = true;
                    }
                }
                return true;
            };
        // 厳密に解析したJSON
        auto parsed = Json::parse(text, callback, true, false);
        // 許容するJSON要素数の残量
        std::size_t remainingElements = MaximumJsonElements;
        if (duplicateKey
            || !JsonElementCountIsSafe(parsed, remainingElements))
        {
            throw std::runtime_error(
                "JSON contains duplicate keys or too many elements.");
        }
        return parsed;
    }

    // objectの項目が期待した集合と完全に一致するかを調べる(object: 検証するJSON, expected: 必須項目の集合)。
    bool HasExactKeys(
        const Json& object,
        const std::initializer_list<std::string_view> expected)
    {
        if (!object.is_object() || object.size() != expected.size())
        {
            return false;
        }
        // 必須キーの存在を確認する(key: 期待する項目名)。
        return std::ranges::all_of(
            expected,
            [&object](const std::string_view key)
            {
                return object.contains(std::string(key));
            });
    }

    // 保存先を検証してwire形式へ変換する(resource: 設定またはスロットの保存先)。
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

    // 項目集合とスロット名を検証して保存先を読む(json: 保存先のJSON)。
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
            // 検証する保存スロット名
            auto slot = json.at("slot").get<std::string>();
            if (LamaPon::Detail::IsValidSaveSlotName(slot))
            {
                return CloudSaveResource::SaveSlot(std::move(slot));
            }
        }
        throw std::runtime_error("Cloud save resource is invalid.");
    }

    // 種別と共通のスロット名判定で保存先を比較する(left: 左の保存先, right: 右の保存先)。
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

    // 保存先を検証して内容の上限を返す(resource: 設定またはスロットの保存先)。
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

    // 小文字の標準UUIDv4形式かを検証する(value: 再送識別子の候補)。
    bool IsCanonicalMutationId(const std::string_view value) noexcept
    {
        if (value.size() != 36)
        {
            return false;
        }
        // UUID文字列の検証位置
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
            // 検証または変換する1文字
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

    // Windows CNGでSHA-256を計算する(data: 読み取るバイト列, size: バイト数)。
    std::array<std::uint8_t, 32> Sha256(
        const std::uint8_t* data,
        const std::size_t size)
    {
        if (size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument("Cloud save content is too large.");
        }
        // SHA-256プロバイダーの所有先
        AlgorithmHandle algorithm;
        if (BCryptOpenAlgorithmProvider(
                &algorithm.value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0)
        {
            throw std::runtime_error("SHA-256 is unavailable.");
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
            throw std::runtime_error("SHA-256 failed.");
        }
        return digest;
    }

    // パディングなしbase64url文字表
    constexpr char Base64UrlAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    // パディングなしのbase64urlへ変換する(data: 入力バイト列, size: 入力のバイト数)。
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
                | (static_cast<std::uint32_t>(data[index + 1]) << 8u)
                | static_cast<std::uint32_t>(data[index + 2]);
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
                | (static_cast<std::uint32_t>(data[index + 1]) << 8u);
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
        }
        return result;
    }

    // base64urlの1文字を6ビット値へ変換する(character: 入力文字)。
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

    // パディングなしの標準base64urlを上限内で復元する(encoded: 変換する文字列, maximumBytes: 復元サイズの上限, decoded: 復元するバイト列の出力先)。
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
        // 検証または変換する1文字
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
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2])) << 6u)
                | static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 3]));
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
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1])) << 12u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
        }
        else if (remaining == 3u)
        {
            // 4文字から復元した24ビット値
            const auto value =
                (static_cast<std::uint32_t>(Base64UrlValue(encoded[index])) << 18u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 1])) << 12u)
                | (static_cast<std::uint32_t>(Base64UrlValue(encoded[index + 2])) << 6u);
            decoded.push_back(static_cast<std::uint8_t>(value >> 16u));
            decoded.push_back(static_cast<std::uint8_t>(value >> 8u));
        }
        return decoded.size() == decodedSize;
    }

    // 保存内容のSHA-256をパディングなしbase64urlで返す(content: 保存するJSONバイト列)。
    std::string ContentHash(const std::vector<std::uint8_t>& content)
    {
        // 32バイトのSHA-256
        const auto digest = Sha256(content.data(), content.size());
        return EncodeBase64Url(digest.data(), digest.size());
    }

    // 32バイトのSHA-256を表す標準base64url形式かを検証する(value: ハッシュ文字列)。
    bool IsCanonicalSha256(const std::string_view value)
    {
        // SHA-256形式の確認用バイト列
        std::vector<std::uint8_t> decoded;
        return value.size() == 43u
            && DecodeBase64Url(value, 32u, decoded)
            && decoded.size() == 32u;
    }

    // JSON値が符号なし整数の型かを調べる(value: 検証するJSON値)。
    bool IsUnsigned(const Json& value) noexcept
    {
        return value.is_number_unsigned();
    }

    // 削除印に応じた項目集合と長さ・ハッシュ形式を検証する(json: manifestの1項目)。
    CloudSaveManifestItem ParseManifestItem(const Json& json)
    {
        if (!json.is_object()
            || !json.contains("deleted")
            || !json.at("deleted").is_boolean())
        {
            throw std::runtime_error("Cloud manifest item is invalid.");
        }
        // 受信した削除済みの印
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
        // 検証する保存データの一覧項目
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

    // 保存先・長さ・SHA-256・内容JSONを検証して読む(json: 受信した保存状態, hasProtocolVersion: wireバージョンを含むか)。
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
        // 受信した削除済みの印
        const bool deleted = json.at("deleted").get<bool>();
        // 削除種別に対応する項目集合か
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

        // 検証する受信保存状態
        CloudSaveSnapshot snapshot;
        snapshot.resource = ParseResource(json.at("resource"));
        snapshot.etag = json.at("etag").get<std::string>();
        snapshot.deleted = deleted;
        // 受信メタデータの内容バイト数
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
        // 受信したbase64urlの保存内容
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
        // 検証する保存内容のJSON文字列
        const std::string contentText(
            snapshot.content.begin(),
            snapshot.content.end());
        static_cast<void>(ParseJsonStrict(contentText));
        return snapshot;
    }

    // 安全な固定診断と待機秒数で通信結果を作る(status: 結果種別, message: エンジン定義の診断文, code: エンジン定義の識別子, retryAfter: 推奨待機秒数)。
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

    // HTTP状態だけを固定の失敗結果へ変換する(response: HTTP応答, readOperation: 保存内容の取得操作か)。
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

    // CAS競合の保存状態とETagを検証して読む(response: 412のHTTP応答, requested: 要求した保存先, maximumBytes: 応答の上限バイト数)。
    CloudSaveItemResult ParseConflict(
        const LamaPon::HttpResponse& response,
        const CloudSaveResource& requested,
        const std::size_t maximumBytes)
    {
        // CAS競合の検証結果
        CloudSaveItemResult result;
        try
        {
            if (!ResponseEnvelopeIsValid(response, maximumBytes, true))
            {
                throw std::runtime_error("Cloud conflict envelope is invalid.");
            }
            // 受信応答の厳密な解析結果
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
            // 競合時点のリモート保存状態
            auto current = ParseSnapshot(json.at("current"), false);
            if (!EquivalentResources(current.resource, requested))
            {
                throw std::runtime_error("Cloud conflict resource mismatched.");
            }
            // 単一の強いETagの検証結果
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

    // 要求検証の失敗結果を作る(message: エンジン定義の診断文)。
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
        // 認証と名前空間付きHTTP要求
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
            // 送信例外に対応する固定の失敗
            HttpResponse failure;
            failure.transportError = "Cloud save sender failed.";
            return failure;
        }
    }

    CloudSaveManifestResult CloudSaveClient::FetchManifest(
        const std::string_view accessToken) const
    {
        // 保存一覧の取得結果
        CloudSaveManifestResult result;
        if (!IsSafeOnlineBearerToken(accessToken))
        {
            result.outcome = InvalidRequest(
                "Cloud save access token is invalid.");
            return result;
        }
        // バックエンドからのHTTP応答
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
            // 受信応答の厳密な解析結果
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
            // 一覧に含まれる保存内容の合計B
            std::uint64_t totalBytes{};
            // 削除済みも含むスロット数
            std::size_t saveSlotCount{};
            // manifest内の保存項目JSON
            for (const auto& entry : json.at("items"))
            {
                // 検証する保存データの一覧項目
                auto item = ParseManifestItem(entry);
                if (item.resource.kind == CloudSaveResourceKind::SaveSlot)
                {
                    ++saveSlotCount;
                    if (saveSlotCount > CloudSaveMaxSlots)
                    {
                        throw std::runtime_error("Cloud manifest has too many slots.");
                    }
                }
                // 重複を調べる確認済みの保存項目
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
        // 保存内容の取得結果
        CloudSaveItemResult result;
        // 保存先を表す要求JSON
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
        // 送信するwire v1のJSON本文
        const Json requestBody{
            { "protocolVersion", 1 },
            { "resource", std::move(resourceBody) }
        };
        // バックエンドからのHTTP応答
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
            // 単一の強いETagの検証結果
            const auto headerEtag = SingleStrongEtag(response);
            // 検証する受信保存状態
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
        // 保存内容の更新結果
        CloudSaveItemResult result;
        // 保存先を表す要求JSON
        Json resourceBody;
        try
        {
            resourceBody = ResourceJson(resource);
            if (content.empty()
                || content.size() > MaximumContentBytes(resource))
            {
                throw std::invalid_argument("Cloud save content is outside limits.");
            }
            // 検証する保存内容のJSON文字列
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
        // 保存内容のbase64url SHA-256
        const auto hash = ContentHash(content);
        // 送信するwire v1のJSON本文
        const Json requestBody{
            { "protocolVersion", 1 },
            { "resource", std::move(resourceBody) },
            { "byteLength", content.size() },
            { "sha256", hash },
            { "content", EncodeBase64Url(content.data(), content.size()) }
        };
        // CASと再送識別子のヘッダー
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
        // バックエンドからのHTTP応答
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
            // 単一の強いETagの検証結果
            const auto headerEtag = SingleStrongEtag(response);
            // 検証する受信保存状態
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
        // 削除状態の更新結果
        CloudSaveItemResult result;
        // 保存先を表す要求JSON
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
        // 送信するwire v1のJSON本文
        const Json requestBody{
            { "protocolVersion", 1 },
            { "resource", std::move(resourceBody) }
        };
        // CASと再送識別子のヘッダー
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        headers.emplace_back(
            L"Idempotency-Key",
            Utf8ToWide(mutationId));
        headers.emplace_back(
            L"If-Match",
            Utf8ToWide(baseEtag));
        // バックエンドからのHTTP応答
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
            // 単一の強いETagの検証結果
            const auto headerEtag = SingleStrongEtag(response);
            // 検証する受信保存状態
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
