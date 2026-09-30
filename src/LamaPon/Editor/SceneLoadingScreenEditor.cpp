#include "LamaPon/Editor/SceneLoadingScreenEditor.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Editor/EditorLayerShared.h"

#include <imgui.h>

#include <array>
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
        SceneLoadingScreenEditResult& result)
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
        SceneLoadingScreenEditResult& result)
    {
        if (edited)
        {
            result.changed = true;
            result.committed = true;
        }
    }

    // assets相対のパスを入力します。Asset Browserからのドロップも
    // 受け付け、acceptsが偽を返すファイルは無視します。
    template <typename Predicate>
    void AssetPathInput(
        const char* label,
        const char* hint,
        std::filesystem::path& path,
        Predicate accepts,
        SceneLoadingScreenEditResult& result)
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
    SceneLoadingScreenEditResult DrawSceneLoadingScreenEditor(
        SceneLoadingScreenSettings& settings)
    {
        SceneLoadingScreenEditResult result;
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
            "時間のあるシーン遷移と組み合わせたときに、読み込み画面を"
            "ふわっと出し入れする時間です");
        return result;
    }
}
