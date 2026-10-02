#include "LamaPon/Editor/VehicleParametersPanel.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Editor/EditorModelPreviewRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <Windows.h>
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
    // 必須項目の型と三成分配列を検証し、不正なら例外を投げます(vehicle: 検証対象の車両設定)。
    void ValidateVehicle(const nlohmann::json& vehicle)
    {
        if (!vehicle.is_object())
        {
            throw std::runtime_error(
                "vehiclesの各要素はオブジェクトにしてください");
        }

        // 必須の文字列項目名
        constexpr std::array stringFields{
            "id", "layout", "menu_name"
        };
        // 検証する項目名
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

        // 必須の数値項目名
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
        // 検証する項目名
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

        // 必須の三成分配列項目名
        constexpr std::array vectorFields{
            "collider_half_extents", "collider_offset"
        };
        // 検証する項目名
        for (const char* field : vectorFields)
        {
            // 三成分の数値配列を検証します(value: 検証する成分)。
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
    // プレビュー画像を用意して車両JSONを読み込みます。
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

    // 車両設定とプレビュー画像を解放します。
    VehicleParametersPanel::~VehicleParametersPanel() = default;

    // 登録された状態通知へ内容を渡します。
    void VehicleParametersPanel::SetStatus(
        std::string message,
        const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    // 車両JSONを検証して編集状態を初期化します。
    bool VehicleParametersPanel::Load()
    {
        try
        {
            // 車両JSONの入力ストリーム
            std::ifstream input(m_dataPath, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error(
                    "車両パラメーターJSONを開けません");
            }
            // 検証前の読込文書
            auto document = std::make_unique<nlohmann::json>();
            // 検証前の読込文書
            input >> *document;
            if (!document->is_object()
                || !document->contains("vehicles")
                || !document->at("vehicles").is_array()
                || document->at("vehicles").empty())
            {
                throw std::runtime_error(
                    "空ではないvehicles配列が必要です");
            }
            // 選択または検証対象の車両設定
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
        // 失敗理由を状態表示へ渡します(exception: 失敗理由)。
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

    // 書込を確認して車両JSONを置換し、失敗時は未保存状態を保持します。
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
            // 置換前の書込ファイルパス
            const auto temporary = m_dataPath.wstring() + L".tmp";
            {
                // 置換用ファイルの出力ストリーム
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
        // 失敗理由を状態表示へ渡します(exception: 失敗理由)。
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "車両パラメーターを保存できません: " }
                    + exception.what(),
                true);
            return false;
        }
    }

    // 車両設定とモデルに重ねた当たり判定を表示します。
    void VehicleParametersPanel::Draw(
        EditorGuiRenderer& guiRenderer,
        EditorModelPreviewRenderer& modelPreviewRenderer,
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

        // カタログの車両配列
        auto& vehicles = m_state.document->at("vehicles");
        m_state.selectedVehicle = std::clamp(
            m_state.selectedVehicle,
            0,
            static_cast<int>(vehicles.size()) - 1);
        // 車種選択用の借用文字列
        std::vector<const char*> labels;
        labels.reserve(vehicles.size());
        // 選択または検証対象の車両設定
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
            // JSONと表示ラベルの参照が差し替わるので、このフレームはここで終えます。
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

        // 選択または検証対象の車両設定
        auto& vehicle = vehicles.at(m_state.selectedVehicle);
        if (m_state.previewVehicle != m_state.selectedVehicle)
        {
            m_state.previewVehicle = m_state.selectedVehicle;
            m_state.previewModel.reset();
            try
            {
                // 選択車種のモデル資産パス
                const auto modelPath = PathFromUtf8(
                    vehicle.value("model_path", std::string{}));
                if (!modelPath.empty())
                {
                    m_state.previewModel =
                        m_assets.CreateModelInstance(modelPath);
                }
            }
            // モデル読込の失敗を通知します(exception: 失敗理由)。
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
        // 数値入力を車両設定へ反映します(label: 入力欄名, key: JSON項目名, speed: 操作時の変化量, minimum: 入力下限, maximum: 入力上限, format: 数値の表示書式)。
        const auto drag = [&vehicle, this](
            const char* label,
            const char* key,
            const float speed,
            const float minimum,
            const float maximum,
            const char* format)
        {
            // 編集中の数値
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
            // コライダー半サイズのJSON配列
            auto& extents = vehicle.at("collider_half_extents");
            // コライダー中心のJSON配列
            auto& offset = vehicle.at("collider_offset");
            // 編集用の半サイズXYZ・m
            float e[3]{
                extents[0].get<float>(),
                extents[1].get<float>(),
                extents[2].get<float>()
            };
            // 編集用の中心オフセットXYZ・m
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
                // 編集するXYZ成分の添字
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
                // 編集するXYZ成分の添字
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

            // プレビュー領域左上・画面座標
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            // プレビュー領域の幅と高さ・px
            const ImVec2 area{
                ImGui::GetContentRegionAvail().x,
                230.f
            };
            ImGui::InvisibleButton("##VehicleColliderPreview", area);
            // プレビューと補助線の描画先
            auto* const draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(
                origin,
                ImVec2{ origin.x + area.x, origin.y + area.y },
                IM_COL32(18, 22, 28, 255));

            // 上面と側面の表示幅・px
            const float previewWidth = std::max(
                (area.x - 12.0f) * 0.5f,
                64.0f);
            // 上面と側面の表示高さ・px
            const float previewHeight = std::min(
                std::max(area.y - 30.0f, 64.0f),
                previewWidth / 1.6f);
            // プレビューの幅と高さの比率
            const float previewAspect =
                previewWidth / previewHeight;
            // 二方向共通の表示範囲幅・m
            float sharedWorldWidth = std::max({
                e[0] * 2.4f,
                e[2] * 2.4f,
                e[2] * 2.4f * previewAspect,
                e[1] * 2.4f * previewAspect,
                0.1f
            });
            // 上面の表示倍率・px毎m
            float topOverlayScale =
                previewWidth / sharedWorldWidth;
            // 側面の表示倍率・px毎m
            float sideOverlayScale = topOverlayScale;
            // モデルまたはコライダー中心
            DirectX::XMFLOAT3 modelCenter{ o[0], o[1], o[2] };

            if (m_state.previewModel
                && m_state.previewModel->hasLocalBounds)
            {
                // モデル描画画像の幅・px
                constexpr std::uint32_t previewTargetWidth = 512;
                // モデル描画画像の高さ・px
                const auto previewTargetHeight =
                    static_cast<std::uint32_t>(std::max(
                        1.0f,
                        std::round(
                            static_cast<float>(previewTargetWidth)
                            / previewAspect)));
                m_graphics.ResizeOffscreenTarget(
                    *m_topPreview,
                    previewTargetWidth,
                    previewTargetHeight);
                m_graphics.ResizeOffscreenTarget(
                    *m_sidePreview,
                    previewTargetWidth,
                    previewTargetHeight);

                // モデルのローカル境界
                const auto& bounds =
                    m_state.previewModel->localBounds;
                // モデル境界の中心
                const DirectX::XMFLOAT3 center3{
                    (bounds.minimum.x + bounds.maximum.x) * 0.5f,
                    (bounds.minimum.y + bounds.maximum.y) * 0.5f,
                    (bounds.minimum.z + bounds.maximum.z) * 0.5f
                };
                // モデルのX方向の長さ・m
                const float sizeX = std::max(
                    bounds.maximum.x - bounds.minimum.x,
                    0.1f);
                // モデルのY方向の長さ・m
                const float sizeY = std::max(
                    bounds.maximum.y - bounds.minimum.y,
                    0.1f);
                // モデルのZ方向の長さ・m
                const float sizeZ = std::max(
                    bounds.maximum.z - bounds.minimum.z,
                    0.1f);
                modelCenter = center3;
                // モデル中心からカメラの距離
                const float distance =
                    std::max({ sizeX, sizeY, sizeZ }) * 3.0f
                    + 1.0f;

                // モデルを描画して画像を公開します(target: 描画先, view: ビュー行列, projection: 射影行列)。
                const auto renderWireframe = [
                    this,
                    &modelPreviewRenderer](
                    RenderTarget& target,
                    const DirectX::XMMATRIX& view,
                    const DirectX::XMMATRIX& projection)
                {
                    // モデル画像の背景色RGBA
                    constexpr float clear[]{
                        0.035f, 0.045f, 0.06f, 1.0f
                    };
                    m_graphics.BeginOffscreenTarget(
                        target,
                        clear);
                    // ワイヤーフレーム用の材質
                    const LitMaterial material{
                        DirectX::XMFLOAT4{
                            0.15f, 0.82f, 1.0f, 1.0f
                        },
                        {},
                        {},
                        0.8f
                    };
                    modelPreviewRenderer.DrawModel(
                        *m_state.previewModel,
                        DirectX::XMMatrixIdentity(),
                        view,
                        projection,
                        material,
                        true);
                    m_graphics.PublishOffscreenTarget(target);
                };

                // カメラの注視点
                const auto focus = DirectX::XMLoadFloat3(&center3);
                // X方向で当たり判定を収める幅
                const float colliderSpanX = 2.0f
                    * (std::abs(o[0] - center3.x) + e[0])
                    * 1.12f;
                // Y方向で当たり判定を収める幅
                const float colliderSpanY = 2.0f
                    * (std::abs(o[1] - center3.y) + e[1])
                    * 1.12f;
                // Z方向で当たり判定を収める幅
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
                // 二方向共通の表示範囲高・m
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
                    guiRenderer.DisplayTextureReference(
                        *m_topPreview),
                    ImVec2{ origin.x, origin.y + 25.0f },
                    ImVec2{
                        origin.x + previewWidth,
                        origin.y + 25.0f + previewHeight
                    });
                draw->AddImage(
                    guiRenderer.DisplayTextureReference(
                        *m_sidePreview),
                    ImVec2{
                        origin.x + area.x - previewWidth,
                        origin.y + 25.0f
                    },
                    ImVec2{
                        origin.x + area.x,
                        origin.y + 25.0f + previewHeight
                    });
            }

            // 上面の当たり判定中心・画面座標
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

            // 側面の当たり判定中心・画面座標
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
