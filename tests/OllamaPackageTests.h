#pragma once

#include <WinSock2.h>

#include "../packages/src/ollama-ai/OllamaWorker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace OllamaTestDetail
{
    // Ollamaの代わりに応答する、このテスト専用の小さなHTTPサーバーです。
    // 127.0.0.1の空いているポートで待ち受け、届いた要求を順に記録します。
    // 要求は1件ずつ処理します（Scriptが同時に送るのは1件までです）。
    class FakeOllama final
    {
    public:
        struct Received final
        {
            std::string path;
            nlohmann::json body;
        };

        // trueの間、読み込み（messagesが空）や会話の応答を返さずに待たせます。
        std::atomic<bool> holdPreload{};
        std::atomic<bool> holdChat{};

        FakeOllama()
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                throw std::runtime_error("WSAStartup failed.");
            }
            m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int length = sizeof(address);
            if (m_listener == INVALID_SOCKET
                || bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
                || listen(m_listener, SOMAXCONN) != 0
                || getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            {
                if (m_listener != INVALID_SOCKET) closesocket(m_listener);
                WSACleanup();
                throw std::runtime_error("The fake Ollama server could not listen.");
            }
            m_port = ntohs(address.sin_port);
            m_thread = std::jthread([this](const std::stop_token stop) { Serve(stop); });
        }

        FakeOllama(const FakeOllama&) = delete;
        FakeOllama& operator=(const FakeOllama&) = delete;

        ~FakeOllama()
        {
            m_thread.request_stop();
            // 待ち受けを閉じると、acceptが失敗して戻ります。
            closesocket(m_listener);
            m_thread.join();
            WSACleanup();
        }

        [[nodiscard]] std::uint16_t Port() const noexcept { return m_port; }

        [[nodiscard]] std::vector<Received> Requests() const
        {
            const std::scoped_lock lock(m_mutex);
            return m_requests;
        }

        [[nodiscard]] std::size_t Count() const
        {
            const std::scoped_lock lock(m_mutex);
            return m_requests.size();
        }

    private:
        void Serve(const std::stop_token stop)
        {
            while (!stop.stop_requested())
            {
                const SOCKET client = accept(m_listener, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                {
                    return;
                }
                Answer(client, stop);
                closesocket(client);
            }
        }

        void Answer(const SOCKET client, const std::stop_token stop)
        {
            // ヘッダーと、Content-Lengthの長さの本文がそろうまで読みます。
            std::string request;
            auto headerEnd = std::string::npos;
            std::size_t contentLength{};
            while (headerEnd == std::string::npos || request.size() < headerEnd + 4 + contentLength)
            {
                char buffer[4096];
                const int received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0)
                {
                    return;
                }
                request.append(buffer, static_cast<std::size_t>(received));
                if (headerEnd != std::string::npos)
                {
                    continue;
                }
                headerEnd = request.find("\r\n\r\n");
                if (headerEnd == std::string::npos)
                {
                    continue;
                }
                auto headers = request.substr(0, headerEnd);
                std::ranges::transform(headers, headers.begin(), [](const unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
                constexpr std::string_view name = "content-length:";
                if (const auto position = headers.find(name); position != std::string::npos)
                {
                    contentLength = std::stoul(headers.substr(position + name.size()));
                }
            }

            // 1行目は "POST /api/chat HTTP/1.1" の形です。
            const auto pathStart = request.find(' ') + 1;
            Received received;
            received.path = request.substr(pathStart, request.find(' ', pathStart) - pathStart);
            received.body = nlohmann::json::parse(request.substr(headerEnd + 4), nullptr, false);
            const auto model = received.body.is_object()
                ? received.body.value("model", std::string{}) : std::string{};
            const bool chat = received.path == "/api/chat" && received.body.is_object()
                && received.body.contains("messages");
            const bool preload = chat && received.body.at("messages").empty();
            {
                const std::scoped_lock lock(m_mutex);
                m_requests.push_back(received);
            }

            int status = 200;
            nlohmann::json reply = nlohmann::json::object();
            if (received.path == "/api/tags")
            {
                reply["models"] = nlohmann::json::array({ nlohmann::json{{"name", "fake:1b"}} });
            }
            else if (model == "missing:1b" || (received.path != "/api/show" && !chat))
            {
                status = 404;
                reply["error"] = "model '" + model + "' not found";
            }
            else if (received.path == "/api/show")
            {
                reply["details"] = nlohmann::json{{"family", "fake"}};
            }
            else
            {
                const auto& hold = preload ? holdPreload : holdChat;
                while (hold && !stop.stop_requested())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                reply["model"] = model;
                reply["done"] = true;
                reply["message"] = nlohmann::json{{"role", "assistant"},
                    {"content", preload ? std::string{} : "返答" + std::to_string(++m_replies)}};
                if (preload)
                {
                    reply["done_reason"] = "load";
                }
            }

            const auto body = reply.dump();
            const auto response = "HTTP/1.1 " + std::to_string(status)
                + (status == 200 ? " OK" : " Not Found")
                + "\r\nContent-Type: application/json\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::size_t sent{};
            while (sent < response.size())
            {
                const int written = send(client, response.data() + sent,
                    static_cast<int>(response.size() - sent), 0);
                if (written <= 0)
                {
                    return;
                }
                sent += static_cast<std::size_t>(written);
            }
            shutdown(client, SD_SEND);
        }

        SOCKET m_listener{ INVALID_SOCKET };
        std::uint16_t m_port{};
        mutable std::mutex m_mutex;
        // m_requests は m_mutex で守ります。m_replies はサーバーのスレッドだけが触ります。
        std::vector<Received> m_requests;
        int m_replies{};
        std::jthread m_thread;
    };

    // Scriptが発行する返答と失敗を数えます。
    struct ChatProbe final
    {
        int replies{};
        int errors{};
        std::string reply;
        std::string errorName;
        float errorNumber{};

        explicit ChatProbe(LamaPon::Scene& scene)
        {
            scene.Events().Subscribe("Ollama.Reply", [this](const LamaPon::EventArgs& args)
            {
                ++replies;
                reply = args.text;
            });
            scene.Events().Subscribe("Ollama.Error", [this](const LamaPon::EventArgs& args)
            {
                ++errors;
                errorName = args.text;
                errorNumber = args.number;
            });
        }
    };
}

// 前半は、Ollamaが起動していなくても通る範囲を確かめます。通信を伴う経路は、
// 何も待ち受けていないポートへ送り、「起動していない」として扱われることを見ます。
// 後半は、返答が届く経路（事前読み込み・履歴・リセット）を、テスト用のサーバーで確かめます。
inline void TestOllamaPackage(LamaPon::GraphicsDevice& graphics,
    const std::filesystem::path& sourceRoot, const std::filesystem::path& outputRoot)
{
    using namespace LamaPon;
    using namespace LamaPonOllama;
    const auto require = [](const bool result, const char* message)
    {
        if (!result) throw std::runtime_error(message);
    };

    // 接続先はこのPCへのhttpと、3つのAPIだけです。
    for (const char* url : {
        "http://127.0.0.1:11434/api/tags", "http://127.0.0.1:11434/api/show",
        "http://127.0.0.1:11434/api/chat", "http://localhost:11434/api/chat",
        "http://[::1]:11434/api/chat", "http://127.0.0.1:1/api/chat",
        "http://127.0.0.1:65535/api/chat"})
    {
        if (!IsAllowedEndpoint(url))
            throw std::runtime_error(std::string("A loopback Ollama endpoint was rejected: ") + url);
    }
    for (const char* url : {
        "", "http://", "http://127.0.0.1:11434", "http://127.0.0.1/api/chat",
        "https://ollama.com/api/chat", "http://ollama.com/api/chat", "http://ollama.com:11434/api/chat",
        "https://127.0.0.1:11434/api/chat", "HTTP://127.0.0.1:11434/api/chat",
        "http://127.0.0.1.ollama.com:11434/api/chat", "http://localhost.ollama.com:11434/api/chat",
        "http://127.0.0.1:11434@ollama.com/api/chat", "http://ollama.com#@127.0.0.1:11434/api/chat",
        "http://127.0.0.1:11434\\@ollama.com/api/chat", "http://127.0.0.1%2eollama.com:11434/api/chat",
        "http://192.168.0.10:11434/api/chat", "http://0.0.0.0:11434/api/chat",
        "http://127.0.0.2:11434/api/chat", "http://LOCALHOST:11434/api/chat",
        "http://[::1]/api/chat", "http://[::1]x:11434/api/chat", "http://[::2]:11434/api/chat",
        "http://127.0.0.1:0/api/chat", "http://127.0.0.1:65536/api/chat",
        "http://127.0.0.1:11434x/api/chat", "http://127.0.0.1::11434/api/chat",
        "http://127.0.0.1:11434/", "http://127.0.0.1:11434/api/generate",
        "http://127.0.0.1:11434/api/pull", "http://127.0.0.1:11434/api/web_search",
        "http://127.0.0.1:11434/api/web_fetch", "http://127.0.0.1:11434/api/chat/",
        "http://127.0.0.1:11434/api/chat?x=1", "http://127.0.0.1:11434/api/chat#x",
        "http://127.0.0.1:11434/api/chat ", "http://127.0.0.1:11434/API/chat"})
    {
        if (IsAllowedEndpoint(url))
            throw std::runtime_error(std::string("A forbidden Ollama endpoint was accepted: ") + url);
    }
    require(BuildEndpoint(DefaultPort, "/api/chat") == "http://127.0.0.1:11434/api/chat"
        && IsAllowedEndpoint(BuildEndpoint(40000, "/api/tags"))
        && !IsAllowedEndpoint(BuildEndpoint(0, "/api/chat"))
        && !IsAllowedEndpoint(BuildEndpoint(DefaultPort, "/api/web_search")),
        "Endpoints must be built from the port only.");

    // クラウドのモデルは名前で拒否します。
    for (const char* model : {
        "gemma3", "gemma3:4b", "llama3.2:latest", "qwen2.5-coder:7b",
        "hf.co/user/Model-GGUF:Q4_K_M", "cloudy:latest", "McLoud_7"})
    {
        if (!IsLocalModelName(model))
            throw std::runtime_error(std::string("A local model name was rejected: ") + model);
    }
    for (const char* model : {
        "", "gemma4:cloud", "GEMMA4:CLOUD", "gpt-oss:120b-cloud", "qwen3-coder:480b-cloud",
        "xxx-cloud", "xxx-cloud:latest", "cloud/model", "my_cloud_model", "my model", "model\n",
        "model?x=1", "モデル"})
    {
        if (IsLocalModelName(model))
            throw std::runtime_error(std::string("A cloud or malformed model name was accepted: ") + model);
    }
    require(!IsLocalModelName(std::string(129, 'a')) && IsLocalModelName(std::string(128, 'a'))
        && HasCloudTag("gemma4:cloud") && !HasCloudTag("my model") && !HasCloudTag(""),
        "Model name limits or the cloud tag check are wrong.");

    // クラウドのモデルは、応答の remote_host / remote_model でも見分けます。
    require(!HasRemoteOrigin(nlohmann::json::parse(R"({"model":"gemma3:4b"})"))
        && !HasRemoteOrigin(nlohmann::json::parse(R"({"remote_host":"","remote_model":null})"))
        && !HasRemoteOrigin(nlohmann::json::parse(R"(["remote_host"])"))
        && HasRemoteOrigin(nlohmann::json::parse(R"({"remote_host":"https://ollama.com:443"})"))
        && HasRemoteOrigin(nlohmann::json::parse(R"({"remote_model":"gemma4:31b"})"))
        && HasRemoteOrigin(nlohmann::json::parse(R"({"remote_host":1})")),
        "Remote model markers were not detected.");

    // 同梱の設定アセットと、エディターで新規作成した直後の既定値。
    graphics.Assets().SetAssetRoot(sourceRoot, false);
    ModelProfile profile;
    std::string error;
    std::size_t profiles{};
    for (const auto& entry : std::filesystem::directory_iterator(sourceRoot / "profiles"))
    {
        const auto asset = graphics.Assets().LoadDataAsset(
            std::filesystem::relative(entry.path(), sourceRoot));
        require(ReadProfile(*asset, profile, error), "A shipped Ollama profile is invalid.");
        require(profile.port == DefaultPort && profile.maxTokens <= 256
            && !profile.fallbackReply.empty(),
            "Shipped Ollama profiles must use the default port and short replies.");
        ++profiles;
    }
    require(profiles == 1, "One Ollama profile must ship in the package.");
    const auto schema = nlohmann::json::parse(ProfileSchema);
    nlohmann::json defaults = nlohmann::json::object();
    for (const auto& field : schema.at("fields"))
        defaults[field.at("name").get<std::string>()] = field.at("default");
    // ホストやAPIキーの欄が増えたら、ここで気付けるように項目名を固定します。
    require(defaults.size() == 8 && defaults.contains("model") && defaults.contains("systemPrompt")
        && defaults.contains("temperature") && defaults.contains("maxTokens")
        && defaults.contains("port") && defaults.contains("fallbackReply")
        && defaults.contains("historyLimit") && defaults.contains("keepAliveMinutes"),
        "The Ollama profile schema must not gain host or key fields.");
    const ModelProfile expected;
    require(ReadProfile(DataAsset::FromJson(nlohmann::json{{"type",ProfileType},{"values",defaults}}.dump()),
        profile, error) && profile.model.empty() && profile.systemPrompt == expected.systemPrompt
        && profile.temperature == expected.temperature && profile.maxTokens == expected.maxTokens
        && profile.port == DefaultPort && profile.fallbackReply == expected.fallbackReply
        && profile.historyLimit == expected.historyLimit
        && profile.keepAliveMinutes == expected.keepAliveMinutes && expected.keepAliveMinutes == 10,
        "New Ollama asset defaults differ from the code defaults.");
    for (const auto& values : {
        R"({"model":"gemma4:cloud"})", R"({"model":"gpt-oss:120b-cloud"})", R"({"model":"my model"})",
        R"({"model":7})", R"({"port":0})", R"({"port":65536})", R"({"port":11434.5})",
        R"({"port":"11434"})", R"({"maxTokens":0})", R"({"maxTokens":4096})",
        R"({"temperature":-0.1})", R"({"temperature":2.5})", R"({"temperature":"hot"})",
        R"({"historyLimit":-1})", R"({"historyLimit":65})", R"({"systemPrompt":false})",
        R"({"keepAliveMinutes":0})", R"({"keepAliveMinutes":1441})", R"({"keepAliveMinutes":1.5})",
        R"({"keepAliveMinutes":"10m"})",
        R"({"port":11434,"host":"ollama.com"})", R"({"port":11434,"apiKey":"secret"})",
        R"({"port":11434,"baseUrl":"https://ollama.com"})"})
    {
        profile = ModelProfile{};
        profile.model = "unchanged";
        require(!ReadProfile(DataAsset::FromJson(std::string("{\"type\":\"Ollama.ModelProfile\",\"values\":")
            + values + "}"), profile, error) && !error.empty() && profile.model == "unchanged",
            "Malformed Ollama profile must fail atomically.");
    }
    require(!ReadProfile(DataAsset::FromJson(R"({"type":"Other","values":{"port":11434}})"),
        profile, error), "Wrong asset type was accepted.");
    require(ReadProfile(DataAsset::FromJson(R"({"type":"Ollama.ModelProfile","values":{
        "model":"gemma3:4b","port":12345,"maxTokens":64,"temperature":0,"historyLimit":0,
        "keepAliveMinutes":1440}})"),
        profile, error) && profile.model == "gemma3:4b" && profile.port == 12345
        && profile.maxTokens == 64 && profile.temperature == 0.0 && profile.historyLimit == 0
        && profile.keepAliveMinutes == 1440 && profile.systemPrompt == expected.systemPrompt,
        "A partial Ollama profile must keep defaults for omitted fields.");

    // リクエストは、返答を1回で受け取る短い会話だけです。キーや追加機能の項目は送りません。
    ModelProfile chatProfile;
    chatProfile.systemPrompt = "system";
    chatProfile.temperature = 0.25;
    chatProfile.maxTokens = 64;
    chatProfile.keepAliveMinutes = 7;
    const std::vector<ChatMessage> history{ { "user", "前の質問" }, { "assistant", "前の返答" } };
    const auto message = [](const char* role, const char* content)
    {
        return nlohmann::json{{"role", role}, {"content", content}};
    };
    auto request = nlohmann::json::parse(BuildChatRequest(chatProfile, "gemma3:4b", history, "こんにちは"));
    require(request.size() == 6 && request.at("model") == "gemma3:4b"
        && request.at("stream") == false && request.at("think") == false
        && request.at("keep_alive") == "7m"
        && request.at("options") == nlohmann::json{{"temperature", 0.25}, {"num_predict", 64}}
        && request.at("messages") == nlohmann::json::array({ message("system", "system"),
            message("user", "前の質問"), message("assistant", "前の返答"), message("user", "こんにちは") }),
        "The chat request is not a short non-streaming local chat.");
    chatProfile.systemPrompt.clear();
    request = nlohmann::json::parse(BuildChatRequest(chatProfile, "gemma3:4b", {}, "\xff"));
    require(request.at("messages").size() == 1 && request.at("messages")[0].at("role") == "user",
        "An empty system prompt or invalid UTF-8 broke the chat request.");
    require(nlohmann::json::parse(BuildShowRequest("gemma3:4b")) == nlohmann::json{{"model", "gemma3:4b"}},
        "The show request is malformed.");
    // 事前読み込みは、messagesが空の会話です。返答を作らせる項目は送りません。
    require(nlohmann::json::parse(BuildPreloadRequest(chatProfile, "gemma3:4b"))
        == nlohmann::json{{"model", "gemma3:4b"}, {"messages", nlohmann::json::array()},
            {"stream", false}, {"keep_alive", "7m"}},
        "The preload request must load the model without generating a reply.");

    // 一覧からはローカルモデルだけを残します。
    std::vector<std::string> models;
    require(ParseTagsResponse(200, R"({"models":[
        {"name":"gemma4:cloud","model":"gemma4:cloud","remote_model":"gemma4:31b","remote_host":"https://ollama.com:443"},
        {"name":"renamed:latest","remote_host":"https://ollama.com:443"},
        {"name":"gpt-oss:120b-cloud"},
        {"name":"gemma3:4b","model":"gemma3:4b","size":3338801804},
        {"name":"llama3.2:latest"},{"name":5},"broken"]})", models).Succeeded()
        && models == std::vector<std::string>{ "gemma3:4b", "llama3.2:latest" },
        "Remote or cloud models were not removed from the model list.");
    require(ParseTagsResponse(200, R"({"models":[]})", models).Succeeded() && models.empty()
        && ParseTagsResponse(200, "not json", models).error == ChatError::Failed
        && ParseTagsResponse(404, "{}", models).error == ChatError::Failed
        && ParseTagsResponse(200, R"({"models":{}})", models).error == ChatError::Failed,
        "Malformed model lists were not rejected.");

    // 送る前の確認と、応答の確認。
    require(ParseShowResponse(200, R"({"details":{"family":"gemma3"},"model_info":{}})").Succeeded()
        && ParseShowResponse(200, R"({"remote_model":"gemma4:31b","remote_host":"https://ollama.com:443"})").error
            == ChatError::CloudRejected
        && ParseShowResponse(404, R"({"error":"model 'x' not found"})").error == ChatError::ModelMissing
        && ParseShowResponse(500, R"({"error":"boom"})").error == ChatError::Failed
        && ParseShowResponse(200, "[]").error == ChatError::Failed,
        "The pre-send model check is wrong.");
    const auto reply = ParseChatResponse(200, R"({"model":"gemma3:4b","message":
        {"role":"assistant","content":"\n こんにちは！ \n"},"done":true,"done_reason":"stop"})");
    require(reply.Succeeded() && reply.reply == "こんにちは！", "A local chat reply was not read.");
    const auto remote = ParseChatResponse(200, R"({"model":"x","remote_model":"gemma4:31b",
        "remote_host":"https://ollama.com:443","message":{"role":"assistant","content":"paid"},"done":true})");
    require(remote.error == ChatError::CloudRejected && remote.reply.empty(),
        "A reply generated by a remote model must be discarded.");
    require(ParseChatResponse(404, R"({"error":"model 'x' not found"})").error == ChatError::ModelMissing
        && ParseChatResponse(200, R"({"message":{"role":"assistant","content":"  "},"done":true})").error
            == ChatError::Failed
        && ParseChatResponse(200, R"({"done":true})").error == ChatError::Failed
        && ParseChatResponse(200, R"({"message":{"content":7}})").error == ChatError::Failed
        && ParseChatResponse(400, R"({"error":"bad"})").error == ChatError::Failed
        && ParseChatResponse(200, "").error == ChatError::Failed,
        "Malformed chat replies were not rejected.");
    require(ParsePreloadResponse(200, R"({"model":"gemma3:4b","message":
            {"role":"assistant","content":""},"done":true,"done_reason":"load"})").Succeeded()
        && ParsePreloadResponse(200, R"({"remote_host":"https://ollama.com:443","done":true})").error
            == ChatError::CloudRejected
        && ParsePreloadResponse(404, R"({"error":"model 'x' not found"})").error == ChatError::ModelMissing
        && ParsePreloadResponse(500, R"({"error":"boom"})").error == ChatError::Failed
        && ParsePreloadResponse(200, "").error == ChatError::Failed,
        "The preload reply check is wrong.");

    require(ClassifyTransportError(
            "WinHttpSendRequest/WinHttpReceiveResponse failed with Windows error 12029") == ChatError::NotRunning
        && ClassifyTransportError(
            "WinHttpSendRequest/WinHttpReceiveResponse failed with Windows error 12002") == ChatError::Timeout
        && ClassifyTransportError("WinHttpReadData failed with Windows error 12030") == ChatError::Failed
        && ClassifyTransportError("HTTP response exceeded its configured size limit.") == ChatError::Failed
        && ClassifyTransportError("") == ChatError::Failed,
        "Transport errors were not classified.");
    require(std::string_view(ErrorName(ChatError::NotRunning)) == "NotRunning"
        && std::string_view(ErrorName(ChatError::ModelMissing)) == "ModelMissing"
        && std::string_view(ErrorName(ChatError::Timeout)) == "Timeout"
        && std::string_view(ErrorName(ChatError::CloudRejected)) == "CloudRejected"
        && std::string_view(ErrorName(ChatError::Failed)) == "Failed"
        && std::string_view(ErrorName(ChatError::Busy)) == "Busy"
        && static_cast<int>(ChatError::Failed) == 5 && static_cast<int>(ChatError::Busy) == 6
        && !std::string_view(ErrorMessage(ChatError::Timeout)).empty()
        && !std::string_view(ErrorMessage(ChatError::Busy)).empty(),
        "Error names are part of the event contract.");

    // 履歴は新しいほうからlimit件で、質問から始まります。
    std::vector<ChatMessage> kept;
    AppendExchange(kept, "q1", "a1", 4);
    AppendExchange(kept, "q2", "a2", 4);
    AppendExchange(kept, "q3", "a3", 4);
    require(kept.size() == 4 && kept.front().content == "q2" && kept.back().content == "a3",
        "History did not keep the newest messages.");
    AppendExchange(kept, "q4", "a4", 3);
    require(kept.size() == 2 && kept.front().role == "user" && kept.front().content == "q4",
        "History must start with a user message.");
    AppendExchange(kept, "q5", "a5", 0);
    require(kept.empty(), "A zero history limit must forget everything.");

    const auto waitUntil = [&require](const auto& condition, const char* message)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!condition())
        {
            require(std::chrono::steady_clock::now() < deadline, message);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // 通信スレッドは同時に1件で、破棄するときに終了を待ちます。
    {
        ChatWorker worker;
        std::atomic<bool> release{};
        const auto blocked = [&release](const std::stop_token stop)
        {
            while (!release && !stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ChatResult result;
            result.reply = "done";
            return result;
        };
        require(!worker.Busy() && !worker.Ready() && !worker.Take(), "A new worker must be idle.");
        require(worker.Start(blocked) && worker.Busy() && !worker.Ready() && !worker.Take(),
            "The worker did not start.");
        require(!worker.Start(blocked), "A second request was started while one was running.");
        release = true;
        waitUntil([&worker] { return worker.Ready(); }, "The worker result timed out.");
        require(worker.Busy() && !worker.Start(blocked),
            "An untaken result must block the next request.");
        const auto result = worker.Take();
        require(result && result->reply == "done" && !worker.Busy() && !worker.Take(),
            "The worker result was not delivered exactly once.");

        release = false;
        require(worker.Start(blocked), "The worker could not be reused.");
        worker.Cancel();
        waitUntil([&worker] { return !worker.Busy(); }, "A cancelled request did not stop.");
        require(!worker.Ready() && !worker.Take(), "A cancelled result must be discarded.");
        require(worker.Start([](const std::stop_token) -> ChatResult { throw std::runtime_error("boom"); }),
            "The worker could not restart after a cancel.");
        waitUntil([&worker] { return worker.Ready(); }, "A throwing job did not finish.");
        const auto failed = worker.Take();
        require(failed && failed->error == ChatError::Failed && failed->detail == "boom",
            "A throwing job must become a failed result.");

        std::atomic<bool> finished{};
        {
            ChatWorker scoped;
            require(scoped.Start([&finished](const std::stop_token stop)
            {
                while (!stop.stop_requested())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                finished = true;
                return ChatResult{};
            }), "The scoped worker did not start.");
        }
        require(finished, "Destroying a worker must wait for its thread.");
    }

    // Scriptは、配布Sceneと何も待ち受けていないポートの設定で動かします。
    auto testAssets = outputRoot / "ollama-test-assets";
    std::filesystem::remove_all(testAssets);
    std::filesystem::create_directories(testAssets / "profiles");
    const struct RemoveTestAssets final
    {
        std::filesystem::path root;
        ~RemoveTestAssets()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } removeTestAssets{ testAssets };
    const auto writeProfile = [&testAssets](const char* name, const std::string& values)
    {
        std::ofstream output(testAssets / "profiles" / name, std::ios::binary);
        output << "{\"type\":\"Ollama.ModelProfile\",\"values\":" << values << "}";
    };
    writeProfile("ClosedPort.asset.json",
        R"({"model":"","port":1,"fallbackReply":"FALLBACK","historyLimit":4})");
    writeProfile("CloudModel.asset.json", R"({"model":"gemma4:cloud","port":1})");

    std::ifstream sceneInput(sourceRoot / "scenes" / "OllamaChatDemo.scene.json");
    nlohmann::json demo;
    sceneInput >> demo;
    std::size_t scripts{};
    for (const auto& object : demo.at("objects"))
    {
        for (const auto& component : object.at("components"))
        {
            if (component.at("type") != "NativeScript") continue;
            const auto& properties = component.at("properties");
            require(component.at("script") == "Ollama.Chat"
                && properties.at("profile") == DefaultProfilePath
                && properties.at("preload") == true
                && properties.at("resetEvent") == "Ollama.Reset",
                "The demo scene must use the shipped script, profile and defaults.");
            ++scripts;
        }
    }
    require(scripts == 1, "The demo scene must contain one Ollama chat script.");
    // 配布Sceneの設定アセットと事前読み込みだけを差し替えたSceneを作ります。
    const auto demoWith = [&demo](const char* profilePath, const bool preload)
    {
        auto copy = demo;
        for (auto& object : copy.at("objects"))
        {
            for (auto& component : object.at("components"))
            {
                if (component.at("type") != "NativeScript") continue;
                component["properties"]["profile"] = profilePath;
                component["properties"]["preload"] = preload;
            }
        }
        return copy.dump();
    };

    graphics.Assets().SetAssetRoot(testAssets, false);
    {
        Scene scene(graphics);
        scene.LoadFromJson(demoWith("profiles/ClosedPort.asset.json", true));
        scene.Update(0.01f);
        auto* chat = scene.FindGameObjectByName("Ollama Chat");
        auto* inputObject = scene.FindGameObjectByName("Ollama Input");
        const auto* replyObject = scene.FindGameObjectByName("Ollama Reply");
        const auto* sendObject = scene.FindGameObjectByName("Ollama Send");
        require(chat && inputObject && replyObject && sendObject, "Demo objects were not loaded.");
        auto* input = inputObject->GetComponent<UIInputFieldComponent>();
        const auto* output = replyObject->GetComponent<TextRendererComponent>();
        const auto* send = sendObject->GetComponent<UIButtonComponent>();
        // Scriptは、送信とリセットの2つのイベントを待ち受けます。
        require(input && output && send && send->ClickEventName() == "Ollama.Send"
            && scene.Events().SubscriptionCount() == 2,
            "Demo UI is not wired to the chat script.");

        int replies{};
        int errors{};
        std::string errorName;
        float errorNumber{};
        const auto replyHandle = scene.Events().Subscribe("Ollama.Reply",
            [&replies](const EventArgs&) { ++replies; });
        const auto errorHandle = scene.Events().Subscribe("Ollama.Error",
            [&errors, &errorName, &errorNumber](const EventArgs& args)
            {
                ++errors;
                errorName = args.text;
                errorNumber = args.number;
            });

        // 空の入力は送らず、ボタンは入力欄の文を送ります。
        scene.Events().Publish(send->ClickEventName());
        require(output->Text() != "考え中...", "An empty message was sent.");
        input->SetText("こんにちは");
        scene.Events().Publish(send->ClickEventName());
        require(input->Text().empty() && output->Text() == "考え中..." && errors == 0,
            "The send button did not start a request from the input field.");
        // 返答待ちの間の送信は送らず、Busyとして知らせます。表示は変えません。
        EventArgs second;
        second.text = "二重送信";
        scene.Events().Publish("Ollama.Send", second);
        require(errors == 1 && errorName == "Busy"
            && errorNumber == static_cast<float>(ChatError::Busy)
            && output->Text() == "考え中...",
            "A second message must be reported as busy without touching the display.");
        waitUntil([&scene, &errors] { scene.Update(0.01f); return errors > 1; },
            "A request to a closed port did not fail in time.");
        for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
        // 事前読み込みの失敗は知らせないので、届く失敗は送った1件の分だけです。
        require(errors == 2 && replies == 0 && errorName == "NotRunning"
            && errorNumber == static_cast<float>(ChatError::NotRunning)
            && output->Text() == "FALLBACK",
            "A stopped Ollama must show the fallback reply and publish one error.");

        // 通信中にScriptを破棄しても、スレッドの終了を待ってから戻ります。
        EventArgs third;
        third.text = "破棄";
        scene.Events().Publish("Ollama.Send", third);
        require(output->Text() == "考え中...", "The event text did not start a request.");
        scene.DestroyGameObject(*chat);
        for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
        require(errors == 2 && replies == 0 && scene.Events().SubscriptionCount() == 2,
            "A destroyed chat script kept a subscription or delivered a result.");

        // クラウドのモデルを指定した設定は、通信せずにその場で拒否します。
        auto& cloud = scene.CreateGameObject("Cloud Chat");
        cloud.AddComponent<NativeScriptComponent>("Ollama.Chat",
            R"({"profile":"profiles/CloudModel.asset.json","inputObject":"","outputObject":"Ollama Reply"})");
        scene.Update(0.01f);
        scene.Events().Publish("Ollama.Send", third);
        require(errors == 3 && errorName == "CloudRejected"
            && output->Text() == ModelProfile{}.fallbackReply,
            "A cloud model profile must be rejected before any request.");
        scene.DestroyGameObject(cloud);

        auto& missing = scene.CreateGameObject("Missing Profile");
        missing.AddComponent<NativeScriptComponent>("Ollama.Chat",
            R"({"profile":"profiles/Missing.asset.json","inputObject":"","outputObject":""})");
        scene.Update(0.01f);
        scene.Events().Publish("Ollama.Send", third);
        require(errors == 4 && errorName == "Failed", "A missing profile must fail without a request.");
        scene.DestroyGameObject(missing);

        scene.Events().Unsubscribe(replyHandle);
        scene.Events().Unsubscribe(errorHandle);
        require(scene.Events().SubscriptionCount() == 0, "Ollama scripts retained event callbacks.");
    }

    // ここからは、Ollamaの代わりに応答するサーバーで、返答が届く経路を確かめます。
    {
        OllamaTestDetail::FakeOllama server;
        const auto port = std::to_string(server.Port());
        writeProfile("Fake.asset.json", R"({"model":"","systemPrompt":"","fallbackReply":"FALLBACK",)"
            R"("historyLimit":4,"keepAliveMinutes":7,"port":)" + port + "}");
        writeProfile("FakeMissing.asset.json",
            R"({"model":"missing:1b","fallbackReply":"FALLBACK","port":)" + port + "}");
        const auto sendText = [](Scene& scene, const char* text)
        {
            EventArgs args;
            args.text = text;
            scene.Events().Publish("Ollama.Send", args);
        };
        // 会話の要求に入っている文を、送った順に取り出します。
        const auto contents = [](const OllamaTestDetail::FakeOllama::Received& received)
        {
            std::vector<std::string> result;
            for (const auto& entry : received.body.at("messages"))
            {
                result.push_back(entry.at("content").get<std::string>());
            }
            return result;
        };
        using Texts = std::vector<std::string>;

        // 事前読み込み、その間の送信、履歴、リセット。
        {
            server.holdPreload = true;
            Scene scene(graphics);
            scene.LoadFromJson(demoWith("profiles/Fake.asset.json", true));
            OllamaTestDetail::ChatProbe probe(scene);
            scene.Update(0.01f);
            const auto* output = scene.FindGameObjectByName("Ollama Reply")
                ->GetComponent<TextRendererComponent>();
            const auto initialText = output->Text();
            const auto waitForReplies = [&](const int count)
            {
                waitUntil([&] { scene.Update(0.01f); return probe.replies == count; },
                    "A reply from the fake Ollama did not arrive in time.");
            };

            // 有効になった時点で、一覧・詳細・読み込みの順に要求します。
            waitUntil([&server] { return server.Count() == 3; }, "The preload request was not sent.");
            auto requests = server.Requests();
            require(requests[0].path == "/api/tags" && requests[1].path == "/api/show"
                && requests[1].body == nlohmann::json{{"model", "fake:1b"}}
                && requests[2].path == "/api/chat"
                && requests[2].body == nlohmann::json{{"model", "fake:1b"},
                    {"messages", nlohmann::json::array()}, {"stream", false}, {"keep_alive", "7m"}},
                "Enabling the script must preload the model without generating a reply.");
            for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
            require(output->Text() == initialText && probe.errors == 0 && probe.replies == 0,
                "Preloading must not touch the display or publish events.");

            // 読み込みの間の1件目は預かり、2件目はBusyです。
            sendText(scene, "一");
            require(output->Text() == "考え中..." && probe.errors == 0 && server.Count() == 3,
                "A message sent during the preload must wait for it.");
            sendText(scene, "二");
            require(probe.errors == 1 && probe.errorName == "Busy" && output->Text() == "考え中...",
                "Only one message may wait for the preload.");
            server.holdPreload = false;
            waitForReplies(1);
            requests = server.Requests();
            require(output->Text() == "返答1" && probe.reply == "返答1" && probe.errors == 1
                && requests.size() == 6 && requests[5].path == "/api/chat"
                && contents(requests[5]) == Texts{ "一" }
                && requests[5].body.at("keep_alive") == "7m"
                && requests[5].body.at("stream") == false,
                "The waiting message was not sent after the preload.");

            // 成功した往復は次の要求へ入り、リセットで消えます。
            sendText(scene, "三");
            waitForReplies(2);
            require(contents(server.Requests().back()) == Texts{ "一", "返答1", "三" },
                "The history was not sent with the next message.");
            scene.Events().Publish("Ollama.Reset");
            sendText(scene, "四");
            waitForReplies(3);
            require(contents(server.Requests().back()) == Texts{ "四" },
                "A reset must start a new conversation.");

            // 返答待ちの間にリセットした往復は表示しますが、新しい会話へは持ち込みません。
            server.holdChat = true;
            const auto before = server.Count();
            sendText(scene, "五");
            waitUntil([&] { return server.Count() == before + 3; }, "The held message was not sent.");
            scene.Events().Publish("Ollama.Reset");
            sendText(scene, "六");
            require(probe.errors == 2 && probe.errorName == "Busy" && output->Text() == "考え中...",
                "A message sent while waiting for a reply must be reported as busy.");
            server.holdChat = false;
            waitForReplies(4);
            require(output->Text() == "返答4" && probe.reply == "返答4",
                "A reply that was already on its way must still be shown after a reset.");
            sendText(scene, "七");
            waitForReplies(5);
            require(contents(server.Requests().back()) == Texts{ "七" } && probe.errors == 2,
                "An exchange that straddled a reset leaked into the new conversation.");
        }

        // 事前読み込みをオフにすると、送るまで何も要求しません。
        {
            const auto before = server.Count();
            Scene scene(graphics);
            scene.LoadFromJson(demoWith("profiles/Fake.asset.json", false));
            OllamaTestDetail::ChatProbe probe(scene);
            for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(server.Count() == before, "A disabled preload still sent a request.");
            sendText(scene, "八");
            waitUntil([&] { scene.Update(0.01f); return probe.replies == 1; },
                "A message without a preload was not answered.");
            const auto requests = server.Requests();
            require(requests.size() == before + 3 && contents(requests.back()) == Texts{ "八" }
                && probe.errors == 0,
                "A message without a preload must send exactly one chat request.");
        }

        // 事前読み込みの失敗は知らせず、送ったときに失敗として知らせます。
        {
            const auto before = server.Count();
            Scene scene(graphics);
            scene.LoadFromJson(demoWith("profiles/FakeMissing.asset.json", true));
            OllamaTestDetail::ChatProbe probe(scene);
            scene.Update(0.01f);
            const auto* output = scene.FindGameObjectByName("Ollama Reply")
                ->GetComponent<TextRendererComponent>();
            const auto initialText = output->Text();
            // モデル名を指定しているので一覧は要求せず、詳細の確認で見つからないと分かります。
            waitUntil([&] { return server.Count() == before + 1; }, "The preload check was not sent.");
            for (int frame = 0; frame < 20; ++frame)
            {
                scene.Update(0.01f);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(probe.errors == 0 && probe.replies == 0 && output->Text() == initialText,
                "A failed preload must stay silent.");
            sendText(scene, "九");
            waitUntil([&] { scene.Update(0.01f); return probe.errors > 0; },
                "A missing model was not reported.");
            for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
            require(probe.errors == 1 && probe.errorName == "ModelMissing" && probe.replies == 0
                && output->Text() == "FALLBACK",
                "A missing model must be reported once, when a message is sent.");
        }
    }
    graphics.Assets().SetAssetRoot(sourceRoot, false);
}
