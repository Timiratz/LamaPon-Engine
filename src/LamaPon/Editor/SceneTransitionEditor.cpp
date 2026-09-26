#include "LamaPon/Editor/SceneTransitionEditor.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Editor/EditorLayerShared.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

using namespace LamaPon;
using namespace LamaPon::EditorDetail;

namespace
{
    void ShowItemTooltip(const char* text)
    {
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled
                | ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    // ドラッグや文字入力のように、確定が後から来る項目です。
    void TrackContinuous(
        const bool edited,
        SceneTransitionEditResult& result)
    {
        if (edited)
        {
            result.changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            result.committed = true;
        }
    }

    // チェックや選択肢のように、変更と同時に確定する項目です。
    void TrackDiscrete(
        const bool edited,
        SceneTransitionEditResult& result)
    {
        if (edited)
        {
            result.changed = true;
            result.committed = true;
        }
    }

    template <typename Enum, typename LabelFunction>
    [[nodiscard]] bool EnumCombo(
        const char* label,
        Enum& value,
        LabelFunction labelOf)
    {
        bool changed = false;
        if (ImGui::BeginCombo(label, labelOf(value)))
        {
            for (int index = 0;
                index < static_cast<int>(Enum::Count);
                ++index)
            {
                const auto candidate = static_cast<Enum>(index);
                const bool selected = candidate == value;
                if (ImGui::Selectable(labelOf(candidate), selected))
                {
                    value = candidate;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    [[nodiscard]] bool IsShaderSource(
        const std::filesystem::path& path)
    {
        auto extension = PathToUtf8(path.extension());
        std::ranges::transform(
            extension,
            extension.begin(),
            [](const char value)
            {
                return static_cast<char>(
                    value >= 'A' && value <= 'Z'
                        ? value - 'A' + 'a'
                        : value);
            });
        return extension == ".hlsl";
    }

    // assets相対のパスを入力します。Asset Browserからのドロップも
    // 受け付け、acceptsが偽を返すファイルは無視します。
    template <typename Predicate>
    void AssetPathInput(
        const char* label,
        const char* hint,
        std::filesystem::path& path,
        Predicate accepts,
        SceneTransitionEditResult& result)
    {
        std::array<char, 512> buffer{};
        const auto text = PathToUtf8(path);
        strncpy_s(
            buffer.data(),
            buffer.size(),
            text.c_str(),
            _TRUNCATE);
        const bool edited = ImGui::InputTextWithHint(
            label,
            hint,
            buffer.data(),
            buffer.size());
        if (edited)
        {
            path = PathFromUtf8(buffer.data());
        }
        TrackContinuous(edited, result);
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                const auto dropped = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (accepts(dropped))
                {
                    path = dropped;
                    TrackDiscrete(true, result);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
}

namespace LamaPon
{
    const char* SceneTransitionEffectLabel(
        const SceneTransitionEffect effect) noexcept
    {
        switch (effect)
        {
        case SceneTransitionEffect::Fade:
            return "フェード";
        case SceneTransitionEffect::Wipe:
            return "ワイプ";
        case SceneTransitionEffect::Iris:
            return "アイリス（円）";
        case SceneTransitionEffect::Diamond:
            return "アイリス（ひし形）";
        case SceneTransitionEffect::Blinds:
            return "ブラインド";
        case SceneTransitionEffect::Tiles:
            return "タイル";
        case SceneTransitionEffect::DiamondTiles:
            return "ひし形タイル";
        case SceneTransitionEffect::Dots:
            return "ドット";
        case SceneTransitionEffect::Shutter:
            return "シャッター";
        case SceneTransitionEffect::Shader:
            return "シェーダー（模様）";
        case SceneTransitionEffect::None:
        default:
            return "なし（読み込み画面だけ）";
        }
    }

    const char* SceneTransitionDirectionLabel(
        const SceneTransitionDirection direction) noexcept
    {
        switch (direction)
        {
        case SceneTransitionDirection::RightToLeft:
            return "右 → 左";
        case SceneTransitionDirection::TopToBottom:
            return "上 → 下";
        case SceneTransitionDirection::BottomToTop:
            return "下 → 上";
        case SceneTransitionDirection::TopLeftToBottomRight:
            return "左上 → 右下";
        case SceneTransitionDirection::TopRightToBottomLeft:
            return "右上 → 左下";
        case SceneTransitionDirection::BottomLeftToTopRight:
            return "左下 → 右上";
        case SceneTransitionDirection::BottomRightToTopLeft:
            return "右下 → 左上";
        case SceneTransitionDirection::LeftToRight:
        default:
            return "左 → 右";
        }
    }

    const char* SceneTransitionEasingLabel(
        const SceneTransitionEasing easing) noexcept
    {
        switch (easing)
        {
        case SceneTransitionEasing::Linear:
            return "一定の速さ";
        case SceneTransitionEasing::EaseInQuad:
            return "ゆっくり始まる（弱）";
        case SceneTransitionEasing::EaseOutQuad:
            return "ゆっくり終わる（弱）";
        case SceneTransitionEasing::EaseInOutQuad:
            return "ゆっくり始まって終わる（弱）";
        case SceneTransitionEasing::EaseInCubic:
            return "ゆっくり始まる";
        case SceneTransitionEasing::EaseOutCubic:
            return "ゆっくり終わる";
        case SceneTransitionEasing::EaseInOutSine:
            return "なめらか（サイン）";
        case SceneTransitionEasing::EaseInOutCubic:
        default:
            return "ゆっくり始まって終わる";
        }
    }

    const char* SceneTransitionShaderPatternLabel(
        const SceneTransitionShaderPattern pattern) noexcept
    {
        switch (pattern)
        {
        case SceneTransitionShaderPattern::RuleImage:
            return "ルール画像";
        case SceneTransitionShaderPattern::Clock:
            return "時計";
        case SceneTransitionShaderPattern::Spiral:
            return "渦巻き";
        case SceneTransitionShaderPattern::Ripple:
            return "波紋";
        case SceneTransitionShaderPattern::Hexagons:
            return "六角形";
        case SceneTransitionShaderPattern::Heart:
            return "ハート";
        case SceneTransitionShaderPattern::Dissolve:
        default:
            return "ディゾルブ（ノイズ）";
        }
    }

    SceneTransitionEditResult DrawSceneTransitionEditor(
        SceneTransitionSettings& settings,
        const bool showPreviewButton)
    {
        SceneTransitionEditResult result;
        TrackDiscrete(
            EnumCombo(
                "演出",
                settings.effect,
                SceneTransitionEffectLabel),
            result);
        const auto effect = settings.effect;
        if (effect == SceneTransitionEffect::None)
        {
            ImGui::TextDisabled(
                "覆いを描かずに切り替えます。非同期読み込みの間は"
                "読み込み画面だけを表示します（従来の動作）。");
        }
        else
        {
            const bool shader = effect == SceneTransitionEffect::Shader;
            const auto pattern = settings.shaderPattern;
            if (shader)
            {
                TrackDiscrete(
                    EnumCombo(
                        "模様",
                        settings.shaderPattern,
                        SceneTransitionShaderPatternLabel),
                    result);
                ShowItemTooltip(
                    "画素ごとに覆う順番を決める模様です。"
                    "shaders/LamaPonSceneTransition.hlslで描きます");
                if (settings.shaderPattern
                    == SceneTransitionShaderPattern::RuleImage)
                {
                    AssetPathInput(
                        "ルール画像",
                        "textures/rule.png",
                        settings.ruleTexture,
                        [](const std::filesystem::path& path)
                        {
                            return IsTextureAsset(path);
                        },
                        result);
                    ShowItemTooltip(
                        "グレースケール画像の暗い所から順に覆います。"
                        "画像が無いときはディゾルブになります");
                }
                AssetPathInput(
                    "独自シェーダー",
                    "（空なら組み込み）",
                    settings.shader,
                    [](const std::filesystem::path& path)
                    {
                        return IsShaderSource(path);
                    },
                    result);
                ShowItemTooltip(
                    "LamaPonSceneTransition.hlslを複製して書き換えた"
                    ".hlslを指定できます。使えないときはフェードで代用します");
            }

            ImGui::SeparatorText("時間");
            ImGui::SetNextItemWidth(160.0f);
            TrackContinuous(
                ImGui::DragFloat(
                    "覆う時間",
                    &settings.coverDuration,
                    0.01f,
                    0.0f,
                    5.0f,
                    "%.2f 秒"),
                result);
            ImGui::SetNextItemWidth(160.0f);
            TrackContinuous(
                ImGui::DragFloat(
                    "覆ったまま待つ時間",
                    &settings.holdDuration,
                    0.01f,
                    0.0f,
                    5.0f,
                    "%.2f 秒"),
                result);
            ShowItemTooltip(
                "読み込みが速くても、最低この時間は覆ったままにします");
            ImGui::SetNextItemWidth(160.0f);
            TrackContinuous(
                ImGui::DragFloat(
                    "開く時間",
                    &settings.revealDuration,
                    0.01f,
                    0.0f,
                    5.0f,
                    "%.2f 秒"),
                result);
            TrackDiscrete(
                EnumCombo(
                    "動き方",
                    settings.easing,
                    SceneTransitionEasingLabel),
                result);

            ImGui::SeparatorText("見た目");
            TrackContinuous(
                ImGui::ColorEdit4("覆いの色", &settings.color.x),
                result);
            if (effect != SceneTransitionEffect::Fade)
            {
                bool accent = settings.accentColor.w > 0.0f;
                if (ImGui::Checkbox("差し色を使う", &accent))
                {
                    settings.accentColor.w = accent ? 1.0f : 0.0f;
                    TrackDiscrete(true, result);
                }
                ShowItemTooltip(
                    "覆いの先端や縁に別の色の帯を入れます");
                if (accent)
                {
                    TrackContinuous(
                        ImGui::ColorEdit4(
                            "差し色",
                            &settings.accentColor.x),
                        result);
                    ImGui::SetNextItemWidth(160.0f);
                    TrackContinuous(
                        ImGui::SliderFloat(
                            "差し色の幅",
                            &settings.accentWidth,
                            0.0f,
                            0.2f,
                            "%.3f"),
                        result);
                }
            }

            const bool directional =
                effect == SceneTransitionEffect::Wipe
                || effect == SceneTransitionEffect::Blinds
                || effect == SceneTransitionEffect::Tiles
                || effect == SceneTransitionEffect::DiamondTiles
                || effect == SceneTransitionEffect::Dots
                || effect == SceneTransitionEffect::Shutter
                || (shader
                    && (pattern == SceneTransitionShaderPattern::Clock
                        || pattern
                            == SceneTransitionShaderPattern::Hexagons));
            if (directional)
            {
                TrackDiscrete(
                    EnumCombo(
                        "向き",
                        settings.direction,
                        SceneTransitionDirectionLabel),
                    result);
                if (shader
                    && pattern == SceneTransitionShaderPattern::Clock)
                {
                    ImGui::TextDisabled(
                        "右向きの方向で時計回り、左向きで反時計回りです。");
                }
            }
            if (effect == SceneTransitionEffect::Wipe
                || effect == SceneTransitionEffect::Shutter
                || shader)
            {
                ImGui::SetNextItemWidth(160.0f);
                TrackContinuous(
                    ImGui::SliderFloat(
                        "境界のぼかし",
                        &settings.softness,
                        0.0f,
                        0.5f,
                        "%.3f"),
                    result);
            }
            const bool tiled =
                effect == SceneTransitionEffect::Blinds
                || effect == SceneTransitionEffect::Tiles
                || effect == SceneTransitionEffect::DiamondTiles
                || effect == SceneTransitionEffect::Dots;
            if (tiled
                || (shader
                    && pattern != SceneTransitionShaderPattern::RuleImage
                    && pattern != SceneTransitionShaderPattern::Clock
                    && pattern != SceneTransitionShaderPattern::Heart))
            {
                int divisions = static_cast<int>(settings.divisions);
                ImGui::SetNextItemWidth(160.0f);
                const bool edited = ImGui::SliderInt(
                    "分割数",
                    &divisions,
                    1,
                    64);
                if (edited)
                {
                    settings.divisions =
                        static_cast<std::uint32_t>(
                            std::clamp(divisions, 1, 64));
                }
                TrackContinuous(edited, result);
                ShowItemTooltip(
                    "帯の本数、横に並ぶタイルの数、模様の細かさです");
            }
            if (tiled
                || (shader
                    && pattern
                        == SceneTransitionShaderPattern::Hexagons))
            {
                ImGui::SetNextItemWidth(160.0f);
                TrackContinuous(
                    ImGui::SliderFloat(
                        "時間差",
                        &settings.stagger,
                        0.0f,
                        0.95f,
                        "%.2f"),
                    result);
                ShowItemTooltip(
                    "0で一斉に、大きいほど1枚ずつ順番に現れます");
            }
            if (effect == SceneTransitionEffect::Iris
                || effect == SceneTransitionEffect::Diamond
                || (shader
                    && (pattern == SceneTransitionShaderPattern::Clock
                        || pattern
                            == SceneTransitionShaderPattern::Spiral
                        || pattern
                            == SceneTransitionShaderPattern::Ripple
                        || pattern
                            == SceneTransitionShaderPattern::Heart)))
            {
                ImGui::SetNextItemWidth(160.0f);
                TrackContinuous(
                    ImGui::SliderFloat2(
                        "中心",
                        &settings.focus.x,
                        0.0f,
                        1.0f,
                        "%.2f"),
                    result);
                ShowItemTooltip(
                    "画面に対する比率です（左上が0,0）。C++から"
                    "プレイヤーの画面位置を入れると、そこへ向かって閉じます");
            }
            if (effect != SceneTransitionEffect::Fade
                && effect != SceneTransitionEffect::Iris
                && effect != SceneTransitionEffect::Diamond
                && effect != SceneTransitionEffect::Shutter)
            {
                TrackDiscrete(
                    ImGui::Checkbox(
                        "開くときは通り抜ける",
                        &settings.passThrough),
                    result);
                ShowItemTooltip(
                    "オンなら覆いが同じ向きへ抜けていき、オフなら"
                    "来た方向へ巻き戻すように開きます");
            }

            ImGui::SeparatorText("動作");
            TrackDiscrete(
                ImGui::Checkbox(
                    "覆っている間は読み込み画面を重ねる",
                    &settings.showLoadingScreen),
                result);
            ShowItemTooltip(
                "覆い終えても読み込みが続いているときだけ表示します");
            TrackDiscrete(
                ImGui::Checkbox(
                    "BGMも一緒にフェードする",
                    &settings.fadeMusic),
                result);
            ShowItemTooltip(
                "MusicバスのAudioSourceの音量を、覆い具合に合わせて"
                "下げて戻します（設定画面の音量は変わりません）");
        }
        TrackDiscrete(
            ImGui::Checkbox(
                "遷移中はUI Buttonを押せなくする",
                &settings.blockInput),
            result);

        if (showPreviewButton
            && effect != SceneTransitionEffect::None)
        {
            if (ImGui::Button("Gameビューでプレビュー"))
            {
                result.previewRequested = true;
            }
            ShowItemTooltip(
                "シーンを切り替えずに、覆って開くまでを再生します");
        }
        if (result.changed)
        {
            settings = SanitizeSceneTransition(settings);
        }
        return result;
    }

    SceneTransitionEditResult DrawSceneLoadingScreenEditor(
        SceneLoadingScreenSettings& settings)
    {
        SceneTransitionEditResult result;
        TrackDiscrete(
            ImGui::Checkbox("読み込み画面を表示", &settings.enabled),
            result);
        if (!settings.enabled)
        {
            ImGui::TextDisabled(
                "独自のUIで読み込みを表示する場合はオフにします。");
            return result;
        }

        std::array<char, 256> message{};
        strncpy_s(
            message.data(),
            message.size(),
            settings.message.c_str(),
            _TRUNCATE);
        const bool messageEdited = ImGui::InputText(
            "メッセージ",
            message.data(),
            message.size());
        if (messageEdited)
        {
            settings.message = message.data();
        }
        TrackContinuous(messageEdited, result);

        std::array<char, 256> hint{};
        strncpy_s(
            hint.data(),
            hint.size(),
            settings.hint.c_str(),
            _TRUNCATE);
        const bool hintEdited = ImGui::InputTextWithHint(
            "ヒント",
            "（表示しない）",
            hint.data(),
            hint.size());
        if (hintEdited)
        {
            settings.hint = hint.data();
        }
        TrackContinuous(hintEdited, result);
        ShowItemTooltip("進捗バーの下に小さく表示する補足です");

        TrackDiscrete(
            ImGui::Checkbox(
                "パーセントを表示",
                &settings.showPercentage),
            result);
        TrackDiscrete(
            ImGui::Checkbox(
                "回転インジケーターを表示",
                &settings.showSpinner),
            result);
        TrackDiscrete(
            ImGui::Checkbox(
                "進捗バーを滑らかに動かす",
                &settings.smoothProgress),
            result);

        AssetPathInput(
            "背景画像",
            "（背景色だけ）",
            settings.backgroundTexture,
            [](const std::filesystem::path& path)
            {
                return IsTextureAsset(path);
            },
            result);
        ShowItemTooltip(
            "縦横比を保ったまま画面全体を覆うように表示します");
        TrackContinuous(
            ImGui::ColorEdit4("背景色", &settings.backgroundColor.x),
            result);
        TrackContinuous(
            ImGui::ColorEdit4(
                "バーの背景",
                &settings.barBackgroundColor.x),
            result);
        TrackContinuous(
            ImGui::ColorEdit4("バーの色", &settings.barFillColor.x),
            result);
        TrackContinuous(
            ImGui::ColorEdit4("文字の色", &settings.textColor.x),
            result);
        ImGui::SetNextItemWidth(160.0f);
        TrackContinuous(
            ImGui::DragFloat(
                "表示のフェード",
                &settings.fadeDuration,
                0.01f,
                0.0f,
                2.0f,
                "%.2f 秒"),
            result);
        ShowItemTooltip(
            "遷移演出と組み合わせたときに、読み込み画面を"
            "ふわっと出し入れする時間です");
        return result;
    }
}
