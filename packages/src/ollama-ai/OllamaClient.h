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

// Loopback上のOllama APIを検証し、会話要求と応答を処理します。
namespace LamaPonOllama
{
    // イベントのEventArgs.numberへそのまま入れるので、番号を変えないでください。
    enum class ChatError
    {
        None = 0,
        // Ollamaが起動していない
        NotRunning = 1,
        // モデルをまだ取得していない
        ModelMissing = 2,
        // 時間内に返答が届かなかった
        Timeout = 3,
        // クラウドのモデル、またはこのPC以外への接続を拒否した
        CloudRejected = 4,
        // 上のどれでもない失敗
        Failed = 5,
        // 返答待ちの間に送ろうとした（待っている返答はそのまま届く）
        Busy = 6
    };

    struct ChatMessage final
    {
        // "user" または "assistant"
        std::string role;
        // 会話へ渡す文章
        std::string content;
    };

    struct ChatResult final
    {
        // 成功または失敗の分類
        ChatError error{ ChatError::None };
        // 成功したときだけ入ります
        std::string reply;
        // 実際に使ったモデル名
        std::string model;
        // ログ向けの補足
        std::string detail;

        // エラーなしで完了したか返します。
        [[nodiscard]] bool Succeeded() const noexcept
        {
            return error == ChatError::None;
        }
    };

    // エラー分類の識別名を返します(error: 分類値)。
    [[nodiscard]] constexpr const char* ErrorName(const ChatError error) noexcept
    {
        switch (error)
        {
        case ChatError::None: return "None";
        case ChatError::NotRunning: return "NotRunning";
        case ChatError::ModelMissing: return "ModelMissing";
        case ChatError::Timeout: return "Timeout";
        case ChatError::CloudRejected: return "CloudRejected";
        case ChatError::Busy: return "Busy";
        case ChatError::Failed: break;
        }
        return "Failed";
    }

    // エラー分類ごとの表示文を返します(error: 分類値)。
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
        case ChatError::Busy:
            return "返答を待っている間は送信できません。返答が届いてから、もう一度送ってください。";
        case ChatError::Failed: break;
        }
        return "Ollamaから返答を受け取れませんでした。";
    }

    // 文章の前後の空白と改行を取り除きます(text: 入力文)。
    [[nodiscard]] inline std::string Trimmed(const std::string_view text)
    {
        // 前後から除去する空白文字
        constexpr std::string_view spaces = " \t\r\n";
        // 最初の非空白位置
        const auto first = text.find_first_not_of(spaces);
        if (first == std::string_view::npos)
        {
            return {};
        }
        return std::string(text.substr(first, text.find_last_not_of(spaces) - first + 1));
    }

    namespace Detail
    {
        // 失敗結果を作ります(error: 分類値, detail: 原因の補足)。
        [[nodiscard]] inline ChatResult Failure(const ChatError error, std::string detail)
        {
            // 返却する失敗情報
            ChatResult result;
            result.error = error;
            result.detail = std::move(detail);
            return result;
        }

        // JSONをUTF-8置換付きで直列化します(value: JSON値)。
        [[nodiscard]] inline std::string Dump(const nlohmann::json& value)
        {
            return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        // roleとcontentを持つChat messageを作ります(role: 発話者, content: 本文)。
        [[nodiscard]] inline nlohmann::json MessageJson(
            const std::string_view role, const std::string_view content)
        {
            // APIへ渡す1件の会話
            auto message = nlohmann::json::object();
            message["role"] = std::string(role);
            message["content"] = std::string(content);
            return message;
        }

        // HTTP JSONから短い失敗理由を抽出します(document: 応答JSON)。
        [[nodiscard]] inline std::string ServerError(const nlohmann::json& document)
        {
            if (document.is_object())
            {
                // 応答内のerror欄
                const auto found = document.find("error");
                if (found != document.end() && found->is_string())
                {
                    return found->get<std::string>().substr(0, 200);
                }
            }
            return {};
        }

        // HTTP状態とJSON形式を検証します(statusCode: HTTP状態, body: 応答文, notFoundMeansModelMissing: 404をモデル不在とするか, document: 出力JSON)。
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

    // モデル詳細のJSON要求を作ります(model: Ollamaモデル名)。
    [[nodiscard]] inline std::string BuildShowRequest(const std::string_view model)
    {
        // /api/showへ送るJSON
        auto request = nlohmann::json::object();
        request["model"] = std::string(model);
        return Detail::Dump(request);
    }

    // モデル保持時間をAPI書式で返します(profile: Model設定)。
    [[nodiscard]] inline std::string KeepAlive(const ModelProfile& profile)
    {
        return std::to_string(profile.keepAliveMinutes) + "m";
    }

    // モデル読込専用のChat JSONを作ります(profile: Model設定, model: 選択モデル)。
    [[nodiscard]] inline std::string BuildPreloadRequest(const ModelProfile& profile,
        const std::string_view model)
    {
        // /api/chatへ送るPreload JSON
        auto request = nlohmann::json::object();
        request["model"] = std::string(model);
        request["messages"] = nlohmann::json::array();
        request["stream"] = false;
        request["keep_alive"] = KeepAlive(profile);
        return Detail::Dump(request);
    }

    // Chat JSONを組み立てます(profile: Model設定, model: 選択モデル, history: 会話履歴, userText: 新しい発言)。
    // stream:false・think:falseで30秒制限内に完了した本文を受け取ります。
    [[nodiscard]] inline std::string BuildChatRequest(const ModelProfile& profile,
        const std::string_view model, const std::vector<ChatMessage>& history,
        const std::string_view userText)
    {
        // system・履歴・新規発言の配列
        auto messages = nlohmann::json::array();
        // 設定済みsystem promptを先頭に加えます。
        if (!profile.systemPrompt.empty())
        {
            messages.push_back(Detail::MessageJson("system", profile.systemPrompt));
        }
        // 過去の会話履歴を追加します(message: 発話者と本文)。
        for (const auto& message : history)
        {
            messages.push_back(Detail::MessageJson(message.role, message.content));
        }
        messages.push_back(Detail::MessageJson("user", userText));

        // 応答生成の数値設定
        auto options = nlohmann::json::object();
        options["temperature"] = profile.temperature;
        options["num_predict"] = profile.maxTokens;

        // /api/chatへ送る最終JSON
        auto request = nlohmann::json::object();
        request["model"] = std::string(model);
        request["messages"] = std::move(messages);
        request["stream"] = false;
        request["think"] = false;
        request["keep_alive"] = KeepAlive(profile);
        request["options"] = std::move(options);
        return Detail::Dump(request);
    }

    // 一覧応答から利用可能なモデル名を抽出します(statusCode: HTTP状態, body: 応答文, models: 出力一覧)。
    [[nodiscard]] inline ChatResult ParseTagsResponse(const std::uint32_t statusCode,
        const std::string_view body, std::vector<std::string>& models)
    {
        models.clear();
        // 解析するOllama応答
        nlohmann::json document;
        // HTTPとJSON形式を確認します。
        if (auto failure = Detail::ReadDocument(statusCode, body, false, document);
            !failure.Succeeded())
        {
            return failure;
        }
        // 応答内のモデル配列
        const auto listed = document.find("models");
        if (listed == document.end() || !listed->is_array())
        {
            return Detail::Failure(ChatError::Failed, "モデル一覧の形式が不正です。");
        }
        // 一覧内のモデル定義
        for (const auto& entry : *listed)
        {
            if (!entry.is_object() || HasRemoteOrigin(entry))
            {
                continue;
            }
            // モデル名欄
            const auto name = entry.find("name");
            if (name != entry.end() && name->is_string()
                && IsLocalModelName(name->get_ref<const std::string&>()))
            {
                models.push_back(name->get<std::string>());
            }
        }
        return {};
    }

    // モデル詳細のHTTP応答を検証します(statusCode: HTTP状態, body: 応答文)。
    [[nodiscard]] inline ChatResult ParseShowResponse(const std::uint32_t statusCode,
        const std::string_view body)
    {
        // 解析する詳細応答
        nlohmann::json document;
        // 404をモデル不在として判定します。
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

    // PreloadのHTTP応答を検証します(statusCode: HTTP状態, body: 応答文)。
    [[nodiscard]] inline ChatResult ParsePreloadResponse(const std::uint32_t statusCode,
        const std::string_view body)
    {
        // 解析するPreload応答
        nlohmann::json document;
        if (auto failure = Detail::ReadDocument(statusCode, body, true, document);
            !failure.Succeeded())
        {
            return failure;
        }
        if (HasRemoteOrigin(document))
        {
            return Detail::Failure(ChatError::CloudRejected,
                "読み込みの応答に remote_host / remote_model があります。");
        }
        return {};
    }

    // Chat応答を検証して返答を取り出します(statusCode: HTTP状態, body: 応答文)。
    [[nodiscard]] inline ChatResult ParseChatResponse(const std::uint32_t statusCode,
        const std::string_view body)
    {
        // 解析するChat応答
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
        // 応答内のmessageオブジェクト
        const auto message = document.find("message");
        if (message == document.end() || !message->is_object())
        {
            return Detail::Failure(ChatError::Failed, "応答に message がありません。");
        }
        // Assistantの返答欄
        const auto content = message->find("content");
        if (content == message->end() || !content->is_string())
        {
            return Detail::Failure(ChatError::Failed, "応答に message.content がありません。");
        }
        // 正常応答のChat結果
        ChatResult result;
        result.reply = Trimmed(content->get_ref<const std::string&>());
        if (result.reply.empty())
        {
            return Detail::Failure(ChatError::Failed,
                "返答が空でした。maxTokensを大きくすると直る場合があります。");
        }
        return result;
    }

    // WinHTTPの通信失敗を分類します(transportError: エラー文字列)。
    [[nodiscard]] constexpr ChatError ClassifyTransportError(
        const std::string_view transportError) noexcept
    {
        // エラー番号の直前にある識別文字列
        constexpr std::string_view marker = "Windows error ";
        // エラー番号の開始位置
        const auto position = transportError.rfind(marker);
        if (position == std::string_view::npos)
        {
            return ChatError::Failed;
        }
        // 解析したWinHTTP error code
        std::uint32_t code{};
        // エラー番号の数字
        for (const char character : transportError.substr(position + marker.size()))
        {
            if (character < '0' || character > '9' || code > 99999)
            {
                break;
            }
            code = code * 10 + static_cast<std::uint32_t>(character - '0');
        }
        // ERROR_WINHTTP_CANNOT_CONNECT
        if (code == 12029)
        {
            return ChatError::NotRunning;
        }
        // ERROR_WINHTTP_TIMEOUT
        if (code == 12002)
        {
            return ChatError::Timeout;
        }
        return ChatError::Failed;
    }

    // 成功した往復を履歴へ追加します(history: 会話履歴, userText: 質問, reply: 返答, limit: 最大発言数)。
    // 上限超過と先頭Assistantを除き、質問から会話を始めます。
    inline void AppendExchange(std::vector<ChatMessage>& history, std::string userText,
        std::string reply, const std::size_t limit)
    {
        history.push_back({ "user", std::move(userText) });
        history.push_back({ "assistant", std::move(reply) });
        // 古い発言から上限を超えた分を削除します。
        if (history.size() > limit)
        {
            history.erase(history.begin(),
                history.begin() + static_cast<std::ptrdiff_t>(history.size() - limit));
        }
        // 先頭がAssistantなら対応する質問も失われているため除きます。
        if (!history.empty() && history.front().role != "user")
        {
            history.erase(history.begin());
        }
    }

    namespace Detail
    {
        // Loopback APIへ要求します(port: 接続ポート, path: API経路, body: JSON要求, statusCode: HTTP状態出力, responseBody: 応答本文出力)。
        // 送信直前に接続先を再検証し、認証もredirect追従もしません。
        [[nodiscard]] inline ChatResult Request(const std::uint16_t port,
            const std::string_view path, const std::string& body,
            std::uint32_t& statusCode, std::string& responseBody)
        {
            // 検証するLoopback URL
            const auto url = BuildEndpoint(port, path);
            if (!IsAllowedEndpoint(url))
            {
                return Failure(ChatError::CloudRejected, "このPC以外への接続を拒否しました: " + url);
            }
            // Engine HttpClientへ渡す要求
            LamaPon::HttpRequest request;
            request.url.assign(url.begin(), url.end());
            request.allowInsecureLoopback = true;
            request.maxResponseBytes = 2u * 1024u * 1024u;
            // 本文がある要求はJSON POSTにします。
            if (!body.empty())
            {
                request.method = L"POST";
                request.headers.emplace_back(L"Content-Type", L"application/json");
                request.body.assign(body.begin(), body.end());
            }
            // WinHTTPから受け取る応答
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

        // 利用モデルを選択・検証します(profile: Model設定, stop: 停止要求, model: 出力名)。
        [[nodiscard]] inline ChatResult SelectLocalModel(const ModelProfile& profile,
            const std::stop_token stop, std::string& model)
        {
            // /api/tags・/api/showのHTTP状態
            std::uint32_t statusCode{};
            // Ollama応答本文
            std::string body;
            // 明示指定または自動選択したモデル
            model = profile.model;
            // 未指定ならローカル一覧から選びます。
            if (model.empty())
            {
                // ローカルモデル一覧を取得します。
                if (auto failure = Request(profile.port, "/api/tags", {}, statusCode, body);
                    !failure.Succeeded())
                {
                    return failure;
                }
                // 検証済みローカルモデル名
                std::vector<std::string> models;
                // 応答から許可されたモデルだけ抽出します。
                if (auto failure = ParseTagsResponse(statusCode, body, models);
                    !failure.Succeeded())
                {
                    return failure;
                }
                // 選択可能なモデルがない場合は中止します。
                if (models.empty())
                {
                    return Failure(ChatError::ModelMissing,
                        "このPCにローカルモデルが1つもありません。");
                }
                model = std::move(models.front());
            }
            // 呼び出し側がProfile検証を通らない場合もCloud名を拒否します。
            if (!IsLocalModelName(model))
            {
                return Failure(ChatError::CloudRejected, "モデル名: " + model);
            }
            // HTTP要求開始前の取消を確認します。
            if (stop.stop_requested())
            {
                return Failure(ChatError::Failed, "送信を取り消しました。");
            }

            // Cloud中継されないモデルか詳細を照会します。
            if (auto failure = Request(profile.port, "/api/show",
                    BuildShowRequest(model), statusCode, body);
                !failure.Succeeded())
            {
                return failure;
            }
            // 詳細応答からRemoteモデルを拒否します。
            if (auto failure = ParseShowResponse(statusCode, body); !failure.Succeeded())
            {
                return failure;
            }
            // ChatまたはPreload要求の直前に取消を確認します。
            if (stop.stop_requested())
            {
                return Failure(ChatError::Failed, "送信を取り消しました。");
            }
            return {};
        }
    }

    // 会話を実行します(profile: Model設定, history: 履歴, userText: 新しい発言, stop: 停止要求)。
    // 同期HTTPは約30秒戻らないためChatWorker経由で呼び、stopは要求間で確認します。
    [[nodiscard]] inline ChatResult Chat(const ModelProfile& profile,
        const std::vector<ChatMessage>& history, const std::string_view userText,
        const std::stop_token stop = {})
    {
        // 検証・選択したモデル名
        std::string model;
        if (auto failure = Detail::SelectLocalModel(profile, stop, model); !failure.Succeeded())
        {
            return failure;
        }
        // /api/chatのHTTP状態
        std::uint32_t statusCode{};
        // /api/chatの応答本文
        std::string body;
        if (auto failure = Detail::Request(profile.port, "/api/chat",
                BuildChatRequest(profile, model, history, userText), statusCode, body);
            !failure.Succeeded())
        {
            return failure;
        }
        // HTTP本文から抽出した返答
        auto result = ParseChatResponse(statusCode, body);
        result.model = std::move(model);
        return result;
    }

    // モデルを事前読込します(profile: Model設定, stop: 停止要求)。
    // 同期HTTPは約30秒戻らないためChatWorker経由で呼びます。
    [[nodiscard]] inline ChatResult Preload(const ModelProfile& profile,
        const std::stop_token stop = {})
    {
        // 検証・選択したモデル名
        std::string model;
        if (auto failure = Detail::SelectLocalModel(profile, stop, model); !failure.Succeeded())
        {
            return failure;
        }
        // Preload要求のHTTP状態
        std::uint32_t statusCode{};
        // Preload応答本文
        std::string body;
        if (auto failure = Detail::Request(profile.port, "/api/chat",
                BuildPreloadRequest(profile, model), statusCode, body);
            !failure.Succeeded())
        {
            return failure;
        }
        // HTTP状態を検証したPreload結果
        auto result = ParsePreloadResponse(statusCode, body);
        result.model = std::move(model);
        return result;
    }
}
