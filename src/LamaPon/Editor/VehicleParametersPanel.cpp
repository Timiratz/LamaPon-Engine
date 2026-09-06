#include "LamaPon/Editor/VehicleParametersPanel.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <Windows.h>
#include <Effects.h>
#include <Model.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace LamaPon::EditorDetail;

namespace
{
    void ValidateVehicle(const nlohmann::json& vehicle)
    {
        if (!vehicle.is_object())
        {
            throw std::runtime_error(
                "vehiclesの各要素はオブジェクトにしてください");
        }

        constexpr std::array stringFields{
            "id", "layout", "menu_name"
        };
        for (const char* field : stringFields)
        {
            if (!vehicle.contains(field)
                || !vehicle.at(field).is_string())
            {
                throw std::runtime_error(
                    std::string{ "車両の" } + field
                    + "は文字列にしてください");
            }
        }
        if (vehicle.contains("model_path")
            && !vehicle.at("model_path").is_string())
        {
            throw std::runtime_error(
                "車両のmodel_pathは文字列にしてください");
        }

        constexpr std::array numberFields{
            "length_m",
            "width_m",
            "height_m",
            "wheelbase_m",
            "front_track_m",
            "rear_track_m",
            "idle_rpm",
            "redline_rpm",
            "front_wheel_radius_m",
            "rear_wheel_radius_m",
            "rear_tyre_width_m",
            "acceleration_scale",
            "brake_scale",
            "grip_cornering_scale",
            "drift_cornering_scale"
        };
        for (const char* field : numberFields)
        {
            if (!vehicle.contains(field)
                || !vehicle.at(field).is_number())
            {
                throw std::runtime_error(
                    std::string{ "車両の" } + field
                    + "は数値にしてください");
            }
        }

        constexpr std::array vectorFields{
            "collider_half_extents", "collider_offset"
        };
        for (const char* field : vectorFields)
        {
            if (!vehicle.contains(field)
                || !vehicle.at(field).is_array()
                || vehicle.at(field).size() != 3
                || !std::ranges::all_of(
                    vehicle.at(field),
                    [](const auto& value)
                    { return value.is_number(); }))
            {
                throw std::runtime_error(
                    std::string{ "車両の" } + field
                    + "は3要素の数値配列にしてください");
            }
        }
    }
}

namespace LamaPon
{
    VehicleParametersPanel::VehicleParametersPanel(
        GraphicsDevice& graphics,
        AssetManager& assets,
        std::filesystem::path dataPath,
        StatusSink status)
        : m_graphics(graphics)
        , m_assets(assets)
        , m_dataPath(std::move(dataPath))
        , m_status(std::move(status))
        , m_topPreview(std::make_unique<RenderTarget>())
        , m_sidePreview(std::make_unique<RenderTarget>())
    {
        Load();
    }

    VehicleParametersPanel::~VehicleParametersPanel() = default;

    void VehicleParametersPanel::SetStatus(
        std::string message,
        const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    bool VehicleParametersPanel::Load()
    {
        try
        {
            std::ifstream input(m_dataPath, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error(
                    "車両パラメーターJSONを開けません");
            }
            auto document = std::make_unique<nlohmann::json>();
            input >> *document;
            if (!document->is_object()
                || !document->contains("vehicles")
                || !document->at("vehicles").is_array()
                || document->at("vehicles").empty())
            {
                throw std::runtime_error(
                    "空ではないvehicles配列が必要です");
            }
            for (const auto& vehicle : document->at("vehicles"))
            {
                ValidateVehicle(vehicle);
            }

            m_state.document = std::move(document);
            m_state.selectedVehicle = 0;
            m_state.previewVehicle = -1;
            m_state.previewModel.reset();
            m_state.loaded = true;
            m_state.dirty = false;
            SetStatus("車両パラメーターを読み込みました");
            return true;
        }
        catch (const std::exception& exception)
        {
            m_state.loaded = false;
            m_state.document.reset();
            SetStatus(
                std::string{ "カーエディタを読み込めません: " }
                    + exception.what(),
                true);
            return false;
        }
    }

    bool VehicleParametersPanel::Save()
    {
        if (!m_state.document)
        {
            SetStatus(
                "車両パラメーターを保存できません: データがありません",
                true);
            return false;
        }
        try
        {
            const auto temporary = m_dataPath.wstring() + L".tmp";
            {
                std::ofstream output(
                    temporary,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw std::runtime_error(
                        "一時ファイルを作成できません");
                }
                output << m_state.document->dump(2) << '\n';
                output.flush();
                if (!output)
                {
                    throw std::runtime_error(
                        "車両パラメーターJSONを書き込めません");
                }
            }
            if (std::filesystem::exists(m_dataPath))
            {
                CopyFileW(
                    m_dataPath.c_str(),
                    (m_dataPath.wstring() + L".bak").c_str(),
                    FALSE);
            }
            if (!MoveFileExW(
                    temporary.c_str(),
                    m_dataPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING
                        | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(temporary.c_str());
                throw std::runtime_error(
                    "車両パラメーターJSONを置き換えられません");
            }
            m_state.dirty = false;
            SetStatus("車両パラメーターを保存しました");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "車両パラメーターを保存できません: " }
                    + exception.what(),
                true);
            return false;
        }
    }

    void VehicleParametersPanel::Draw(
        const std::string& title,
        bool& open,
        const std::function<void()>& onSaved)
    {
        ImGui::SetNextWindowSize(
            ImVec2{ 760.0f, 680.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                title.c_str(),
                &open,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        if (!m_state.loaded || !m_state.document)
        {
            ImGui::TextUnformatted(
                "車両データを読み込めませんでした。");
            if (ImGui::Button("再読み込み"))
            {
                Load();
            }
            ImGui::End();
            return;
        }

        auto& vehicles = m_state.document->at("vehicles");
        m_state.selectedVehicle = std::clamp(
            m_state.selectedVehicle,
            0,
            static_cast<int>(vehicles.size()) - 1);
        std::vector<const char*> labels;
        labels.reserve(vehicles.size());
        for (auto& vehicle : vehicles)
        {
            labels.push_back(
                vehicle.at("menu_name")
                    .get_ref<std::string&>()
                    .c_str());
        }
        ImGui::SetNextItemWidth(300.0f);
        ImGui::Combo(
            "車種",
            &m_state.selectedVehicle,
            labels.data(),
            static_cast<int>(labels.size()));
        ImGui::SameLine();
        if (ImGui::Button("保存") && Save() && onSaved)
        {
            onSaved();
        }
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            // JSONと表示ラベルの参照が差し替わるので、このフレームは
            // ここで終えます。
            Load();
            ImGui::End();
            return;
        }
        if (m_state.dirty)
        {
            ImGui::SameLine();
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.72f, 0.2f, 1.0f },
                "未保存");
        }
        ImGui::Separator();

        auto& vehicle = vehicles.at(m_state.selectedVehicle);
        if (m_state.previewVehicle != m_state.selectedVehicle)
        {
            m_state.previewVehicle = m_state.selectedVehicle;
            m_state.previewModel.reset();
            try
            {
                const auto modelPath = PathFromUtf8(
                    vehicle.value("model_path", std::string{}));
                if (!modelPath.empty())
                {
                    m_state.previewModel =
                        m_assets.CreateModelInstance(modelPath);
                }
            }
            catch (const std::exception& exception)
            {
                SetStatus(
                    std::string{ "車両モデルを読み込めません: " }
                        + exception.what(),
                    true);
            }
        }

        ImGui::Text(
            "ID: %s   駆動方式: %s",
            vehicle.value("id", "").c_str(),
            vehicle.value("layout", "").c_str());
        const auto drag = [&vehicle, this](
            const char* label,
            const char* key,
            const float speed,
            const float minimum,
            const float maximum,
            const char* format)
        {
            float value = vehicle.at(key).get<float>();
            if (ImGui::DragFloat(
                    label,
                    &value,
                    speed,
                    minimum,
                    maximum,
                    format))
            {
                vehicle[key] = value;
                m_state.dirty = true;
            }
        };

        if (ImGui::CollapsingHeader(
                "車体寸法",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            drag("全長", "length_m", .01f, .5f, 10.f, "%.3f m");
            drag("全幅", "width_m", .01f, .5f, 5.f, "%.3f m");
            drag("全高", "height_m", .01f, .2f, 5.f, "%.3f m");
            drag(
                "ホイールベース",
                "wheelbase_m",
                .01f,
                .5f,
                6.f,
                "%.3f m");
            drag(
                "前トレッド",
                "front_track_m",
                .01f,
                .2f,
                4.f,
                "%.3f m");
            drag(
                "後トレッド",
                "rear_track_m",
                .01f,
                .2f,
                4.f,
                "%.3f m");
        }
        if (ImGui::CollapsingHeader(
                "エンジン・タイヤ",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            drag(
                "アイドル回転数",
                "idle_rpm",
                10.f,
                100.f,
                5000.f,
                "%.0f rpm");
            drag(
                "レッドゾーン",
                "redline_rpm",
                25.f,
                1000.f,
                20000.f,
                "%.0f rpm");
            drag(
                "前輪半径",
                "front_wheel_radius_m",
                .001f,
                .05f,
                1.f,
                "%.3f m");
            drag(
                "後輪半径",
                "rear_wheel_radius_m",
                .001f,
                .05f,
                1.f,
                "%.3f m");
            drag(
                "後輪幅",
                "rear_tyre_width_m",
                .001f,
                .05f,
                1.f,
                "%.3f m");
        }
        if (ImGui::CollapsingHeader(
                "走行性能",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            drag(
                "加速倍率",
                "acceleration_scale",
                .005f,
                .1f,
                3.f,
                "%.3f x");
            drag(
                "制動倍率",
                "brake_scale",
                .005f,
                .1f,
                3.f,
                "%.3f x");
            drag(
                "グリップ旋回倍率",
                "grip_cornering_scale",
                .005f,
                .1f,
                3.f,
                "%.3f x");
            drag(
                "ドリフト旋回倍率",
                "drift_cornering_scale",
                .005f,
                .1f,
                3.f,
                "%.3f x");
        }
        if (ImGui::CollapsingHeader(
                "当たり判定",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto& extents = vehicle.at("collider_half_extents");
            auto& offset = vehicle.at("collider_offset");
            float e[3]{
                extents[0].get<float>(),
                extents[1].get<float>(),
                extents[2].get<float>()
            };
            float o[3]{
                offset[0].get<float>(),
                offset[1].get<float>(),
                offset[2].get<float>()
            };
            if (ImGui::DragFloat3(
                    "半サイズ X/Y/Z",
                    e,
                    .005f,
                    .02f,
                    10.f,
                    "%.3f m"))
            {
                for (int index = 0; index < 3; ++index)
                {
                    extents[index] = e[index];
                }
                m_state.dirty = true;
            }
            if (ImGui::DragFloat3(
                    "中心オフセット X/Y/Z",
                    o,
                    .005f,
                    -10.f,
                    10.f,
                    "%.3f m"))
            {
                for (int index = 0; index < 3; ++index)
                {
                    offset[index] = o[index];
                }
                m_state.dirty = true;
            }
            ImGui::TextWrapped(
                "半サイズは中心から片側までの距離です。"
                "全体の大きさは X=%.2fm / Y=%.2fm / Z=%.2fm",
                e[0] * 2.f,
                e[1] * 2.f,
                e[2] * 2.f);
            ImGui::TextUnformatted(
                "上面と側面は共通縮尺（同じ1m=同じ画面上の長さ）です。");

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 area{
                ImGui::GetContentRegionAvail().x,
                230.f
            };
            ImGui::InvisibleButton("##VehicleColliderPreview", area);
            auto* const draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(
                origin,
                ImVec2{ origin.x + area.x, origin.y + area.y },
                IM_COL32(18, 22, 28, 255));

            const float previewWidth = std::max(
                (area.x - 12.0f) * 0.5f,
                64.0f);
            const float previewHeight = std::min(
                std::max(area.y - 30.0f, 64.0f),
                previewWidth / 1.6f);
            const float previewAspect =
                previewWidth / previewHeight;
            float sharedWorldWidth = std::max({
                e[0] * 2.4f,
                e[2] * 2.4f,
                e[2] * 2.4f * previewAspect,
                e[1] * 2.4f * previewAspect,
                0.1f
            });
            float topOverlayScale =
                previewWidth / sharedWorldWidth;
            float sideOverlayScale = topOverlayScale;
            DirectX::XMFLOAT3 modelCenter{ o[0], o[1], o[2] };

            if (m_state.previewModel
                && m_state.previewModel->hasLocalBounds)
            {
                constexpr std::uint32_t previewTargetWidth = 512;
                const auto previewTargetHeight =
                    static_cast<std::uint32_t>(std::max(
                        1.0f,
                        std::round(
                            static_cast<float>(previewTargetWidth)
                            / previewAspect)));
                m_topPreview->Resize(
                    m_graphics.Device(),
                    previewTargetWidth,
                    previewTargetHeight);
                m_sidePreview->Resize(
                    m_graphics.Device(),
                    previewTargetWidth,
                    previewTargetHeight);

                const auto& bounds =
                    m_state.previewModel->localBounds;
                const DirectX::XMFLOAT3 center3{
                    (bounds.minimum.x + bounds.maximum.x) * 0.5f,
                    (bounds.minimum.y + bounds.maximum.y) * 0.5f,
                    (bounds.minimum.z + bounds.maximum.z) * 0.5f
                };
                const float sizeX = std::max(
                    bounds.maximum.x - bounds.minimum.x,
                    0.1f);
                const float sizeY = std::max(
                    bounds.maximum.y - bounds.minimum.y,
                    0.1f);
                const float sizeZ = std::max(
                    bounds.maximum.z - bounds.minimum.z,
                    0.1f);
                modelCenter = center3;
                const float distance =
                    std::max({ sizeX, sizeY, sizeZ }) * 3.0f
                    + 1.0f;

                const auto renderWireframe = [this](
                    RenderTarget& target,
                    const DirectX::XMMATRIX& view,
                    const DirectX::XMMATRIX& projection)
                {
                    constexpr float clear[]{
                        0.035f, 0.045f, 0.06f, 1.0f
                    };
                    target.Bind(m_graphics.Context());
                    target.Clear(m_graphics.Context(), clear);
                    const LitMaterial material{
                        DirectX::XMFLOAT4{
                            0.15f, 0.82f, 1.0f, 1.0f
                        },
                        {},
                        {},
                        0.8f
                    };
                    if (m_state.previewModel->skeletalModel)
                    {
                        m_state.previewModel->skeletalModel->Draw(
                            m_graphics.Context(),
                            m_graphics.States(),
                            m_graphics.Lighting(),
                            DirectX::XMMatrixIdentity(),
                            view,
                            projection,
                            nullptr,
                            0.0f,
                            true,
                            &material);
                    }
                    else if (m_state.previewModel->model)
                    {
                        m_state.previewModel->model->UpdateEffects(
                            [](DirectX::IEffect* effect)
                            {
                                if (auto* const basic =
                                        dynamic_cast<
                                            DirectX::BasicEffect*>(
                                                effect))
                                {
                                    basic->SetTextureEnabled(false);
                                    basic->SetDiffuseColor(
                                        DirectX::XMVectorSet(
                                            0.15f,
                                            0.82f,
                                            1.0f,
                                            1.0f));
                                }
                            });
                        m_state.previewModel->model->Draw(
                            m_graphics.Context(),
                            m_graphics.States(),
                            DirectX::XMMatrixIdentity(),
                            view,
                            projection,
                            true);
                    }
                    target.CopyToDisplay(m_graphics.Context());
                };

                const auto focus = DirectX::XMLoadFloat3(&center3);
                const float colliderSpanX = 2.0f
                    * (std::abs(o[0] - center3.x) + e[0])
                    * 1.12f;
                const float colliderSpanY = 2.0f
                    * (std::abs(o[1] - center3.y) + e[1])
                    * 1.12f;
                const float colliderSpanZ = 2.0f
                    * (std::abs(o[2] - center3.z) + e[2])
                    * 1.12f;
                sharedWorldWidth = std::max({
                    sizeX * 1.25f,
                    sizeZ * 1.25f,
                    sizeZ * 1.25f * previewAspect,
                    sizeY * 1.35f * previewAspect,
                    colliderSpanX,
                    colliderSpanZ,
                    colliderSpanZ * previewAspect,
                    colliderSpanY * previewAspect,
                    0.1f
                });
                const float sharedWorldHeight =
                    sharedWorldWidth / previewAspect;
                topOverlayScale =
                    previewWidth / sharedWorldWidth;
                sideOverlayScale = topOverlayScale;

                renderWireframe(
                    *m_topPreview,
                    DirectX::XMMatrixLookAtLH(
                        DirectX::XMVectorSet(
                            center3.x,
                            center3.y + distance,
                            center3.z,
                            1.0f),
                        focus,
                        DirectX::XMVectorSet(0, 0, 1, 0)),
                    DirectX::XMMatrixOrthographicLH(
                        sharedWorldWidth,
                        sharedWorldHeight,
                        0.01f,
                        distance * 2.0f));
                renderWireframe(
                    *m_sidePreview,
                    DirectX::XMMatrixLookAtLH(
                        DirectX::XMVectorSet(
                            center3.x + distance,
                            center3.y,
                            center3.z,
                            1.0f),
                        focus,
                        DirectX::XMVectorSet(0, 1, 0, 0)),
                    DirectX::XMMatrixOrthographicLH(
                        sharedWorldWidth,
                        sharedWorldHeight,
                        0.01f,
                        distance * 2.0f));

                draw->AddImage(
                    MakeTextureReference(
                        m_topPreview
                            ->DisplayShaderResourceView()),
                    ImVec2{ origin.x, origin.y + 25.0f },
                    ImVec2{
                        origin.x + previewWidth,
                        origin.y + 25.0f + previewHeight
                    });
                draw->AddImage(
                    MakeTextureReference(
                        m_sidePreview
                            ->DisplayShaderResourceView()),
                    ImVec2{
                        origin.x + area.x - previewWidth,
                        origin.y + 25.0f
                    },
                    ImVec2{
                        origin.x + area.x,
                        origin.y + 25.0f + previewHeight
                    });
            }

            const ImVec2 center{
                origin.x + previewWidth * .5f
                    + (o[0] - modelCenter.x) * topOverlayScale,
                origin.y + 25.0f + previewHeight * .5f
                    - (o[2] - modelCenter.z) * topOverlayScale
            };
            draw->AddRect(
                ImVec2{
                    center.x - e[0] * topOverlayScale,
                    center.y - e[2] * topOverlayScale
                },
                ImVec2{
                    center.x + e[0] * topOverlayScale,
                    center.y + e[2] * topOverlayScale
                },
                IM_COL32(255, 185, 55, 255),
                0,
                0,
                2.f);
            draw->AddText(
                ImVec2{ origin.x + 8, origin.y + 7 },
                IM_COL32_WHITE,
                "上面 (X/Z)");

            const ImVec2 side{
                origin.x + area.x - previewWidth * .5f
                    + (o[2] - modelCenter.z) * sideOverlayScale,
                origin.y + 25.0f + previewHeight * .5f
                    - (o[1] - modelCenter.y) * sideOverlayScale
            };
            draw->AddRect(
                ImVec2{
                    side.x - e[2] * sideOverlayScale,
                    side.y - e[1] * sideOverlayScale
                },
                ImVec2{
                    side.x + e[2] * sideOverlayScale,
                    side.y + e[1] * sideOverlayScale
                },
                IM_COL32(255, 185, 55, 255),
                0,
                0,
                2.f);
            draw->AddText(
                ImVec2{
                    origin.x + area.x * .5f + 8,
                    origin.y + 7
                },
                IM_COL32_WHITE,
                "側面 (Z/Y)");
            draw->AddText(
                ImVec2{
                    origin.x + area.x * .5f - 95.0f,
                    origin.y + 7
                },
                IM_COL32(90, 215, 255, 255),
                "水色=実モデル形状");
            draw->AddText(
                ImVec2{
                    origin.x + area.x - 145.0f,
                    origin.y + 7
                },
                IM_COL32(255, 185, 55, 255),
                "橙=当たり判定");
        }
        ImGui::End();
    }
}
