#pragma once

#include "../packages/src/ollama-ai/OllamaWorker.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

// Ollamaが起動していなくても通る範囲だけを確かめます。通信を伴う経路は、
// 何も待ち受けていないポートへ送り、「起動していない」として扱われることを見ます。
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
    require(defaults.size() == 7 && defaults.contains("model") && defaults.contains("systemPrompt")
        && defaults.contains("temperature") && defaults.contains("maxTokens")
        && defaults.contains("port") && defaults.contains("fallbackReply")
        && defaults.contains("historyLimit"),
        "The Ollama profile schema must not gain host or key fields.");
    const ModelProfile expected;
    require(ReadProfile(DataAsset::FromJson(nlohmann::json{{"type",ProfileType},{"values",defaults}}.dump()),
        profile, error) && profile.model.empty() && profile.systemPrompt == expected.systemPrompt
        && profile.temperature == expected.temperature && profile.maxTokens == expected.maxTokens
        && profile.port == DefaultPort && profile.fallbackReply == expected.fallbackReply
        && profile.historyLimit == expected.historyLimit,
        "New Ollama asset defaults differ from the code defaults.");
    for (const auto& values : {
        R"({"model":"gemma4:cloud"})", R"({"model":"gpt-oss:120b-cloud"})", R"({"model":"my model"})",
        R"({"model":7})", R"({"port":0})", R"({"port":65536})", R"({"port":11434.5})",
        R"({"port":"11434"})", R"({"maxTokens":0})", R"({"maxTokens":4096})",
        R"({"temperature":-0.1})", R"({"temperature":2.5})", R"({"temperature":"hot"})",
        R"({"historyLimit":-1})", R"({"historyLimit":65})", R"({"systemPrompt":false})",
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
        "model":"gemma3:4b","port":12345,"maxTokens":64,"temperature":0,"historyLimit":0}})"),
        profile, error) && profile.model == "gemma3:4b" && profile.port == 12345
        && profile.maxTokens == 64 && profile.temperature == 0.0 && profile.historyLimit == 0
        && profile.systemPrompt == expected.systemPrompt,
        "A partial Ollama profile must keep defaults for omitted fields.");

    // リクエストは、返答を1回で受け取る短い会話だけです。キーや追加機能の項目は送りません。
    ModelProfile chatProfile;
    chatProfile.systemPrompt = "system";
    chatProfile.temperature = 0.25;
    chatProfile.maxTokens = 64;
    const std::vector<ChatMessage> history{ { "user", "前の質問" }, { "assistant", "前の返答" } };
    const auto message = [](const char* role, const char* content)
    {
        return nlohmann::json{{"role", role}, {"content", content}};
    };
    auto request = nlohmann::json::parse(BuildChatRequest(chatProfile, "gemma3:4b", history, "こんにちは"));
    require(request.size() == 5 && request.at("model") == "gemma3:4b"
        && request.at("stream") == false && request.at("think") == false
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
        && !std::string_view(ErrorMessage(ChatError::Timeout)).empty(),
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
    const auto writeProfile = [&testAssets](const char* name, const char* values)
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
    for (auto& object : demo.at("objects"))
    {
        for (auto& component : object.at("components"))
        {
            if (component.at("type") != "NativeScript") continue;
            require(component.at("script") == "Ollama.Chat"
                && component.at("properties").at("profile") == DefaultProfilePath,
                "The demo scene must use the shipped script and profile.");
            component["properties"]["profile"] = "profiles/ClosedPort.asset.json";
            ++scripts;
        }
    }
    require(scripts == 1, "The demo scene must contain one Ollama chat script.");

    graphics.Assets().SetAssetRoot(testAssets, false);
    {
        Scene scene(graphics);
        scene.LoadFromJson(demo.dump());
        scene.Update(0.01f);
        auto* chat = scene.FindGameObjectByName("Ollama Chat");
        auto* inputObject = scene.FindGameObjectByName("Ollama Input");
        const auto* replyObject = scene.FindGameObjectByName("Ollama Reply");
        const auto* sendObject = scene.FindGameObjectByName("Ollama Send");
        require(chat && inputObject && replyObject && sendObject, "Demo objects were not loaded.");
        auto* input = inputObject->GetComponent<UIInputFieldComponent>();
        const auto* output = replyObject->GetComponent<TextRendererComponent>();
        const auto* send = sendObject->GetComponent<UIButtonComponent>();
        require(input && output && send && send->ClickEventName() == "Ollama.Send"
            && scene.Events().SubscriptionCount() == 1,
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
        // 返答待ちの間の送信は無視します。
        EventArgs second;
        second.text = "二重送信";
        scene.Events().Publish("Ollama.Send", second);
        waitUntil([&scene, &errors] { scene.Update(0.01f); return errors > 0; },
            "A request to a closed port did not fail in time.");
        for (int frame = 0; frame < 5; ++frame) scene.Update(0.01f);
        require(errors == 1 && replies == 0 && errorName == "NotRunning"
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
        require(errors == 1 && replies == 0 && scene.Events().SubscriptionCount() == 2,
            "A destroyed chat script kept a subscription or delivered a result.");

        // クラウドのモデルを指定した設定は、通信せずにその場で拒否します。
        auto& cloud = scene.CreateGameObject("Cloud Chat");
        cloud.AddComponent<NativeScriptComponent>("Ollama.Chat",
            R"({"profile":"profiles/CloudModel.asset.json","inputObject":"","outputObject":"Ollama Reply"})");
        scene.Update(0.01f);
        scene.Events().Publish("Ollama.Send", third);
        require(errors == 2 && errorName == "CloudRejected"
            && output->Text() == ModelProfile{}.fallbackReply,
            "A cloud model profile must be rejected before any request.");
        scene.DestroyGameObject(cloud);

        auto& missing = scene.CreateGameObject("Missing Profile");
        missing.AddComponent<NativeScriptComponent>("Ollama.Chat",
            R"({"profile":"profiles/Missing.asset.json","inputObject":"","outputObject":""})");
        scene.Update(0.01f);
        scene.Events().Publish("Ollama.Send", third);
        require(errors == 3 && errorName == "Failed", "A missing profile must fail without a request.");
        scene.DestroyGameObject(missing);

        scene.Events().Unsubscribe(replyHandle);
        scene.Events().Unsubscribe(errorHandle);
        require(scene.Events().SubscriptionCount() == 0, "Ollama scripts retained event callbacks.");
    }
    graphics.Assets().SetAssetRoot(sourceRoot, false);
}
