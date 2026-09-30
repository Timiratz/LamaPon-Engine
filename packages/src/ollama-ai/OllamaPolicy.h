#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// Ollamaを「このPCで動くローカルモデル」だけに限定するための判定です。
// Ollama Cloudは無料枠を含めて使いません。接続先・モデル名・応答を別々に
// 確かめ、どれか1つをすり抜けても、クラウドで生成した結果をゲームへ
// 渡さないようにします。
// どの関数も状態を持たず、OSのAPIを呼びません。
namespace LamaPonOllama
{
    inline constexpr std::uint16_t DefaultPort = 11434;

    namespace Detail
    {
        [[nodiscard]] constexpr bool IsAsciiAlphaNumeric(const char character) noexcept
        {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z');
        }

        [[nodiscard]] constexpr char ToAsciiLower(const char character) noexcept
        {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        }

        // 1～65535の10進数だけを受け付けます。
        [[nodiscard]] constexpr bool IsPortText(const std::string_view text) noexcept
        {
            if (text.empty() || text.size() > 5)
            {
                return false;
            }
            std::uint32_t port{};
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

    // このパッケージが呼ぶAPIは3つだけです。Web検索やWeb取得のように
    // サインインを前提にしたAPIのパスは、ここで止めます。
    [[nodiscard]] constexpr bool IsAllowedApiPath(const std::string_view path) noexcept
    {
        return path == "/api/tags" || path == "/api/show" || path == "/api/chat";
    }

    // 接続先はこのPC（loopback）へのhttpだけです。エンジンのHttpSendは
    // HTTPSならどこへでも送れるため、送る前に必ずこの関数で確かめます。
    // ホストは 127.0.0.1 / localhost / [::1] の完全一致、ポートは必須です。
    // 判定を単純に保つため、小文字だけを受け付け、クエリやユーザー情報を
    // 含むURLは内容を見ずに拒否します。
    [[nodiscard]] constexpr bool IsAllowedEndpoint(std::string_view url) noexcept
    {
        constexpr std::string_view scheme = "http://";
        if (url.size() > 128 || !url.starts_with(scheme))
        {
            return false;
        }
        url.remove_prefix(scheme.size());
        for (const char character : url)
        {
            // '@'より前はユーザー情報になり、後ろが実際の接続先になります。
            // '\\'や'%'も、解釈する側によってホストの区切りが変わります。
            if (character <= ' ' || character > '~'
                || character == '@' || character == '\\' || character == '%'
                || character == '?' || character == '#')
            {
                return false;
            }
        }

        const auto pathStart = url.find('/');
        if (pathStart == std::string_view::npos)
        {
            return false;
        }
        const auto authority = url.substr(0, pathStart);
        // IPv6は"[::1]"の中にも':'があるため、']'の後ろからポートを探します。
        const auto hostEnd = authority.starts_with('[')
            ? authority.find(']') + 1
            : authority.find(':');
        if (hostEnd == 0 || hostEnd >= authority.size() || authority[hostEnd] != ':')
        {
            return false;
        }
        const auto host = authority.substr(0, hostEnd);
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
        {
            return false;
        }
        return Detail::IsPortText(authority.substr(hostEnd + 1))
            && IsAllowedApiPath(url.substr(pathStart));
    }

    // 設定できるのはポートだけです。ホストを引数に取らないことで、
    // 設定アセットやScriptから接続先を変えられないようにします。
    [[nodiscard]] inline std::string BuildEndpoint(
        const std::uint16_t port, const std::string_view path)
    {
        return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
    }

    // クラウドのモデルは "gemma4:cloud" や "gpt-oss:120b-cloud" のように、
    // 名前のどこかに cloud という語を持ちます。英数字以外で区切った語の
    // 1つが cloud ならクラウド扱いにします。ローカルモデルに同じ語を
    // 付けた場合も拒否しますが、取りこぼすより安全な側へ倒します。
    [[nodiscard]] constexpr bool HasCloudTag(const std::string_view model) noexcept
    {
        constexpr std::string_view word = "cloud";
        std::size_t start{};
        while (start < model.size())
        {
            auto end = start;
            while (end < model.size() && Detail::IsAsciiAlphaNumeric(model[end]))
            {
                ++end;
            }
            if (end - start == word.size())
            {
                bool matches = true;
                for (std::size_t index = 0; index < word.size(); ++index)
                {
                    matches = matches
                        && Detail::ToAsciiLower(model[start + index]) == word[index];
                }
                if (matches)
                {
                    return true;
                }
            }
            start = end + 1;
        }
        return false;
    }

    // 設定や一覧から受け取ったモデル名を、送ってよい名前かどうか判定します。
    // 空の名前は「自動選択」を表すので、呼び出し側で先に分けてください。
    [[nodiscard]] constexpr bool IsLocalModelName(const std::string_view model) noexcept
    {
        if (model.empty() || model.size() > 128)
        {
            return false;
        }
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

    // クラウドのモデルは、一覧（/api/tags）・詳細（/api/show）・生成結果
    // （/api/chat）のどれにも remote_host と remote_model が付きます
    // （Ollamaの api/types.go）。名前だけでは見分けられないモデルを、
    // ここで見つけます。文字列以外の値が入っていた場合も拒否します。
    [[nodiscard]] inline bool HasRemoteOrigin(const nlohmann::json& value) noexcept
    {
        if (!value.is_object())
        {
            return false;
        }
        for (const char* key : { "remote_host", "remote_model" })
        {
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
