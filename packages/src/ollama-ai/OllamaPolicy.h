#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// Ollamaを「このPCで動くローカルモデル」だけに限定するための判定です。
// Ollama Cloudは無料枠を含めて使いません。
// 接続先・モデル名・応答を別々に確かめ、どれか1つをすり抜けても、クラウドで生成した結果をゲームへ渡さないようにします。
// どの関数も状態を持たず、OSのAPIを呼びません。
namespace LamaPonOllama
{
    // 接続先を制限する既定ポート
    inline constexpr std::uint16_t DefaultPort = 11434;

    namespace Detail
    {
        // ASCII英数字か判定します(character: 検査する文字)。
        [[nodiscard]] constexpr bool IsAsciiAlphaNumeric(const char character) noexcept
        {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z');
        }

        // ASCII英大文字を小文字へ変換します(character: 変換する文字)。
        [[nodiscard]] constexpr char ToAsciiLower(const char character) noexcept
        {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        }

        // 1～65535の10進数か判定します(text: ポート文字列)。
        [[nodiscard]] constexpr bool IsPortText(const std::string_view text) noexcept
        {
            if (text.empty() || text.size() > 5)
            {
                return false;
            }
            // 数字列を変換したポート
            std::uint32_t port{};
            // ポート文字列の各数字
            for (const char character : text)
            {
                if (character < '0' || character > '9')
                {
                    return false;
                }
                port = port * 10 + static_cast<std::uint32_t>(character - '0');
            }
            return port >= 1 && port <= 65535;
        }
    }

    // 許可するAPI経路を3種類に制限します(path: 要求パス)。
    [[nodiscard]] constexpr bool IsAllowedApiPath(const std::string_view path) noexcept
    {
        return path == "/api/tags" || path == "/api/show" || path == "/api/chat";
    }

    // LoopbackのHTTP APIだけを許可します(url: 接続先)。
    // localhost系3種・必須port・小文字のみを受け付け、queryとuserinfoは拒否します。
    [[nodiscard]] constexpr bool IsAllowedEndpoint(std::string_view url) noexcept
    {
        // 許可するHTTP scheme
        constexpr std::string_view scheme = "http://";
        // 長さ・schemeを検証します。
        if (url.size() > 128 || !url.starts_with(scheme))
        {
            return false;
        }
        url.remove_prefix(scheme.size());
        // URL内の禁止文字を確認します(character: URLの各文字)。
        for (const char character : url)
        {
            // 曖昧な接続先解釈につながる文字を拒否します。
            if (character <= ' ' || character > '~'
                || character == '@' || character == '\\' || character == '%'
                || character == '?' || character == '#')
            {
                return false;
            }
        }

        // authorityとAPI pathの境界
        const auto pathStart = url.find('/');
        if (pathStart == std::string_view::npos)
        {
            return false;
        }
        // ホストとポートの組
        const auto authority = url.substr(0, pathStart);
        // IPv6は角括弧の後からポートを解析します。
        const auto hostEnd = authority.starts_with('[')
            ? authority.find(']') + 1
            : authority.find(':');
        if (hostEnd == 0 || hostEnd >= authority.size() || authority[hostEnd] != ':')
        {
            return false;
        }
        // 許可対象と照合するホスト名
        const auto host = authority.substr(0, hostEnd);
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
        {
            return false;
        }
        return Detail::IsPortText(authority.substr(hostEnd + 1))
            && IsAllowedApiPath(url.substr(pathStart));
    }

    // Loopbackの接続先を組み立てます(port: 接続ポート, path: API経路)。
    [[nodiscard]] inline std::string BuildEndpoint(
        const std::uint16_t port, const std::string_view path)
    {
        return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
    }

    // 区切られたcloud語を含むモデル名を検出します(model: モデル名)。
    [[nodiscard]] constexpr bool HasCloudTag(const std::string_view model) noexcept
    {
        // 拒否するモデル種別語
        constexpr std::string_view word = "cloud";
        // 現在の語頭位置
        std::size_t start{};
        // 英数字で区切られた語を調べます。
        while (start < model.size())
        {
            // 現在の語末位置
            auto end = start;
            // 現在の英数字語を読み進めます。
            while (end < model.size() && Detail::IsAsciiAlphaNumeric(model[end]))
            {
                ++end;
            }
            // cloud語と同じ長さだけ照合します。
            if (end - start == word.size())
            {
                // 大小文字を無視した一致状態
                bool matches = true;
                // cloud語の各文字
                for (std::size_t index = 0; index < word.size(); ++index)
                {
                    matches = matches
                        && Detail::ToAsciiLower(model[start + index]) == word[index];
                }
                // 一致したらクラウド扱いにします。
                if (matches)
                {
                    return true;
                }
            }
            start = end + 1;
        }
        return false;
    }

    // ローカルモデル名として使えるか判定します(model: モデル名)。
    [[nodiscard]] constexpr bool IsLocalModelName(const std::string_view model) noexcept
    {
        if (model.empty() || model.size() > 128)
        {
            return false;
        }
        // Ollamaモデル名に使える文字を確認します(character: 各文字)。
        for (const char character : model)
        {
            if (!Detail::IsAsciiAlphaNumeric(character)
                && character != '.' && character != '_' && character != '-'
                && character != ':' && character != '/')
            {
                return false;
            }
        }
        return !HasCloudTag(model);
    }

    // Remoteモデルの応答欄を検査します(value: Ollama応答オブジェクト)。
    // null以外の文字列以外の値も拒否します。
    [[nodiscard]] inline bool HasRemoteOrigin(const nlohmann::json& value) noexcept
    {
        if (!value.is_object())
        {
            return false;
        }
        // Remote由来を示す応答キー(key: 検査対象名)
        for (const char* key : { "remote_host", "remote_model" })
        {
            // 現在のキーに対応するJSON値
            const auto found = value.find(key);
            if (found == value.end() || found->is_null())
            {
                continue;
            }
            if (!found->is_string() || !found->get_ref<const std::string&>().empty())
            {
                return true;
            }
        }
        return false;
    }
}
