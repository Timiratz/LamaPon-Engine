#include "LamaPon/LamaPon.h"
#include "SceneTransitionAssets.h"
#include "SceneTransitionSchema.h"

namespace
{
    constexpr char ControllerSchema[] = R"({"fields":[
        {"name":"transition","displayName":"遷移プリセット","type":"asset",
         "assetType":"data","dataType":"SceneTransition.Preset",
         "default":"packages/scene-transition-showcase/presets/IrisGold.asset.json"},
        {"name":"destination","displayName":"移動先Scene（空なら演出だけ）",
         "type":"asset","assetType":"scene","default":""},
        {"name":"eventName","displayName":"再生イベント名","type":"string",
         "default":"SceneTransition.Play",
         "tooltip":"UI Buttonのクリックイベント名と揃えます。ボタンの移動先Sceneは空にします。"},
        {"name":"playOnStart","displayName":"再生開始時に一度実行","type":"bool","default":false}
    ]})";
}

class SceneTransitionController final : public LamaPon::Script
{
public:
    void OnEnable() override { m_enabled = true; }
    void OnDisable() override { m_enabled = false; }

    void Start() override
    {
        if (!m_eventName.empty())
        {
            On(m_eventName, [this] { Play(); });
        }
        m_autoplayPending = m_playOnStart;
    }

    void Update(float) override
    {
        // 起動Sceneの読み込みと重ならないよう、完了後に一度だけ実行します。
        auto& scenes = GetScene().Scenes();
        if (m_autoplayPending && !scenes.IsTransitioning() && !scenes.IsLoading())
        {
            Play();
        }
    }

    void LoadProperties(const std::string_view text) override
    {
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        if (!properties.is_object())
        {
            return;
        }
        // 型違いのプロパティもDataAssetの安全なアクセサーで読みます。
        const auto values = LamaPon::DataAsset::FromJson(
            nlohmann::json{{"values", properties}}.dump());
        m_transition = values.GetText("transition",
            LamaPonSceneShowcase::DefaultPresetPath);
        m_destination = values.GetText("destination");
        m_eventName = values.GetText("eventName", "SceneTransition.Play");
        m_playOnStart = values.GetBool("playOnStart");
    }

    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{
            {"transition", m_transition}, {"destination", m_destination},
            {"eventName", m_eventName}, {"playOnStart", m_playOnStart}}.dump();
    }

private:
    void Play()
    {
        auto& scenes = GetScene().Scenes();
        if (!m_enabled || scenes.IsTransitioning() || scenes.IsLoading())
        {
            return;
        }
        m_autoplayPending = false;
        LamaPon::SceneTransitionSettings transition;
        const auto asset = LoadDataAsset(LamaPon::PathFromUtf8(m_transition));
        if (!LamaPonSceneShowcase::ReadPreset(*asset, transition))
        {
            LamaPon::Logger::Instance().Warning(
                "遷移プリセットを読み込めません: " + m_transition);
            return;
        }
        const bool accepted = m_destination.empty()
            ? scenes.PlayTransition(transition)
            : scenes.RequestLoadAsync(LamaPon::PathFromUtf8(m_destination), transition);
        if (!accepted)
        {
            LamaPon::Logger::Instance().Warning(
                "シーン遷移を開始できません: " + scenes.LastError());
        }
    }

    std::string m_transition{ LamaPonSceneShowcase::DefaultPresetPath };
    std::string m_destination;
    std::string m_eventName{ "SceneTransition.Play" };
    bool m_playOnStart{};
    bool m_autoplayPending{};
    bool m_enabled{};
};

LAMAPON_DATA_ASSET("SceneTransition.Preset", "シーン遷移プリセット",
    LamaPonSceneShowcase::PresetSchema);

LAMAPON_SCRIPT_WITH_SCHEMA(SceneTransitionController, "SceneTransition.Controller",
    "シーン遷移コントローラー", ControllerSchema);
