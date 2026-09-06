#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    class RenderTarget;
    struct ModelAsset;

    // 車種別の走行性能・車体寸法・Colliderを編集するプロジェクトパネルです。
    // JSONの読み書きとプレビュー状態をEditorLayerから分離し、同じ仕組みで
    // プロジェクト固有パネルを追加できる構造を保ちます。
    class VehicleParametersPanel final
    {
    public:
        using StatusSink = std::function<void(std::string, bool)>;

        VehicleParametersPanel(
            GraphicsDevice& graphics,
            AssetManager& assets,
            std::filesystem::path dataPath,
            StatusSink status);
        ~VehicleParametersPanel();

        VehicleParametersPanel(const VehicleParametersPanel&) = delete;
        VehicleParametersPanel& operator=(
            const VehicleParametersPanel&) = delete;

        void Draw(
            const std::string& title,
            bool& open,
            const std::function<void()>& onSaved);

        [[nodiscard]] bool Matches(
            const std::filesystem::path& dataPath) const
        {
            return m_dataPath == dataPath;
        }

    private:
        bool Load();
        bool Save();
        void SetStatus(std::string message, bool error = false) const;

        struct State final
        {
            int selectedVehicle{};
            bool loaded{};
            bool dirty{};
            std::unique_ptr<nlohmann::json> document;
            int previewVehicle{ -1 };
            std::shared_ptr<const ModelAsset> previewModel;
        };

        GraphicsDevice& m_graphics;
        AssetManager& m_assets;
        std::filesystem::path m_dataPath;
        StatusSink m_status;
        State m_state;
        std::unique_ptr<RenderTarget> m_topPreview;
        std::unique_ptr<RenderTarget> m_sidePreview;
    };
}
