#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorLayerShared.h"

#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/Scene.h"

#include <commdlg.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace LamaPon::EditorDetail;

namespace LamaPon
{
    void EditorLayer::DrawAnimationTimeline()
    {
        if (!m_animationTimelineOpen)
        {
            return;
        }

        bool windowOpen = true;
        ImGui::SetNextWindowSize(
            ImVec2{ 760.0f, 560.0f },
            ImGuiCond_FirstUseEver);
        const std::string title =
            std::string{ "Animation Timeline" }
            + (m_animationTimelineDirty
                ? " *"
                : "")
            + "###AnimationTimeline";
        const bool visible = ImGui::Begin(
            title.c_str(),
            &windowOpen);

        auto* target = m_scene.FindGameObject(
            m_animationTimelineTargetId);
        if (target == nullptr)
        {
            if (visible)
            {
                ImGui::TextDisabled(
                    "対象GameObjectが存在しません。");
            }
            ImGui::End();
            CloseAnimationTimeline(false);
            return;
        }

        if (visible)
        {
            ImGui::TextWrapped(
                "Clip: %s",
                PathToUtf8(
                    m_animationTimelineClipPath).c_str());
            ImGui::Text(
                "対象: %s",
                target->Name().c_str());

            if (!m_animationTimelineError.empty())
            {
                ImGui::TextWrapped(
                    "エラー: %s",
                    m_animationTimelineError.c_str());
            }

            ImGui::BeginDisabled(m_playing);
            if (ImGui::Button("保存"))
            {
                SaveAnimationTimeline();
            }
            ImGui::SameLine();
            if (ImGui::Button("再読み込み"))
            {
                try
                {
                    LoadAnimationTimeline(
                        m_animationTimelineClipPath);
                    PreviewAnimationTimeline();
                    SetStatus(
                        "Animation Timelineを再読み込みしました");
                }
                catch (const std::exception& exception)
                {
                    m_animationTimelineError =
                        exception.what();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(
                "プレビュー前のTransformへ戻す"))
            {
                target->GetTransform() =
                    m_animationTimelineOriginalTransform;
            }

            if (ImGui::InputText(
                    "Clip名",
                    m_animationTimelineName.data(),
                    m_animationTimelineName.size()))
            {
                m_animationTimelineDirty = true;
            }

            float maximumKeyTime{};
            for (const auto& keyframe :
                m_animationTimelineKeyframes)
            {
                maximumKeyTime = std::max(
                    maximumKeyTime,
                    keyframe.time);
            }
            if (ImGui::DragFloat(
                    "長さ（秒）",
                    &m_animationTimelineDuration,
                    0.05f,
                    std::max(0.01f, maximumKeyTime),
                    3600.0f,
                    "%.2f"))
            {
                m_animationTimelineDuration =
                    std::max(
                        m_animationTimelineDuration,
                        std::max(
                            maximumKeyTime,
                            0.01f));
                m_animationTimelineTime =
                    std::min(
                        m_animationTimelineTime,
                        m_animationTimelineDuration);
                m_animationTimelineDirty = true;
            }
            if (ImGui::Checkbox(
                    "ループ",
                    &m_animationTimelineLoop))
            {
                m_animationTimelineDirty = true;
            }

            if (ImGui::SliderFloat(
                    "現在時刻",
                    &m_animationTimelineTime,
                    0.0f,
                    m_animationTimelineDuration,
                    "%.3f秒"))
            {
                PreviewAnimationTimeline();
            }

            ImGui::SeparatorText("Keyframe");
            for (std::size_t index = 0;
                index < m_animationTimelineKeyframes.size();
                ++index)
            {
                ImGui::PushID(
                    static_cast<int>(index));
                const std::string keyLabel =
                    std::to_string(index)
                    + ": "
                    + std::to_string(
                        m_animationTimelineKeyframes[
                            index].time)
                    + "秒";
                if (ImGui::Selectable(
                        keyLabel.c_str(),
                        m_animationTimelineSelectedKey
                            == index))
                {
                    m_animationTimelineSelectedKey =
                        index;
                    m_animationTimelineTime =
                        m_animationTimelineKeyframes[
                            index].time;
                    PreviewAnimationTimeline();
                }
                ImGui::PopID();
            }

            if (ImGui::Button(
                    "現在時刻へKeyframe追加"))
            {
                const bool duplicateTime =
                    std::ranges::any_of(
                        m_animationTimelineKeyframes,
                        [this](
                            const TransformKeyframe& keyframe)
                        {
                            return std::abs(
                                keyframe.time
                                - m_animationTimelineTime)
                                < 0.0001f;
                        });
                if (duplicateTime)
                {
                    m_animationTimelineError =
                        "同じ時刻にKeyframeがあります。";
                }
                else
                {
                    const auto& transform =
                        target->GetTransform();
                    m_animationTimelineKeyframes.push_back(
                        TransformKeyframe{
                            m_animationTimelineTime,
                            TransformAnimationSample{
                                transform.position,
                                transform.EulerAngles(),
                                transform.scale
                            }
                        });
                    std::ranges::sort(
                        m_animationTimelineKeyframes,
                        {},
                        &TransformKeyframe::time);
                    const auto selected =
                        std::ranges::find_if(
                            m_animationTimelineKeyframes,
                            [this](
                                const TransformKeyframe& keyframe)
                            {
                                return std::abs(
                                    keyframe.time
                                    - m_animationTimelineTime)
                                    < 0.0001f;
                            });
                    m_animationTimelineSelectedKey =
                        static_cast<std::size_t>(
                            std::distance(
                                m_animationTimelineKeyframes.begin(),
                                selected));
                    m_animationTimelineDirty = true;
                    m_animationTimelineError.clear();
                }
            }

            const bool validSelection =
                m_animationTimelineSelectedKey
                    < m_animationTimelineKeyframes.size();
            ImGui::SameLine();
            ImGui::BeginDisabled(!validSelection);
            if (ImGui::Button(
                    "選択Keyを現在時刻へ移動"))
            {
                const bool duplicateTime =
                    std::ranges::any_of(
                        m_animationTimelineKeyframes,
                        [this](
                            const TransformKeyframe& keyframe)
                        {
                            return &keyframe
                                    != &m_animationTimelineKeyframes[
                                        m_animationTimelineSelectedKey]
                                && std::abs(
                                    keyframe.time
                                    - m_animationTimelineTime)
                                    < 0.0001f;
                        });
                if (duplicateTime)
                {
                    m_animationTimelineError =
                        "移動先の時刻にKeyframeがあります。";
                }
                else
                {
                    auto moved =
                        m_animationTimelineKeyframes[
                            m_animationTimelineSelectedKey];
                    moved.time =
                        m_animationTimelineTime;
                    m_animationTimelineKeyframes.erase(
                        m_animationTimelineKeyframes.begin()
                        + static_cast<std::ptrdiff_t>(
                            m_animationTimelineSelectedKey));
                    m_animationTimelineKeyframes.push_back(
                        moved);
                    std::ranges::sort(
                        m_animationTimelineKeyframes,
                        {},
                        &TransformKeyframe::time);
                    const auto selected =
                        std::ranges::find_if(
                            m_animationTimelineKeyframes,
                            [this](
                                const TransformKeyframe& keyframe)
                            {
                                return std::abs(
                                    keyframe.time
                                    - m_animationTimelineTime)
                                    < 0.0001f;
                            });
                    m_animationTimelineSelectedKey =
                        static_cast<std::size_t>(
                            std::distance(
                                m_animationTimelineKeyframes.begin(),
                                selected));
                    m_animationTimelineDirty = true;
                    m_animationTimelineError.clear();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(
                m_animationTimelineKeyframes.size() <= 1);
            if (ImGui::Button("選択Keyを削除"))
            {
                m_animationTimelineKeyframes.erase(
                    m_animationTimelineKeyframes.begin()
                    + static_cast<std::ptrdiff_t>(
                        m_animationTimelineSelectedKey));
                m_animationTimelineSelectedKey =
                    std::min(
                        m_animationTimelineSelectedKey,
                        m_animationTimelineKeyframes.size()
                            - 1);
                m_animationTimelineDirty = true;
                PreviewAnimationTimeline();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();

            if (validSelection
                && m_animationTimelineSelectedKey
                    < m_animationTimelineKeyframes.size())
            {
                auto& keyframe =
                    m_animationTimelineKeyframes[
                        m_animationTimelineSelectedKey];
                ImGui::SeparatorText(
                    "選択KeyframeのTransform");

                auto position =
                    keyframe.transform.position;
                if (ImGui::InputFloat3(
                        "位置##AnimationKey",
                        &position.x))
                {
                    keyframe.transform.position =
                        position;
                    m_animationTimelineDirty = true;
                    PreviewAnimationTimeline();
                }

                DirectX::XMFLOAT3 rotationDegrees{
                    DirectX::XMConvertToDegrees(
                        keyframe.transform.rotation.x),
                    DirectX::XMConvertToDegrees(
                        keyframe.transform.rotation.y),
                    DirectX::XMConvertToDegrees(
                        keyframe.transform.rotation.z)
                };
                if (ImGui::InputFloat3(
                        "回転##AnimationKey",
                        &rotationDegrees.x))
                {
                    keyframe.transform.rotation = {
                        DirectX::XMConvertToRadians(
                            rotationDegrees.x),
                        DirectX::XMConvertToRadians(
                            rotationDegrees.y),
                        DirectX::XMConvertToRadians(
                            rotationDegrees.z)
                    };
                    m_animationTimelineDirty = true;
                    PreviewAnimationTimeline();
                }

                auto scale =
                    keyframe.transform.scale;
                if (ImGui::InputFloat3(
                        "拡縮##AnimationKey",
                        &scale.x))
                {
                    keyframe.transform.scale =
                        scale;
                    m_animationTimelineDirty = true;
                    PreviewAnimationTimeline();
                }

                if (ImGui::Button(
                        "現在のGameObject Transformを記録"))
                {
                    const auto& transform =
                        target->GetTransform();
                    keyframe.transform = {
                        transform.position,
                        transform.EulerAngles(),
                        transform.scale
                    };
                    m_animationTimelineDirty = true;
                    PreviewAnimationTimeline();
                }
            }

            ImGui::TextDisabled(
                "Timelineを閉じるとプレビュー前のTransformへ戻ります。");
            ImGui::EndDisabled();
        }

        ImGui::End();
        if (!windowOpen)
        {
            CloseAnimationTimeline(true);
        }
    }

    void EditorLayer::DrawAnimatorControllerGraph()
    {
        if (!m_animatorGraphOpen
            || !m_animatorGraphDocument)
        {
            return;
        }

        bool windowOpen = true;
        const std::string title =
            "Animator Controller"
            + std::string{
                m_animatorGraphDirty ? " *##" : "##"
            }
            + PathToUtf8(
                m_animatorGraphControllerPath);
        ImGui::SetNextWindowSize(
            ImVec2{ 1040.0f, 680.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                title.c_str(),
                &windowOpen,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        auto& document =
            *m_animatorGraphDocument;
        auto& states = document["states"];
        auto& transitions =
            document["transitions"];
        if (!transitions.is_array())
        {
            transitions =
                nlohmann::json::array();
        }
        auto& parameters =
            document["parameters"];
        if (!parameters.is_array())
        {
            parameters =
                nlohmann::json::array();
        }

        const auto addState =
            [this, &states, &document](
                const DirectX::XMFLOAT2 position)
            {
                std::string name = "New State";
                for (std::size_t suffix = 2;; ++suffix)
                {
                    const bool exists =
                        std::ranges::any_of(
                            states,
                            [&name](
                                const nlohmann::json& state)
                            {
                                return state.value(
                                    "name",
                                    std::string{})
                                    == name;
                            });
                    if (!exists)
                    {
                        break;
                    }
                    name = "New State "
                        + std::to_string(suffix);
                }
                states.push_back({
                    { "name", name },
                    { "modelClip", name },
                    { "speed", 1.0f },
                    { "loop", true },
                    {
                        "editorPosition",
                        { position.x, position.y }
                    }
                });
                if (states.size() == 1)
                {
                    document["entry"] = name;
                }
                m_animatorGraphSelectedState =
                    states.size() - 1;
                m_animatorGraphDirty = true;
            };

        if (ImGui::Button("保存  Ctrl+S"))
        {
            try
            {
                SaveAnimatorControllerGraph();
            }
            catch (const std::exception& exception)
            {
                m_animatorGraphError =
                    exception.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            try
            {
                LoadAnimatorControllerGraph();
            }
            catch (const std::exception& exception)
            {
                m_animatorGraphError =
                    exception.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("+ State"))
        {
            addState({
                60.0f
                    - m_animatorGraphScrolling.x,
                60.0f
                    - m_animatorGraphScrolling.y
            });
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "左ドラッグ: State移動 / 中ドラッグ: Canvas移動 / 右クリック: State追加");
        if (!m_animatorGraphError.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "%s",
                m_animatorGraphError.c_str());
        }

        const bool saveShortcut =
            ImGui::GetIO().KeyCtrl
            && ImGui::IsKeyPressed(
                ImGuiKey_S,
                false)
            && ImGui::IsWindowFocused(
                ImGuiFocusedFlags_RootAndChildWindows);
        if (saveShortcut)
        {
            try
            {
                SaveAnimatorControllerGraph();
            }
            catch (const std::exception& exception)
            {
                m_animatorGraphError =
                    exception.what();
            }
        }

        const float inspectorWidth =
            std::clamp(
                ImGui::GetContentRegionAvail().x
                    * 0.34f,
                330.0f,
                430.0f);
        ImGui::BeginChild(
            "AnimatorGraphCanvas",
            ImVec2{
                -inspectorWidth
                    - ImGui::GetStyle().ItemSpacing.x,
                0.0f
            },
            true,
            ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 canvasPosition =
            ImGui::GetWindowPos();
        const ImVec2 canvasSize =
            ImGui::GetWindowSize();
        auto* drawList =
            ImGui::GetWindowDrawList();
        const bool canvasHovered =
            ImGui::IsWindowHovered();
        if (canvasHovered
            && ImGui::IsMouseDragging(
                ImGuiMouseButton_Middle,
                0.0f))
        {
            const auto delta =
                ImGui::GetIO().MouseDelta;
            m_animatorGraphScrolling.x +=
                delta.x;
            m_animatorGraphScrolling.y +=
                delta.y;
        }
        constexpr float gridStep = 32.0f;
        for (float x = std::fmod(
                m_animatorGraphScrolling.x,
                gridStep);
            x < canvasSize.x;
            x += gridStep)
        {
            drawList->AddLine(
                ImVec2{
                    canvasPosition.x + x,
                    canvasPosition.y
                },
                ImVec2{
                    canvasPosition.x + x,
                    canvasPosition.y
                        + canvasSize.y
                },
                IM_COL32(52, 60, 72, 100));
        }
        for (float y = std::fmod(
                m_animatorGraphScrolling.y,
                gridStep);
            y < canvasSize.y;
            y += gridStep)
        {
            drawList->AddLine(
                ImVec2{
                    canvasPosition.x,
                    canvasPosition.y + y
                },
                ImVec2{
                    canvasPosition.x
                        + canvasSize.x,
                    canvasPosition.y + y
                },
                IM_COL32(52, 60, 72, 100));
        }

        constexpr ImVec2 nodeSize{
            180.0f,
            82.0f
        };
        const auto stateScreenPosition =
            [&states,
                &canvasPosition = canvasPosition,
                this](const std::string_view name)
                -> std::optional<ImVec2>
            {
                for (const auto& state : states)
                {
                    if (state.value(
                            "name",
                            std::string{}) != name)
                    {
                        continue;
                    }
                    const auto& position =
                        state["editorPosition"];
                    return ImVec2{
                        canvasPosition.x
                            + m_animatorGraphScrolling.x
                            + position.at(0).get<float>(),
                        canvasPosition.y
                            + m_animatorGraphScrolling.y
                            + position.at(1).get<float>()
                    };
                }
                return std::nullopt;
            };

        for (const auto& transition : transitions)
        {
            const auto from =
                stateScreenPosition(
                    transition.value(
                        "from",
                        std::string{}));
            const auto to =
                stateScreenPosition(
                    transition.value(
                        "to",
                        std::string{}));
            if (!from || !to)
            {
                continue;
            }
            const ImVec2 start{
                from->x + nodeSize.x,
                from->y + nodeSize.y * 0.5f
            };
            const ImVec2 end{
                to->x,
                to->y + nodeSize.y * 0.5f
            };
            const ImU32 color =
                IM_COL32(115, 175, 235, 210);
            drawList->AddBezierCubic(
                start,
                ImVec2{
                    start.x + 65.0f,
                    start.y
                },
                ImVec2{
                    end.x - 65.0f,
                    end.y
                },
                end,
                color,
                2.0f);
            drawList->AddTriangleFilled(
                ImVec2{ end.x, end.y },
                ImVec2{
                    end.x - 9.0f,
                    end.y - 5.0f
                },
                ImVec2{
                    end.x - 9.0f,
                    end.y + 5.0f
                },
                color);
        }

        for (std::size_t index = 0;
            index < states.size();
            ++index)
        {
            auto& state = states[index];
            auto& position =
                state["editorPosition"];
            const ImVec2 screenPosition{
                canvasPosition.x
                    + m_animatorGraphScrolling.x
                    + position.at(0).get<float>(),
                canvasPosition.y
                    + m_animatorGraphScrolling.y
                    + position.at(1).get<float>()
            };
            ImGui::SetCursorScreenPos(
                screenPosition);
            ImGui::PushID(
                static_cast<int>(index));
            ImGui::InvisibleButton(
                "##AnimatorStateNode",
                nodeSize);
            if (ImGui::IsItemClicked())
            {
                m_animatorGraphSelectedState =
                    index;
            }
            if (ImGui::IsItemActive()
                && ImGui::IsMouseDragging(
                    ImGuiMouseButton_Left,
                    0.0f))
            {
                const auto delta =
                    ImGui::GetIO().MouseDelta;
                position[0] =
                    position.at(0).get<float>()
                    + delta.x;
                position[1] =
                    position.at(1).get<float>()
                    + delta.y;
                m_animatorGraphDirty = true;
            }
            const bool selected =
                index
                == m_animatorGraphSelectedState;
            const bool entry =
                document.value(
                    "entry",
                    std::string{})
                == state.value(
                    "name",
                    std::string{});
            const ImU32 bodyColor = selected
                ? IM_COL32(44, 78, 116, 255)
                : IM_COL32(34, 42, 54, 255);
            const ImU32 headerColor = entry
                ? IM_COL32(50, 145, 92, 255)
                : state.contains("blendTree")
                    ? IM_COL32(132, 84, 185, 255)
                    : IM_COL32(52, 94, 146, 255);
            drawList->AddRectFilled(
                screenPosition,
                ImVec2{
                    screenPosition.x
                        + nodeSize.x,
                    screenPosition.y
                        + nodeSize.y
                },
                bodyColor,
                7.0f);
            drawList->AddRectFilled(
                screenPosition,
                ImVec2{
                    screenPosition.x
                        + nodeSize.x,
                    screenPosition.y + 28.0f
                },
                headerColor,
                7.0f,
                ImDrawFlags_RoundCornersTop);
            drawList->AddRect(
                screenPosition,
                ImVec2{
                    screenPosition.x
                        + nodeSize.x,
                    screenPosition.y
                        + nodeSize.y
                },
                selected
                    ? IM_COL32(
                        75,
                        190,
                        255,
                        255)
                    : IM_COL32(
                        87,
                        98,
                        116,
                        255),
                7.0f,
                0,
                selected ? 2.5f : 1.0f);
            drawList->AddText(
                ImVec2{
                    screenPosition.x + 9.0f,
                    screenPosition.y + 6.0f
                },
                IM_COL32_WHITE,
                state.value(
                    "name",
                    std::string{
                        "Unnamed" }).c_str());
            const std::string detail =
                state.contains("blendTree")
                ? state["blendTree"].value(
                        "type",
                        std::string{ "1D" })
                    + " Blend Tree"
                : state.value(
                    "modelClip",
                    state.value(
                        "clip",
                        std::string{
                            "(Clip未設定)" }));
            drawList->AddText(
                ImVec2{
                    screenPosition.x + 9.0f,
                    screenPosition.y + 43.0f
                },
                IM_COL32(
                    190,
                    202,
                    216,
                    255),
                detail.c_str());
            ImGui::PopID();
        }

        if (canvasHovered
            && ImGui::IsMouseClicked(
                ImGuiMouseButton_Right)
            && !ImGui::IsAnyItemHovered())
        {
            ImGui::OpenPopup(
                "AnimatorGraphCanvasMenu");
        }
        if (ImGui::BeginPopup(
                "AnimatorGraphCanvasMenu"))
        {
            if (ImGui::MenuItem("Stateを追加"))
            {
                const auto mouse =
                    ImGui::GetMousePosOnOpeningCurrentPopup();
                addState({
                    mouse.x - canvasPosition.x
                        - m_animatorGraphScrolling.x,
                    mouse.y - canvasPosition.y
                        - m_animatorGraphScrolling.y
                });
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild(
            "AnimatorGraphInspector",
            ImVec2{ 0.0f, 0.0f },
            true);
        ImGui::SeparatorText("Float Parameters");
        std::optional<std::size_t>
            parameterToDelete;
        for (std::size_t index = 0;
            index < parameters.size();
            ++index)
        {
            auto& parameter = parameters[index];
            ImGui::PushID(
                static_cast<int>(index));
            std::array<char, 128> nameBuffer{};
            const auto oldName = parameter.value(
                "name",
                std::string{});
            strcpy_s(
                nameBuffer.data(),
                nameBuffer.size(),
                oldName.c_str());
            ImGui::SetNextItemWidth(-95.0f);
            if (ImGui::InputText(
                    "##ParameterName",
                    nameBuffer.data(),
                    nameBuffer.size()))
            {
                const std::string newName{
                    nameBuffer.data()
                };
                parameter["name"] = newName;
                for (auto& state : states)
                {
                    if (!state.contains("blendTree"))
                    {
                        continue;
                    }

                    auto& blendTree =
                        state["blendTree"];
                    for (const char* key :
                        { "parameter",
                          "parameterX",
                          "parameterY" })
                    {
                        if (blendTree.value(
                                key,
                                std::string{})
                            == oldName)
                        {
                            blendTree[key] =
                                newName;
                        }
                    }
                }
                m_animatorGraphDirty = true;
            }
            ImGui::SameLine();
            float defaultValue =
                parameter.value(
                    "default",
                    0.0f);
            ImGui::SetNextItemWidth(58.0f);
            if (ImGui::DragFloat(
                    "##ParameterDefault",
                    &defaultValue,
                    0.05f))
            {
                parameter["default"] =
                    defaultValue;
                m_animatorGraphDirty = true;
            }
            ImGui::SameLine();
            const bool used =
                std::ranges::any_of(
                    states,
                    [&oldName](
                        const nlohmann::json& state)
                    {
                        if (!state.contains(
                                "blendTree"))
                        {
                            return false;
                        }

                        const auto& blendTree =
                            state["blendTree"];
                        return blendTree.value(
                                   "parameter",
                                   std::string{})
                                == oldName
                            || blendTree.value(
                                   "parameterX",
                                   std::string{})
                                == oldName
                            || blendTree.value(
                                   "parameterY",
                                   std::string{})
                                == oldName;
                    });
            ImGui::BeginDisabled(used);
            if (ImGui::SmallButton("削除"))
            {
                parameterToDelete =
                    index;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (parameterToDelete)
        {
            parameters.erase(
                parameters.begin()
                    + static_cast<
                        nlohmann::json::difference_type>(
                            *parameterToDelete));
            m_animatorGraphDirty = true;
        }
        if (ImGui::Button("+ Float Parameter"))
        {
            std::string name = "Blend";
            std::size_t suffix = 2;
            while (std::ranges::any_of(
                parameters,
                [&name](
                    const nlohmann::json& value)
                {
                    return value.value(
                        "name",
                        std::string{})
                        == name;
                }))
            {
                name = "Blend"
                    + std::to_string(
                        suffix++);
            }
            parameters.push_back({
                { "name", name },
                { "type", "Float" },
                { "default", 0.0f }
            });
            m_animatorGraphDirty = true;
        }

        if (m_animatorGraphSelectedState
            < states.size())
        {
            auto& state =
                states[
                    m_animatorGraphSelectedState];
            ImGui::SeparatorText("State");
            const std::string oldName =
                state.value(
                    "name",
                    std::string{});
            std::array<char, 128> nameBuffer{};
            strcpy_s(
                nameBuffer.data(),
                nameBuffer.size(),
                oldName.c_str());
            if (ImGui::InputText(
                    "名前",
                    nameBuffer.data(),
                    nameBuffer.size()))
            {
                const std::string newName{
                    nameBuffer.data()
                };
                if (!newName.empty())
                {
                    state["name"] = newName;
                    if (document.value(
                            "entry",
                            std::string{})
                        == oldName)
                    {
                        document["entry"] =
                            newName;
                    }
                    for (auto& transition :
                        transitions)
                    {
                        if (transition.value(
                                "from",
                                std::string{})
                            == oldName)
                        {
                            transition["from"] =
                                newName;
                        }
                        if (transition.value(
                                "to",
                                std::string{})
                            == oldName)
                        {
                            transition["to"] =
                                newName;
                        }
                    }
                    m_animatorGraphDirty = true;
                }
            }
            if (document.value(
                    "entry",
                    std::string{})
                != state.value(
                    "name",
                    std::string{}))
            {
                if (ImGui::Button(
                        "Entry Stateに設定"))
                {
                    document["entry"] =
                        state["name"];
                    m_animatorGraphDirty = true;
                }
            }
            else
            {
                ImGui::TextColored(
                    ImVec4{
                        0.35f,
                        0.85f,
                        0.50f,
                        1.0f
                    },
                    "Entry State");
            }

            float speed =
                state.value("speed", 1.0f);
            if (ImGui::DragFloat(
                    "速度",
                    &speed,
                    0.05f,
                    0.01f,
                    100.0f))
            {
                state["speed"] =
                    std::clamp(
                        speed,
                        0.01f,
                        100.0f);
                m_animatorGraphDirty = true;
            }
            bool loop =
                state.value("loop", true);
            if (ImGui::Checkbox(
                    "ループ",
                    &loop))
            {
                state["loop"] = loop;
                m_animatorGraphDirty = true;
            }

            int blendMode{};
            if (state.contains("blendTree"))
            {
                blendMode =
                    state["blendTree"].value(
                        "type",
                        std::string{ "1D" })
                        == "2D"
                    ? 2
                    : 1;
            }
            const char* blendModeNames[]{
                "なし",
                "1D Blend Tree",
                "2D Freeform Cartesian"
            };
            if (ImGui::Combo(
                    "Blend Tree",
                    &blendMode,
                    blendModeNames,
                    static_cast<int>(
                        std::size(
                            blendModeNames))))
            {
                if (blendMode == 0)
                {
                    state.erase("blendTree");
                }
                else
                {
                    if (parameters.empty())
                    {
                        parameters.push_back({
                            { "name", "Blend" },
                            { "type", "Float" },
                            { "default", 0.0f }
                        });
                    }
                    if (!state.contains("blendTree"))
                    {
                        const std::string clip =
                            state.value(
                                "modelClip",
                                state.value(
                                    "name",
                                    std::string{}));
                        state["blendTree"] = {
                            {
                                "children",
                                nlohmann::json::array({
                                    {
                                        {
                                            "modelClip",
                                            clip
                                        },
                                        {
                                            "threshold",
                                            0.0f
                                        },
                                        {
                                            "position",
                                            { 0.0f, 0.0f }
                                        }
                                    },
                                    {
                                        {
                                            "modelClip",
                                            clip
                                        },
                                        {
                                            "threshold",
                                            1.0f
                                        },
                                        {
                                            "position",
                                            { 1.0f, 0.0f }
                                        }
                                    }
                                })
                            }
                        };
                    }
                    auto& blendTree =
                        state["blendTree"];
                    auto& children =
                        blendTree["children"];
                    if (blendMode == 1)
                    {
                        const std::string parameter =
                            blendTree.value(
                                "parameter",
                                blendTree.value(
                                    "parameterX",
                                    parameters.front().value(
                                        "name",
                                        std::string{
                                            "Blend" })));
                        blendTree["type"] = "1D";
                        blendTree["parameter"] =
                            parameter;
                        for (auto& child : children)
                        {
                            if (!child.contains(
                                    "threshold")
                                && child.contains(
                                    "position"))
                            {
                                child["threshold"] =
                                    child["position"]
                                        .at(0);
                            }
                        }
                    }
                    else
                    {
                        if (parameters.size() < 2)
                        {
                            parameters.push_back({
                                { "name", "BlendY" },
                                { "type", "Float" },
                                { "default", 0.0f }
                            });
                        }
                        blendTree["type"] = "2D";
                        blendTree["parameterX"] =
                            blendTree.value(
                                "parameterX",
                                blendTree.value(
                                    "parameter",
                                    parameters.front().value(
                                        "name",
                                        std::string{
                                            "Blend" })));
                        blendTree["parameterY"] =
                            blendTree.value(
                                "parameterY",
                                parameters[1].value(
                                    "name",
                                    std::string{
                                        "BlendY" }));
                        for (auto& child : children)
                        {
                            if (!child.contains(
                                    "position"))
                            {
                                child["position"] = {
                                    child.value(
                                        "threshold",
                                        0.0f),
                                    0.0f
                                };
                            }
                        }
                    }
                }
                m_animatorGraphDirty = true;
            }
            const bool usesBlendTree =
                blendMode != 0;
            const bool uses2DBlendTree =
                blendMode == 2;

            const auto inputString =
                [this](
                    const char* label,
                    nlohmann::json& object,
                    const char* key)
                {
                    std::array<char, 512> buffer{};
                    const auto value = object.value(
                        key,
                        std::string{});
                    strcpy_s(
                        buffer.data(),
                        buffer.size(),
                        value.c_str());
                    if (ImGui::InputText(
                            label,
                            buffer.data(),
                            buffer.size()))
                    {
                        object[key] =
                            std::string{
                                buffer.data()
                            };
                        m_animatorGraphDirty =
                            true;
                    }
                };

            if (!usesBlendTree)
            {
                inputString(
                    "モデル内Clip",
                    state,
                    "modelClip");
                inputString(
                    "外部Clip",
                    state,
                    "clip");
            }
            else
            {
                auto& blendTree =
                    state["blendTree"];
                const auto drawParameterCombo =
                    [this, &parameters, &blendTree](
                        const char* label,
                        const char* key)
                {
                    const std::string current =
                        blendTree.value(
                            key,
                            std::string{});
                    if (!ImGui::BeginCombo(
                            label,
                            current.c_str()))
                    {
                        return;
                    }
                    for (const auto& parameter :
                        parameters)
                    {
                        const auto name =
                            parameter.value(
                                "name",
                                std::string{});
                        if (ImGui::Selectable(
                                name.c_str(),
                                name == current))
                        {
                            blendTree[key] = name;
                            m_animatorGraphDirty =
                                true;
                        }
                    }
                    ImGui::EndCombo();
                };
                drawParameterCombo(
                    uses2DBlendTree
                        ? "X Parameter"
                        : "Blend Parameter",
                    uses2DBlendTree
                        ? "parameterX"
                        : "parameter");
                if (uses2DBlendTree)
                {
                    drawParameterCombo(
                        "Y Parameter",
                        "parameterY");
                }
                auto& children =
                    blendTree["children"];
                std::optional<std::size_t>
                    childToDelete;
                for (std::size_t index = 0;
                    index < children.size();
                    ++index)
                {
                    auto& child =
                        children[index];
                    ImGui::PushID(
                        static_cast<int>(index));
                    ImGui::Text(
                        "Child %zu",
                        index + 1);
                    inputString(
                        "モデル内Clip",
                        child,
                        "modelClip");
                    inputString(
                        "外部Clip",
                        child,
                        "clip");
                    if (uses2DBlendTree)
                    {
                        if (!child.contains(
                                "position")
                            || !child["position"]
                                .is_array()
                            || child["position"]
                                .size() != 2)
                        {
                            child["position"] = {
                                0.0f,
                                0.0f
                            };
                        }
                        std::array<float, 2>
                            position{
                                child["position"]
                                    .at(0).get<float>(),
                                child["position"]
                                    .at(1).get<float>()
                            };
                        if (ImGui::DragFloat2(
                                "2D座標",
                                position.data(),
                                0.05f))
                        {
                            child["position"] = {
                                position[0],
                                position[1]
                            };
                            m_animatorGraphDirty =
                                true;
                        }
                    }
                    else
                    {
                        float threshold =
                            child.value(
                                "threshold",
                                0.0f);
                        if (ImGui::DragFloat(
                                "しきい値",
                                &threshold,
                                0.05f))
                        {
                            child["threshold"] =
                                threshold;
                            m_animatorGraphDirty =
                                true;
                        }
                    }
                    ImGui::BeginDisabled(
                        children.size() <= 2);
                    if (ImGui::SmallButton(
                            "Childを削除"))
                    {
                        childToDelete =
                            index;
                    }
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (childToDelete)
                {
                    children.erase(
                        children.begin()
                            + static_cast<
                                nlohmann::json::
                                    difference_type>(
                                    *childToDelete));
                    m_animatorGraphDirty =
                        true;
                }
                if (uses2DBlendTree
                    && !children.empty())
                {
                    float minimumX =
                        std::numeric_limits<float>::max();
                    float maximumX =
                        std::numeric_limits<float>::lowest();
                    float minimumY =
                        std::numeric_limits<float>::max();
                    float maximumY =
                        std::numeric_limits<float>::lowest();
                    for (const auto& child : children)
                    {
                        const auto position =
                            child.value(
                                "position",
                                nlohmann::json::array({
                                    0.0f,
                                    0.0f
                                }));
                        const float x =
                            position.at(0).get<float>();
                        const float y =
                            position.at(1).get<float>();
                        minimumX = std::min(
                            minimumX,
                            x);
                        maximumX = std::max(
                            maximumX,
                            x);
                        minimumY = std::min(
                            minimumY,
                            y);
                        maximumY = std::max(
                            maximumY,
                            y);
                    }
                    minimumX = std::min(
                        minimumX,
                        0.0f);
                    maximumX = std::max(
                        maximumX,
                        0.0f);
                    minimumY = std::min(
                        minimumY,
                        0.0f);
                    maximumY = std::max(
                        maximumY,
                        0.0f);
                    const float spanX = std::max(
                        maximumX - minimumX,
                        1.0f);
                    const float spanY = std::max(
                        maximumY - minimumY,
                        1.0f);
                    minimumX -= spanX * 0.15f;
                    maximumX += spanX * 0.15f;
                    minimumY -= spanY * 0.15f;
                    maximumY += spanY * 0.15f;
                    ImGui::InvisibleButton(
                        "##Blend2DPreview",
                        ImVec2{ -1.0f, 180.0f });
                    const ImVec2 areaMin =
                        ImGui::GetItemRectMin();
                    const ImVec2 areaMax =
                        ImGui::GetItemRectMax();
                    auto* previewDrawList =
                        ImGui::GetWindowDrawList();
                    previewDrawList->AddRectFilled(
                        areaMin,
                        areaMax,
                        IM_COL32(
                            24,
                            30,
                            40,
                            255),
                        5.0f);
                    previewDrawList->AddRect(
                        areaMin,
                        areaMax,
                        IM_COL32(
                            72,
                            86,
                            105,
                            255),
                        5.0f);
                    const auto toScreen =
                        [areaMin,
                            areaMax,
                            minimumX,
                            maximumX,
                            minimumY,
                            maximumY](
                            const float x,
                            const float y)
                        {
                            return ImVec2{
                                std::lerp(
                                    areaMin.x + 8.0f,
                                    areaMax.x - 8.0f,
                                    (x - minimumX)
                                        / (maximumX
                                            - minimumX)),
                                std::lerp(
                                    areaMax.y - 8.0f,
                                    areaMin.y + 8.0f,
                                    (y - minimumY)
                                        / (maximumY
                                            - minimumY))
                            };
                        };
                    const ImVec2 origin =
                        toScreen(0.0f, 0.0f);
                    previewDrawList->AddLine(
                        ImVec2{
                            areaMin.x,
                            origin.y
                        },
                        ImVec2{
                            areaMax.x,
                            origin.y
                        },
                        IM_COL32(
                            70,
                            82,
                            98,
                            210));
                    previewDrawList->AddLine(
                        ImVec2{
                            origin.x,
                            areaMin.y
                        },
                        ImVec2{
                            origin.x,
                            areaMax.y
                        },
                        IM_COL32(
                            70,
                            82,
                            98,
                            210));
                    for (const auto& child : children)
                    {
                        const auto position =
                            child.value(
                                "position",
                                nlohmann::json::array({
                                    0.0f,
                                    0.0f
                                }));
                        const ImVec2 point =
                            toScreen(
                                position.at(0)
                                    .get<float>(),
                                position.at(1)
                                    .get<float>());
                        previewDrawList->
                            AddCircleFilled(
                                point,
                                6.0f,
                                IM_COL32(
                                    82,
                                    190,
                                    255,
                                    255));
                        const auto label =
                            child.value(
                                "modelClip",
                                std::string{});
                        previewDrawList->AddText(
                            ImVec2{
                                point.x + 8.0f,
                                point.y - 7.0f
                            },
                            IM_COL32(
                                210,
                                220,
                                232,
                                255),
                            label.c_str());
                    }
                }
                if (children.size() < 16
                    && ImGui::Button("+ Child"))
                {
                    const auto& last =
                        children.back();
                    children.push_back({
                        {
                            "modelClip",
                            last.value(
                                "modelClip",
                                std::string{})
                        },
                        {
                            "threshold",
                            last.value(
                                "threshold",
                                0.0f) + 1.0f
                        },
                        {
                            "position",
                            uses2DBlendTree
                            ? nlohmann::json{
                                last.value(
                                    "position",
                                    nlohmann::json::array({
                                        0.0f,
                                        0.0f
                                    })).at(0).get<float>()
                                    + 1.0f,
                                last.value(
                                    "position",
                                    nlohmann::json::array({
                                        0.0f,
                                        0.0f
                                    })).at(1).get<float>()
                            }
                            : nlohmann::json{
                                0.0f,
                                0.0f
                            }
                        }
                    });
                    m_animatorGraphDirty =
                        true;
                }
            }

            ImGui::SeparatorText("Animation Events");
            if (!state.contains("events")
                || !state["events"].is_array())
            {
                state["events"] =
                    nlohmann::json::array();
            }
            auto& animationEvents =
                state["events"];
            std::optional<std::size_t>
                eventToDelete;
            for (std::size_t index = 0;
                index < animationEvents.size();
                ++index)
            {
                auto& animationEvent =
                    animationEvents[index];
                ImGui::PushID(
                    static_cast<int>(index));
                inputString(
                    "Event名",
                    animationEvent,
                    "name");
                inputString(
                    "Payload",
                    animationEvent,
                    "payload");
                float eventTime =
                    animationEvent.value(
                        "time",
                        0.0f);
                if (ImGui::SliderFloat(
                        "正規化時刻",
                        &eventTime,
                        0.0f,
                        1.0f,
                        "%.3f"))
                {
                    animationEvent["time"] =
                        eventTime;
                    m_animatorGraphDirty =
                        true;
                }
                if (ImGui::SmallButton(
                        "Eventを削除"))
                {
                    eventToDelete =
                        index;
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (eventToDelete)
            {
                animationEvents.erase(
                    animationEvents.begin()
                        + static_cast<
                            nlohmann::json::
                                difference_type>(
                                *eventToDelete));
                m_animatorGraphDirty = true;
            }
            if (animationEvents.size() < 128
                && ImGui::Button("+ Animation Event"))
            {
                animationEvents.push_back({
                    { "name", "AnimationEvent" },
                    { "payload", "" },
                    { "time", 0.5f }
                });
                m_animatorGraphDirty = true;
            }

            ImGui::SeparatorText("Transitions");
            std::optional<std::size_t>
                transitionToDelete;
            const std::string stateName =
                state.value(
                    "name",
                    std::string{});
            for (std::size_t index = 0;
                index < transitions.size();
                ++index)
            {
                auto& transition =
                    transitions[index];
                if (transition.value(
                        "from",
                        std::string{})
                    != stateName)
                {
                    continue;
                }
                ImGui::PushID(
                    static_cast<int>(index));
                const auto target =
                    transition.value(
                        "to",
                        std::string{});
                if (ImGui::TreeNode(
                        "Transition",
                        "→ %s",
                        target.c_str()))
                {
                    if (ImGui::BeginCombo(
                            "遷移先",
                            target.c_str()))
                    {
                        for (const auto&
                            targetState : states)
                        {
                            const auto targetName =
                                targetState.value(
                                    "name",
                                    std::string{});
                            if (targetName
                                    != stateName
                                && ImGui::Selectable(
                                    targetName.c_str(),
                                    targetName
                                        == target))
                            {
                                transition["to"] =
                                    targetName;
                                m_animatorGraphDirty =
                                    true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    inputString(
                        "Trigger",
                        transition,
                        "trigger");
                    float exitTime =
                        transition.value(
                            "exitTime",
                            -1.0f);
                    if (ImGui::DragFloat(
                            "Exit Time",
                            &exitTime,
                            0.01f,
                            -1.0f,
                            1.0f))
                    {
                        transition["exitTime"] =
                            std::clamp(
                                exitTime,
                                -1.0f,
                                1.0f);
                        m_animatorGraphDirty =
                            true;
                    }
                    float duration =
                        transition.value(
                            "duration",
                            0.2f);
                    if (ImGui::DragFloat(
                            "遷移時間",
                            &duration,
                            0.01f,
                            0.0f,
                            10.0f))
                    {
                        transition["duration"] =
                            std::clamp(
                                duration,
                                0.0f,
                                10.0f);
                        m_animatorGraphDirty =
                            true;
                    }
                    if (ImGui::Button(
                            "Transitionを削除"))
                    {
                        transitionToDelete =
                            index;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (transitionToDelete)
            {
                transitions.erase(
                    transitions.begin()
                        + static_cast<
                            nlohmann::json::
                                difference_type>(
                                *transitionToDelete));
                m_animatorGraphDirty = true;
            }
            if (states.size() > 1
                && ImGui::Button(
                    "+ Transition"))
            {
                const auto target =
                    std::ranges::find_if(
                        states,
                        [&stateName](
                            const nlohmann::json&
                                candidate)
                        {
                            return candidate.value(
                                "name",
                                std::string{})
                                != stateName;
                        });
                if (target != states.end())
                {
                    transitions.push_back({
                        { "from", stateName },
                        {
                            "to",
                            target->value(
                                "name",
                                std::string{})
                        },
                        { "trigger", "" },
                        { "exitTime", 1.0f },
                        { "duration", 0.2f }
                    });
                    m_animatorGraphDirty =
                        true;
                }
            }

            ImGui::Separator();
            ImGui::BeginDisabled(
                states.size() <= 1);
            if (ImGui::Button(
                    "Stateを削除",
                    ImVec2{ -1.0f, 0.0f }))
            {
                transitions.erase(
                    std::remove_if(
                        transitions.begin(),
                        transitions.end(),
                        [&stateName](
                            const nlohmann::json&
                                transition)
                        {
                            return transition.value(
                                    "from",
                                    std::string{})
                                    == stateName
                                || transition.value(
                                    "to",
                                    std::string{})
                                    == stateName;
                        }),
                    transitions.end());
                states.erase(
                    states.begin()
                        + static_cast<
                            nlohmann::json::
                                difference_type>(
                                m_animatorGraphSelectedState));
                m_animatorGraphSelectedState =
                    std::min(
                        m_animatorGraphSelectedState,
                        states.size() - 1);
                if (document.value(
                        "entry",
                        std::string{})
                    == stateName)
                {
                    document["entry"] =
                        states.front().value(
                            "name",
                            std::string{});
                }
                m_animatorGraphDirty = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::End();

        if (!windowOpen)
        {
            if (m_animatorGraphDirty)
            {
                try
                {
                    SaveAnimatorControllerGraph();
                }
                catch (const std::exception& exception)
                {
                    m_animatorGraphError =
                        exception.what();
                    m_animatorGraphOpen = true;
                    return;
                }
            }
            m_animatorGraphOpen = false;
            m_animatorGraphDocument.reset();
            m_animatorGraphControllerPath.clear();
        }
    }

    void EditorLayer::OpenAnimatorControllerGraph(
        const std::filesystem::path& controllerPath)
    {
        if (controllerPath.empty() || m_playing)
        {
            return;
        }
        try
        {
            if (m_animatorGraphOpen
                && m_animatorGraphDirty)
            {
                SaveAnimatorControllerGraph();
            }
            m_animatorGraphControllerPath =
                controllerPath;
            LoadAnimatorControllerGraph();
            m_animatorGraphOpen = true;
            SetStatus(
                "Animator Controllerを開きました: "
                + PathToUtf8(controllerPath));
        }
        catch (const std::exception& exception)
        {
            m_animatorGraphError = exception.what();
            m_animatorGraphOpen = false;
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::LoadAnimatorControllerGraph()
    {
        const auto path =
            m_graphics.Assets().ResolvePath(
                m_animatorGraphControllerPath);
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Animator Controllerを開けません: "
                + PathToUtf8(path));
        }
        auto document =
            std::make_unique<nlohmann::json>();
        input >> *document;
        static_cast<void>(
            AnimatorController::FromJson(
                document->dump()));

        auto& states = document->at("states");
        for (std::size_t index = 0;
            index < states.size();
            ++index)
        {
            auto& state = states[index];
            const auto position =
                state.find("editorPosition");
            if (position == state.end()
                || !position->is_array()
                || position->size() != 2)
            {
                state["editorPosition"] = {
                    40.0f
                        + static_cast<float>(
                            index % 3) * 230.0f,
                    40.0f
                        + static_cast<float>(
                            index / 3) * 140.0f
                };
            }
        }
        m_animatorGraphDocument =
            std::move(document);
        m_animatorGraphSelectedState =
            states.empty()
            ? static_cast<std::size_t>(-1)
            : 0;
        m_animatorGraphScrolling = {};
        m_animatorGraphDirty = false;
        m_animatorGraphError.clear();
    }

    void EditorLayer::SaveAnimatorControllerGraph()
    {
        if (!m_animatorGraphDocument
            || m_animatorGraphControllerPath.empty())
        {
            return;
        }
        const std::string serialized =
            m_animatorGraphDocument->dump(2)
            + '\n';
        static_cast<void>(
            AnimatorController::FromJson(
                serialized));

        const auto path =
            m_graphics.Assets().ResolvePath(
                m_animatorGraphControllerPath);
        const auto temporary =
            std::filesystem::path{
                path.wstring() + L".tmp"
            };
        {
            std::ofstream output(
                temporary,
                std::ios::binary
                    | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Animator Controllerを保存できません: "
                    + PathToUtf8(path));
            }
            output.write(
                serialized.data(),
                static_cast<std::streamsize>(
                    serialized.size()));
            output.flush();
            if (!output)
            {
                throw std::runtime_error(
                    "Animator Controllerの書き込みに失敗しました: "
                    + PathToUtf8(path));
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code cleanupError;
            std::filesystem::remove(
                temporary,
                cleanupError);
            throw std::runtime_error(
                "Animator Controllerを置き換えられません: "
                + PathToUtf8(path));
        }

        static_cast<void>(
            m_graphics.Assets().
                ReloadAnimatorController(
                    m_animatorGraphControllerPath));
        for (const auto& gameObject :
            m_scene.GameObjects())
        {
            if (auto* animator =
                    gameObject->GetComponent<
                        TransformAnimatorComponent>();
                animator != nullptr
                    && animator->ControllerPath()
                        == m_animatorGraphControllerPath)
            {
                animator->ReloadController();
            }
            if (auto* model =
                    gameObject->GetComponent<
                        ModelRendererComponent>();
                model != nullptr
                    && model->AnimationControllerPath()
                        == m_animatorGraphControllerPath)
            {
                model->ReloadAnimationController();
            }
        }
        m_animatorGraphDirty = false;
        m_animatorGraphError.clear();
        SetStatus(
            "Animator Controllerを保存しました: "
            + PathToUtf8(
                m_animatorGraphControllerPath));
    }

    void EditorLayer::OpenAnimationTimeline()
    {
        auto* gameObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        auto* animator = gameObject != nullptr
            ? gameObject->GetComponent<
                TransformAnimatorComponent>()
            : nullptr;
        if (animator == nullptr
            || animator->ClipPath().empty()
            || m_playing)
        {
            return;
        }

        try
        {
            if (m_animationTimelineOpen)
            {
                CloseAnimationTimeline(true);
            }
            m_animationTimelineTargetId =
                gameObject->Id();
            m_animationTimelineOriginalTransform =
                gameObject->GetTransform();
            LoadAnimationTimeline(
                animator->ClipPath());
            m_animationTimelineOpen = true;
            PreviewAnimationTimeline();
            SetStatus(
                "Animation Timelineを開きました: "
                + PathToUtf8(
                    animator->ClipPath()));
        }
        catch (const std::exception& exception)
        {
            m_animationTimelineOpen = false;
            m_animationTimelineTargetId = 0;
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::CreateAnimationClipForSelected()
    {
        auto* gameObject =
            m_scene.FindGameObject(
                m_selectedObjectId);
        auto* animator = gameObject != nullptr
            ? gameObject->GetComponent<
                TransformAnimatorComponent>()
            : nullptr;
        if (animator == nullptr || m_playing)
        {
            return;
        }
        if (m_animationTimelineOpen)
        {
            CloseAnimationTimeline(true);
        }

        std::wstring baseName =
            PathFromUtf8(
                gameObject->Name()).filename().wstring();
        for (auto& character : baseName)
        {
            if (character < L' '
                || std::wstring_view{
                    L"<>:\"/\\|?*"
                }.find(character)
                    != std::wstring_view::npos)
            {
                character = L'_';
            }
        }
        while (!baseName.empty()
            && (baseName.back() == L' '
                || baseName.back() == L'.'))
        {
            baseName.pop_back();
        }
        if (baseName.empty())
        {
            baseName = L"NewAnimation";
        }

        std::array<wchar_t, 32768> filename{};
        const std::wstring suggestedName =
            baseName + L".animation.json";
        wcscpy_s(
            filename.data(),
            filename.size(),
            suggestedName.c_str());

        const auto assetRoot =
            std::filesystem::absolute(
                m_graphics.Assets().AssetRoot()).
                    lexically_normal();
        const auto animationDirectory =
            assetRoot / L"animations";
        std::error_code directoryError;
        std::filesystem::create_directories(
            animationDirectory,
            directoryError);
        if (directoryError)
        {
            SetStatus(
                "Animationフォルダーを作成できませんでした",
                true);
            return;
        }
        const std::wstring initialDirectory =
            animationDirectory.wstring();
        constexpr wchar_t filter[] =
            L"LamaPon Animation (*.animation.json)\0*.animation.json\0"
            L"JSON (*.json)\0*.json\0\0";

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile =
            static_cast<DWORD>(filename.size());
        dialog.lpstrInitialDir =
            initialDirectory.c_str();
        dialog.lpstrTitle =
            L"新規Animation Clipを作成";
        dialog.lpstrDefExt =
            L"animation.json";
        dialog.Flags =
            OFN_OVERWRITEPROMPT
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&dialog))
        {
            if (CommDlgExtendedError() != 0)
            {
                SetStatus(
                    "Animation保存ダイアログを開けませんでした",
                    true);
            }
            return;
        }

        try
        {
            std::filesystem::path destination{
                filename.data()
            };
            if (!IsAnimationAsset(destination))
            {
                destination.replace_extension(
                    L".animation.json");
            }
            destination =
                std::filesystem::absolute(
                    destination).lexically_normal();
            if (!IsPathWithin(
                    assetRoot,
                    destination))
            {
                throw std::runtime_error(
                    "Animation Clipはassetsフォルダー内へ保存してください。");
            }

            const auto& transform =
                gameObject->GetTransform();
            const auto clip =
                AnimationClip::Create(
                    gameObject->Name()
                        + " Animation",
                    1.0f,
                    true,
                    {
                        TransformKeyframe{
                            0.0f,
                            TransformAnimationSample{
                                transform.position,
                                transform.EulerAngles(),
                                transform.scale
                            }
                        }
                    });
            clip.SaveToFile(destination);
            const auto relativePath =
                destination.lexically_relative(
                    assetRoot);
            animator->SetClipPath(
                relativePath);
            RecordHistory();
            RefreshAssets();
            m_selectedAsset = relativePath;
            m_assetDirectory =
                relativePath.parent_path();
            SetStatus(
                "Animation Clipを作成しました: "
                + PathToUtf8(relativePath));
            OpenAnimationTimeline();
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::LoadAnimationTimeline(
        const std::filesystem::path& clipPath)
    {
        const auto resolvedPath =
            clipPath.is_absolute()
                ? clipPath.lexically_normal()
                : m_graphics.Assets().ResolvePath(
                    clipPath);
        const auto clip =
            AnimationClip::LoadFromFile(
                resolvedPath);
        m_animationTimelineClipPath =
            clipPath.lexically_normal();
        m_animationTimelineKeyframes =
            clip.Keyframes();
        m_animationTimelineName.fill('\0');
        strncpy_s(
            m_animationTimelineName.data(),
            m_animationTimelineName.size(),
            clip.Name().c_str(),
            _TRUNCATE);
        m_animationTimelineDuration =
            clip.Duration();
        m_animationTimelineLoop =
            clip.Loop();
        m_animationTimelineSelectedKey = 0;
        m_animationTimelineTime =
            m_animationTimelineKeyframes.front().time;
        m_animationTimelineDirty = false;
        m_animationTimelineError.clear();
    }

    void EditorLayer::SaveAnimationTimeline()
    {
        if (m_playing
            || m_animationTimelineClipPath.empty())
        {
            return;
        }
        try
        {
            const auto clip =
                AnimationClip::Create(
                    m_animationTimelineName.data(),
                    m_animationTimelineDuration,
                    m_animationTimelineLoop,
                    m_animationTimelineKeyframes);
            const auto resolvedPath =
                m_animationTimelineClipPath.is_absolute()
                    ? m_animationTimelineClipPath.
                        lexically_normal()
                    : m_graphics.Assets().ResolvePath(
                        m_animationTimelineClipPath);
            clip.SaveToFile(resolvedPath);

            if (auto* target =
                    m_scene.FindGameObject(
                        m_animationTimelineTargetId))
            {
                if (auto* animator =
                        target->GetComponent<
                            TransformAnimatorComponent>();
                    animator != nullptr)
                {
                    animator->ReloadClip();
                }
            }
            m_animationTimelineDirty = false;
            m_animationTimelineError.clear();
            RefreshAssets();
            SetStatus(
                "Animation Clipを保存しました: "
                + PathToUtf8(
                    m_animationTimelineClipPath));
        }
        catch (const std::exception& exception)
        {
            m_animationTimelineError =
                exception.what();
            SetStatus(exception.what(), true);
        }
    }

    void EditorLayer::CloseAnimationTimeline(
        const bool restoreTransform)
    {
        if (restoreTransform)
        {
            if (auto* target =
                    m_scene.FindGameObject(
                        m_animationTimelineTargetId))
            {
                target->GetTransform() =
                    m_animationTimelineOriginalTransform;
            }
        }
        if (m_animationTimelineDirty)
        {
            SetStatus(
                "未保存のAnimation Timeline変更を破棄しました");
        }
        m_animationTimelineOpen = false;
        m_animationTimelineTargetId = 0;
        m_animationTimelineClipPath.clear();
        m_animationTimelineKeyframes.clear();
        m_animationTimelineSelectedKey =
            static_cast<std::size_t>(-1);
        m_animationTimelineError.clear();
        m_animationTimelineDirty = false;
    }

    void EditorLayer::PreviewAnimationTimeline()
    {
        auto* target =
            m_scene.FindGameObject(
                m_animationTimelineTargetId);
        if (target == nullptr
            || m_animationTimelineKeyframes.empty())
        {
            return;
        }
        try
        {
            float lastKeyTime{};
            for (const auto& keyframe :
                m_animationTimelineKeyframes)
            {
                lastKeyTime = std::max(
                    lastKeyTime,
                    keyframe.time);
            }
            const auto previewClip =
                AnimationClip::Create(
                    m_animationTimelineName.data(),
                    std::max(
                        m_animationTimelineDuration,
                        std::max(
                            lastKeyTime,
                            0.01f)),
                    m_animationTimelineLoop,
                    m_animationTimelineKeyframes);
            const auto sample =
                previewClip.Sample(
                    m_animationTimelineTime);
            auto& transform =
                target->GetTransform();
            transform.position =
                sample.position;
            const auto rotation =
                previewClip.SampleRotationQuaternion(
                    m_animationTimelineTime);
            transform.SetRotationVector(
                DirectX::XMLoadFloat4(&rotation));
            transform.scale =
                sample.scale;
            m_animationTimelineError.clear();
        }
        catch (const std::exception& exception)
        {
            m_animationTimelineError =
                exception.what();
        }
    }
}
