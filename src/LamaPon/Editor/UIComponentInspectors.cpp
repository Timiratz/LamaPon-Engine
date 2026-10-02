#include "LamaPon/Editor/UIComponentInspectors.h"
#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include <imgui.h>
#include <array>
#include <cstring>
#include <exception>
#include <iterator>

using namespace LamaPon;
using namespace LamaPon::EditorDetail;

namespace
{
    // 無効な項目も含め、ホバーした項目の説明を表示します(text: 表示する説明)。
    void ShowItemTooltip(const char* text)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    // キャンバスの基準解像度と追従比率を編集します(canvas: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UICanvasComponent& canvas, const UIInspectorContext& context)
    {
        // キャンバスの基準解像度・px
        auto resolution =
            canvas.
                ReferenceResolution();
        if (ImGui::DragFloat2(
                "基準解像度",
                &resolution.x,
                1.0f,
                1.0f,
                8192.0f,
                "%.0f px"))
        {
            canvas.
                SetReferenceResolution(
                    resolution);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }

        // 幅0・高さ1の解像度追従比率
        float match =
            canvas.
                MatchWidthOrHeight();
        if (ImGui::SliderFloat(
                "幅 ↔ 高さ",
                &match,
                0.0f,
                1.0f,
                "%.2f"))
        {
            canvas.
                SetMatchWidthOrHeight(
                    match);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "0: 幅に一致 / 1: 高さに一致");
        ImGui::TextDisabled(
            "子UIのアンカーとサイズを画面解像度へ追従させます。");
    }

    // UI矩形のアンカー・位置・寸法を編集します(uiTransform: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UIRectTransformComponent& uiTransform, const UIInspectorContext& context)
    {
        // アンカープリセットの表示名
        const char* presets[]{
            "中央",
            "左上",
            "右上",
            "左下",
            "右下",
            "全画面Stretch"
        };
        // 選択プリセットの番号
        int selectedPreset = -1;
        if (ImGui::Combo(
                "アンカープリセット",
                &selectedPreset,
                presets,
                static_cast<int>(
                    std::size(presets))))
        {
            // プリセットのアンカー位置
            DirectX::XMFLOAT2 anchor{
                0.5f, 0.5f };
            // プリセットに応じてアンカーを設定します。
            switch (selectedPreset)
            {
            case 1:
                anchor = { 0.0f, 0.0f };
                break;
            case 2:
                anchor = { 1.0f, 0.0f };
                break;
            case 3:
                anchor = { 0.0f, 1.0f };
                break;
            case 4:
                anchor = { 1.0f, 1.0f };
                break;
            case 5:
                uiTransform.SetAnchorMin(
                    { 0.0f, 0.0f });
                uiTransform.SetAnchorMax(
                    { 1.0f, 1.0f });
                uiTransform.SetPivot(
                    { 0.5f, 0.5f });
                uiTransform.SetAnchoredPosition(
                    {});
                uiTransform.SetSizeDelta(
                    {});
                context.recordHistory();
                break;
            default:
                break;
            }
            if (selectedPreset >= 0
                && selectedPreset < 5)
            {
                uiTransform.SetAnchorMin(
                    anchor);
                uiTransform.SetAnchorMax(
                    anchor);
                uiTransform.SetPivot(
                    anchor);
                context.recordHistory();
            }
        }

        // アンカー領域の左上
        auto anchorMin =
            uiTransform.AnchorMin();
        if (ImGui::DragFloat2(
                "Anchor Min",
                &anchorMin.x,
                0.01f,
                0.0f,
                1.0f,
                "%.2f"))
        {
            uiTransform.
                SetAnchorMin(
                    anchorMin);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // アンカー領域の右下
        auto anchorMax =
            uiTransform.AnchorMax();
        if (ImGui::DragFloat2(
                "Anchor Max",
                &anchorMax.x,
                0.01f,
                0.0f,
                1.0f,
                "%.2f"))
        {
            uiTransform.
                SetAnchorMax(
                    anchorMax);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // 矩形内の基準点
        auto pivot =
            uiTransform.Pivot();
        if (ImGui::DragFloat2(
                "Pivot",
                &pivot.x,
                0.01f,
                0.0f,
                1.0f,
                "%.2f"))
        {
            uiTransform.SetPivot(
                pivot);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // アンカーからの位置・px
        auto anchoredPosition =
            uiTransform.
                AnchoredPosition();
        if (ImGui::DragFloat2(
                "Anchored Position",
                &anchoredPosition.x,
                1.0f,
                -8192.0f,
                8192.0f,
                "%.0f px"))
        {
            uiTransform.
                SetAnchoredPosition(
                    anchoredPosition);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // アンカー領域との差分寸法・px
        auto sizeDelta =
            uiTransform.SizeDelta();
        if (ImGui::DragFloat2(
                "Size Delta",
                &sizeDelta.x,
                1.0f,
                -8192.0f,
                8192.0f,
                "%.0f px"))
        {
            uiTransform.
                SetSizeDelta(
                    sizeDelta);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }

        // 表示領域で解決した矩形
        const auto preview =
            uiTransform.Resolve(
                static_cast<float>(
                    context.viewportWidth),
                static_cast<float>(
                    context.viewportHeight));
        // 解決後の矩形寸法・px
        const auto previewSize =
            preview.Size();
        ImGui::TextDisabled(
            "計算結果: (%.0f, %.0f)  %.0f x %.0f",
            preview.minimum.x,
            preview.minimum.y,
            previewSize.x,
            previewSize.y);
    }

    // ボタンの外観とクリック時の動作を編集します(button: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UIButtonComponent& button, const UIInspectorContext& context)
    {
        // ボタンのラベル入力バッファ
        std::array<char, 256>
            labelBuffer{};
        strncpy_s(
            labelBuffer.data(),
            labelBuffer.size(),
            button.Label().c_str(),
            _TRUNCATE);
        if (ImGui::InputText(
                "ラベル",
                labelBuffer.data(),
                labelBuffer.size()))
        {
            button.SetLabel(
                labelBuffer.data());
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }

        // 文字の表示サイズ・px
        float fontSize =
            button.FontSize();
        if (ImGui::SliderFloat(
                "文字サイズ",
                &fontSize,
                8.0f,
                128.0f,
                "%.0f px"))
        {
            button.SetFontSize(
                fontSize);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }

        // ボタンを操作できるか
        bool interactable =
            button.Interactable();
        if (ImGui::Checkbox(
                "操作可能",
                &interactable))
        {
            button.SetInteractable(
                interactable);
            context.recordHistory();
        }

        // ボタンの通常色RGBA
        auto normal =
            button.NormalColor();
        if (ImGui::ColorEdit4(
                "通常色",
                &normal.x,
                ImGuiColorEditFlags_AlphaBar))
        {
            button.SetNormalColor(
                normal);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // ボタンのホバー色RGBA
        auto hovered =
            button.HoveredColor();
        if (ImGui::ColorEdit4(
                "Hover色",
                &hovered.x,
                ImGuiColorEditFlags_AlphaBar))
        {
            button.SetHoveredColor(
                hovered);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // ボタンの押下色RGBA
        auto pressed =
            button.PressedColor();
        if (ImGui::ColorEdit4(
                "Pressed色",
                &pressed.x,
                ImGuiColorEditFlags_AlphaBar))
        {
            button.SetPressedColor(
                pressed);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // ボタンの無効色RGBA
        auto disabled =
            button.DisabledColor();
        if (ImGui::ColorEdit4(
                "Disabled色",
                &disabled.x,
                ImGuiColorEditFlags_AlphaBar))
        {
            button.SetDisabledColor(
                disabled);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // ボタンの文字色RGBA
        auto textColor =
            button.TextColor();
        if (ImGui::ColorEdit4(
                "文字色",
                &textColor.x,
                ImGuiColorEditFlags_AlphaBar))
        {
            button.SetTextColor(
                textColor);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }

        ImGui::Text(
            "状態: %s",
            !button.Interactable()
                ? "Disabled"
                : button.IsPressed()
                    ? "Pressed"
                    : button.IsHovered()
                        ? "Hovered"
                        : "Normal");
        // 背景画像パスの表示文字列
        const auto buttonTexture =
            PathToUtf8(
                button.TexturePath());
        ImGui::TextWrapped(
            "背景画像: %s",
            buttonTexture.empty()
                ? "単色"
                : buttonTexture.c_str());
        ImGui::Button(
            "画像をここへドロップ##UIButton",
            ImVec2{ -1.0f, 0.0f });
        if (ImGui::BeginDragDropTarget())
        {
            // ドロップされた資産のデータ
            if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    AssetPayload))
            {
                // ドロップされた資産パス
                const auto dropped =
                    PathFromUtf8(
                        static_cast<
                            const char*>(
                                payload->Data));
                if (IsTextureAsset(dropped))
                {
                    try
                    {
                        button.
                            SetTexturePath(
                                dropped);
                        context.recordHistory();
                    }
                    // 画像設定の失敗を通知します(exception: 失敗理由)。
                    catch (const std::exception&
                        exception)
                    {
                        context.setStatus(
                            exception.what(),
                            true);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (!buttonTexture.empty()
            && ImGui::Button(
                "背景画像を解除"))
        {
            button.SetTexturePath({});
            context.recordHistory();
        }
        ImGui::SeparatorText(
            "クリック時のイベント");
        {
            // クリック時のイベント名入力
            std::array<char, 64> eventBuffer{};
            strncpy_s(
                eventBuffer.data(),
                eventBuffer.size(),
                button.ClickEventName().c_str(),
                _TRUNCATE);
            if (ImGui::InputTextWithHint(
                    "イベント名",
                    "（発行しない）",
                    eventBuffer.data(),
                    eventBuffer.size()))
            {
                button.SetClickEventName(
                    eventBuffer.data());
            }
            if (ImGui::
                IsItemDeactivatedAfterEdit())
            {
                context.recordHistory();
            }
            ShowItemTooltip(
                "クリック時にこの名前のイベントを発行します。"
                "C++ Scriptの On(\"イベント名\", ...) で受信できます");
        }
        ImGui::SeparatorText(
            "クリック時のScene操作");
        // 現在のシーンを再読込するか
        bool reloadCurrent =
            button.
                ReloadCurrentScene();
        if (ImGui::Checkbox(
                "現在のSceneを再読み込み",
                &reloadCurrent))
        {
            button.
                SetReloadCurrentScene(
                    reloadCurrent);
            context.recordHistory();
        }
        if (!reloadCurrent)
        {
            // 移動先シーンの表示パス
            const auto targetScene =
                PathToUtf8(
                    button.TargetScene());
            ImGui::TextWrapped(
                "移動先: %s",
                targetScene.empty()
                    ? "未設定"
                    : targetScene.c_str());
            ImGui::Button(
                "Sceneをここへドロップ##UIButtonScene",
                ImVec2{ -1.0f, 0.0f });
            if (ImGui::
                BeginDragDropTarget())
            {
                // ドロップされた資産のデータ
                if (const ImGuiPayload*
                    payload =
                        ImGui::
                            AcceptDragDropPayload(
                                AssetPayload))
                {
                    // ドロップされた資産パス
                    const auto dropped =
                        PathFromUtf8(
                            static_cast<
                                const char*>(
                                    payload->Data));
                    if (IsSceneAsset(
                            dropped))
                    {
                        button.
                            SetTargetScene(
                                dropped);
                        context.recordHistory();
                    }
                }
                ImGui::
                    EndDragDropTarget();
            }
            if (IsSceneAsset(
                    context.selectedAsset)
                && ImGui::Button(
                    "選択中のSceneを設定"))
            {
                button.SetTargetScene(
                    context.selectedAsset);
                context.recordHistory();
            }
            if (!targetScene.empty()
                && ImGui::Button(
                    "移動先を解除"))
            {
                button.SetTargetScene(
                    {});
                context.recordHistory();
            }
            // 移動先を追加読込するか
            bool loadAdditive =
                button.
                    LoadTargetAdditive();
            if (ImGui::Checkbox(
                    "追加読み込み（Additive）で開く",
                    &loadAdditive))
            {
                button.
                    SetLoadTargetAdditive(
                        loadAdditive);
                context.recordHistory();
            }
            ShowItemTooltip(
                "オンにすると、今のSceneを消さずに移動先を"
                "重ねて読み込みます。ポーズ画面や設定画面に"
                "向いています");
        }
        if (reloadCurrent
            || (!button.TargetScene().empty()
                && !button.LoadTargetAdditive()))
        {
            ImGui::TextDisabled(
                "切り替えには既定の遷移を使います（既定はすぐ切り替え）。");
            ShowItemTooltip(
                "ScriptからScenes().SetDefaultTransition(...)で"
                "覆う・開く時間を指定できます。画面を覆う絵は"
                "Scenes().TransitionCoverage()を読んで自分で描きます");
        }

        // ボタンの描画順
        auto buttonSortOrder =
            button.SortOrder();
        if (ImGui::InputInt(
                "描画順",
                &buttonSortOrder))
        {
            button.SetSortOrder(
                buttonSortOrder);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "数値が大きいほど手前に表示されます");

        ImGui::TextDisabled(
            "C++: button.ConsumeClick() でクリックを取得できます。");
    }

    // 画像と9-slice境界を編集します(image: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UIImageComponent& image, const UIInspectorContext& context)
    {
        if (!image.TexturePath().empty())
        {
            ImGui::TextWrapped(
                "テクスチャ: %s",
                image.TexturePath()
                    .u8string().c_str());
            if (ImGui::Button(
                    "テクスチャを解除##UIImage"))
            {
                image.SetTexturePath({});
                context.recordHistory();
            }
        }
        else
        {
            ImGui::TextDisabled(
                "テクスチャ未設定（白で塗りつぶし）");
        }
        // 画像選択の変更値とUndo確定
        const auto pickedImageTexture =
            context.pickRenderTexture(
                "UIImageRenderTexture",
                image.RenderTexture());
        if (pickedImageTexture.value.has_value())
        {
            image.SetRenderTexture(
                *pickedImageTexture.value);
        }
        if (pickedImageTexture.commit)
        {
            context.recordHistory();
        }
        // 画像の表示色RGBA
        auto imageColor = image.Color();
        if (ImGui::ColorEdit4(
                "色##UIImage",
                &imageColor.x))
        {
            image.SetColor(imageColor);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // 左上右下の9-slice境界・px
        auto border = image.Border();
        if (ImGui::DragFloat4(
                "9-slice境界",
                &border.x,
                1.0f,
                0.0f,
                512.0f,
                "%.0f px"))
        {
            image.SetBorder(border);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "左・上・右・下の順。0で通常描画");
        // 画像の描画順
        auto imageSortOrder =
            image.SortOrder();
        if (ImGui::InputInt(
                "描画順##UIImage",
                &imageSortOrder))
        {
            image.SetSortOrder(
                imageSortOrder);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
    }

    // トグルの値と外観を編集します(toggle: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UIToggleComponent& toggle, const UIInspectorContext& context)
    {
        // トグルがオンか
        bool isOn = toggle.IsOn();
        if (ImGui::Checkbox(
                "オン##UIToggle",
                &isOn))
        {
            toggle.SetIsOn(isOn);
            context.recordHistory();
        }
        // トグルのラベル入力バッファ
        std::array<char, 256>
            toggleLabel{};
        strncpy_s(
            toggleLabel.data(),
            toggleLabel.size(),
            toggle.Label().c_str(),
            _TRUNCATE);
        if (ImGui::InputText(
                "ラベル##UIToggle",
                toggleLabel.data(),
                toggleLabel.size()))
        {
            toggle.SetLabel(
                toggleLabel.data());
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // トグルを操作できるか
        bool toggleInteractable =
            toggle.Interactable();
        if (ImGui::Checkbox(
                "操作可能##UIToggle",
                &toggleInteractable))
        {
            toggle.SetInteractable(
                toggleInteractable);
            context.recordHistory();
        }
        // トグルのボックス色RGBA
        auto boxColor = toggle.BoxColor();
        if (ImGui::ColorEdit4(
                "ボックス色",
                &boxColor.x))
        {
            toggle.SetBoxColor(boxColor);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // トグルのチェック色RGBA
        auto checkColor =
            toggle.CheckColor();
        if (ImGui::ColorEdit4(
                "チェック色",
                &checkColor.x))
        {
            toggle.SetCheckColor(
                checkColor);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // トグルの描画順
        auto toggleSortOrder =
            toggle.SortOrder();
        if (ImGui::InputInt(
                "描画順##UIToggle",
                &toggleSortOrder))
        {
            toggle.SetSortOrder(
                toggleSortOrder);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "C++: toggle.ConsumeValueChanged() / IsOn()");
    }

    // スライダーの値域と外観を編集します(slider: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UISliderComponent& slider, const UIInspectorContext& context)
    {
        // スライダーの最小値
        float minimumValue =
            slider.MinimumValue();
        // スライダーの最大値
        float maximumValue =
            slider.MaximumValue();
        if (ImGui::DragFloat(
                "最小値",
                &minimumValue,
                0.1f))
        {
            slider.SetRange(
                minimumValue,
                maximumValue);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        if (ImGui::DragFloat(
                "最大値",
                &maximumValue,
                0.1f))
        {
            slider.SetRange(
                minimumValue,
                maximumValue);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // スライダーの現在値
        float sliderValue = slider.Value();
        if (ImGui::SliderFloat(
                "値##UISlider",
                &sliderValue,
                slider.MinimumValue(),
                slider.MaximumValue()))
        {
            slider.SetValue(sliderValue);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // スライダーを整数値にするか
        bool wholeNumbers =
            slider.WholeNumbers();
        if (ImGui::Checkbox(
                "整数のみ",
                &wholeNumbers))
        {
            slider.SetWholeNumbers(
                wholeNumbers);
            context.recordHistory();
        }
        // スライダーを操作できるか
        bool sliderInteractable =
            slider.Interactable();
        if (ImGui::Checkbox(
                "操作可能##UISlider",
                &sliderInteractable))
        {
            slider.SetInteractable(
                sliderInteractable);
            context.recordHistory();
        }
        // スライダーのフィル色RGBA
        auto fillColor =
            slider.FillColor();
        if (ImGui::ColorEdit4(
                "フィル色",
                &fillColor.x))
        {
            slider.SetFillColor(fillColor);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // スライダーの描画順
        auto sliderSortOrder =
            slider.SortOrder();
        if (ImGui::InputInt(
                "描画順##UISlider",
                &sliderSortOrder))
        {
            slider.SetSortOrder(
                sliderSortOrder);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "C++: slider.Value() / ConsumeValueChanged()");
    }

    // テキスト欄の値と入力制約を編集します(inputField: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UIInputFieldComponent& inputField, const UIInspectorContext& context)
    {
        // テキスト入力用バッファ
        std::array<char, 512> textBuffer{};
        strncpy_s(
            textBuffer.data(),
            textBuffer.size(),
            inputField.Text().c_str(),
            _TRUNCATE);
        if (ImGui::InputText(
                "テキスト##UIInputField",
                textBuffer.data(),
                textBuffer.size()))
        {
            inputField.SetText(
                textBuffer.data());
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // 未入力時の表示文字列バッファ
        std::array<char, 256>
            placeholderBuffer{};
        strncpy_s(
            placeholderBuffer.data(),
            placeholderBuffer.size(),
            inputField.Placeholder()
                .c_str(),
            _TRUNCATE);
        if (ImGui::InputText(
                "プレースホルダー",
                placeholderBuffer.data(),
                placeholderBuffer.size()))
        {
            inputField.SetPlaceholder(
                placeholderBuffer.data());
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // 入力文字数の上限
        int maxLength = static_cast<int>(
            inputField.MaxLength());
        if (ImGui::InputInt(
                "最大文字数",
                &maxLength))
        {
            inputField.SetMaxLength(
                static_cast<std::size_t>(
                    std::max(maxLength, 1)));
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // テキスト欄を操作できるか
        bool fieldInteractable =
            inputField.Interactable();
        if (ImGui::Checkbox(
                "操作可能##UIInputField",
                &fieldInteractable))
        {
            inputField.SetInteractable(
                fieldInteractable);
            context.recordHistory();
        }
        // テキスト欄の描画順
        auto fieldSortOrder =
            inputField.SortOrder();
        if (ImGui::InputInt(
                "描画順##UIInputField",
                &fieldSortOrder))
        {
            inputField.SetSortOrder(
                fieldSortOrder);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "C++: field.Text() / ConsumeSubmit()");
    }

    // 子UIの配置方向と余白を編集します(layoutGroup: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UILayoutGroupComponent& layoutGroup, const UIInspectorContext& context)
    {
        // 配置方向・0水平、1垂直
        int axis = layoutGroup.Axis()
            == UILayoutAxis::Horizontal
            ? 0
            : 1;
        if (ImGui::Combo(
                "並び方向",
                &axis,
                "水平\0垂直\0"))
        {
            layoutGroup.SetAxis(
                axis == 0
                    ? UILayoutAxis::Horizontal
                    : UILayoutAxis::Vertical);
            context.recordHistory();
        }
        // 子要素の間隔・px
        float spacing =
            layoutGroup.Spacing();
        if (ImGui::DragFloat(
                "間隔",
                &spacing,
                0.5f,
                0.0f,
                512.0f,
                "%.0f px"))
        {
            layoutGroup.SetSpacing(spacing);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // 左上右下の余白・px
        auto padding =
            layoutGroup.Padding();
        if (ImGui::DragFloat4(
                "余白",
                &padding.x,
                0.5f,
                0.0f,
                512.0f,
                "%.0f px"))
        {
            layoutGroup.SetPadding(padding);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "左・上・右・下の順");
        // 子要素の揃え位置
        int alignment = static_cast<int>(
            layoutGroup.ChildAlignment());
        if (ImGui::Combo(
                "子の揃え",
                &alignment,
                "先頭\0中央\0末尾\0"))
        {
            layoutGroup.SetChildAlignment(
                static_cast<
                    UILayoutAlignment>(
                        alignment));
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "直下の子のUI Rect Transformを自動整列します");
    }

    // スクロールの速度と外観を編集します(scrollView: 編集対象, context: 描画中だけ借用する操作)。
    void Draw(UIScrollViewComponent& scrollView, const UIInspectorContext& context)
    {
        ImGui::TextDisabled(
            "コンテンツ高さ: %.0f / スクロール: %.0f",
            scrollView.ContentHeight(),
            scrollView.ScrollOffset());
        // ホイール一操作の移動量・px
        float scrollSpeed =
            scrollView.ScrollSpeed();
        if (ImGui::DragFloat(
                "ホイール速度",
                &scrollSpeed,
                1.0f,
                1.0f,
                512.0f,
                "%.0f px"))
        {
            scrollView.SetScrollSpeed(
                scrollSpeed);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // スクロール領域を操作できるか
        bool scrollInteractable =
            scrollView.Interactable();
        if (ImGui::Checkbox(
                "操作可能##UIScrollView",
                &scrollInteractable))
        {
            scrollView.SetInteractable(
                scrollInteractable);
            context.recordHistory();
        }
        // スクロール領域の背景色RGBA
        auto scrollBackground =
            scrollView.BackgroundColor();
        if (ImGui::ColorEdit4(
                "背景色##UIScrollView",
                &scrollBackground.x))
        {
            scrollView.SetBackgroundColor(
                scrollBackground);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // スクロールバーの色RGBA
        auto scrollbarColor =
            scrollView.ScrollbarColor();
        if (ImGui::ColorEdit4(
                "バー色##UIScrollView",
                &scrollbarColor.x))
        {
            scrollView.SetScrollbarColor(
                scrollbarColor);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        // スクロール領域の描画順
        auto scrollSortOrder =
            scrollView.SortOrder();
        if (ImGui::InputInt(
                "描画順##UIScrollView",
                &scrollSortOrder))
        {
            scrollView.SetSortOrder(
                scrollSortOrder);
        }
        if (ImGui::
            IsItemDeactivatedAfterEdit())
        {
            context.recordHistory();
        }
        ImGui::TextDisabled(
            "子GameObjectがコンテンツになり、"
            "表示領域外はクリッピングされます");
    }
}

namespace LamaPon
{
    // 対応するUI型のInspectorを描画し、未対応なら変更せずfalseを返します。
    bool DrawUIComponentInspector(Component& component, const UIInspectorContext& context)
    {
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UICanvasComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UIRectTransformComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UIButtonComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UIImageComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UIToggleComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UISliderComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UIInputFieldComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UILayoutGroupComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        // 対応型へ変換済みの借用参照
        if (auto* typed = dynamic_cast<UIScrollViewComponent*>(&component))
        {
            Draw(*typed, context);
            return true;
        }
        return false;
    }
}
