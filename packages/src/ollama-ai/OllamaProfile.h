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
    inline constexpr auto ProfileType = "Ollama.ModelProfile";
    inline constexpr auto DefaultProfilePath =
        "packages/ollama-ai/profiles/Default.asset.json";

    // 接続先のホストとAPIキーの欄はありません。接続先はこのPCに固定で、
    // 変えられるのはポートだけです。
    // maxTokensの既定が小さいのは、エンジンのHTTP受信が30秒で打ち切られ、
    // 返答を少しずつ受け取ることもできないためです。
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
        std::string model;
        std::string systemPrompt{
            "あなたはゲームの登場人物です。日本語で、1～2文の短い返事をしてください。" };
        double temperature{ 0.7 };
        std::uint32_t maxTokens{ 128 };
        std::uint16_t port{ DefaultPort };
        std::string fallbackReply{ "（いまは返事ができません）" };
        std::uint32_t historyLimit{ 8 };
        std::uint32_t keepAliveMinutes{ 10 };
    };

    // 不正な設定は1項目も適用せず、outputを変えません。クラウドのモデル名は
    // ここで拒否するので、通信を始める前に気付けます。スキーマにない項目も
    // 拒否します。"host"や"apiKey"を書き足しても効かないことを、黙って
    // 無視せずエラーで伝えるためです。
    [[nodiscard]] inline bool ReadProfile(const LamaPon::DataAsset& asset,
        ModelProfile& output, std::string& error)
    {
        try
        {
            if (asset.TypeName() != ProfileType || asset.IsEmpty())
            {
                throw std::invalid_argument("Ollama設定アセットの型または内容が不正です。");
            }
            const auto source = nlohmann::json::parse(asset.SerializeToJson()).at("values");
            for (const auto& item : source.items())
            {
                const std::string_view key = item.key();
                if (key != "model" && key != "systemPrompt" && key != "temperature"
                    && key != "maxTokens" && key != "port" && key != "fallbackReply"
                    && key != "historyLimit" && key != "keepAliveMinutes")
                {
                    throw std::invalid_argument("Ollama設定アセットに使えない項目があります: "
                        + item.key() + "（接続先のホストやAPIキーは設定できません）");
                }
            }

            ModelProfile profile;
            const auto text = [&source](const char* key, std::string fallback,
                const std::size_t maxBytes)
            {
                if (!source.contains(key))
                {
                    return fallback;
                }
                const auto& field = source.at(key);
                if (!field.is_string() || field.get_ref<const std::string&>().size() > maxBytes)
                {
                    throw std::invalid_argument("Ollama設定の文字列項目が不正か、長すぎます。");
                }
                return field.get<std::string>();
            };
            // DataAssetは整数もdoubleとして保持します。丸めて設定を変えず、
            // 非整数・範囲外は拒否します。
            const auto integer = [&source](const char* key, const std::uint32_t fallback,
                const std::uint32_t minimum, const std::uint32_t maximum)
            {
                if (!source.contains(key))
                {
                    return fallback;
                }
                const auto& field = source.at(key);
                if (!field.is_number())
                {
                    throw std::invalid_argument("Ollama設定の整数項目が不正です。");
                }
                const auto number = field.get<double>();
                if (!std::isfinite(number) || number < minimum || number > maximum
                    || std::floor(number) != number)
                {
                    throw std::invalid_argument("Ollama設定の整数項目が範囲外です。");
                }
                return static_cast<std::uint32_t>(number);
            };

            profile.model = text("model", profile.model, 128);
            if (HasCloudTag(profile.model))
            {
                throw std::invalid_argument("クラウドのモデルは使えません: " + profile.model
                    + "（このPCで動くローカルモデルを指定してください）");
            }
            if (!profile.model.empty() && !IsLocalModelName(profile.model))
            {
                throw std::invalid_argument("モデル名に使えない文字があります: " + profile.model);
            }
            profile.systemPrompt = text("systemPrompt", profile.systemPrompt, 4000);
            profile.fallbackReply = text("fallbackReply", profile.fallbackReply, 400);
            if (source.contains("temperature"))
            {
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
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }
}
