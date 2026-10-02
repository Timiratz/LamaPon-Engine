#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class EditorGuiRenderer;
    class EditorModelPreviewRenderer;
    class GraphicsDevice;
    class RenderTarget;
    struct ModelAsset;

    // 車種別の走行性能・車体寸法・コライダーを編集し、モデル形状を比較表示します。
    class VehicleParametersPanel final
    {
    public:
        // 状態を通知します（文字列: 通知内容、bool: エラーか）。
        using StatusSink = std::function<void(std::string, bool)>;

        // 描画系と資産管理を借用し車両JSONを読み込みます(graphics: パネルより長く生存する描画系, assets: パネルより長く生存する資産管理, dataPath: 車両JSONのパス, status: 状態通知・空も可)。
        VehicleParametersPanel(
            GraphicsDevice& graphics,
            AssetManager& assets,
            std::filesystem::path dataPath,
            StatusSink status);
        // 車両設定とプレビュー画像を解放します。
        ~VehicleParametersPanel();

        // プレビュー画像の独立性を保つためコピーを禁止します。
        VehicleParametersPanel(const VehicleParametersPanel&) = delete;
        // プレビュー画像の独立性を保つためコピー代入を禁止します。
        VehicleParametersPanel& operator=(
            const VehicleParametersPanel&) = delete;

        // 車両設定を編集し上面と側面を共通縮尺で表示します(guiRenderer: 画像参照の取得先, modelPreviewRenderer: モデルの描画先, title: ウィンドウ名, open: 表示状態, onSaved: 保存成功時の通知・空も可)。
        void Draw(
            EditorGuiRenderer& guiRenderer,
            EditorModelPreviewRenderer& modelPreviewRenderer,
            const std::string& title,
            bool& open,
            const std::function<void()>& onSaved);

        // 編集中の車両JSONのパスと一致するか返します(dataPath: 照合するパス)。
        [[nodiscard]] bool Matches(
            const std::filesystem::path& dataPath) const
        {
            return m_dataPath == dataPath;
        }

    private:
        // 必須項目を検証して車両JSONを読み込み、失敗時は編集中の文書を解除します。
        bool Load();
        // 置換用ファイルを書き終えてから車両JSONを置換し、成功時だけ未保存状態を解除します。
        bool Save();
        // 状態通知があれば呼び出します(message: 通知内容, error: エラー通知か)。
        void SetStatus(std::string message, bool error = false) const;

        struct State final
        {
            // 選択車種の配列添字
            int selectedVehicle{};
            // 車両データを読込済みか
            bool loaded{};
            // 未保存の編集があるか
            bool dirty{};
            // 編集中の車両JSON
            std::unique_ptr<nlohmann::json> document;
            // プレビューモデルの車種添字
            int previewVehicle{ -1 };
            // 表示モデルの共有所有先
            std::shared_ptr<const ModelAsset> previewModel;
        };

        // パネルより長く生存する描画系
        GraphicsDevice& m_graphics;
        // パネルより長く生存する資産管理
        AssetManager& m_assets;
        // 編集対象の車両JSONパス
        std::filesystem::path m_dataPath;
        // 状態通知の所有先
        StatusSink m_status;
        // 車両データとプレビュー状態
        State m_state;
        // 上面モデル画像の所有先
        std::unique_ptr<RenderTarget> m_topPreview;
        // 側面モデル画像の所有先
        std::unique_ptr<RenderTarget> m_sidePreview;
    };
}
