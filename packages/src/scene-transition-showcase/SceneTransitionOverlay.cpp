#include "LamaPon/LamaPon.h"
#include "SceneTransitionOverlay.h"

// Persistent Spriteへ遷移の覆い具合を適用し、終了時に隠します。
class SceneTransitionOverlay final : public LamaPon::Script
{
public:
    // Sprite表示とShader引数を遷移状態へ同期します。
    void Update(float) override
    {
        // このGameObjectの覆いSprite
        auto* sprite = GetComponent<LamaPon::SpriteRendererComponent>();
        // Spriteがない場合は表示を更新できません。
        if (sprite == nullptr)
        {
            return;
        }
        // 初回更新時にSpriteへ外観を反映します。
        if (!m_appearanceApplied)
        {
            ApplyAppearance(*sprite);
            m_appearanceApplied = true;
        }

        // 表示を頼まれた遷移が終わったら、次にShowOverlayで作り直されるまで隠れます（別のScriptが始めた遷移は描きません）。
        // Scene遷移状態を参照します。
        const auto& scenes = GetScene().Scenes();
        // このOverlayが有効な遷移を見たことがあるか記録します。
        if (scenes.IsTransitioning())
        {
            m_sawTransition = true;
        }
        // このOverlayが見ていた遷移の終了を記録します。
        else if (m_sawTransition)
        {
            m_transitionEnded = true;
        }
        // 終了済みでない遷移の覆い割合
        const float coverage = scenes.IsTransitioning() && !m_transitionEnded
            ? scenes.TransitionCoverage()
            : 0.0f;
        // 覆いが必要な状態か
        const bool visible = coverage > 0.0f;
        // 覆い割合に合わせてSpriteを表示します。
        if (sprite->IsEnabled() != visible)
        {
            sprite->SetEnabled(visible);
        }
        // 表示対象外なら以降の描画設定を省略します。
        if (!visible)
        {
            return;
        }
        // Shader失敗時はフェードへ切り替えます。
        if (!m_shaderFailed && !sprite->ShaderError().empty())
        {
            // 独自シェーダーの誤りなどで描けないときも遷移は止めず、単色のフェードで覆います（エラーは一度だけ知らせます）。
            m_shaderFailed = true;
            LamaPon::Logger::Instance().Warning(
                "シーン遷移のシェーダーを使えないため、フェードで覆います: "
                + sprite->ShaderError());
            sprite->SetShaderPath({});
            sprite->SetTexturePath({});
        }
        // Shader失敗後は単色フェードを描きます。
        if (m_shaderFailed)
        {
            // Presetの覆い色
            const auto& color = m_look.color;
            sprite->SetColor({ color.x, color.y, color.z, color.w * coverage });
            return;
        }
        // 現在フレームのShader引数
        const auto frame = LamaPonSceneShowcase::BuildShaderFrame(
            m_look,
            coverage,
            scenes.TransitionPhase() == LamaPon::SceneTransitionPhase::Revealing,
            m_hasRuleTexture);
        // Shaderへ渡す各Custom Parameter
        for (std::size_t index{}; index < frame.parameters.size(); ++index)
        {
            sprite->SetCustomParameter(index, frame.parameters[index]);
        }
    }

    // Script設定を読み込みます(text: JSON文字列)。
    void LoadProperties(const std::string_view text) override
    {
        // Script設定のJSONオブジェクト
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        // look欄がある場合だけPreset見た目を更新します。
        if (properties.is_object() && properties.contains("look"))
        {
            m_look = LamaPonSceneShowcase::LookFromJson(properties.at("look"));
        }
    }

    // Preset見た目をJSON文字列へ保存します。
    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{
            { "look", LamaPonSceneShowcase::LookToJson(m_look) } }.dump();
    }

private:
    // SpriteへPresetの外観を適用します(sprite: 遷移Overlay)。
    void ApplyAppearance(LamaPon::SpriteRendererComponent& sprite)
    {
        // Shaderは覆い色をCustomParameters[5]から読みます。
        sprite.SetColor(m_look.color);
        sprite.SetShaderPath(LamaPon::PathFromUtf8(
            m_look.shader.empty()
                ? std::string(LamaPonSceneShowcase::DefaultShaderPath)
                : m_look.shader));
        m_hasRuleTexture = false;
        // Rule画像は失敗しても既定の模様へフォールバックします。
        try
        {
            sprite.SetTexturePath(LamaPon::PathFromUtf8(m_look.ruleTexture));
            m_hasRuleTexture = !m_look.ruleTexture.empty();
        }
        // ルール画像の読込失敗理由
        catch (const std::exception& exception)
        {
            // 画像が無くても遷移は止めず、ディゾルブで覆います。
            sprite.SetTexturePath({});
            LamaPon::Logger::Instance().Warning(
                "ルール画像を読み込めません: " + m_look.ruleTexture
                + " | " + exception.what());
        }
    }

    // 遷移Shaderと色の設定
    LamaPonSceneShowcase::Look m_look;
    // 外観をSpriteへ適用済みか
    bool m_appearanceApplied{};
    // ルール画像を割り当てたか
    bool m_hasRuleTexture{};
    // 対象Scene遷移を観測済みか
    bool m_sawTransition{};
    // 対象Scene遷移が終了したか
    bool m_transitionEnded{};
    // Shader失敗を検出済みか
    bool m_shaderFailed{};
};

LAMAPON_SCRIPT_NAMED(SceneTransitionOverlay, "SceneTransition.Overlay",
    "シーン遷移の覆い（自動で追加）");
