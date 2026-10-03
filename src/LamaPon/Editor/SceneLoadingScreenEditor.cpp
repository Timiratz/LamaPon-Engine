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
    // 直前の項目へ遅延付きの説明を表示する(text: 表示する説明文)。
    void ShowItemTooltip(const char* text)
    {
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled
                | ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    // 継続入力の変更を記録し操作終了時だけUndoへの確定を通知する(edited: 当該項目が変更されたか, result: 変更と確定を集める編集結果)。
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

    // チェックや選択の変更を即時確定する(edited: 当該項目が変更されたか, result: 変更と確定を集める編集結果)。
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

    // assets相対パスを編集し条件に合うドロップだけを即時確定する(label: 入力欄の表示名, hint: 空欄に表示する説明, path: 編集するassetのパス, accepts: ドロップを判定する処理, result: 変更と確定を集める編集結果)。
    template <typename Predicate>
    void AssetPathInput(
        const char* label,
        const char* hint,
        std::filesystem::path& path,
        Predicate accepts,
        SceneLoadingScreenEditResult& result)
    {
        // assets相対パスの入力バッファ
        std::array<char, 512> buffer{};
        // 編集中のパスのUTF8表記
        const auto text = PathToUtf8(path);
        strncpy_s(
            buffer.data(),
            buffer.size(),
            text.c_str(),
            _TRUNCATE);
        // assetパスが入力で変わったか
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
            // Asset Browserから渡された文書
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                // ドロップされたassets相対パス
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
        // 変更とUndo確定を通知する編集結果
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

        // 読込画面の文言の入力バッファ
        std::array<char, 256> message{};
        strncpy_s(
            message.data(),
            message.size(),
            settings.message.c_str(),
            _TRUNCATE);
        // 文言が入力で変わったか
        const bool messageEdited = ImGui::InputText(
            "メッセージ",
            message.data(),
            message.size());
        if (messageEdited)
        {
            settings.message = message.data();
        }
        TrackContinuous(messageEdited, result);

        // 読込画面の補足の入力バッファ
        std::array<char, 256> hint{};
        strncpy_s(
            hint.data(),
            hint.size(),
            settings.hint.c_str(),
            _TRUNCATE);
        // 補足が入力で変わったか
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

        // textureとして扱えるassetだけを受け付ける(path: ドロップされたassetパス)。
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
