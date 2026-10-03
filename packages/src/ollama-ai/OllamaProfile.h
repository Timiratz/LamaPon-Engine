#pragma once

#include "LamaPon/Assets/DataAsset.h"
#include "OllamaPolicy.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace LamaPonOllama
{
    // Data Assetへ登録するProfile型名
    inline constexpr auto ProfileType = "Ollama.ModelProfile";
    // 配布する初期Model Profile
    inline constexpr auto DefaultProfilePath =
        "packages/ollama-ai/profiles/Default.asset.json";

    // 接続先はLoopback固定で、API key欄を持たないModel Profileです。
    // maxTokens既定値は30秒の同期HTTP受信上限に合わせています。
    inline constexpr char ProfileSchema[] = R"schema({"fields":[
        {"name":"model","displayName":"モデル名（空なら最初のローカルモデル）","type":"string","default":"",
         "tooltip":"ollama list に出る名前です。cloud が付くモデルは使えません。"},
        {"name":"systemPrompt","displayName":"システムプロンプト","type":"string",
         "default":"あなたはゲームの登場人物です。日本語で、1～2文の短い返事をしてください。"},
        {"name":"temperature","displayName":"返答のゆらぎ（temperature）","type":"float","default":0.7,"min":0,"max":2},
        {"name":"maxTokens","displayName":"返答の最大トークン数（num_predict）","type":"int","default":128,"min":1,"max":1024,
         "tooltip":"大きくすると返答が長くなり、30秒の制限に間に合わなくなります。"},
        {"name":"port","displayName":"Ollamaのポート（接続先は127.0.0.1固定）","type":"int","default":11434,"min":1,"max":65535},
        {"name":"fallbackReply","displayName":"失敗したときに表示する返答","type":"string","default":"（いまは返事ができません）"},
        {"name":"historyLimit","displayName":"覚えておく会話の件数","type":"int","default":8,"min":0,"max":64,
         "tooltip":"質問と返答をそれぞれ1件と数えます。0にすると毎回はじめての会話になります。"},
        {"name":"keepAliveMinutes","displayName":"モデルをメモリに残す時間（分）","type":"int","default":10,"min":1,"max":1440,
         "tooltip":"最後の送信からこの時間は、Ollamaがモデルを読み込んだままにします。長くすると間が空いても返答が速くなりますが、その間メモリを使い続けます。"}
    ]})schema";

    struct ModelProfile final
    {
        // 空なら最初のローカルモデルを選択
        std::string model;
        // モデルへ渡す会話の役割指示
        std::string systemPrompt{
            "あなたはゲームの登場人物です。日本語で、1～2文の短い返事をしてください。" };
        // 応答のランダム性
        double temperature{ 0.7 };
        // 応答の最大生成Token数
        std::uint32_t maxTokens{ 128 };
        // Loopback接続ポート
        std::uint16_t port{ DefaultPort };
        // 失敗時に表示する返答
        std::string fallbackReply{ "（いまは返事ができません）" };
        // 会話へ保持する発言数
        std::uint32_t historyLimit{ 8 };
        // Ollamaがモデルを保持する分数
        std::uint32_t keepAliveMinutes{ 10 };
    };

    // Profileを検証して読み込みます(asset: 入力アセット, output: 出力先, error: 失敗理由)。
    // 不正項目・Cloud名・未定義keyを拒否し、失敗時はoutputを保持します。
    [[nodiscard]] inline bool ReadProfile(const LamaPon::DataAsset& asset,
        ModelProfile& output, std::string& error)
    {
        try
        {
            if (asset.TypeName() != ProfileType || asset.IsEmpty())
            {
                throw std::invalid_argument("Ollama設定アセットの型または内容が不正です。");
            }
            // Assetに保存されたvalues
            const auto source = nlohmann::json::parse(asset.SerializeToJson()).at("values");
            // 許可されたProfile項目を照合します(item: keyと値の組)。
            for (const auto& item : source.items())
            {
                // 検査するProfile項目名
                const std::string_view key = item.key();
                // 未定義項目を拒否します。
                if (key != "model" && key != "systemPrompt" && key != "temperature"
                    && key != "maxTokens" && key != "port" && key != "fallbackReply"
                    && key != "historyLimit" && key != "keepAliveMinutes")
                {
                    throw std::invalid_argument("Ollama設定アセットに使えない項目があります: "
                        + item.key() + "（接続先のホストやAPIキーは設定できません）");
                }
            }

            // 検証中のProfile
            ModelProfile profile;
            // 文字列項目を検証します(key: 欄名, fallback: 未設定値, maxBytes: 最大長)。
            const auto text = [&source](const char* key, std::string fallback,
                const std::size_t maxBytes)
            {
                // 未設定項目は既定値を返します。
                if (!source.contains(key))
                {
                    return fallback;
                }
                // 検証対象のJSON値
                const auto& field = source.at(key);
                if (!field.is_string() || field.get_ref<const std::string&>().size() > maxBytes)
                {
                    throw std::invalid_argument("Ollama設定の文字列項目が不正か、長すぎます。");
                }
                return field.get<std::string>();
            };
            // 整数項目を検証します(key: 欄名, fallback: 既定値, minimum/maximum: 許容範囲)。
            const auto integer = [&source](const char* key, const std::uint32_t fallback,
                const std::uint32_t minimum, const std::uint32_t maximum)
            {
                // 未設定項目は既定値を返します。
                if (!source.contains(key))
                {
                    return fallback;
                }
                // 検証対象のJSON値
                const auto& field = source.at(key);
                if (!field.is_number())
                {
                    throw std::invalid_argument("Ollama設定の整数項目が不正です。");
                }
                // 整数性と範囲を検査する数値
                const auto number = field.get<double>();
                // 小数・範囲外は丸めず拒否します。
                if (!std::isfinite(number) || number < minimum || number > maximum
                    || std::floor(number) != number)
                {
                    throw std::invalid_argument("Ollama設定の整数項目が範囲外です。");
                }
                return static_cast<std::uint32_t>(number);
            };

            // モデル名を読み込みます。
            profile.model = text("model", profile.model, 128);
            if (HasCloudTag(profile.model))
            {
                throw std::invalid_argument("クラウドのモデルは使えません: " + profile.model
                    + "（このPCで動くローカルモデルを指定してください）");
            }
            // 空欄以外はローカルモデル名として検証します。
            if (!profile.model.empty() && !IsLocalModelName(profile.model))
            {
                throw std::invalid_argument("モデル名に使えない文字があります: " + profile.model);
            }
            profile.systemPrompt = text("systemPrompt", profile.systemPrompt, 4000);
            profile.fallbackReply = text("fallbackReply", profile.fallbackReply, 400);
            // Temperatureが設定されている場合は数値範囲を検証します。
            if (source.contains("temperature"))
            {
                // JSONのTemperature値
                const auto& field = source.at("temperature");
                if (!field.is_number())
                {
                    throw std::invalid_argument("temperatureが数値ではありません。");
                }
                profile.temperature = field.get<double>();
                if (!std::isfinite(profile.temperature)
                    || profile.temperature < 0.0 || profile.temperature > 2.0)
                {
                    throw std::invalid_argument("temperatureは0～2で指定してください。");
                }
            }
            profile.maxTokens = integer("maxTokens", profile.maxTokens, 1, 1024);
            profile.port = static_cast<std::uint16_t>(integer("port", profile.port, 1, 65535));
            profile.historyLimit = integer("historyLimit", profile.historyLimit, 0, 64);
            profile.keepAliveMinutes = integer("keepAliveMinutes", profile.keepAliveMinutes, 1, 1440);

            output = std::move(profile);
            error.clear();
            return true;
        }
        // Profile検証の失敗理由
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }
}
