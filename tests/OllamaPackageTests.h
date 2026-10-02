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
    // Ollama互換HTTPサーバーとして127.0.0.1で要求を逐次処理し、要求一覧をロック保護、返答数をワーカーだけで更新します。
    class FakeOllama final
    {
    public:
        struct Received final
        {
            // 要求パス
            std::string path;
            // 受信JSON本文
            nlohmann::json body;
        };

        // trueで事前読み込みを保留します。
        std::atomic<bool> holdPreload{};
        // trueで会話応答を保留します。
        std::atomic<bool> holdChat{};

        // FakeOllama() テスト用HTTPサーバーを起動します。
        FakeOllama()
        {
            // Winsock起動情報
            WSADATA data{};
            // Winsockの初期化可否を調べます。
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                // Winsock初期化失敗を通知します。
                throw std::runtime_error("WSAStartup failed.");
            }
            m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            // 127.0.0.1待受先
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            // ソケットアドレス長
            int length = sizeof(address);
            // 待受ソケット設定の成否を調べます。
            if (m_listener == INVALID_SOCKET
                || bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
                || listen(m_listener, SOMAXCONN) != 0
                || getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            {
                // 有効なソケットだけを閉じます。
                if (m_listener != INVALID_SOCKET) closesocket(m_listener);
                WSACleanup();
                // 待受サーバーの起動失敗を通知します。
                throw std::runtime_error("The fake Ollama server could not listen.");
            }
            m_port = ntohs(address.sin_port);
            // 停止要求を受けて要求処理スレッドを終えます(stop: 停止要求)。
            m_thread = std::jthread([this](const std::stop_token stop) { Serve(stop); });
        }

        // FakeOllama(const FakeOllama&) コピー生成を禁止します。
        FakeOllama(const FakeOllama&) = delete;
        // operator= コピー代入を禁止します。
        FakeOllama& operator=(const FakeOllama&) = delete;

        // ~FakeOllama() 待受スレッドとWinsockを終了します。
        ~FakeOllama()
        {
            m_thread.request_stop();
            // 待ち受けを閉じると、acceptが失敗して戻ります。
            closesocket(m_listener);
            m_thread.join();
            WSACleanup();
        }

        // Port() 待受ポート番号を返します。
        [[nodiscard]] std::uint16_t Port() const noexcept { return m_port; }

        // Requests() 記録済み要求のコピーを返します。
        [[nodiscard]] std::vector<Received> Requests() const
        {
            // 要求一覧の排他ロック
            const std::scoped_lock lock(m_mutex);
            // ロック解放後に要求一覧を返します。
            return m_requests;
        }

        // Count() 記録した要求数を返します。
        [[nodiscard]] std::size_t Count() const
        {
            // 要求一覧の排他ロック
            const std::scoped_lock lock(m_mutex);
            // ロック解放後に要求数を返します。
            return m_requests.size();
        }

    private:
        // Serve(stop: 停止要求) 接続を順に処理します。
        void Serve(const std::stop_token stop)
        {
            // 停止要求がない間は接続を待ちます。
            while (!stop.stop_requested())
            {
                // 受け付けた接続
                const SOCKET client = accept(m_listener, nullptr, nullptr);
                // 接続受付失敗で待受を終了します。
                if (client == INVALID_SOCKET)
                {
                    // 受付ソケットが閉じたため終了します。
                    return;
                }
                Answer(client, stop);
                closesocket(client);
            }
        }

        // Answer(client: 接続ソケット, stop: 停止要求) 要求に応答します。
        void Answer(const SOCKET client, const std::stop_token stop)
        {
            // 受信要求バッファ
            std::string request;
            // HTTPヘッダー終端
            auto headerEnd = std::string::npos;
            // 本文の宣言サイズ
            std::size_t contentLength{};
            // ヘッダーと本文を受信し終えるまで待ちます。
            while (headerEnd == std::string::npos || request.size() < headerEnd + 4 + contentLength)
            {
                // 受信バッファ
                char buffer[4096];
                // 今回の受信バイト数
                const int received = recv(client, buffer, sizeof(buffer), 0);
                // 切断または受信失敗を調べます。
                if (received <= 0)
                {
                    // 不完全な要求を破棄します。
                    return;
                }
                request.append(buffer, static_cast<std::size_t>(received));
                // ヘッダー受信後は本文長を待ちます。
                if (headerEnd != std::string::npos)
                {
                    // ヘッダー境界がなければ受信を続けます。
                    continue;
                }
                headerEnd = request.find("\r\n\r\n");
                // ヘッダー境界を探します。
                if (headerEnd == std::string::npos)
                {
                    // 境界未受信なら読み込みを続けます。
                    continue;
                }
                // HTTPヘッダー文字列
                auto headers = request.substr(0, headerEnd);
                // 受信ヘッダーを小文字化します(character: 変換対象文字)。
                std::ranges::transform(headers, headers.begin(), [](const unsigned char character)
                {
                    // 変換後の文字を返します。
                    return static_cast<char>(std::tolower(character));
                });
                // 本文長ヘッダー名
                constexpr std::string_view name = "content-length:";
                // Content-Lengthの位置を調べます(position: ヘッダー内の位置)。
                if (const auto position = headers.find(name); position != std::string::npos)
                {
                    contentLength = std::stoul(headers.substr(position + name.size()));
                }
            }

            // パスの開始位置
            const auto pathStart = request.find(' ') + 1;
            // 解析済み要求
            Received received;
            received.path = request.substr(pathStart, request.find(' ', pathStart) - pathStart);
            received.body = nlohmann::json::parse(request.substr(headerEnd + 4), nullptr, false);
            // 要求モデル名
            const auto model = received.body.is_object()
                ? received.body.value("model", std::string{}) : std::string{};
            // チャット要求の判定
            const bool chat = received.path == "/api/chat" && received.body.is_object()
                && received.body.contains("messages");
            // 事前読み込みの判定
            const bool preload = chat && received.body.at("messages").empty();
            {
                // 要求一覧の排他ロック
                const std::scoped_lock lock(m_mutex);
                m_requests.push_back(received);
            }

            // HTTP応答ステータス
            int status = 200;
            // 応答JSON本文
            nlohmann::json reply = nlohmann::json::object();
            // モデル一覧の要求へ応答します。
            if (received.path == "/api/tags")
            {
                reply["models"] = nlohmann::json::array({ nlohmann::json{{"name", "fake:1b"}} });
            }
            // 無効なモデル要求を拒否します。
            else if (model == "missing:1b" || (received.path != "/api/show" && !chat))
            {
                status = 404;
                reply["error"] = "model '" + model + "' not found";
            }
            // モデル詳細の要求へ応答します。
            else if (received.path == "/api/show")
            {
                reply["details"] = nlohmann::json{{"family", "fake"}};
            }
            // チャット要求へ応答します。
            else
            {
                // 応答を保留するフラグ
                const auto& hold = preload ? holdPreload : holdChat;
                // テスト指定の間は応答を保留します。
                while (hold && !stop.stop_requested())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                reply["model"] = model;
                reply["done"] = true;
                reply["message"] = nlohmann::json{{"role", "assistant"},
                    {"content", preload ? std::string{} : "返答" + std::to_string(++m_replies)}};
                // 事前読み込みの完了理由を付けます。
                if (preload)
                {
                    reply["done_reason"] = "load";
                }
            }

            // 送信するJSON本文
            const auto body = reply.dump();
            // HTTP応答メッセージ
            const auto response = "HTTP/1.1 " + std::to_string(status)
                + (status == 200 ? " OK" : " Not Found")
                + "\r\nContent-Type: application/json\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            // 送信済みバイト数
            std::size_t sent{};
            // 応答を全て送るまで送信します。
            while (sent < response.size())
            {
                // 今回の送信バイト数
                const int written = send(client, response.data() + sent,
                    static_cast<int>(response.size() - sent), 0);
                // 送信失敗を調べます。
                if (written <= 0)
                {
                    // 応答送信を中断します。
                    return;
                }
                sent += static_cast<std::size_t>(written);
            }
            shutdown(client, SD_SEND);
        }

        // 待ち受けソケット
        SOCKET m_listener{ INVALID_SOCKET };
        // 割り当て済みポート
        std::uint16_t m_port{};
        // 要求一覧ロック
        mutable std::mutex m_mutex;
        // 記録済み要求
        std::vector<Received> m_requests;
        // 生成した返答数
        int m_replies{};
        // 要求処理スレッド
        std::jthread m_thread;
    };

    // Scriptが発行する返答と失敗を数えます。
    struct ChatProbe final
    {
        // 返答イベント数
        int replies{};
        // エラーイベント数
        int errors{};
        // 最後の返答文
        std::string reply;
        // 最後のエラー名
        std::string errorName;
        // 最後のエラー番号
        float errorNumber{};

        // ChatProbe(scene: 監視するScene) 応答イベントを記録します。
        explicit ChatProbe(LamaPon::Scene& scene)
        {
            // 返答イベントを記録します(args: 受信したイベント)。
            scene.Events().Subscribe("Ollama.Reply", [this](const LamaPon::EventArgs& args)
            {
                ++replies;
                reply = args.text;
            });
            // エラーイベントを記録します(args: 受信したイベント)。
            scene.Events().Subscribe("Ollama.Error", [this](const LamaPon::EventArgs& args)
            {
                ++errors;
                errorName = args.text;
                errorNumber = args.number;
            });
        }
    };
}

// Ollamaパッケージを検証します(graphics: 描画機器, sourceRoot: 同梱アセット, outputRoot: 検証出力先)。
inline void TestOllamaPackage(LamaPon::GraphicsDevice& graphics,
    const std::filesystem::path& sourceRoot, const std::filesystem::path& outputRoot)
{
    using namespace LamaPon;
    using namespace LamaPonOllama;
    // require(result: 成否, message: 失敗理由)でテスト条件を検証します。
    const auto require = [](const bool result, const char* message)
    {
        // テスト条件の失敗を検出します。
        if (!result) throw std::runtime_error(message);
    };

    // 許可する接続先URL
    for (const char* url : {
        "http://127.0.0.1:11434/api/tags", "http://127.0.0.1:11434/api/show",
        "http://127.0.0.1:11434/api/chat", "http://localhost:11434/api/chat",
        "http://[::1]:11434/api/chat", "http://127.0.0.1:1/api/chat",
        "http://127.0.0.1:65535/api/chat"})
    {
        // 許可URLの誤拒否を検出します。
        if (!IsAllowedEndpoint(url))
            // 不許可の接続先をテスト失敗にします。
            throw std::runtime_error(std::string("A loopback Ollama endpoint was rejected: ") + url);
    }
    // 禁止URL候補
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
        // 禁止URL候補
        if (IsAllowedEndpoint(url))
            // 禁止URLが許可された場合は失敗にします。
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
        // ローカルモデル名を検証
        if (!IsLocalModelName(model))
            // 無効なモデル名をテスト失敗にします。
            throw std::runtime_error(std::string("A local model name was rejected: ") + model);
    }
    // 禁止モデル名候補
    for (const char* model : {
        "", "gemma4:cloud", "GEMMA4:CLOUD", "gpt-oss:120b-cloud", "qwen3-coder:480b-cloud",
        "xxx-cloud", "xxx-cloud:latest", "cloud/model", "my_cloud_model", "my model", "model\n",
        "model?x=1", "モデル"})
    {
        // 禁止名の誤許可を検出します。
        if (IsLocalModelName(model))
            // 禁止名が許可された場合は失敗にします。
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
    // 読み込んだ設定
    ModelProfile profile;
    // 設定読込エラー
    std::string error;
    // 同梱設定数
    std::size_t profiles{};
    // 同梱設定アセット
    for (const auto& entry : std::filesystem::directory_iterator(sourceRoot / "profiles"))
    {
        // 読み込み対象アセット
        const auto asset = graphics.Assets().LoadDataAsset(
            std::filesystem::relative(entry.path(), sourceRoot));
        require(ReadProfile(*asset, profile, error), "A shipped Ollama profile is invalid.");
        require(profile.port == DefaultPort && profile.maxTokens <= 256
            && !profile.fallbackReply.empty(),
            "Shipped Ollama profiles must use the default port and short replies.");
        ++profiles;
    }
    require(profiles == 1, "One Ollama profile must ship in the package.");
    // 設定項目定義
    const auto schema = nlohmann::json::parse(ProfileSchema);
    // 設定の既定値
    nlohmann::json defaults = nlohmann::json::object();
    // 設定項目定義
    for (const auto& field : schema.at("fields"))
        defaults[field.at("name").get<std::string>()] = field.at("default");
    // ホストやAPIキーの欄が増えたら、ここで気付けるように項目名を固定します。
    require(defaults.size() == 8 && defaults.contains("model") && defaults.contains("systemPrompt")
        && defaults.contains("temperature") && defaults.contains("maxTokens")
        && defaults.contains("port") && defaults.contains("fallbackReply")
        && defaults.contains("historyLimit") && defaults.contains("keepAliveMinutes"),
        "The Ollama profile schema must not gain host or key fields.");
    // コード既定設定
    const ModelProfile expected;
    require(ReadProfile(DataAsset::FromJson(nlohmann::json{{"type",ProfileType},{"values",defaults}}.dump()),
        profile, error) && profile.model.empty() && profile.systemPrompt == expected.systemPrompt
        && profile.temperature == expected.temperature && profile.maxTokens == expected.maxTokens
        && profile.port == DefaultPort && profile.fallbackReply == expected.fallbackReply
        && profile.historyLimit == expected.historyLimit
        && profile.keepAliveMinutes == expected.keepAliveMinutes && expected.keepAliveMinutes == 10,
        "New Ollama asset defaults differ from the code defaults.");
    // 不正設定JSON
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

    // 会話設定
    ModelProfile chatProfile;
    chatProfile.systemPrompt = "system";
    chatProfile.temperature = 0.25;
    chatProfile.maxTokens = 64;
    chatProfile.keepAliveMinutes = 7;
    // 会話履歴
    const std::vector<ChatMessage> history{ { "user", "前の質問" }, { "assistant", "前の返答" } };
    // message(role: 発言者, content: 本文)をJSON化します。
    const auto message = [](const char* role, const char* content)
    {
        // JSONメッセージを返します。
        return nlohmann::json{{"role", role}, {"content", content}};
    };
    // 認証項目なしの非ストリーミング要求JSON
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
    // 事前読み込み要求は空のmessagesで応答生成を抑止します。
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
    // 解析したチャット応答
    const auto reply = ParseChatResponse(200, R"({"model":"gemma3:4b","message":
        {"role":"assistant","content":"\n こんにちは！ \n"},"done":true,"done_reason":"stop"})");
    require(reply.Succeeded() && reply.reply == "こんにちは！", "A local chat reply was not read.");
    // 解析したリモート応答
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

    // 最新の会話履歴を保持します。
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

    // waitUntil(condition: 完了条件, message: 期限切れ理由)を待ちます。
    const auto waitUntil = [&require](const auto& condition, const char* message)
    {
        // 待機期限
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        // 完了条件が満たされるまで待ちます。
        while (!condition())
        {
            require(std::chrono::steady_clock::now() < deadline, message);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // 通信スレッドは同時に1件で、破棄するときに終了を待ちます。
    {
        // テスト対象ワーカー
        ChatWorker worker;
        // 仕事を解放するフラグ
        std::atomic<bool> release{};
        // blocked(stop: 停止要求)で解放待ちの結果を作ります。
        const auto blocked = [&release](const std::stop_token stop)
        {
            // 解放か停止要求まで仕事を保留します。
            while (!release && !stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // ワーカー応答
            ChatResult result;
            result.reply = "done";
            // 完了した仕事の結果を返します。
            return result;
        };
        require(!worker.Busy() && !worker.Ready() && !worker.Take(), "A new worker must be idle.");
        require(worker.Start(blocked) && worker.Busy() && !worker.Ready() && !worker.Take(),
            "The worker did not start.");
        require(!worker.Start(blocked), "A second request was started while one was running.");
        release = true;
        // Worker完了状態を待ちます。
        waitUntil([&worker] { return worker.Ready(); }, "The worker result timed out.");
        require(worker.Busy() && !worker.Start(blocked),
            "An untaken result must block the next request.");
        // 受け取った応答
        const auto result = worker.Take();
        require(result && result->reply == "done" && !worker.Busy() && !worker.Take(),
            "The worker result was not delivered exactly once.");

        release = false;
        require(worker.Start(blocked), "The worker could not be reused.");
        worker.Cancel();
        // キャンセル後の終了を待ちます。
        waitUntil([&worker] { return !worker.Busy(); }, "A cancelled request did not stop.");
        require(!worker.Ready() && !worker.Take(), "A cancelled result must be discarded.");
        // 例外を失敗結果に変換します(stop: 停止要求)。
        require(worker.Start([](const std::stop_token) -> ChatResult { throw std::runtime_error("boom"); }),
            "The worker could not restart after a cancel.");
        // 例外を返す仕事の完了を待ちます。
        waitUntil([&worker] { return worker.Ready(); }, "A throwing job did not finish.");
        // 失敗した仕事の結果
        const auto failed = worker.Take();
        require(failed && failed->error == ChatError::Failed && failed->detail == "boom",
            "A throwing job must become a failed result.");

        // スレッド終了フラグ
        std::atomic<bool> finished{};
        {
            // スコープ内ワーカー
            ChatWorker scoped;
            // 破棄時に終了を待つ仕事を実行します(stop: 停止要求)。
            require(scoped.Start([&finished](const std::stop_token stop)
            {
                // 停止要求まで終了処理を待ちます。
                while (!stop.stop_requested())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                finished = true;
                // 結果のない応答を返します。
                return ChatResult{};
            }), "The scoped worker did not start.");
        }
        require(finished, "Destroying a worker must wait for its thread.");
    }

    // 検証用アセットルート
    auto testAssets = outputRoot / "ollama-test-assets";
    std::filesystem::remove_all(testAssets);
    std::filesystem::create_directories(testAssets / "profiles");
    // removeTestAssets: テスト用アセットを破棄時に削除します。
    const struct RemoveTestAssets final
    {
        // 削除対象ルート
        std::filesystem::path root;
        // ~RemoveTestAssets() 一時アセットを削除します。
        ~RemoveTestAssets()
        {
            // 後始末エラー
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } removeTestAssets{ testAssets };
    // writeProfile(name: 設定名, values: 設定JSON)を書き出します。
    const auto writeProfile = [&testAssets](const char* name, const std::string& values)
    {
        // 設定ファイル出力先
        std::ofstream output(testAssets / "profiles" / name, std::ios::binary);
        output << "{\"type\":\"Ollama.ModelProfile\",\"values\":" << values << "}";
    };
    writeProfile("ClosedPort.asset.json",
        R"({"model":"","port":1,"fallbackReply":"FALLBACK","historyLimit":4})");
    writeProfile("CloudModel.asset.json", R"({"model":"gemma4:cloud","port":1})");

    // 配布Scene入力
    std::ifstream sceneInput(sourceRoot / "scenes" / "OllamaChatDemo.scene.json");
    // 読み込んだScene JSON
    nlohmann::json demo;
    sceneInput >> demo;
    // 対象Script数
    std::size_t scripts{};
    // 配布Sceneオブジェクト
    for (const auto& object : demo.at("objects"))
    {
        // Sceneコンポーネント
        for (const auto& component : object.at("components"))
        {
            // NativeScript以外を読み飛ばします。
            if (component.at("type") != "NativeScript") continue;
            // Script設定群
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

    // demoWith(profilePath: 設定パス, preload: 読込指定)を反映したScene JSONを返します。
    const auto demoWith = [&demo](const char* profilePath, const bool preload)
    {
        // 編集用Scene複製
        auto copy = demo;
        // 編集対象オブジェクト
        for (auto& object : copy.at("objects"))
        {
            // NativeScript設定
            for (auto& component : object.at("components"))
            {
                // NativeScript以外を読み飛ばします。
                if (component.at("type") != "NativeScript") continue;
                component["properties"]["profile"] = profilePath;
                component["properties"]["preload"] = preload;
            }
        }
        // 編集したScene JSONを返します。
        return copy.dump();
    };

    graphics.Assets().SetAssetRoot(testAssets, false);
    {
        // Script動作確認Scene
        Scene scene(graphics);
        scene.LoadFromJson(demoWith("profiles/ClosedPort.asset.json", true));
        scene.Update(0.01f);
        // チャットGameObject
        auto* chat = scene.FindGameObjectByName("Ollama Chat");
        // 入力GameObject
        auto* inputObject = scene.FindGameObjectByName("Ollama Input");
        // 返答GameObject
        const auto* replyObject = scene.FindGameObjectByName("Ollama Reply");
        // 送信GameObject
        const auto* sendObject = scene.FindGameObjectByName("Ollama Send");
        require(chat && inputObject && replyObject && sendObject, "Demo objects were not loaded.");
        // 入力欄UI
        auto* input = inputObject->GetComponent<UIInputFieldComponent>();
        // 返答表示UI
        const auto* output = replyObject->GetComponent<TextRendererComponent>();
        // 送信ボタンUI
        const auto* send = sendObject->GetComponent<UIButtonComponent>();
        // Scriptは、送信とリセットの2つのイベントを待ち受けます。
        require(input && output && send && send->ClickEventName() == "Ollama.Send"
            && scene.Events().SubscriptionCount() == 2,
            "Demo UI is not wired to the chat script.");

        // 返答イベント数
        int replies{};
        // エラーイベント数
        int errors{};
        // 最後のエラー名
        std::string errorName;
        // 最後のエラー番号
        float errorNumber{};
        // onReply(args: 受信イベント)で返答数を記録します。
        const auto onReply = [&replies](const EventArgs&) { ++replies; };
        // 返答購読ハンドル
        const auto replyHandle = scene.Events().Subscribe("Ollama.Reply", onReply);
        // onError(args: 受信イベント)で失敗情報を記録します。
        const auto onError = [&errors, &errorName, &errorNumber](const EventArgs& args)
        {
            ++errors;
            errorName = args.text;
            errorNumber = args.number;
        };
        // エラー購読ハンドル
        const auto errorHandle = scene.Events().Subscribe("Ollama.Error", onError);

        // 空の入力は送らず、ボタンは入力欄の文を送ります。
        scene.Events().Publish(send->ClickEventName());
        require(output->Text() != "考え中...", "An empty message was sent.");
        input->SetText("こんにちは");
        scene.Events().Publish(send->ClickEventName());
        require(input->Text().empty() && output->Text() == "考え中..." && errors == 0,
            "The send button did not start a request from the input field.");
        // 二重送信イベント
        EventArgs second;
        second.text = "二重送信";
        // 返答待ちの送信はBusyで拒否し、表示を保ちます。
        scene.Events().Publish("Ollama.Send", second);
        require(errors == 1 && errorName == "Busy"
            && errorNumber == static_cast<float>(ChatError::Busy)
            && output->Text() == "考え中...",
            "A second message must be reported as busy without touching the display.");
        // Scene更新中に通信失敗を待ちます。
        waitUntil([&scene, &errors] { scene.Update(0.01f); return errors > 1; },
            "A request to a closed port did not fail in time.");
        // 送信失敗後の更新回数
        for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
        // 事前読み込みの失敗は知らせないので、届く失敗は送った1件の分だけです。
        require(errors == 2 && replies == 0 && errorName == "NotRunning"
            && errorNumber == static_cast<float>(ChatError::NotRunning)
            && output->Text() == "FALLBACK",
            "A stopped Ollama must show the fallback reply and publish one error.");

        // Script破棄後の動作確認
        EventArgs third;
        third.text = "破棄";
        scene.Events().Publish("Ollama.Send", third);
        require(output->Text() == "考え中...", "The event text did not start a request.");
        scene.DestroyGameObject(*chat);
        // Script破棄後の更新回数
        for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
        require(errors == 2 && replies == 0 && scene.Events().SubscriptionCount() == 2,
            "A destroyed chat script kept a subscription or delivered a result.");

        // クラウド設定を通信前に拒否します。
        auto& cloud = scene.CreateGameObject("Cloud Chat");
        cloud.AddComponent<NativeScriptComponent>("Ollama.Chat",
            R"({"profile":"profiles/CloudModel.asset.json","inputObject":"","outputObject":"Ollama Reply"})");
        scene.Update(0.01f);
        scene.Events().Publish("Ollama.Send", third);
        require(errors == 3 && errorName == "CloudRejected"
            && output->Text() == ModelProfile{}.fallbackReply,
            "A cloud model profile must be rejected before any request.");
        scene.DestroyGameObject(cloud);

        // 欠落設定のScriptを検証します。
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
        // 偽Ollamaサーバー
        OllamaTestDetail::FakeOllama server;
        // 偽サーバーのポート
        const auto port = std::to_string(server.Port());
        writeProfile("Fake.asset.json", R"({"model":"","systemPrompt":"","fallbackReply":"FALLBACK",)"
            R"("historyLimit":4,"keepAliveMinutes":7,"port":)" + port + "}");
        writeProfile("FakeMissing.asset.json",
            R"({"model":"missing:1b","fallbackReply":"FALLBACK","port":)" + port + "}");
        // sendText(scene: 対象Scene, text: 送信文)を送信します。
        const auto sendText = [](Scene& scene, const char* text)
        {
            // 送信イベント引数
            EventArgs args;
            args.text = text;
            scene.Events().Publish("Ollama.Send", args);
        };
        // contents(received: 記録要求)から会話文を抽出します。
        const auto contents = [](const OllamaTestDetail::FakeOllama::Received& received)
        {
            // 抽出した会話文
            std::vector<std::string> result;
            // 会話メッセージ
            for (const auto& entry : received.body.at("messages"))
            {
                result.push_back(entry.at("content").get<std::string>());
            }
            // 取り出した会話文を返します。
            return result;
        };
        using Texts = std::vector<std::string>;

        // 読込中の送信、履歴、リセットを確認します。
        {
            server.holdPreload = true;
            // 事前読込テストScene
            Scene scene(graphics);
            scene.LoadFromJson(demoWith("profiles/Fake.asset.json", true));
            // 応答監視プローブ
            OllamaTestDetail::ChatProbe probe(scene);
            scene.Update(0.01f);
            // 返答表示UI
            const auto* output = scene.FindGameObjectByName("Ollama Reply")
                ->GetComponent<TextRendererComponent>();
            // 読み込み前の表示
            const auto initialText = output->Text();
            // waitForReplies(count: 必要な返答数)を待ちます。
            const auto waitForReplies = [&](const int count)
            {
                // Sceneを更新しながら返答数を待ちます。
                waitUntil([&] { scene.Update(0.01f); return probe.replies == count; },
                    "A reply from the fake Ollama did not arrive in time.");
            };

            // 有効になった時点で、一覧・詳細・読み込みの順に要求します。
            waitUntil([&server] { return server.Count() == 3; }, "The preload request was not sent.");
            // 受信済み要求一覧
            auto requests = server.Requests();
            require(requests[0].path == "/api/tags" && requests[1].path == "/api/show"
                && requests[1].body == nlohmann::json{{"model", "fake:1b"}}
                && requests[2].path == "/api/chat"
                && requests[2].body == nlohmann::json{{"model", "fake:1b"},
                    {"messages", nlohmann::json::array()}, {"stream", false}, {"keep_alive", "7m"}},
                "Enabling the script must preload the model without generating a reply.");
            // 読込無効の確認回数
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
            // 試験開始時の要求数
            const auto before = server.Count();
            sendText(scene, "五");
            // チャット要求3件の到着を待ちます。
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
            // 試験開始時の要求数
            const auto before = server.Count();
            // 読込無効テストScene
            Scene scene(graphics);
            scene.LoadFromJson(demoWith("profiles/Fake.asset.json", false));
            // 応答監視プローブ
            OllamaTestDetail::ChatProbe probe(scene);
            // 読込無効の確認回数
            for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(server.Count() == before, "A disabled preload still sent a request.");
            sendText(scene, "八");
            // Scene更新中に返答を待ちます。
            waitUntil([&] { scene.Update(0.01f); return probe.replies == 1; },
                "A message without a preload was not answered.");
            // 受信済み要求一覧
            const auto requests = server.Requests();
            require(requests.size() == before + 3 && contents(requests.back()) == Texts{ "八" }
                && probe.errors == 0,
                "A message without a preload must send exactly one chat request.");
        }

        // 事前読み込みの失敗は知らせず、送ったときに失敗として知らせます。
        {
            // 試験開始時の要求数
            const auto before = server.Count();
            // 欠落モデルテストScene
            Scene scene(graphics);
            scene.LoadFromJson(demoWith("profiles/FakeMissing.asset.json", true));
            // 応答監視プローブ
            OllamaTestDetail::ChatProbe probe(scene);
            scene.Update(0.01f);
            // 返答表示UI
            const auto* output = scene.FindGameObjectByName("Ollama Reply")
                ->GetComponent<TextRendererComponent>();
            // 失敗前の表示
            const auto initialText = output->Text();
            // 指定モデルは一覧を省き、詳細APIで存在を確認します。
            waitUntil([&] { return server.Count() == before + 1; }, "The preload check was not sent.");
            // 失敗確認の更新回数
            for (int frame = 0; frame < 20; ++frame)
            {
                scene.Update(0.01f);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(probe.errors == 0 && probe.replies == 0 && output->Text() == initialText,
                "A failed preload must stay silent.");
            sendText(scene, "九");
            // Scene更新中にモデルエラーを待ちます。
            waitUntil([&] { scene.Update(0.01f); return probe.errors > 0; },
                "A missing model was not reported.");
            // 事前読込確認の更新回数
            for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
            require(probe.errors == 1 && probe.errorName == "ModelMissing" && probe.replies == 0
                && output->Text() == "FALLBACK",
                "A missing model must be reported once, when a message is sent.");
        }
    }
    graphics.Assets().SetAssetRoot(sourceRoot, false);
}
