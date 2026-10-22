#pragma once

#include "LamaPon/Scripting/Script.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class GraphicsViewHandle;
    struct SpriteMeshVertex;
}

namespace LamaPonLive2D
{
    // Cubism Coreモデルを読み込み、パラメータ操作と2D描画を提供します。
    class Live2DModel final : public LamaPon::Script
    {
    public:
        // 描画状態を初期化します。
        Live2DModel();
        // Cubism Coreのモデルと共有描画資源を解放します。
        ~Live2DModel() override;

        // モデル3 JSONの相対パスを指定して読み込みます(modelPath: assets基準のUTF-8パス)。
        void LoadModel(std::string modelPath);
        // 読み込み済みモデルのUTF-8パスを返します。
        [[nodiscard]] const std::string& ModelPath() const noexcept;
        // 描画モデルの中心位置を設定します(position: 画面ピクセル座標)。
        void SetPosition(DirectX::XMFLOAT2 position) noexcept;
        // 画面ピクセル座標でのモデル中心を返します。
        [[nodiscard]] DirectX::XMFLOAT2 Position() const noexcept;
        // 1モデル単位あたりの描画ピクセル数を設定します(pixelsPerUnit: 0はCoreの値を使う)。
        void SetPixelsPerUnit(float pixelsPerUnit);
        // 現在の描画倍率を返します。
        [[nodiscard]] float PixelsPerUnit() const noexcept;
        // パラメータ値をCoreへ設定します(parameterId: CubismパラメータID, value: 適用値)。
        [[nodiscard]] bool SetParameter(
            std::string_view parameterId,
            float value);
        // Cubismパラメータ値を取得します(parameterId: CubismパラメータID)。
        [[nodiscard]] std::optional<float> GetParameter(
            std::string_view parameterId) const;

        // 保存済みモデルを初回更新前に読み込みます。
        void Start() override;
        // Sceneの更新後にCubism Coreを評価してメッシュを描きます(sprites: 有効な2D描画窓口)。
        void OnRender2D(const LamaPon::SpriteDrawContext& sprites) override;
        // モデル設定を読み込みます(properties: スクリプト設定JSON)。
        void LoadProperties(std::string_view properties) override;
        // モデル設定を保存します。
        [[nodiscard]] std::string SaveProperties() const override;

    private:
        struct State;

        // モデル3 JSONが参照するMOC・画像・Coreモデルを初期化します(modelPath: assets基準のUTF-8パス)。
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
        [[nodiscard]] std::unique_ptr<State> CreateState(
            const std::string& modelPath);
#endif

        // Cubismモデルと対応描画資源
        std::unique_ptr<State> m_state;
        // 保存するモデル3 JSONの相対パス
        std::string m_modelPath;
        // 画面ピクセル座標の中心
        DirectX::XMFLOAT2 m_position{};
        // 1モデル単位あたりのピクセル倍率、0はCore既定
        float m_pixelsPerUnit{};
        // 描画メッシュの再利用頂点列
        std::vector<LamaPon::SpriteMeshVertex> m_vertices;
        // 描画メッシュの再利用索引列
        std::vector<std::uint16_t> m_indices;
    };
}
