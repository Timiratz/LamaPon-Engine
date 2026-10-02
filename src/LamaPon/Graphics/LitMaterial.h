#pragma once

#include "LamaPon/Graphics/ShaderVariants.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <utility>

namespace LamaPon
{
    // 画像を所有せずパスと数値設定を保持し、数値設定には有限値を渡す。
    class LitMaterial final
    {
    public:
        // カスタム定数の個数
        static constexpr std::size_t CustomParameterCount = 8;
        // カスタムベクトルの個数
        static constexpr std::size_t CustomVectorCount = 64;

        // 基本色・画像パス・表面設定を持つLit材質を作る(baseColor: アルファ乗算前のRGBA, albedoTexture: 基本色画像のパス, normalTexture: 法線画像のパス, roughness: 表面の粗さ, normalStrength: 法線補正の強さ)。
        explicit LitMaterial(
            DirectX::XMFLOAT4 baseColor = {
                0.16f,
                0.65f,
                0.95f,
                1.0f
            },
            std::filesystem::path albedoTexture = {},
            std::filesystem::path normalTexture = {},
            float roughness = 0.5f,
            float normalStrength = 1.0f) noexcept
            : m_baseColor(baseColor)
            , m_albedoTexture(std::move(albedoTexture))
            , m_normalTexture(std::move(normalTexture))
            , m_roughness(roughness)
            , m_normalStrength(normalStrength)
        {
            SetBaseColor(baseColor);
            SetRoughness(roughness);
            SetNormalStrength(normalStrength);
        }

        // 基本色の各成分を0〜1に収めて設定する(color: アルファ乗算前のRGBA)。
        void SetBaseColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_baseColor = {
                std::clamp(color.x, 0.0f, 1.0f),
                std::clamp(color.y, 0.0f, 1.0f),
                std::clamp(color.z, 0.0f, 1.0f),
                std::clamp(color.w, 0.0f, 1.0f)
            };
        }
        // アルファ乗算前の基本色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            BaseColor() const noexcept
        {
            return m_baseColor;
        }

        // 基本色画像のパスを設定する(path: 画像パスで空は未指定)。
        void SetAlbedoTexture(
            std::filesystem::path path) noexcept
        {
            m_albedoTexture = std::move(path);
        }
        // 基本色画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            AlbedoTexture() const noexcept
        {
            return m_albedoTexture;
        }

        // 追加画像はt7〜t10を使い、t0〜t6はエンジン用として維持する。
        // 追加画像スロットの個数
        static constexpr std::size_t CustomTextureCount = 4;
        // 追加画像の先頭スロット番号
        static constexpr std::size_t CustomTextureFirstSlot = 7;

        // t7からの追加画像パスを設定し、範囲外の番号なら無視する(index: 0〜3の追加画像番号, path: 画像パスで空は未指定)。
        void SetCustomTexture(
            const std::size_t index,
            std::filesystem::path path) noexcept
        {
            if (index < CustomTextureCount)
            {
                m_customTextures[index] = std::move(path);
            }
        }
        // 追加画像のパスを取得し、範囲外なら空のパスを返す(index: 0〜3の追加画像番号)。
        [[nodiscard]] const std::filesystem::path&
            CustomTexture(
                const std::size_t index) const noexcept
        {
            // 範囲外の取得で返す空のパス
            static const std::filesystem::path empty;
            return index < CustomTextureCount
                ? m_customTextures[index]
                : empty;
        }
        // 追加画像パスの配列を取得する。
        [[nodiscard]] const std::array<
            std::filesystem::path,
            CustomTextureCount>&
            CustomTextures() const noexcept
        {
            return m_customTextures;
        }

        // 法線画像のパスを設定する(path: 画像パスで空は未指定)。
        void SetNormalTexture(
            std::filesystem::path path) noexcept
        {
            m_normalTexture = std::move(path);
        }
        // 法線画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            NormalTexture() const noexcept
        {
            return m_normalTexture;
        }

        // 粗さを0.02〜1に収める(roughness: 表面の粗さ)。
        void SetRoughness(const float roughness) noexcept
        {
            m_roughness = std::clamp(
                roughness,
                0.02f,
                1.0f);
        }
        // 表面の粗さを取得する。
        [[nodiscard]] float Roughness() const noexcept
        {
            return m_roughness;
        }

        // 法線補正の強さを0〜2に収める(strength: 法線画像の補正倍率)。
        void SetNormalStrength(const float strength) noexcept
        {
            m_normalStrength = std::clamp(
                strength,
                0.0f,
                2.0f);
        }
        // 法線画像の補正倍率を取得する。
        [[nodiscard]] float NormalStrength() const noexcept
        {
            return m_normalStrength;
        }

        // 金属度を0〜1に収める(metallic: 0で非金属から1で金属)。
        void SetMetallic(const float metallic) noexcept
        {
            m_metallic = std::clamp(
                metallic,
                0.0f,
                1.0f);
        }
        // 金属度を取得する。
        [[nodiscard]] float Metallic() const noexcept
        {
            return m_metallic;
        }


        // 粗さに掛ける画像のパスを設定する(path: G成分を使う画像パス)。
        void SetRoughnessTexture(
            std::filesystem::path path) noexcept
        {
            m_roughnessTexture = std::move(path);
        }
        // 粗さ画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            RoughnessTexture() const noexcept
        {
            return m_roughnessTexture;
        }

        // 金属度に掛ける画像のパスを設定する(path: B成分を使う画像パス)。
        void SetMetallicTexture(
            std::filesystem::path path) noexcept
        {
            m_metallicTexture = std::move(path);
        }
        // 金属度画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            MetallicTexture() const noexcept
        {
            return m_metallicTexture;
        }


        // 環境光とIBLへの遮蔽画像のパスを設定する(path: R成分を使う画像パス)。
        void SetOcclusionTexture(
            std::filesystem::path path) noexcept
        {
            m_occlusionTexture = std::move(path);
        }
        // 遮蔽画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            OcclusionTexture() const noexcept
        {
            return m_occlusionTexture;
        }

        // 遮蔽の強さを0〜1に収める(strength: 遮蔽を適用する強さ)。
        void SetOcclusionStrength(
            const float strength) noexcept
        {
            m_occlusionStrength = std::clamp(
                strength,
                0.0f,
                1.0f);
        }
        // 遮蔽を適用する強さを取得する。
        [[nodiscard]] float OcclusionStrength() const noexcept
        {
            return m_occlusionStrength;
        }


        // 光源や影によらず加算する発光画像のパスを設定する(path: 発光画像のパス)。
        void SetEmissiveTexture(
            std::filesystem::path path) noexcept
        {
            m_emissiveTexture = std::move(path);
        }
        // 発光画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            EmissiveTexture() const noexcept
        {
            return m_emissiveTexture;
        }

        // 発光色の各成分を0以上に収め、1を超えるHDR値も保持する(color: 発光のRGB倍率)。
        void SetEmissiveColor(
            const DirectX::XMFLOAT3& color) noexcept
        {
            m_emissiveColor = {
                std::max(color.x, 0.0f),
                std::max(color.y, 0.0f),
                std::max(color.z, 0.0f)
            };
        }
        // 発光色のRGB倍率を取得する。
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmissiveColor() const noexcept
        {
            return m_emissiveColor;
        }

        // バリアントのキーワード集合を置き換える(keywords: 有効にするキーワード集合)。
        void SetShaderKeywords(ShaderKeywordSet keywords) noexcept
        {
            m_keywords = std::move(keywords);
        }
        // 設定されたキーワード集合を取得する。
        [[nodiscard]] const ShaderKeywordSet&
            ShaderKeywords() const noexcept
        {
            return m_keywords;
        }
        // バリアントのキーワードを集合へ追加する(keyword: 有効にするキーワード名)。
        void EnableShaderKeyword(std::string keyword)
        {
            m_keywords.Enable(std::move(keyword));
        }
        // バリアントのキーワードを集合から除く(keyword: 無効にするキーワード名)。
        void DisableShaderKeyword(
            const std::string_view keyword)
        {
            m_keywords.Disable(keyword);
        }
        // 指定のキーワードが集合内で有効か確認する(keyword: 調べるキーワード名)。
        [[nodiscard]] bool IsShaderKeywordEnabled(
            const std::string_view keyword) const noexcept
        {
            return m_keywords.IsEnabled(keyword);
        }

        // シェーダーのパスを設定する(path: HLSLかManifestのパス)。
        void SetShader(std::filesystem::path path) noexcept
        {
            m_shader = std::move(path);
        }
        // 設定したシェーダーのパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            Shader() const noexcept
        {
            return m_shader;
        }

        // 範囲内のカスタム定数を設定し、範囲外なら無視する(index: 0〜7の定数番号, value: 設定する四成分の定数)。
        void SetCustomParameter(
            const std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            if (index < m_customParameters.size())
            {
                m_customParameters[index] = value;
            }
        }
        // カスタム定数を取得し、範囲外なら最後の要素を返す(index: 0〜7の定数番号)。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(const std::size_t index) const noexcept
        {
            return m_customParameters[
                std::min(index, m_customParameters.size() - 1)];
        }
        // カスタム定数の配列を取得する。
        [[nodiscard]] const std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>& CustomParameters() const noexcept
        {
            return m_customParameters;
        }
        // 範囲内のカスタムベクトルを設定し、範囲外なら無視する(index: 0〜63のベクトル番号, value: 設定する四成分の値)。
        void SetCustomVector(
            const std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            if (index < m_customVectors.size())
            {
                m_customVectors[index] = value;
            }
        }
        // カスタムベクトルを取得し、範囲外なら最後の要素を返す(index: 0〜63のベクトル番号)。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomVector(const std::size_t index) const noexcept
        {
            return m_customVectors[
                std::min(index, m_customVectors.size() - 1)];
        }
        // カスタムベクトルの配列を取得する。
        [[nodiscard]] const std::array<
            DirectX::XMFLOAT4,
            CustomVectorCount>& CustomVectors() const noexcept
        {
            return m_customVectors;
        }

    private:
        // アルファ乗算前の基本色RGBA
        DirectX::XMFLOAT4 m_baseColor;
        // 基本色画像のパス
        std::filesystem::path m_albedoTexture;
        // 法線画像のパス
        std::filesystem::path m_normalTexture;
        // 粗さ画像のパス
        std::filesystem::path m_roughnessTexture;
        // 金属度画像のパス
        std::filesystem::path m_metallicTexture;
        // 遮蔽画像のパス
        std::filesystem::path m_occlusionTexture;
        // 発光画像のパス
        std::filesystem::path m_emissiveTexture;
        // HDRを許す発光のRGB倍率
        DirectX::XMFLOAT3 m_emissiveColor{};
        // 遮蔽を適用する強さ
        float m_occlusionStrength{ 1.0f };
        // 4個の追加画像のパス
        std::array<
            std::filesystem::path,
            CustomTextureCount> m_customTextures;
        // 表面の粗さ
        float m_roughness;
        // 法線画像の補正倍率
        float m_normalStrength;
        // 表面の金属度
        float m_metallic{};
        // HLSLかManifestのパス
        std::filesystem::path m_shader;
        // 8個のカスタム定数
        std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount> m_customParameters{};
        // 64個のカスタムベクトル
        std::array<
            DirectX::XMFLOAT4,
            CustomVectorCount> m_customVectors{};
        // 既存フィールドの位置を変えないため、キーワード設定は末尾に置く。
        // バリアントのキーワード集合
        ShaderKeywordSet m_keywords;
    };
}
