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
        // ボディ分類の表示名
        constexpr std::array<const char*, 5> BodyFilterLabels{
            "すべて",
            "動的",
            "キネマティック",
            "静的コライダー",
            "スリープ中"
        };

        // 三次元ベクトルの長さを返します(value: 計測するベクトル)。
        [[nodiscard]] float Length(const DirectX::XMFLOAT3& value)
        {
            return std::sqrt(
                value.x * value.x
                + value.y * value.y
                + value.z * value.z);
        }

        // 基点へ倍率付きの方向ベクトルを加えます(origin: 基点, direction: 加算する方向, scale: 方向の倍率)。
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

        // 対象のワールド座標を返します(object: 位置を読み取る対象)。
        [[nodiscard]] DirectX::XMFLOAT3 WorldPosition(
            const GameObject& object)
        {
            // 対象のワールド座標
            DirectX::XMFLOAT3 position{};
            DirectX::XMStoreFloat3(
                &position,
                object.WorldMatrix().r[3]);
            return position;
        }


        // コライダーとCharacterControllerの種類名を連結し、無ければ空を返します(object: 調べる対象)。
        [[nodiscard]] std::string ColliderNames(const GameObject& object)
        {
            // 連結済みコライダー名
            std::string names;
            // 対象のコンポーネント
            for (const auto& component : object.Components())
            {
                // コンポーネントの種類名
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
            // シーン内の対象への借用参照
            // シーン内の対象
            GameObject* object{};
            // 有効な物理ボディの借用参照
            // 対象の物理ボディ
            const RigidbodyComponent* body{};
            // コライダーの種類名一覧
            // 対象のコライダー名一覧
            std::string colliders;
            // 一覧表示用のボディ分類
            PhysicsDebuggerPanel::BodyFilter kind{
                PhysicsDebuggerPanel::BodyFilter::StaticCollider };
        };

        // 一覧用のボディ分類名を返します(kind: ボディの分類)。
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

        // ボディまたはコライダーを持つ対象への借用参照を集めます(scene: 調べるシーン)。
        [[nodiscard]] std::vector<BodyRow> CollectBodies(const Scene& scene)
        {
            // 表示候補のボディ一覧
            std::vector<BodyRow> rows;
            // シーン内の対象
            for (const auto& object : scene.GameObjects())
            {
                // 対象の物理ボディ
                const auto* body =
                    object->GetComponent<RigidbodyComponent>();
                // 対象のコライダー名一覧
                auto colliders = ColliderNames(*object);
                if (body == nullptr && colliders.empty())
                {
                    continue;
                }
                // 表示対象のボディ行
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

        // 対象名を返し、見つからなければIDを表示名に使います(scene: 検索先のシーン, id: 対象のID)。
        [[nodiscard]] std::string ObjectLabel(
            const Scene& scene,
            const GameObjectId id)
        {
            // シーン内の対象
            const auto* object = scene.FindGameObject(id);
            return object != nullptr
                ? object->Name()
                : "#" + std::to_string(id);
        }

        // 三軸の十字を線分頂点の組で追加します(points: 線分頂点の追加先, center: 十字の中心, size: 各軸の半径)。
        void AppendCross(
            std::vector<DirectX::XMFLOAT3>& points,
            const DirectX::XMFLOAT3& center,
            const float size)
        {
            // 十字の軸方向と半径
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

    // オブジェクト選択の通知を保持します。
    PhysicsDebuggerPanel::PhysicsDebuggerPanel(SelectObject select)
        : m_select(std::move(select))
    {
    }

    // パネルの開閉に接触記録と補助描画を同期します。
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

    // 物理統計とボディ・接触一覧を描画します。
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

        // 直前の物理ステップの統計
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

    // 名前と種類で絞り込んだボディの状態を表示します。
    void PhysicsDebuggerPanel::DrawBodyTable(
        const Scene& scene,
        const GameObjectId selectedObject)
    {
        ImGui::SetNextItemWidth(200.0f);
        // 名前の入力バッファ
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
        // 選択中のボディ分類番号
        int bodyFilter = static_cast<int>(m_bodyFilter);
        if (ImGui::Combo(
                "種類",
                &bodyFilter,
                BodyFilterLabels.data(),
                static_cast<int>(BodyFilterLabels.size())))
        {
            m_bodyFilter = static_cast<BodyFilter>(bodyFilter);
        }

        // オブジェクト別の接触数
        std::unordered_map<GameObjectId, std::size_t> contactCounts;
        // 記録済みの接触
        for (const auto& contact : scene.PhysicsDebugContacts())
        {
            ++contactCounts[contact.left];
            ++contactCounts[contact.right];
        }

        // 表示候補のボディ一覧
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

        // 表示対象のボディ行
        for (const auto& row : rows)
        {
            // ボディがスリープ中か
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
            // 接触一覧または接触数の検索結果
            const auto contacts = contactCounts.find(row.object->Id());
            ImGui::Text(
                "%zu",
                contacts == contactCounts.end() ? 0u : contacts->second);
        }
        ImGui::EndTable();
    }

    // 可視行に限って接触の相手・位置・法線を表示します。
    void PhysicsDebuggerPanel::DrawContactTable(const Scene& scene)
    {
        // 接触一覧または接触数の検索結果
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

        // 可視行だけを描画する範囲
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(contacts.size()));
        while (clipper.Step())
        {
            // 表示する接触の添字
            for (int index = clipper.DisplayStart;
                index < clipper.DisplayEnd;
                ++index)
            {
                // 記録済みの接触
                const auto& contact =
                    contacts[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::PushID(index);
                ImGui::TableSetColumnIndex(0);
                // 接触相手Aの表示名
                const auto left = ObjectLabel(scene, contact.left);
                if (ImGui::Selectable(left.c_str()) && m_select)
                {
                    m_select(contact.left);
                }
                ImGui::TableSetColumnIndex(1);
                // 接触相手Bの表示名
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

    // 接触とスリープしていない有効なボディの速度を重ね描きします。
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
        // 選択対象の絞り込みに合うか判定します(id: 対象のオブジェクトID)。
        const auto isShown = [this, selectedObject](
            const GameObjectId id)
        {
            return !m_onlySelected || id == selectedObject;
        };

        if (m_showContacts)
        {
            // 衝突点と法線の線分頂点
            std::vector<DirectX::XMFLOAT3> collisionLines;
            // トリガー点と法線の線分頂点
            std::vector<DirectX::XMFLOAT3> triggerLines;
            // 記録済みの接触
            for (const auto& contact : scene.PhysicsDebugContacts())
            {
                if (!isShown(contact.left) && !isShown(contact.right))
                {
                    continue;
                }
                // 接触種類に対応する描画先
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
        // 速度ベクトルの線分頂点
        std::vector<DirectX::XMFLOAT3> velocityLines;
        // 角速度ベクトルの線分頂点
        std::vector<DirectX::XMFLOAT3> angularLines;
        // シーン内の対象
        for (const auto& object : scene.GameObjects())
        {
            if (!object->IsActiveInHierarchy() || !isShown(object->Id()))
            {
                continue;
            }
            // 対象の物理ボディ
            const auto* body = object->GetComponent<RigidbodyComponent>();
            if (body == nullptr
                || !body->IsEnabled()
                || body->IsSleeping())
            {
                continue;
            }
            // 対象のワールド座標
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
