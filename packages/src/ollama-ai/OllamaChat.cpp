// Windows専用です。
// 通信に使うエンジンのHttpSend（WinHTTP）とスレッドがWeb書き出しにはないので、Windows以外では中身のないファイルになります。
#if defined(_WIN32)

#include "LamaPon/LamaPon.h"
#include "OllamaWorker.h"

namespace
{
    // Ollama.ChatのScript設定欄
    constexpr char OllamaChatSchema[] = R"schema({"fields":[
        {"name":"profile","displayName":"Ollama設定アセット","type":"asset","assetType":"data",
         "dataType":"Ollama.ModelProfile","default":"packages/ollama-ai/profiles/Default.asset.json"},
        {"name":"inputObject","displayName":"入力欄のGameObject名","type":"string","default":"Ollama Input",
         "tooltip":"UIInputFieldを持つGameObjectです。空にすると、送信イベントだけを受け付けます。"},
        {"name":"outputObject","displayName":"返答を表示するGameObject名","type":"string","default":"Ollama Reply",
         "tooltip":"TextRendererを持つGameObjectです。空にすると表示せず、イベントだけを発行します。"},
        {"name":"waitingText","displayName":"返答待ちの表示","type":"string","default":"考え中..."},
        {"name":"preload","displayName":"有効になったらモデルを読み込んでおく","type":"bool","default":true,
         "tooltip":"最初の返答が、モデルの読み込みの分だけ早く届きます。読み込みが終わる前の送信は、終わってから送ります。"},
        {"name":"sendEvent","displayName":"送信イベント","type":"string","default":"Ollama.Send",
         "tooltip":"EventArgs.textの文を送ります。空なら入力欄の文を送ります。"},
        {"name":"resetEvent","displayName":"リセットイベント","type":"string","default":"Ollama.Reset",
         "tooltip":"覚えている会話を忘れ、次の送信を新しい会話として始めます。"},
        {"name":"replyEvent","displayName":"返答イベント","type":"string","default":"Ollama.Reply",
         "tooltip":"EventArgs.textへ返答を入れて発行します。"},
        {"name":"errorEvent","displayName":"失敗イベント","type":"string","default":"Ollama.Error",
         "tooltip":"EventArgs.textへ失敗の種類（NotRunningなど）を入れて発行します。"}
    ]})schema";
}

class OllamaChat final : public LamaPon::Script
{
public:
    // Profileを読み込み、送信・リセットイベントを購読します。
    void OnEnable() override
    {
        m_profile = {};
        m_profileError.clear();
        m_profileIsCloud = false;
        // 設定元のData Asset
        const auto asset = LoadDataAsset(LamaPon::PathFromUtf8(m_profilePath));
        // Profile検証の失敗理由
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
            // 送信文をScriptへ渡します(args: イベント本文)。
            m_sendSubscription = On(m_sendEvent, [this](const LamaPon::EventArgs& args)
            {
                Send(args.text);
            });
        }
        if (!m_resetEvent.empty())
        {
            m_resetSubscription = On(m_resetEvent, [this](const LamaPon::EventArgs&)
            {
                ResetHistory();
            });
        }
        if (m_preload && m_profileError.empty())
        {
            StartPreload();
        }
    }

    // イベント購読と進行中の応答待ちを解除します。
    void OnDisable() override
    {
        // このScriptが所有する購読ID
        for (auto* subscription : { &m_sendSubscription, &m_resetSubscription })
        {
            if (*subscription != 0)
            {
                Off(*subscription);
                *subscription = 0;
            }
        }
        // 無効の間は返答を表示できないので、待つのをやめて結果を捨てます。
        // 進行中の通信は止められないため、終わるまで次の送信は受け付けません。
        StopCoroutine(m_waiting);
        m_waiting = 0;
        m_worker.Cancel();
        m_preloading = false;
        m_pendingText.clear();
    }

    // 入力欄からの送信を毎フレーム確認します。
    void Update(float) override
    {
        // Scriptに設定された入力欄
        auto* input = InputField();
        if (input != nullptr && input->ConsumeSubmit())
        {
            Send({});
        }
    }

    // Script設定を読み込みます(text: JSON文字列)。
    void LoadProperties(const std::string_view text) override
    {
        // Script設定のJSONオブジェクト
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        if (!properties.is_object()) return;
        // DataAsset形式に変換した値
        const auto values = LamaPon::DataAsset::FromJson(nlohmann::json{{"values", properties}}.dump());
        m_profilePath = values.GetText("profile", LamaPonOllama::DefaultProfilePath);
        m_inputObject = values.GetText("inputObject", "Ollama Input");
        m_outputObject = values.GetText("outputObject", "Ollama Reply");
        m_waitingText = values.GetText("waitingText", "考え中...");
        m_preload = values.GetBool("preload", true);
        m_sendEvent = values.GetText("sendEvent", "Ollama.Send");
        m_resetEvent = values.GetText("resetEvent", "Ollama.Reset");
        m_replyEvent = values.GetText("replyEvent", "Ollama.Reply");
        m_errorEvent = values.GetText("errorEvent", "Ollama.Error");
    }

    // Script設定をJSON文字列へ保存します。
    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{{"profile", m_profilePath}, {"inputObject", m_inputObject},
            {"outputObject", m_outputObject}, {"waitingText", m_waitingText},
            {"preload", m_preload}, {"sendEvent", m_sendEvent}, {"resetEvent", m_resetEvent},
            {"replyEvent", m_replyEvent}, {"errorEvent", m_errorEvent}}.dump();
    }

private:
    // 入力欄または引数の文を1件送ります(text: 送信文)。
    void Send(std::string text)
    {
        // 送信元の入力欄
        auto* input = InputField();
        // 入力欄から送信するか
        const bool fromInput = text.empty() && input != nullptr;
        if (fromInput)
        {
            text = input->Text();
        }
        text = LamaPonOllama::Trimmed(text);
        if (text.empty())
        {
            return;
        }
        if (!m_profileError.empty())
        {
            Fail(m_profileIsCloud ? LamaPonOllama::ChatError::CloudRejected
                : LamaPonOllama::ChatError::Failed, m_profileError);
            return;
        }
        // Preload中の最初の送信は完了まで保留します。
        const bool afterPreload = m_preloading && m_pendingText.empty();
        if (m_worker.Busy() && !afterPreload)
        {
            // 進行中の返答と入力内容を維持します。
            Report(LamaPonOllama::ChatError::Busy, {});
            return;
        }
        if (fromInput)
        {
            input->SetText({});
        }
        Show(m_waitingText);
        if (afterPreload)
        {
            m_pendingText = std::move(text);
            return;
        }
        StartChat(std::move(text));
    }

    // Chatを別スレッドへ依頼します(text: 送信文)。
    void StartChat(std::string text)
    {
        // Job用に設定・履歴・文を複製します(stop: Worker停止要求)。
        const bool started = m_worker.Start(
            [profile = m_profile, history = m_history, text](const std::stop_token stop)
            {
                return LamaPonOllama::Chat(profile, history, text, stop);
            });
        if (started)
        {
            m_waiting = StartCoroutine(WaitForReply(std::move(text), m_conversation));
        }
    }

    // 返答を表示します(userText: 送信文, conversation: 送信時の世代)。
    LamaPon::Coroutine WaitForReply(std::string userText, const std::uint64_t conversation)
    {
        co_await LamaPon::WaitUntil{ [this] { return m_worker.Ready(); } };
        m_waiting = 0;
        // Workerから受け取ったChat結果
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
        // 失敗・リセット済みの往復は会話履歴へ追加しません。
        if (conversation == m_conversation)
        {
            LamaPonOllama::AppendExchange(m_history, std::move(userText), result->reply,
                m_profile.historyLimit);
        }
        Show(result->reply);
        if (!m_replyEvent.empty())
        {
            // 返答イベントの本文
            LamaPon::EventArgs args;
            args.text = std::move(result->reply);
            Emit(m_replyEvent, std::move(args));
        }
    }

    // Chat開始前にモデルを事前読込します。
    void StartPreload()
    {
        // モデルを事前読込します(stop: Workerの停止要求)。
        const bool started = m_worker.Start([profile = m_profile](const std::stop_token stop)
        {
            return LamaPonOllama::Preload(profile, stop);
        });
        if (started)
        {
            m_preloading = true;
            m_waiting = StartCoroutine(WaitForPreload());
        }
    }

    // Preload結果を処理し、待機中の送信を再開します。
    // Preload失敗はConsoleへ記録し、送信時に失敗を通知します。
    LamaPon::Coroutine WaitForPreload()
    {
        co_await LamaPon::WaitUntil{ [this] { return m_worker.Ready(); } };
        m_waiting = 0;
        m_preloading = false;
        // Workerから受け取ったPreload結果
        const auto result = m_worker.Take();
        if (result && !result->Succeeded())
        {
            Warn("モデルを事前に読み込めませんでした。" + Describe(result->error, result->detail));
        }
        if (!m_pendingText.empty())
        {
            StartChat(std::exchange(m_pendingText, {}));
        }
    }

    // 会話履歴を消し、応答を次の会話世代へ分けます。
    void ResetHistory()
    {
        m_history.clear();
        ++m_conversation;
    }

    // 代替返答を表示します(error: 失敗分類, detail: 補足情報)。
    void Fail(const LamaPonOllama::ChatError error, const std::string& detail)
    {
        Show(m_profile.fallbackReply);
        Report(error, detail);
    }

    // Consoleと失敗イベントへ通知します(error: 分類, detail: 補足)。表示文は保ちます。
    void Report(const LamaPonOllama::ChatError error, const std::string& detail)
    {
        Warn(Describe(error, detail));
        if (!m_errorEvent.empty())
        {
            // 失敗イベントの分類と説明
            LamaPon::EventArgs args;
            args.number = static_cast<float>(error);
            args.text = LamaPonOllama::ErrorName(error);
            Emit(m_errorEvent, std::move(args));
        }
    }

    // 失敗分類と補足を表示文へ整形します(error: 失敗分類, detail: 補足)。
    [[nodiscard]] static std::string Describe(
        const LamaPonOllama::ChatError error, const std::string& detail)
    {
        return std::string(LamaPonOllama::ErrorMessage(error))
            + (detail.empty() ? std::string{} : "（" + detail + "）");
    }

    // 指定したText Rendererへ文章を表示します(text: 表示文)。
    void Show(const std::string& text)
    {
        // 返答先のGameObject
        auto* object = m_outputObject.empty() ? nullptr : Find(m_outputObject);
        // 返答先のText Renderer
        auto* renderer = object != nullptr
            ? object->GetComponent<LamaPon::TextRendererComponent>() : nullptr;
        if (renderer != nullptr && !text.empty())
        {
            renderer->SetText(text);
        }
    }

    // 設定名に一致するUI Input Fieldを返します。
    [[nodiscard]] LamaPon::UIInputFieldComponent* InputField() const
    {
        // 入力欄のGameObject
        auto* object = m_inputObject.empty() ? nullptr : Find(m_inputObject);
        return object != nullptr
            ? object->GetComponent<LamaPon::UIInputFieldComponent>() : nullptr;
    }

    // Warningログを記録します(text: 警告文)。
    static void Warn(const std::string& text) { LamaPon::Logger::Instance().Warning(text); }

    // 検証済みOllama設定
    LamaPonOllama::ModelProfile m_profile;
    // Profile読込の失敗理由
    std::string m_profileError;
    // 失敗したProfileがCloud名を指定したか
    bool m_profileIsCloud{};
    // モデルへ渡す会話履歴
    std::vector<LamaPonOllama::ChatMessage> m_history;
    // 応答が属する会話を識別する世代番号
    std::uint64_t m_conversation{};
    // モデルを事前読込中か
    bool m_preloading{};
    // Preload完了まで保留する送信文
    std::string m_pendingText;
    // Sendイベントの購読ID
    std::uint64_t m_sendSubscription{};
    // Resetイベントの購読ID
    std::uint64_t m_resetSubscription{};
    // 実行中Coroutineの識別子
    std::uint64_t m_waiting{};
    // 設定アセットのパス
    std::string m_profilePath{ LamaPonOllama::DefaultProfilePath };
    // 入力欄のGameObject名
    std::string m_inputObject{ "Ollama Input" };
    // 返答表示先のGameObject名
    std::string m_outputObject{ "Ollama Reply" };
    // 応答待ちの表示文
    std::string m_waitingText{ "考え中..." };
    // 有効化時にモデルを事前読込するか
    bool m_preload{ true };
    // 送信イベント名
    std::string m_sendEvent{ "Ollama.Send" };
    // 会話リセットイベント名
    std::string m_resetEvent{ "Ollama.Reset" };
    // 返答イベント名
    std::string m_replyEvent{ "Ollama.Reply" };
    // 失敗イベント名
    std::string m_errorEvent{ "Ollama.Error" };
    // Script破棄前にWorker threadをjoinするため最後に宣言します。
    LamaPonOllama::ChatWorker m_worker;
};

LAMAPON_DATA_ASSET("Ollama.ModelProfile", "Ollama設定アセット", LamaPonOllama::ProfileSchema);
LAMAPON_SCRIPT_WITH_SCHEMA(OllamaChat, "Ollama.Chat", "Ollamaチャット", OllamaChatSchema);

#endif
