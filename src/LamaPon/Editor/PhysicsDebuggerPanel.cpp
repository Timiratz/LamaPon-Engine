#include "LamaPon/Editor/PhysicsDebuggerPanel.h"

#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Scene/Component.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <imgui.h>

#include <array>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LamaPon
{
    namespace
    {
        constexpr std::array<const char*, 5> BodyFilterLabels{
            "すべて",
            "動的",
            "キネマティック",
            "静的コライダー",
            "スリープ中"
        };

        [[nodiscard]] float Length(const DirectX::XMFLOAT3& value)
        {
            return std::sqrt(
                value.x * value.x
                + value.y * value.y
                + value.z * value.z);
        }

        [[nodiscard]] DirectX::XMFLOAT3 Add(
            const DirectX::XMFLOAT3& origin,
            const DirectX::XMFLOAT3& direction,
            const float scale)
        {
            return {
                origin.x + direction.x * scale,
                origin.y + direction.y * scale,
                origin.z + direction.z * scale
            };
        }

        [[nodiscard]] DirectX::XMFLOAT3 WorldPosition(
            const GameObject& object)
        {
            DirectX::XMFLOAT3 position{};
            DirectX::XMStoreFloat3(
                &position,
                object.WorldMatrix().r[3]);
            return position;
        }

        // コライダーの種類名を「BoxCollider3D, SphereCollider3D」の
        // ように連結します。コライダーを持たない場合は空です。
        [[nodiscard]] std::string ColliderNames(const GameObject& object)
        {
            std::string names;
            for (const auto& component : object.Components())
            {
                const auto typeName = component->TypeName();
                if (typeName.find("Collider") == std::string_view::npos
                    && typeName != "CharacterController")
                {
                    continue;
                }
                if (!names.empty())
                {
                    names += ", ";
                }
                names += typeName;
                if (!component->IsEnabled())
                {
                    names += "（無効）";
                }
            }
            return names;
        }

        struct BodyRow final
        {
            GameObject* object{};
            const RigidbodyComponent* body{};
            std::string colliders;
            PhysicsDebuggerPanel::BodyFilter kind{
                PhysicsDebuggerPanel::BodyFilter::StaticCollider };
        };

        [[nodiscard]] const char* KindLabel(
            const PhysicsDebuggerPanel::BodyFilter kind)
        {
            switch (kind)
            {
            case PhysicsDebuggerPanel::BodyFilter::Dynamic:
                return "動的";
            case PhysicsDebuggerPanel::BodyFilter::Kinematic:
                return "キネマティック";
            case PhysicsDebuggerPanel::BodyFilter::StaticCollider:
                return "静的";
            case PhysicsDebuggerPanel::BodyFilter::All:
            case PhysicsDebuggerPanel::BodyFilter::Sleeping:
                break;
            }
            return "";
        }

        [[nodiscard]] std::vector<BodyRow> CollectBodies(const Scene& scene)
        {
            std::vector<BodyRow> rows;
            for (const auto& object : scene.GameObjects())
            {
                const auto* body =
                    object->GetComponent<RigidbodyComponent>();
                auto colliders = ColliderNames(*object);
                if (body == nullptr && colliders.empty())
                {
                    continue;
                }
                BodyRow row;
                row.object = object.get();
                row.body = body != nullptr && body->IsEnabled()
                    ? body
                    : nullptr;
                row.colliders = std::move(colliders);
                row.kind = row.body == nullptr
                    ? PhysicsDebuggerPanel::BodyFilter::StaticCollider
                    : (row.body->IsKinematic()
                        ? PhysicsDebuggerPanel::BodyFilter::Kinematic
                        : PhysicsDebuggerPanel::BodyFilter::Dynamic);
                rows.push_back(std::move(row));
            }
            return rows;
        }

        [[nodiscard]] std::string ObjectLabel(
            const Scene& scene,
            const GameObjectId id)
        {
            const auto* object = scene.FindGameObject(id);
            return object != nullptr
                ? object->Name()
                : "#" + std::to_string(id);
        }

        // 点を中心とする3軸の小さな十字を線分の組で追加します。
        void AppendCross(
            std::vector<DirectX::XMFLOAT3>& points,
            const DirectX::XMFLOAT3& center,
            const float size)
        {
            for (const DirectX::XMFLOAT3 axis : {
                    DirectX::XMFLOAT3{ size, 0.0f, 0.0f },
                    DirectX::XMFLOAT3{ 0.0f, size, 0.0f },
                    DirectX::XMFLOAT3{ 0.0f, 0.0f, size } })
            {
                points.push_back(Add(center, axis, -1.0f));
                points.push_back(Add(center, axis, 1.0f));
            }
        }
    }

    PhysicsDebuggerPanel::PhysicsDebuggerPanel(SelectObject select)
        : m_select(std::move(select))
    {
    }

    void PhysicsDebuggerPanel::SynchronizeCapture(
        Scene& scene,
        const bool panelOpen)
    {
        m_overlayActive = panelOpen;
        if (scene.IsPhysicsDebugCaptureEnabled() != panelOpen)
        {
            scene.SetPhysicsDebugCaptureEnabled(panelOpen);
        }
    }

    void PhysicsDebuggerPanel::Draw(
        const char* const title,
        bool& open,
        Scene& scene,
        const GameObjectId selectedObject)
    {
        if (!open)
        {
            return;
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 760.0f, 520.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, &open))
        {
            ImGui::End();
            return;
        }

        ImGui::Checkbox("接触点と法線", &m_showContacts);
        ImGui::SameLine();
        ImGui::Checkbox("速度", &m_showVelocities);
        ImGui::SameLine();
        ImGui::Checkbox("角速度", &m_showAngularVelocities);
        ImGui::SameLine();
        ImGui::Checkbox("選択中だけ", &m_onlySelected);
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat(
            "速度の表示倍率",
            &m_velocityScale,
            0.01f,
            2.0f,
            "%.2f 秒",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat(
            "法線の長さ",
            &m_normalLength,
            0.05f,
            2.0f,
            "%.2f m");
        ImGui::TextDisabled(
            "Scene Viewに赤=衝突、黄=トリガーの接触点と法線、"
            "水色=速度、紫=角速度を描きます。");

        const auto& statistics = scene.PhysicsStats();
        ImGui::Text(
            "固定ステップ %zu回  補間α %.2f  Collider 2D/3D %zu / %zu"
            "  候補 %zu  Narrow %zu  接触中 %zu",
            scene.PhysicsFixedStepsLastFrame(),
            scene.PhysicsInterpolationAlpha(),
            statistics.colliderCount2D,
            statistics.colliderCount3D,
            statistics.candidatePairCount2D
                + statistics.candidatePairCount3D,
            statistics.narrowPhaseTestCount2D
                + statistics.narrowPhaseTestCount3D,
            statistics.activeContactCount);

        if (ImGui::BeginTabBar("PhysicsDebuggerTabs"))
        {
            if (ImGui::BeginTabItem("ボディ"))
            {
                DrawBodyTable(scene, selectedObject);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("接触"))
            {
                DrawContactTable(scene);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void PhysicsDebuggerPanel::DrawBodyTable(
        const Scene& scene,
        const GameObjectId selectedObject)
    {
        ImGui::SetNextItemWidth(200.0f);
        char filter[128]{};
        m_filter.copy(filter, sizeof(filter) - 1);
        if (ImGui::InputTextWithHint(
                "##PhysicsFilter",
                "名前で絞り込み",
                filter,
                sizeof(filter)))
        {
            m_filter = filter;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        int bodyFilter = static_cast<int>(m_bodyFilter);
        if (ImGui::Combo(
                "種類",
                &bodyFilter,
                BodyFilterLabels.data(),
                static_cast<int>(BodyFilterLabels.size())))
        {
            m_bodyFilter = static_cast<BodyFilter>(bodyFilter);
        }

        std::unordered_map<GameObjectId, std::size_t> contactCounts;
        for (const auto& contact : scene.PhysicsDebugContacts())
        {
            ++contactCounts[contact.left];
            ++contactCounts[contact.right];
        }

        const auto rows = CollectBodies(scene);
        if (!ImGui::BeginTable(
                "PhysicsBodies",
                8,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_Resizable
                    | ImGuiTableFlags_ScrollY,
                ImVec2{ 0.0f, 0.0f }))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            "GameObject",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "種類",
            ImGuiTableColumnFlags_WidthFixed,
            90.0f);
        ImGui::TableSetupColumn(
            "コライダー",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "質量",
            ImGuiTableColumnFlags_WidthFixed,
            55.0f);
        ImGui::TableSetupColumn(
            "速さ m/s",
            ImGuiTableColumnFlags_WidthFixed,
            70.0f);
        ImGui::TableSetupColumn(
            "角速度",
            ImGuiTableColumnFlags_WidthFixed,
            65.0f);
        ImGui::TableSetupColumn(
            "状態",
            ImGuiTableColumnFlags_WidthFixed,
            65.0f);
        ImGui::TableSetupColumn(
            "接触",
            ImGuiTableColumnFlags_WidthFixed,
            40.0f);
        ImGui::TableHeadersRow();

        for (const auto& row : rows)
        {
            const bool sleeping =
                row.body != nullptr && row.body->IsSleeping();
            if (m_bodyFilter == BodyFilter::Sleeping
                    ? !sleeping
                    : (m_bodyFilter != BodyFilter::All
                        && row.kind != m_bodyFilter))
            {
                continue;
            }
            if (!m_filter.empty()
                && row.object->Name().find(m_filter) == std::string::npos)
            {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(row.object->Id()));
            if (ImGui::Selectable(
                    row.object->Name().c_str(),
                    row.object->Id() == selectedObject,
                    ImGuiSelectableFlags_SpanAllColumns)
                && m_select)
            {
                m_select(row.object->Id());
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(KindLabel(row.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled(
                "%s",
                row.colliders.empty()
                    ? "（なし）"
                    : row.colliders.c_str());
            if (row.body != nullptr)
            {
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", row.body->Mass());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.2f", Length(row.body->Velocity()));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text(
                    "%.2f",
                    Length(row.body->AngularVelocity()));
                ImGui::TableSetColumnIndex(6);
                if (sleeping)
                {
                    ImGui::TextDisabled("スリープ");
                }
                else
                {
                    ImGui::TextUnformatted("起動中");
                }
            }
            ImGui::TableSetColumnIndex(7);
            const auto contacts = contactCounts.find(row.object->Id());
            ImGui::Text(
                "%zu",
                contacts == contactCounts.end() ? 0u : contacts->second);
        }
        ImGui::EndTable();
    }

    void PhysicsDebuggerPanel::DrawContactTable(const Scene& scene)
    {
        const auto& contacts = scene.PhysicsDebugContacts();
        if (contacts.empty())
        {
            ImGui::TextDisabled(
                "直前の物理ステップに接触はありません。"
                "再生中に更新されます。");
            return;
        }
        if (!ImGui::BeginTable(
                "PhysicsContacts",
                6,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_Resizable
                    | ImGuiTableFlags_ScrollY,
                ImVec2{ 0.0f, 0.0f }))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("A");
        ImGui::TableSetupColumn("B");
        ImGui::TableSetupColumn(
            "種類",
            ImGuiTableColumnFlags_WidthFixed,
            80.0f);
        ImGui::TableSetupColumn(
            "位置",
            ImGuiTableColumnFlags_WidthFixed,
            170.0f);
        ImGui::TableSetupColumn(
            "法線",
            ImGuiTableColumnFlags_WidthFixed,
            150.0f);
        ImGui::TableSetupColumn(
            "めり込み",
            ImGuiTableColumnFlags_WidthFixed,
            70.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(contacts.size()));
        while (clipper.Step())
        {
            for (int index = clipper.DisplayStart;
                index < clipper.DisplayEnd;
                ++index)
            {
                const auto& contact =
                    contacts[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::PushID(index);
                ImGui::TableSetColumnIndex(0);
                const auto left = ObjectLabel(scene, contact.left);
                if (ImGui::Selectable(left.c_str()) && m_select)
                {
                    m_select(contact.left);
                }
                ImGui::TableSetColumnIndex(1);
                const auto right = ObjectLabel(scene, contact.right);
                if (ImGui::Selectable(right.c_str()) && m_select)
                {
                    m_select(contact.right);
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text(
                    "%s %s",
                    contact.is3D ? "3D" : "2D",
                    contact.isTrigger ? "トリガー" : "衝突");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text(
                    "%.2f, %.2f, %.2f",
                    contact.point.x,
                    contact.point.y,
                    contact.point.z);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text(
                    "%.2f, %.2f, %.2f",
                    contact.normal.x,
                    contact.normal.y,
                    contact.normal.z);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.4f", contact.penetration);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void PhysicsDebuggerPanel::DrawSceneOverlay(
        const Scene& scene,
        DebugRenderer& debug,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const GameObjectId selectedObject) const
    {
        if (!m_overlayActive)
        {
            return;
        }
        const auto isShown = [this, selectedObject](
            const GameObjectId id)
        {
            return !m_onlySelected || id == selectedObject;
        };

        if (m_showContacts)
        {
            std::vector<DirectX::XMFLOAT3> collisionLines;
            std::vector<DirectX::XMFLOAT3> triggerLines;
            for (const auto& contact : scene.PhysicsDebugContacts())
            {
                if (!isShown(contact.left) && !isShown(contact.right))
                {
                    continue;
                }
                auto& lines =
                    contact.isTrigger ? triggerLines : collisionLines;
                AppendCross(lines, contact.point, 0.05f);
                lines.push_back(contact.point);
                lines.push_back(
                    Add(contact.point, contact.normal, m_normalLength));
            }
            debug.DrawLines(
                collisionLines,
                DirectX::XMVectorSet(1.0f, 0.25f, 0.2f, 1.0f),
                view,
                projection);
            debug.DrawLines(
                triggerLines,
                DirectX::XMVectorSet(1.0f, 0.9f, 0.2f, 1.0f),
                view,
                projection);
        }

        if (!m_showVelocities && !m_showAngularVelocities)
        {
            return;
        }
        std::vector<DirectX::XMFLOAT3> velocityLines;
        std::vector<DirectX::XMFLOAT3> angularLines;
        for (const auto& object : scene.GameObjects())
        {
            if (!object->IsActiveInHierarchy() || !isShown(object->Id()))
            {
                continue;
            }
            const auto* body = object->GetComponent<RigidbodyComponent>();
            if (body == nullptr
                || !body->IsEnabled()
                || body->IsSleeping())
            {
                continue;
            }
            const auto origin = WorldPosition(*object);
            if (m_showVelocities && Length(body->Velocity()) > 1.0e-4f)
            {
                velocityLines.push_back(origin);
                velocityLines.push_back(
                    Add(origin, body->Velocity(), m_velocityScale));
            }
            if (m_showAngularVelocities
                && Length(body->AngularVelocity()) > 1.0e-4f)
            {
                angularLines.push_back(origin);
                angularLines.push_back(
                    Add(origin, body->AngularVelocity(), m_velocityScale));
            }
        }
        debug.DrawLines(
            velocityLines,
            DirectX::XMVectorSet(0.3f, 0.85f, 1.0f, 1.0f),
            view,
            projection);
        debug.DrawLines(
            angularLines,
            DirectX::XMVectorSet(0.8f, 0.4f, 1.0f, 1.0f),
            view,
            projection);
    }
}
