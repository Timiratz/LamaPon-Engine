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
    class LitMaterial final
    {
    public:
        static constexpr std::size_t CustomParameterCount = 8;
        static constexpr std::size_t CustomVectorCount = 64;

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
        [[nodiscard]] const DirectX::XMFLOAT4&
            BaseColor() const noexcept
        {
            return m_baseColor;
        }

        void SetAlbedoTexture(
            std::filesystem::path path) noexcept
        {
            m_albedoTexture = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            AlbedoTexture() const noexcept
        {
            return m_albedoTexture;
        }

        // カスタムShaderが宣言した追加テクスチャ（t7以降）。
        // エンジンはt0〜t6を使うため、自作Shaderで使える枠は
        // register(t7)からです。マスクやランプなどに使います。
        static constexpr std::size_t CustomTextureCount = 4;
        static constexpr std::size_t CustomTextureFirstSlot = 7;

        void SetCustomTexture(
            const std::size_t index,
            std::filesystem::path path) noexcept
        {
            if (index < CustomTextureCount)
            {
                m_customTextures[index] = std::move(path);
            }
        }
        [[nodiscard]] const std::filesystem::path&
            CustomTexture(
                const std::size_t index) const noexcept
        {
            static const std::filesystem::path empty;
            return index < CustomTextureCount
                ? m_customTextures[index]
                : empty;
        }
        [[nodiscard]] const std::array<
            std::filesystem::path,
            CustomTextureCount>&
            CustomTextures() const noexcept
        {
            return m_customTextures;
        }

        void SetNormalTexture(
            std::filesystem::path path) noexcept
        {
            m_normalTexture = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            NormalTexture() const noexcept
        {
            return m_normalTexture;
        }

        void SetRoughness(const float roughness) noexcept
        {
            m_roughness = std::clamp(
                roughness,
                0.02f,
                1.0f);
        }
        [[nodiscard]] float Roughness() const noexcept
        {
            return m_roughness;
        }

        void SetNormalStrength(const float strength) noexcept
        {
            m_normalStrength = std::clamp(
                strength,
                0.0f,
                2.0f);
        }
        [[nodiscard]] float NormalStrength() const noexcept
        {
            return m_normalStrength;
        }

        // 0=非金属、1=金属（PBRのF0をアルベドへ寄せます）。
        void SetMetallic(const float metallic) noexcept
        {
            m_metallic = std::clamp(
                metallic,
                0.0f,
                1.0f);
        }
        [[nodiscard]] float Metallic() const noexcept
        {
            return m_metallic;
        }

        // PBRマップ。粗さ・金属度は上のスカラー値へ掛け算されます。
        // 読むチャンネルはglTFのmetallicRoughness規約に合わせて
        // 粗さ=G、金属度=B、遮蔽=Rです（グレースケール画像なら
        // どのチャンネルでも同じ値なので気にせず使えます）。
        void SetRoughnessTexture(
            std::filesystem::path path) noexcept
        {
            m_roughnessTexture = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            RoughnessTexture() const noexcept
        {
            return m_roughnessTexture;
        }

        void SetMetallicTexture(
            std::filesystem::path path) noexcept
        {
            m_metallicTexture = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            MetallicTexture() const noexcept
        {
            return m_metallicTexture;
        }

        // 遮蔽（AO）マップ。環境光／IBLにだけ掛かります。
        void SetOcclusionTexture(
            std::filesystem::path path) noexcept
        {
            m_occlusionTexture = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            OcclusionTexture() const noexcept
        {
            return m_occlusionTexture;
        }

        void SetOcclusionStrength(
            const float strength) noexcept
        {
            m_occlusionStrength = std::clamp(
                strength,
                0.0f,
                1.0f);
        }
        [[nodiscard]] float OcclusionStrength() const noexcept
        {
            return m_occlusionStrength;
        }

        // 発光（emissive）。ライティングと無関係に加算されるので、
        // 影の中でも光ります。1を超える値も許して、Bloomが拾える
        // ようにしています。
        void SetEmissiveTexture(
            std::filesystem::path path) noexcept
        {
            m_emissiveTexture = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            EmissiveTexture() const noexcept
        {
            return m_emissiveTexture;
        }

        void SetEmissiveColor(
            const DirectX::XMFLOAT3& color) noexcept
        {
            m_emissiveColor = {
                std::max(color.x, 0.0f),
                std::max(color.y, 0.0f),
                std::max(color.z, 0.0f)
            };
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmissiveColor() const noexcept
        {
            return m_emissiveColor;
        }

        // バリアントのキーワード（#pragma multi_compileで宣言した
        // もの）。立てるとそのキーワード付きでコンパイルされた
        // シェーダーが使われ、分岐そのものが消えます。宣言に無い
        // キーワードは描画時に落とされるので、シェーダーを差し替えた
        // 後でも安全です。
        void SetShaderKeywords(ShaderKeywordSet keywords) noexcept
        {
            m_keywords = std::move(keywords);
        }
        [[nodiscard]] const ShaderKeywordSet&
            ShaderKeywords() const noexcept
        {
            return m_keywords;
        }
        void EnableShaderKeyword(std::string keyword)
        {
            m_keywords.Enable(std::move(keyword));
        }
        void DisableShaderKeyword(
            const std::string_view keyword)
        {
            m_keywords.Disable(keyword);
        }
        [[nodiscard]] bool IsShaderKeywordEnabled(
            const std::string_view keyword) const noexcept
        {
            return m_keywords.IsEnabled(keyword);
        }

        void SetShader(std::filesystem::path path) noexcept
        {
            m_shader = std::move(path);
        }
        [[nodiscard]] const std::filesystem::path&
            Shader() const noexcept
        {
            return m_shader;
        }

        void SetCustomParameter(
            const std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            if (index < m_customParameters.size())
            {
                m_customParameters[index] = value;
            }
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(const std::size_t index) const noexcept
        {
            return m_customParameters[
                std::min(index, m_customParameters.size() - 1)];
        }
        [[nodiscard]] const std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>& CustomParameters() const noexcept
        {
            return m_customParameters;
        }
        void SetCustomVector(
            const std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            if (index < m_customVectors.size())
            {
                m_customVectors[index] = value;
            }
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomVector(const std::size_t index) const noexcept
        {
            return m_customVectors[
                std::min(index, m_customVectors.size() - 1)];
        }
        [[nodiscard]] const std::array<
            DirectX::XMFLOAT4,
            CustomVectorCount>& CustomVectors() const noexcept
        {
            return m_customVectors;
        }

    private:
        DirectX::XMFLOAT4 m_baseColor;
        std::filesystem::path m_albedoTexture;
        std::filesystem::path m_normalTexture;
        std::filesystem::path m_roughnessTexture;
        std::filesystem::path m_metallicTexture;
        std::filesystem::path m_occlusionTexture;
        std::filesystem::path m_emissiveTexture;
        DirectX::XMFLOAT3 m_emissiveColor{};
        float m_occlusionStrength{ 1.0f };
        std::array<
            std::filesystem::path,
            CustomTextureCount> m_customTextures;
        float m_roughness;
        float m_normalStrength;
        float m_metallic{};
        std::filesystem::path m_shader;
        std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount> m_customParameters{};
        std::array<
            DirectX::XMFLOAT4,
            CustomVectorCount> m_customVectors{};
        // 末尾に足しています。既存メンバーの位置がずれないので、
        // 作り直していないGame Module DLLでも読み書きが壊れません。
        ShaderKeywordSet m_keywords;
    };
}
