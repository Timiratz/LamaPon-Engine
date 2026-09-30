#include "LamaPon/LamaPon.h"
#include "SceneTransitionOverlay.h"

// 画面全体を覆うSpriteへ、エンジンが計算した覆い具合を毎フレーム
// 渡すScriptです。ShowOverlayが作るGameObjectに付き、シーンを
// 切り替えても残ります。エンジン側は覆いを描かないため、この
// Scriptが無いと遷移は時間だけ進み、画面は何も覆われません。
class SceneTransitionOverlay final : public LamaPon::Script
{
public:
    void Update(float) override
    {
        auto* sprite = GetComponent<LamaPon::SpriteRendererComponent>();
        if (sprite == nullptr)
        {
            return;
        }
        if (!m_appearanceApplied)
        {
            ApplyAppearance(*sprite);
            m_appearanceApplied = true;
        }

        // 表示を頼まれた遷移が終わったら、次にShowOverlayで作り直される
        // まで隠れます（別のScriptが始めた遷移は描きません）。
        const auto& scenes = GetScene().Scenes();
        if (scenes.IsTransitioning())
        {
            m_sawTransition = true;
        }
        else if (m_sawTransition)
        {
            m_transitionEnded = true;
        }
        const float coverage = scenes.IsTransitioning() && !m_transitionEnded
            ? scenes.TransitionCoverage()
            : 0.0f;
        const bool visible = coverage > 0.0f;
        if (sprite->IsEnabled() != visible)
        {
            sprite->SetEnabled(visible);
        }
        if (!visible)
        {
            return;
        }
        if (!m_shaderFailed && !sprite->ShaderError().empty())
        {
            // 独自シェーダーの誤りなどで描けないときも遷移は止めず、
            // 単色のフェードで覆います（エラーは一度だけ知らせます）。
            m_shaderFailed = true;
            LamaPon::Logger::Instance().Warning(
                "シーン遷移のシェーダーを使えないため、フェードで覆います: "
                + sprite->ShaderError());
            sprite->SetShaderPath({});
            sprite->SetTexturePath({});
        }
        if (m_shaderFailed)
        {
            const auto& color = m_look.color;
            sprite->SetColor({ color.x, color.y, color.z, color.w * coverage });
            return;
        }
        const auto frame = LamaPonSceneShowcase::BuildShaderFrame(
            m_look,
            coverage,
            scenes.TransitionPhase() == LamaPon::SceneTransitionPhase::Revealing,
            m_hasRuleTexture);
        for (std::size_t index{}; index < frame.parameters.size(); ++index)
        {
            sprite->SetCustomParameter(index, frame.parameters[index]);
        }
    }

    void LoadProperties(const std::string_view text) override
    {
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        if (properties.is_object() && properties.contains("look"))
        {
            m_look = LamaPonSceneShowcase::LookFromJson(properties.at("look"));
        }
    }

    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{
            { "look", LamaPonSceneShowcase::LookToJson(m_look) } }.dump();
    }

private:
    void ApplyAppearance(LamaPon::SpriteRendererComponent& sprite)
    {
        // 覆いの色はSpriteの色として渡し、シェーダーは
        // CustomParameters[5]（premultiplyしない色）で受け取ります。
        sprite.SetColor(m_look.color);
        sprite.SetShaderPath(LamaPon::PathFromUtf8(
            m_look.shader.empty()
                ? std::string(LamaPonSceneShowcase::DefaultShaderPath)
                : m_look.shader));
        m_hasRuleTexture = false;
        try
        {
            sprite.SetTexturePath(LamaPon::PathFromUtf8(m_look.ruleTexture));
            m_hasRuleTexture = !m_look.ruleTexture.empty();
        }
        catch (const std::exception& exception)
        {
            // 画像が無くても遷移は止めず、ディゾルブで覆います。
            sprite.SetTexturePath({});
            LamaPon::Logger::Instance().Warning(
                "ルール画像を読み込めません: " + m_look.ruleTexture
                + " | " + exception.what());
        }
    }

    LamaPonSceneShowcase::Look m_look;
    bool m_appearanceApplied{};
    bool m_hasRuleTexture{};
    bool m_sawTransition{};
    bool m_transitionEnded{};
    bool m_shaderFailed{};
};

LAMAPON_SCRIPT_NAMED(SceneTransitionOverlay, "SceneTransition.Overlay",
    "シーン遷移の覆い（自動で追加）");
