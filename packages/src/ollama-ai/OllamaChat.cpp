// Windows専用です。通信に使うエンジンのHttpSend（WinHTTP）とスレッドが
// Web書き出しにはないので、Windows以外では中身のないファイルになります。
#if defined(_WIN32)

#include "LamaPon/LamaPon.h"
#include "OllamaWorker.h"

namespace
{
    constexpr char OllamaChatSchema[] = R"schema({"fields":[
        {"name":"profile","displayName":"Ollama設定アセット","type":"asset","assetType":"data",
         "dataType":"Ollama.ModelProfile","default":"packages/ollama-ai/profiles/Default.asset.json"},
        {"name":"inputObject","displayName":"入力欄のGameObject名","type":"string","default":"Ollama Input",
         "tooltip":"UIInputFieldを持つGameObjectです。空にすると、送信イベントだけを受け付けます。"},
        {"name":"outputObject","displayName":"返答を表示するGameObject名","type":"string","default":"Ollama Reply",
         "tooltip":"TextRendererを持つGameObjectです。空にすると表示せず、イベントだけを発行します。"},
        {"name":"waitingText","displayName":"返答待ちの表示","type":"string","default":"考え中..."},
        {"name":"sendEvent","displayName":"送信イベント","type":"string","default":"Ollama.Send",
         "tooltip":"EventArgs.textの文を送ります。空なら入力欄の文を送ります。"},
        {"name":"replyEvent","displayName":"返答イベント","type":"string","default":"Ollama.Reply",
         "tooltip":"EventArgs.textへ返答を入れて発行します。"},
        {"name":"errorEvent","displayName":"失敗イベント","type":"string","default":"Ollama.Error",
         "tooltip":"EventArgs.textへ失敗の種類（NotRunningなど）を入れて発行します。"}
    ]})schema";
}

class OllamaChat final : public LamaPon::Script
{
public:
    void OnEnable() override
    {
        m_profile = {};
        m_profileError.clear();
        m_profileIsCloud = false;
        const auto asset = LoadDataAsset(LamaPon::PathFromUtf8(m_profilePath));
        std::string error;
        if (!LamaPonOllama::ReadProfile(*asset, m_profile, error))
        {
            // 設定が不正でもゲームは止めず、送信のたびに失敗として知らせます。
            m_profileIsCloud = LamaPonOllama::HasCloudTag(asset->GetText("model"));
            m_profileError = "Ollamaの設定を読み込めません: " + error;
            Warn(m_profileError);
        }
        if (!m_sendEvent.empty())
        {
            m_subscription = On(m_sendEvent, [this](const LamaPon::EventArgs& args)
            {
                Send(args.text);
            });
        }
    }

    void OnDisable() override
    {
        if (m_subscription != 0)
        {
            Off(m_subscription);
            m_subscription = 0;
        }
        // 無効の間は返答を表示できないので、待つのをやめて結果を捨てます。
        // 進行中の通信は止められないため、終わるまで次の送信は受け付けません。
        StopCoroutine(m_waiting);
        m_waiting = 0;
        m_worker.Cancel();
    }

    void Update(float) override
    {
        auto* input = InputField();
        if (input != nullptr && input->ConsumeSubmit())
        {
            Send({});
        }
    }

    void LoadProperties(const std::string_view text) override
    {
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        if (!properties.is_object()) return;
        const auto values = LamaPon::DataAsset::FromJson(nlohmann::json{{"values", properties}}.dump());
        m_profilePath = values.GetText("profile", LamaPonOllama::DefaultProfilePath);
        m_inputObject = values.GetText("inputObject", "Ollama Input");
        m_outputObject = values.GetText("outputObject", "Ollama Reply");
        m_waitingText = values.GetText("waitingText", "考え中...");
        m_sendEvent = values.GetText("sendEvent", "Ollama.Send");
        m_replyEvent = values.GetText("replyEvent", "Ollama.Reply");
        m_errorEvent = values.GetText("errorEvent", "Ollama.Error");
    }

    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{{"profile", m_profilePath}, {"inputObject", m_inputObject},
            {"outputObject", m_outputObject}, {"waitingText", m_waitingText},
            {"sendEvent", m_sendEvent}, {"replyEvent", m_replyEvent},
            {"errorEvent", m_errorEvent}}.dump();
    }

private:
    // textが空なら入力欄の文を送ります。返答待ちの間の送信は無視します
    // （同時に送るのは1件までです）。
    void Send(std::string text)
    {
        auto* input = InputField();
        const bool fromInput = text.empty() && input != nullptr;
        if (fromInput)
        {
            text = input->Text();
        }
        text = LamaPonOllama::Trimmed(text);
        if (text.empty() || m_worker.Busy())
        {
            return;
        }
        if (!m_profileError.empty())
        {
            Fail(m_profileIsCloud ? LamaPonOllama::ChatError::CloudRejected
                : LamaPonOllama::ChatError::Failed, m_profileError);
            return;
        }
        if (fromInput)
        {
            input->SetText({});
        }
        Show(m_waitingText);
        // 別スレッドへは設定・履歴・文のコピーを渡します。通信中にScriptが
        // 破棄されても、スレッドがScriptのメンバーを読むことはありません。
        const bool started = m_worker.Start(
            [profile = m_profile, history = m_history, text](const std::stop_token stop)
            {
                return LamaPonOllama::Chat(profile, history, text, stop);
            });
        if (started)
        {
            m_waiting = StartCoroutine(WaitForReply(std::move(text)));
        }
    }

    // 結果が届くまで毎フレーム確認し、届いたらゲームのスレッドで表示します。
    LamaPon::Coroutine WaitForReply(std::string userText)
    {
        co_await LamaPon::WaitUntil{ [this] { return m_worker.Ready(); } };
        m_waiting = 0;
        auto result = m_worker.Take();
        if (!result)
        {
            co_return;
        }
        if (!result->Succeeded())
        {
            Fail(result->error, result->detail);
            co_return;
        }
        // 失敗した往復は履歴へ入れません。次の送信でモデルを混乱させないためです。
        LamaPonOllama::AppendExchange(m_history, std::move(userText), result->reply,
            m_profile.historyLimit);
        Show(result->reply);
        if (!m_replyEvent.empty())
        {
            LamaPon::EventArgs args;
            args.text = std::move(result->reply);
            Emit(m_replyEvent, std::move(args));
        }
    }

    void Fail(const LamaPonOllama::ChatError error, const std::string& detail)
    {
        Warn(std::string(LamaPonOllama::ErrorMessage(error))
            + (detail.empty() ? std::string{} : "（" + detail + "）"));
        Show(m_profile.fallbackReply);
        if (!m_errorEvent.empty())
        {
            LamaPon::EventArgs args;
            args.number = static_cast<float>(error);
            args.text = LamaPonOllama::ErrorName(error);
            Emit(m_errorEvent, std::move(args));
        }
    }

    void Show(const std::string& text)
    {
        auto* object = m_outputObject.empty() ? nullptr : Find(m_outputObject);
        auto* renderer = object != nullptr
            ? object->GetComponent<LamaPon::TextRendererComponent>() : nullptr;
        if (renderer != nullptr && !text.empty())
        {
            renderer->SetText(text);
        }
    }

    [[nodiscard]] LamaPon::UIInputFieldComponent* InputField() const
    {
        auto* object = m_inputObject.empty() ? nullptr : Find(m_inputObject);
        return object != nullptr
            ? object->GetComponent<LamaPon::UIInputFieldComponent>() : nullptr;
    }

    static void Warn(const std::string& text) { LamaPon::Logger::Instance().Warning(text); }

    LamaPonOllama::ModelProfile m_profile;
    std::string m_profileError;
    bool m_profileIsCloud{};
    std::vector<LamaPonOllama::ChatMessage> m_history;
    std::uint64_t m_subscription{};
    std::uint64_t m_waiting{};
    std::string m_profilePath{ LamaPonOllama::DefaultProfilePath };
    std::string m_inputObject{ "Ollama Input" };
    std::string m_outputObject{ "Ollama Reply" };
    std::string m_waitingText{ "考え中..." };
    std::string m_sendEvent{ "Ollama.Send" };
    std::string m_replyEvent{ "Ollama.Reply" };
    std::string m_errorEvent{ "Ollama.Error" };
    // 最後に宣言し、最初に破棄します。デストラクターがスレッドの終了を待つので、
    // Scriptが消えたあとにスレッドだけが残ることはありません。
    LamaPonOllama::ChatWorker m_worker;
};

LAMAPON_DATA_ASSET("Ollama.ModelProfile", "Ollama設定アセット", LamaPonOllama::ProfileSchema);
LAMAPON_SCRIPT_WITH_SCHEMA(OllamaChat, "Ollama.Chat", "Ollamaチャット", OllamaChatSchema);

#endif
