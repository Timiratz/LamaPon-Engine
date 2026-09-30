#pragma once

#include "LamaPon/Core/HttpClient.h"
#include "OllamaPolicy.h"
#include "OllamaProfile.h"

#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// このPCのOllamaと会話するための関数です。使うAPIは /api/tags（一覧）、
// /api/show（詳細）、/api/chat（会話）の3つだけです。
// 前半はJSONの組み立てと解析で、通信もOSのAPIも使いません。
// 後半だけがエンジンのHttpSend（Windows専用）を呼びます。
namespace LamaPonOllama
{
    // イベントのEventArgs.numberへそのまま入れるので、番号を変えないでください。
    enum class ChatError
    {
        None = 0,
        NotRunning = 1,     // Ollamaが起動していない
        ModelMissing = 2,   // モデルをまだ取得していない
        Timeout = 3,        // 時間内に返答が届かなかった
        CloudRejected = 4,  // クラウドのモデル、またはこのPC以外への接続を拒否した
        Failed = 5          // 上のどれでもない失敗
    };

    struct ChatMessage final
    {
        std::string role;       // "user" または "assistant"
        std::string content;
    };

    struct ChatResult final
    {
        ChatError error{ ChatError::None };
        std::string reply;      // 成功したときだけ入ります
        std::string model;      // 実際に使ったモデル名
        std::string detail;     // ログ向けの補足

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return error == ChatError::None;
        }
    };

    [[nodiscard]] constexpr const char* ErrorName(const ChatError error) noexcept
    {
        switch (error)
        {
        case ChatError::None: return "None";
        case ChatError::NotRunning: return "NotRunning";
        case ChatError::ModelMissing: return "ModelMissing";
        case ChatError::Timeout: return "Timeout";
        case ChatError::CloudRejected: return "CloudRejected";
        case ChatError::Failed: break;
        }
        return "Failed";
    }

    [[nodiscard]] constexpr const char* ErrorMessage(const ChatError error) noexcept
    {
        switch (error)
        {
        case ChatError::None: return "";
        case ChatError::NotRunning:
            return "Ollamaが起動していません。Ollamaを起動してから、もう一度送ってください。";
        case ChatError::ModelMissing:
            return "モデルがこのPCにありません。ollama pull <モデル名> で取得してください。";
        case ChatError::Timeout:
            return "30秒以内に返答が届きませんでした。はじめの1回はモデルの読み込みで間に合わないことがあります。"
                "もう一度送っても同じなら、maxTokensを小さくするか、小さいモデルを使ってください。";
        case ChatError::CloudRejected:
            return "クラウドのモデルは使えません。このPCで動くローカルモデルを指定してください。";
        case ChatError::Failed: break;
        }
        return "Ollamaから返答を受け取れませんでした。";
    }

    // 前後の空白と改行を取り除きます。モデルの返答は改行で始まることがあります。
    [[nodiscard]] inline std::string Trimmed(const std::string_view text)
    {
        constexpr std::string_view spaces = " \t\r\n";
        const auto first = text.find_first_not_of(spaces);
        if (first == std::string_view::npos)
        {
            return {};
        }
        return std::string(text.substr(first, text.find_last_not_of(spaces) - first + 1));
    }

    namespace Detail
    {
        [[nodiscard]] inline ChatResult Failure(const ChatError error, std::string detail)
        {
            ChatResult result;
            result.error = error;
            result.detail = std::move(detail);
            return result;
        }

        // 入力に不正なUTF-8が混ざっていても、例外にせず置き換え文字で送ります。
        [[nodiscard]] inline std::string Dump(const nlohmann::json& value)
        {
            return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        [[nodiscard]] inline nlohmann::json MessageJson(
            const std::string_view role, const std::string_view content)
        {
            auto message = nlohmann::json::object();
            message["role"] = std::string(role);
            message["content"] = std::string(content);
            return message;
        }

        // Ollamaは失敗の理由を {"error":"..."} で返します。ログが長くならないよう切ります。
        [[nodiscard]] inline std::string ServerError(const nlohmann::json& document)
        {
            if (document.is_object())
            {
                const auto found = document.find("error");
                if (found != document.end() && found->is_string())
                {
                    return found->get<std::string>().substr(0, 200);
                }
            }
            return {};
        }

        // 一覧・詳細・会話に共通の確認です。/api/show と /api/chat は、
        // モデルがないときに404を返します。
        [[nodiscard]] inline ChatResult ReadDocument(const std::uint32_t statusCode,
            const std::string_view body, const bool notFoundMeansModelMissing,
            nlohmann::json& document)
        {
            document = nlohmann::json::parse(body, nullptr, false);
            if (statusCode == 404 && notFoundMeansModelMissing)
            {
                return Failure(ChatError::ModelMissing, ServerError(document));
            }
            if (statusCode < 200 || statusCode >= 300)
            {
                return Failure(ChatError::Failed,
                    "HTTP " + std::to_string(statusCode) + " " + ServerError(document));
            }
            if (!document.is_object())
            {
                return Failure(ChatError::Failed, "Ollamaの応答がJSONオブジェクトではありません。");
            }
            return {};
        }
    }

    [[nodiscard]] inline std::string BuildShowRequest(const std::string_view model)
    {
        auto request = nlohmann::json::object();
        request["model"] = std::string(model);
        return Detail::Dump(request);
    }

    // stream:false で返答を1回にまとめて受け取り、num_predictで長さを抑えます。
    // エンジンのHTTP受信は30秒で打ち切られ、途中経過も受け取れないためです。
    // think:false は、考える過程を出力するモデルがその過程だけでnum_predictを
    // 使い切り、返答が空になるのを防ぎます。
    [[nodiscard]] inline std::string BuildChatRequest(const ModelProfile& profile,
        const std::string_view model, const std::vector<ChatMessage>& history,
        const std::string_view userText)
    {
        auto messages = nlohmann::json::array();
        if (!profile.systemPrompt.empty())
        {
            messages.push_back(Detail::MessageJson("system", profile.systemPrompt));
        }
        for (const auto& message : history)
        {
            messages.push_back(Detail::MessageJson(message.role, message.content));
        }
        messages.push_back(Detail::MessageJson("user", userText));

        auto options = nlohmann::json::object();
        options["temperature"] = profile.temperature;
        options["num_predict"] = profile.maxTokens;

        auto request = nlohmann::json::object();
        request["model"] = std::string(model);
        request["messages"] = std::move(messages);
        request["stream"] = false;
        request["think"] = false;
        request["options"] = std::move(options);
        return Detail::Dump(request);
    }

    // 一覧からローカルモデルの名前だけを取り出します。remote_host / remote_model が
    // 付いたモデルと、名前に cloud が付いたモデルは、無かったものとして扱います。
    [[nodiscard]] inline ChatResult ParseTagsResponse(const std::uint32_t statusCode,
        const std::string_view body, std::vector<std::string>& models)
    {
        models.clear();
        nlohmann::json document;
        if (auto failure = Detail::ReadDocument(statusCode, body, false, document);
            !failure.Succeeded())
        {
            return failure;
        }
        const auto listed = document.find("models");
        if (listed == document.end() || !listed->is_array())
        {
            return Detail::Failure(ChatError::Failed, "モデル一覧の形式が不正です。");
        }
        for (const auto& entry : *listed)
        {
            if (!entry.is_object() || HasRemoteOrigin(entry))
            {
                continue;
            }
            const auto name = entry.find("name");
            if (name != entry.end() && name->is_string()
                && IsLocalModelName(name->get_ref<const std::string&>()))
            {
                models.push_back(name->get<std::string>());
            }
        }
        return {};
    }

    // 送る前の確認です。名前だけではローカルに見えるモデルも、詳細に
    // remote_host / remote_model があればクラウドへ中継されるので拒否します。
    [[nodiscard]] inline ChatResult ParseShowResponse(const std::uint32_t statusCode,
        const std::string_view body)
    {
        nlohmann::json document;
        if (auto failure = Detail::ReadDocument(statusCode, body, true, document);
            !failure.Succeeded())
        {
            return failure;
        }
        if (HasRemoteOrigin(document))
        {
            return Detail::Failure(ChatError::CloudRejected,
                "モデルの詳細に remote_host / remote_model があります。");
        }
        return {};
    }

    // 応答に remote_host / remote_model があれば、返答を取り出さずに捨てます。
    [[nodiscard]] inline ChatResult ParseChatResponse(const std::uint32_t statusCode,
        const std::string_view body)
    {
        nlohmann::json document;
        if (auto failure = Detail::ReadDocument(statusCode, body, true, document);
            !failure.Succeeded())
        {
            return failure;
        }
        if (HasRemoteOrigin(document))
        {
            return Detail::Failure(ChatError::CloudRejected,
                "応答に remote_host / remote_model があるため、結果を捨てました。");
        }
        const auto message = document.find("message");
        if (message == document.end() || !message->is_object())
        {
            return Detail::Failure(ChatError::Failed, "応答に message がありません。");
        }
        const auto content = message->find("content");
        if (content == message->end() || !content->is_string())
        {
            return Detail::Failure(ChatError::Failed, "応答に message.content がありません。");
        }
        ChatResult result;
        result.reply = Trimmed(content->get_ref<const std::string&>());
        if (result.reply.empty())
        {
            return Detail::Failure(ChatError::Failed,
                "返答が空でした。maxTokensを大きくすると直る場合があります。");
        }
        return result;
    }

    // HttpSendは失敗の理由を "... failed with Windows error 12029" の形の
    // 文字列で返します。末尾の番号（WinHTTPのエラーコード）で分けます。
    [[nodiscard]] constexpr ChatError ClassifyTransportError(
        const std::string_view transportError) noexcept
    {
        constexpr std::string_view marker = "Windows error ";
        const auto position = transportError.rfind(marker);
        if (position == std::string_view::npos)
        {
            return ChatError::Failed;
        }
        std::uint32_t code{};
        for (const char character : transportError.substr(position + marker.size()))
        {
            if (character < '0' || character > '9' || code > 99999)
            {
                break;
            }
            code = code * 10 + static_cast<std::uint32_t>(character - '0');
        }
        if (code == 12029)  // ERROR_WINHTTP_CANNOT_CONNECT
        {
            return ChatError::NotRunning;
        }
        if (code == 12002)  // ERROR_WINHTTP_TIMEOUT
        {
            return ChatError::Timeout;
        }
        return ChatError::Failed;
    }

    // 成功した1往復を履歴へ足し、新しいほうからlimit件だけ残します。
    // 先頭が返答になると、質問のない返答から会話が始まってしまうので、
    // そのときは先頭の1件も落とします。
    inline void AppendExchange(std::vector<ChatMessage>& history, std::string userText,
        std::string reply, const std::size_t limit)
    {
        history.push_back({ "user", std::move(userText) });
        history.push_back({ "assistant", std::move(reply) });
        if (history.size() > limit)
        {
            history.erase(history.begin(),
                history.begin() + static_cast<std::ptrdiff_t>(history.size() - limit));
        }
        if (!history.empty() && history.front().role != "user")
        {
            history.erase(history.begin());
        }
    }

    namespace Detail
    {
        // すべての要求がここを通ります。接続先はポートだけから組み立て、送る直前に
        // もう一度判定します。APIキーを扱わないので、認証ヘッダーは付けません。
        // リダイレクトにも追従しないので、応答で別のホストへ誘導されても送りません。
        // bodyが空ならGET、あればJSONのPOSTです。
        [[nodiscard]] inline ChatResult Request(const std::uint16_t port,
            const std::string_view path, const std::string& body,
            std::uint32_t& statusCode, std::string& responseBody)
        {
            const auto url = BuildEndpoint(port, path);
            if (!IsAllowedEndpoint(url))
            {
                return Failure(ChatError::CloudRejected, "このPC以外への接続を拒否しました: " + url);
            }
            LamaPon::HttpRequest request;
            request.url.assign(url.begin(), url.end());
            request.allowInsecureLoopback = true;
            request.maxResponseBytes = 2u * 1024u * 1024u;
            if (!body.empty())
            {
                request.method = L"POST";
                request.headers.emplace_back(L"Content-Type", L"application/json");
                request.body.assign(body.begin(), body.end());
            }
            const auto response = LamaPon::HttpSend(request);
            if (!response.TransportSucceeded())
            {
                return Failure(ClassifyTransportError(response.transportError),
                    response.transportError);
            }
            statusCode = response.statusCode;
            responseBody = response.Text();
            return {};
        }
    }

    // 1回分の会話を最後まで実行します。応答が届くまで戻らない（最大で約30秒）
    // ので、ゲームのスレッドからは呼ばず、ChatWorkerを通してください。
    // HttpSendは途中で止められないため、stopは段階の切れ目でだけ確認します。
    [[nodiscard]] inline ChatResult Chat(const ModelProfile& profile,
        const std::vector<ChatMessage>& history, const std::string_view userText,
        const std::stop_token stop = {})
    {
        std::uint32_t statusCode{};
        std::string body;
        auto model = profile.model;
        if (model.empty())
        {
            if (auto failure = Detail::Request(profile.port, "/api/tags", {}, statusCode, body);
                !failure.Succeeded())
            {
                return failure;
            }
            std::vector<std::string> models;
            if (auto failure = ParseTagsResponse(statusCode, body, models);
                !failure.Succeeded())
            {
                return failure;
            }
            if (models.empty())
            {
                return Detail::Failure(ChatError::ModelMissing,
                    "このPCにローカルモデルが1つもありません。");
            }
            model = std::move(models.front());
        }
        // 設定の読み込みでも拒否していますが、ここは設定アセットを通らない
        // 呼び出しに備えた最後の確認です。
        if (!IsLocalModelName(model))
        {
            return Detail::Failure(ChatError::CloudRejected, "モデル名: " + model);
        }
        if (stop.stop_requested())
        {
            return Detail::Failure(ChatError::Failed, "送信を取り消しました。");
        }

        if (auto failure = Detail::Request(profile.port, "/api/show",
                BuildShowRequest(model), statusCode, body);
            !failure.Succeeded())
        {
            return failure;
        }
        if (auto failure = ParseShowResponse(statusCode, body); !failure.Succeeded())
        {
            return failure;
        }
        if (stop.stop_requested())
        {
            return Detail::Failure(ChatError::Failed, "送信を取り消しました。");
        }

        if (auto failure = Detail::Request(profile.port, "/api/chat",
                BuildChatRequest(profile, model, history, userText), statusCode, body);
            !failure.Succeeded())
        {
            return failure;
        }
        auto result = ParseChatResponse(statusCode, body);
        result.model = std::move(model);
        return result;
    }
}
