#include "LamaPonLive2D/Live2DModel.h"
#include "LamaPon/Graphics/SpriteRendering.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

namespace
{
    // Live2DモデルのInspector設定
    constexpr char Live2DModelSchema[] = R"schema({"fields":[
        {"name":"modelPath","displayName":"モデル3 JSON","type":"string","default":""},
        {"name":"positionX","displayName":"位置 X","type":"float","default":0.0},
        {"name":"positionY","displayName":"位置 Y","type":"float","default":0.0},
        {"name":"pixelsPerUnit","displayName":"ピクセル倍率","type":"float","default":0.0}
    ]})schema";
}

#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <Live2DCubismCore.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <utility>
#endif

namespace
{
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
    struct DrawableOrder final
    {
        // Coreが返すモデル内の描画順
        int renderOrder{};
        // 対応するDrawable番号
        int drawableIndex{};

        // 描画順とDrawable番号で比較します(left: 左の描画項目, right: 右の描画項目)。
        [[nodiscard]] friend bool operator<(
            const DrawableOrder& left,
            const DrawableOrder& right) noexcept
        {
            return left.renderOrder == right.renderOrder
                ? left.drawableIndex < right.drawableIndex
                : left.renderOrder < right.renderOrder;
        }
    };

    // 相対UTF-8パスをCubism設定の文字列から作ります(value: 設定文字列)。
    [[nodiscard]] std::filesystem::path ToAssetPath(
        const std::string& value)
    {
        return LamaPon::PathFromUtf8(value);
    }
#endif
}

namespace LamaPonLive2D
{
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
    struct Live2DModel::State final
    {
        // Coreが読む64バイト境界のMOC記憶域
        void* mocMemory{};
        // Coreが読む16バイト境界のモデル記憶域
        void* modelMemory{};
        // CoreのMOC構造
        csmMoc* moc{};
        // Coreの更新済みモデル
        csmModel* model{};
        // Coreが計算した標準単位倍率
        float pixelsPerUnit{};
        // モデル画像のGPUビュー
        std::vector<LamaPon::GraphicsViewHandle> textures;
        // 描画順に並べ替える作業列
        std::vector<DrawableOrder> drawableOrder;

        // Core所有の記憶域をモデルの先に解放します。
        ~State()
        {
            _aligned_free(modelMemory);
            _aligned_free(mocMemory);
        }
    };
#else
    struct Live2DModel::State final
    {
    };
#endif

    Live2DModel::Live2DModel() = default;

    Live2DModel::~Live2DModel() = default;

    void Live2DModel::LoadModel(std::string modelPath)
    {
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
        if (modelPath.empty())
        {
            throw std::invalid_argument("Live2D model path is empty.");
        }
        // Cubism CoreとGPU資源を完全に作り直した状態
        auto state = CreateState(modelPath);
        m_state = std::move(state);
        m_modelPath = std::move(modelPath);
#else
        static_cast<void>(modelPath);
        throw std::runtime_error(
            "Live2D Cubism Core is not installed for this project.");
#endif
    }

    const std::string& Live2DModel::ModelPath() const noexcept
    {
        return m_modelPath;
    }

    void Live2DModel::SetPosition(
        const DirectX::XMFLOAT2 position) noexcept
    {
        if (std::isfinite(position.x) && std::isfinite(position.y))
        {
            m_position = position;
        }
    }

    DirectX::XMFLOAT2 Live2DModel::Position() const noexcept
    {
        return m_position;
    }

    void Live2DModel::SetPixelsPerUnit(
        const float pixelsPerUnit)
    {
        if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit < 0.0f)
        {
            throw std::invalid_argument(
                "Live2D pixelsPerUnit must be finite and non-negative.");
        }
        m_pixelsPerUnit = pixelsPerUnit;
    }

    float Live2DModel::PixelsPerUnit() const noexcept
    {
        return m_pixelsPerUnit;
    }

    bool Live2DModel::SetParameter(
        const std::string_view parameterId,
        const float value)
    {
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
        if (m_state == nullptr || m_state->model == nullptr
            || !std::isfinite(value))
        {
            return false;
        }
        // モデルが所有するCubismパラメータ数
        const int parameterCount = csmGetParameterCount(m_state->model);
        // CubismパラメータID配列
        const char** parameterIds = csmGetParameterIds(m_state->model);
        // Coreが読み書きする現在値
        float* parameterValues = csmGetParameterValues(m_state->model);
        // Coreが返す下限値
        const float* minimumValues =
            csmGetParameterMinimumValues(m_state->model);
        // Coreが返す上限値
        const float* maximumValues =
            csmGetParameterMaximumValues(m_state->model);
        // 対象パラメータの番号
        for (int index = 0; index < parameterCount; ++index)
        {
            if (parameterIds[index] == nullptr
                || parameterId != parameterIds[index])
            {
                continue;
            }
            parameterValues[index] = std::clamp(
                value,
                minimumValues[index],
                maximumValues[index]);
            return true;
        }
        return false;
#else
        static_cast<void>(parameterId);
        static_cast<void>(value);
        return false;
#endif
    }

    std::optional<float> Live2DModel::GetParameter(
        const std::string_view parameterId) const
    {
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
        if (m_state == nullptr || m_state->model == nullptr)
        {
            return std::nullopt;
        }
        // モデルが所有するCubismパラメータ数
        const int parameterCount = csmGetParameterCount(m_state->model);
        // CubismパラメータID配列
        const char** parameterIds = csmGetParameterIds(m_state->model);
        // Coreが返す現在値
        const float* parameterValues =
            csmGetParameterValues(m_state->model);
        // 対象パラメータの番号
        for (int index = 0; index < parameterCount; ++index)
        {
            if (parameterIds[index] != nullptr
                && parameterId == parameterIds[index])
            {
                return parameterValues[index];
            }
        }
        return std::nullopt;
#else
        static_cast<void>(parameterId);
        return std::nullopt;
#endif
    }

    void Live2DModel::Start()
    {
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
        if (m_state == nullptr && !m_modelPath.empty())
        {
            LoadModel(m_modelPath);
        }
#endif
    }

    void Live2DModel::OnRender2D(
        const LamaPon::SpriteDrawContext& sprites)
    {
#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
        if (!sprites || m_state == nullptr || m_state->model == nullptr)
        {
            return;
        }
        csmUpdateModel(m_state->model);
        // モデル内のDrawable数
        const int drawableCount = csmGetDrawableCount(m_state->model);
#if defined(LAMAPON_LIVE2D_CUBISM_53)
        // DrawableとOffscreenを含むCore描画順
        const int* renderOrders = csmGetRenderOrders(m_state->model);
        // Cubism 5.3のOffscreenは現在のSprite APIで合成できません。
        if (csmGetOffscreenCount(m_state->model) > 0)
        {
            return;
        }
#else
        // DrawableのCore描画順
        const int* renderOrders =
            csmGetDrawableRenderOrders(m_state->model);
#endif
        // Drawableの可視状態
        const csmFlags* dynamicFlags =
            csmGetDrawableDynamicFlags(m_state->model);
        // Drawableのテクスチャ番号
        const int* textureIndices =
            csmGetDrawableTextureIndices(m_state->model);
        // Drawableの不透明度
        const float* opacities = csmGetDrawableOpacities(m_state->model);
        // Drawableの頂点数
        const int* vertexCounts = csmGetDrawableVertexCounts(m_state->model);
        // DrawableのCore座標
        const csmVector2** positions =
            csmGetDrawableVertexPositions(m_state->model);
        // Drawableのテクスチャ座標
        const csmVector2** uvs =
            csmGetDrawableVertexUvs(m_state->model);
        // Drawableの索引数
        const int* indexCounts = csmGetDrawableIndexCounts(m_state->model);
        // Drawableの三角形索引
        const unsigned short** indices =
            csmGetDrawableIndices(m_state->model);
        if (drawableCount <= 0 || renderOrders == nullptr
            || dynamicFlags == nullptr || textureIndices == nullptr
            || opacities == nullptr || vertexCounts == nullptr
            || positions == nullptr || uvs == nullptr
            || indexCounts == nullptr || indices == nullptr)
        {
            return;
        }

        m_state->drawableOrder.resize(
            static_cast<std::size_t>(drawableCount));
        // 描画順テーブルを更新するDrawable番号
        for (int index = 0; index < drawableCount; ++index)
        {
            m_state->drawableOrder[static_cast<std::size_t>(index)] =
                DrawableOrder{ renderOrders[index], index };
        }
        std::sort(
            m_state->drawableOrder.begin(),
            m_state->drawableOrder.end());

        // 描画順に並んだDrawable番号
        for (const auto& entry : m_state->drawableOrder)
        {
            // 現在描くDrawable番号
            const int drawableIndex = entry.drawableIndex;
            // 対応する画像番号
            const int textureIndex = textureIndices[drawableIndex];
            // 現在Drawableが参照する頂点数
            const int vertexCount = vertexCounts[drawableIndex];
            // 現在Drawableが参照する索引数
            const int indexCount = indexCounts[drawableIndex];
            if ((dynamicFlags[drawableIndex] & csmIsVisible) == 0
                || textureIndex < 0
                || static_cast<std::size_t>(textureIndex)
                    >= m_state->textures.size()
                || vertexCount <= 0
                || static_cast<std::size_t>(vertexCount)
                    > LamaPon::MaximumSpriteMeshVertices
                || indexCount <= 0
                || static_cast<std::size_t>(indexCount)
                    > LamaPon::MaximumSpriteMeshIndices
                || positions[drawableIndex] == nullptr
                || uvs[drawableIndex] == nullptr
                || indices[drawableIndex] == nullptr)
            {
                continue;
            }

            m_vertices.resize(static_cast<std::size_t>(vertexCount));
            // 現在Drawableの頂点番号
            for (int vertexIndex = 0;
                vertexIndex < vertexCount;
                ++vertexIndex)
            {
                // Coreが返したモデル座標
                const auto& position = positions[drawableIndex][vertexIndex];
                // Coreが返したUV座標
                const auto& uv = uvs[drawableIndex][vertexIndex];
                m_vertices[static_cast<std::size_t>(vertexIndex)] =
                    LamaPon::SpriteMeshVertex{
                        DirectX::XMFLOAT2{
                            m_position.x + position.X
                                * (m_pixelsPerUnit > 0.0f
                                    ? m_pixelsPerUnit
                                    : m_state->pixelsPerUnit),
                            m_position.y - position.Y
                                * (m_pixelsPerUnit > 0.0f
                                    ? m_pixelsPerUnit
                                    : m_state->pixelsPerUnit)
                        },
                        DirectX::XMFLOAT2{ uv.X, uv.Y },
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f,
                            std::clamp(opacities[drawableIndex], 0.0f, 1.0f)
                        }
                    };
            }

            m_indices.resize(static_cast<std::size_t>(indexCount));
            // 現在Drawableの三角形索引番号
            for (int index = 0; index < indexCount; ++index)
            {
                m_indices[static_cast<std::size_t>(index)] =
                    indices[drawableIndex][index];
            }
            // 現在Drawableのテクスチャとメッシュ要求
            const LamaPon::SpriteMeshDrawRequest request{
                m_state->textures[static_cast<std::size_t>(textureIndex)],
                std::span<const LamaPon::SpriteMeshVertex>{
                    m_vertices.data(), m_vertices.size() },
                std::span<const std::uint16_t>{
                    m_indices.data(), m_indices.size() },
                static_cast<float>(entry.renderOrder)
            };
            static_cast<void>(sprites.DrawMesh(request));
        }
        csmResetDrawableDynamicFlags(m_state->model);
#else
        static_cast<void>(sprites);
#endif
    }

    void Live2DModel::LoadProperties(
        const std::string_view properties)
    {
        // スクリプト設定JSON
        const auto values = nlohmann::json::parse(properties);
        m_modelPath = values.value("modelPath", std::string{});
        m_position.x = values.value("positionX", 0.0f);
        m_position.y = values.value("positionY", 0.0f);
        m_pixelsPerUnit = values.value("pixelsPerUnit", 0.0f);
        if (!std::isfinite(m_position.x))
        {
            m_position.x = 0.0f;
        }
        if (!std::isfinite(m_position.y))
        {
            m_position.y = 0.0f;
        }
        if (!std::isfinite(m_pixelsPerUnit) || m_pixelsPerUnit < 0.0f)
        {
            m_pixelsPerUnit = 0.0f;
        }
    }

    std::string Live2DModel::SaveProperties() const
    {
        // 保存するモデル設定JSON
        const nlohmann::json values{
            { "modelPath", m_modelPath },
            { "positionX", m_position.x },
            { "positionY", m_position.y },
            { "pixelsPerUnit", m_pixelsPerUnit }
        };
        return values.dump();
    }

#if defined(LAMAPON_LIVE2D_CUBISM_ENABLED)
    std::unique_ptr<Live2DModel::State> Live2DModel::CreateState(
        const std::string& modelPath)
    {
        // プロジェクトの画像・モデル管理
        auto& assets = Graphics().Assets();
        // assets基準のmodel3 JSON相対パス
        const auto relativeModelPath = ToAssetPath(modelPath);
        // model3 JSONバイト列
        const auto modelJsonBytes = assets.ReadFileBytes(relativeModelPath);
        if (modelJsonBytes.empty())
        {
            throw std::runtime_error(
                "Live2D model3 JSON could not be read: " + modelPath);
        }
        // 解析するmodel3 JSON値
        const auto modelJson = nlohmann::json::parse(
            modelJsonBytes.begin(), modelJsonBytes.end());
        // model3が指定するMOC相対パス
        const auto mocPath = relativeModelPath.parent_path()
            / ToAssetPath(
                modelJson.at("FileReferences").at("Moc").get<std::string>());
        // Cubism MOCのバイト列
        const auto mocBytes = assets.ReadFileBytes(mocPath);
        if (mocBytes.empty()
            || mocBytes.size() > std::numeric_limits<unsigned int>::max())
        {
            throw std::runtime_error(
                "Live2D MOC3 file is missing or too large: "
                + LamaPon::PathToUtf8(mocPath));
        }
        // 例外時もCore記憶域を解放する状態
        auto state = std::make_unique<State>();
        state->mocMemory = _aligned_malloc(mocBytes.size(), csmAlignofMoc);
        if (state->mocMemory == nullptr)
        {
            throw std::bad_alloc();
        }
        std::memcpy(state->mocMemory, mocBytes.data(), mocBytes.size());
        state->moc = csmReviveMocInPlace(
            state->mocMemory,
            static_cast<unsigned int>(mocBytes.size()));
        if (state->moc == nullptr)
        {
            throw std::runtime_error("Live2D Core rejected the MOC3 file.");
        }
        // Coreモデルの要求記憶域サイズ
        const unsigned int modelMemorySize = csmGetSizeofModel(state->moc);
        if (modelMemorySize == 0)
        {
            throw std::runtime_error(
                "Live2D Core could not determine the model size.");
        }
        state->modelMemory = _aligned_malloc(
            modelMemorySize,
            csmAlignofModel);
        if (state->modelMemory == nullptr)
        {
            throw std::bad_alloc();
        }
        state->model = csmInitializeModelInPlace(
            state->moc,
            state->modelMemory,
            modelMemorySize);
        if (state->model == nullptr)
        {
            throw std::runtime_error(
                "Live2D Core could not initialize the model.");
        }
        // Cubismキャンバスのピクセル寸法
        csmVector2 canvasSize{};
        // Cubismモデルのキャンバス原点
        csmVector2 canvasOrigin{};
        csmReadCanvasInfo(
            state->model,
            &canvasSize,
            &canvasOrigin,
            &state->pixelsPerUnit);

        // model3 JSONが参照する画像一覧
        const auto& texturePaths = modelJson.at("FileReferences").at("Textures");
        state->textures.reserve(texturePaths.size());
        // 現在読み込むテクスチャ相対パス
        for (const auto& textureValue : texturePaths)
        {
            // assets基準のテクスチャ相対パス
            const auto texturePath = relativeModelPath.parent_path()
                / ToAssetPath(textureValue.get<std::string>());
            // Core texture indexに対応するGPU画像
            const auto textureAsset = assets.LoadTexture(texturePath);
            // GPU画像の共有資源ビュー
            const auto resources = textureAsset->resources.Acquire();
            if (!resources || !resources->shaderResourceView)
            {
                throw std::runtime_error(
                    "Live2D texture could not be uploaded: "
                    + LamaPon::PathToUtf8(texturePath));
            }
            state->textures.push_back(resources->shaderResourceView);
        }
        // 描画順を保持する作業列
        const int drawableCount = csmGetDrawableCount(state->model);
        if (drawableCount <= 0)
        {
            throw std::runtime_error(
                "Live2D model does not contain drawable meshes.");
        }
        state->drawableOrder.resize(
            static_cast<std::size_t>(drawableCount));
        static_cast<void>(canvasSize);
        static_cast<void>(canvasOrigin);
        return state;
    }
#endif

}

LAMAPON_SCRIPT_WITH_SCHEMA(
    LamaPonLive2D::Live2DModel,
    "Game.Live2DModel",
    "Live2Dモデル",
    Live2DModelSchema);
