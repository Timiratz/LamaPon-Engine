#include "LamaPon/LamaPon.h"
#include "SceneTransitionOverlay.h"
#include "SceneTransitionSchema.h"

namespace
{
    // SceneTransition.Controllerの編集欄
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
    // 有効状態を記録します。
    void OnEnable() override { m_enabled = true; }
    // 無効状態を記録します。
    void OnDisable() override { m_enabled = false; }

    // 再生イベントを登録し、自動再生の予約を初期化します。
    void Start() override
    {
        // 設定されたイベント名がある場合だけ購読します。
        if (!m_eventName.empty())
        {
            On(m_eventName, [this] { Play(); });
        }
        m_autoplayPending = m_playOnStart;
    }

    // Scene読込が落ち着いてから予約された遷移を開始します。
    void Update(float) override
    {
        // 起動Sceneの読み込みと重ならないよう、完了後に一度だけ実行します。
        // Scene遷移と読込の現在状態
        auto& scenes = GetScene().Scenes();
        // 起動待ちで他の遷移がない場合に自動再生します。
        if (m_autoplayPending && !scenes.IsTransitioning() && !scenes.IsLoading())
        {
            Play();
        }
    }

    // Script設定を読み込みます(text: JSON文字列)。
    void LoadProperties(const std::string_view text) override
    {
        // JSONとして解析したScript設定
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        // オブジェクト以外は適用しません。
        if (!properties.is_object())
        {
            return;
        }
        // 型安全なアクセサーで設定値を取得します。
        const auto values = LamaPon::DataAsset::FromJson(
            nlohmann::json{{"values", properties}}.dump());
        m_transition = values.GetText("transition",
            LamaPonSceneShowcase::DefaultPresetPath);
        m_destination = values.GetText("destination");
        m_eventName = values.GetText("eventName", "SceneTransition.Play");
        m_playOnStart = values.GetBool("playOnStart");
    }

    // Script設定をJSON文字列へ保存します。
    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{
            {"transition", m_transition}, {"destination", m_destination},
            {"eventName", m_eventName}, {"playOnStart", m_playOnStart}}.dump();
    }

private:
    // 有効なときだけScene遷移を開始します。
    void Play()
    {
        // Scene遷移と読込の現在状態
        auto& scenes = GetScene().Scenes();
        // 非表示または別遷移中は開始しません。
        if (!m_enabled || scenes.IsTransitioning() || scenes.IsLoading())
        {
            return;
        }
        m_autoplayPending = false;
        // 遷移の時間はエンジンへ渡し、覆いはパッケージのSpriteとシェーダーで描きます（SceneTransitionOverlay.h）。
        // プリセット読込・遷移開始の失敗理由
        std::string error;
        // Presetを適用して遷移を要求します。
        if (!LamaPonSceneShowcase::PlayPreset(GetScene(),
                LamaPon::PathFromUtf8(m_transition),
                LamaPon::PathFromUtf8(m_destination), error))
        {
            LamaPon::Logger::Instance().Warning(error);
        }
    }

    // 再生する遷移Presetのパス
    std::string m_transition{ LamaPonSceneShowcase::DefaultPresetPath };
    // 空なら遷移先Sceneを切り替えない
    std::string m_destination;
    // 再生要求を受けるイベント名
    std::string m_eventName{ "SceneTransition.Play" };
    // 起動時に一度再生するか
    bool m_playOnStart{};
    // 起動後の自動再生待ち
    bool m_autoplayPending{};
    // Scriptが有効状態か
    bool m_enabled{};
};

LAMAPON_DATA_ASSET("SceneTransition.Preset", "シーン遷移プリセット",
    LamaPonSceneShowcase::PresetSchema);

LAMAPON_SCRIPT_WITH_SCHEMA(SceneTransitionController, "SceneTransition.Controller",
    "シーン遷移コントローラー", ControllerSchema);
