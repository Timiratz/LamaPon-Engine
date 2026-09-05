#pragma once

#include "LamaPon/Web/WebMath.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace LamaPon::Web
{
    struct Color final
    {
        float r{};
        float g{};
        float b{};
        float a{ 1.0f };
    };

    struct Vertex3D final
    {
        Vec3 position{};
        Vec3 normal{ 0.0f, 1.0f, 0.0f };
        Vec2 uv{};
    };

    struct Camera3D final
    {
        Vec3 position{ 0.0f, 3.0f, 8.0f };
        Vec3 target{};
        Vec3 up{ 0.0f, 1.0f, 0.0f };
        float verticalFieldOfViewRadians{ 0.8f };
        float nearPlane{ 0.05f };
        float farPlane{ 500.0f };
    };

    struct Fog3D final
    {
        bool enabled{};
        Color color{ 0.74f, 0.60f, 0.52f, 1.0f };
        float startDistance{ 110.0f };
        float endDistance{ 520.0f };
    };

    struct Lighting3D final
    {
        Color ambientColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        float ambientIntensity{ 0.42f };
        // Windows版のDirectionalLightComponentと同じく、光が進む方向を
        // 表します。
        Vec3 directionalDirection{ -0.25f, -0.75f, -0.90f };
        Color directionalColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        float directionalIntensity{ 0.80f };
    };

    struct Sky3D final
    {
        bool enabled{};
        Color topColor{ 0.722f, 0.620f, 0.572f, 1.0f };
        Color horizonColor{ 0.879f, 0.780f, 0.561f, 1.0f };
    };

    using MeshId = std::uint32_t;
    using TextureId = std::uint32_t;

    // Portable 3D向けのWebGL2描画機能です。GLuintやDirectXの型を
    // 公開せず、形状とマテリアルのデータだけを受け取ります。
    class Renderer3D final
    {
    public:
        Renderer3D();
        ~Renderer3D();

        Renderer3D(const Renderer3D&) = delete;
        Renderer3D& operator=(const Renderer3D&) = delete;

        [[nodiscard]] bool Initialize(
            const char* canvasSelector = "#canvas",
            std::uint32_t width = 1280,
            std::uint32_t height = 720) noexcept;
        void Resize(std::uint32_t width, std::uint32_t height) noexcept;
        void BeginFrame(Color clearColor) noexcept;
        void EndFrame() noexcept;
        void SetCamera(const Camera3D& camera) noexcept;
        void SetFog(const Fog3D& fog) noexcept;
        void SetLighting(const Lighting3D& lighting) noexcept;
        void SetSky(const Sky3D& sky) noexcept;

        [[nodiscard]] MeshId CreateMesh(
            const std::vector<Vertex3D>& vertices,
            const std::vector<std::uint32_t>& indices) noexcept;
        [[nodiscard]] TextureId CreateTexture(
            const char* virtualPath) noexcept;
        void UpdateMesh(
            MeshId mesh,
            const std::vector<Vertex3D>& vertices,
            const std::vector<std::uint32_t>& indices) noexcept;
        void DestroyMesh(MeshId mesh) noexcept;
        void DrawMesh(
            MeshId mesh,
            const Mat4& model,
            Color color,
            float roughness = 0.65f,
            TextureId texture = 0,
            bool doubleSided = false,
            bool alphaBlended = false,
            float alphaCutoff = -1.0f,
            TextureId normalTexture = 0,
            float normalStrength = 1.0f,
            float metallic = 0.0f,
            TextureId metallicRoughnessTexture = 0,
            TextureId roughnessTexture = 0,
            TextureId metallicTexture = 0,
            TextureId occlusionTexture = 0,
            float occlusionStrength = 1.0f,
            TextureId emissiveTexture = 0,
            Color emissiveColor = {},
            bool unlit = false,
            Color dielectricSpecular = {
                0.04f, 0.04f, 0.04f, 1.0f },
            bool additiveBlend = false) noexcept;

        [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
        [[nodiscard]] bool UsesCanvas2DFallback() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        Mat4 m_view{ Mat4::Identity() };
        Mat4 m_projection{ Mat4::Identity() };
        Vec3 m_cameraPosition{};
        Fog3D m_fog{};
        Lighting3D m_lighting{};
        Sky3D m_sky{};
        bool m_initialized{};
    };
}
