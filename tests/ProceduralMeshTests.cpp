#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Physics/CollisionTypes.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    int g_failures{};

    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right) noexcept
    {
        return std::abs(left - right) <= 1.0e-5f;
    }
}

int main()
{
    LamaPon::MeshRendererComponent mesh;
    std::vector<LamaPon::ProceduralMeshVertex> vertices{
        { { -2.0f, 0.0f, -1.0f }, {}, { 0.0f, 0.0f } },
        { { 3.0f, 0.0f, -1.0f }, {}, { 1.0f, 0.0f } },
        { { -2.0f, 0.0f, 4.0f }, {}, { 0.0f, 1.0f } },
    };
    mesh.SetProceduralMesh(vertices, { 0u, 2u, 1u }, true);
    Require(mesh.HasProceduralMesh(), "The procedural mesh was not stored.");
    Require(
        mesh.ProceduralIndices().size() == 3u
            && mesh.ProceduralVertices().size() == 3u,
        "The procedural mesh counts changed.");
    for (const auto& vertex : mesh.ProceduralVertices())
    {
        Require(
            NearlyEqual(vertex.normal.x, 0.0f)
                && NearlyEqual(vertex.normal.y, 1.0f)
                && NearlyEqual(vertex.normal.z, 0.0f),
            "Recalculated normals must be normalized and face upward.");
    }
    LamaPon::Bounds3D bounds{};
    Require(
        mesh.TryGetLocalBounds(bounds)
            && NearlyEqual(bounds.minimum.x, -2.0f)
            && NearlyEqual(bounds.minimum.z, -1.0f)
            && NearlyEqual(bounds.maximum.x, 3.0f)
            && NearlyEqual(bounds.maximum.z, 4.0f),
        "Procedural bounds must cover every vertex for culling.");

    bool rejectedInvalidIndex = false;
    try
    {
        mesh.SetProceduralMesh(vertices, { 0u, 1u, 9u });
    }
    catch (const std::out_of_range&)
    {
        rejectedInvalidIndex = true;
    }
    Require(
        rejectedInvalidIndex
            && mesh.ProceduralIndices().size() == 3u,
        "Invalid geometry must be rejected without replacing the old mesh.");

    mesh.ClearProceduralMesh();
    Require(
        !mesh.HasProceduralMesh()
            && !mesh.TryGetLocalBounds(bounds),
        "Clearing must restore the primitive mesh mode.");

    // Instance batchingは代表Meshのmaterialを全instanceへ使うため、
    // instance attributeの色以外が違うmaterialは別keyでなければなりません。
    const LamaPon::MeshRendererComponent batchBaseline;
    const auto baselineKey = batchBaseline.InstanceBatchKey();
    const LamaPon::MeshRendererComponent matchingBatchMaterial;
    Require(
        matchingBatchMaterial.InstanceBatchKey() == baselineKey,
        "Matching mesh materials must share an instance batch key.");
    const auto requireDifferentBatchKey =
        [baselineKey](const char* const field, auto&& mutate)
    {
        LamaPon::MeshRendererComponent candidate;
        mutate(candidate);
        Require(
            candidate.InstanceBatchKey() != baselineKey,
            std::string{ field }
                + " must participate in the mesh instance batch key.");
    };
    requireDifferentBatchKey(
        "roughness texture",
        [](auto& candidate)
        {
            candidate.SetRoughnessTexturePath(L"roughness.png");
        });
    requireDifferentBatchKey(
        "metallic texture",
        [](auto& candidate)
        {
            candidate.SetMetallicTexturePath(L"metallic.png");
        });
    requireDifferentBatchKey(
        "occlusion texture",
        [](auto& candidate)
        {
            candidate.SetOcclusionTexturePath(L"occlusion.png");
        });
    requireDifferentBatchKey(
        "emissive texture",
        [](auto& candidate)
        {
            candidate.SetEmissiveTexturePath(L"emissive.png");
        });
    for (std::size_t index{};
        index < LamaPon::LitMaterial::CustomTextureCount;
        ++index)
    {
        requireDifferentBatchKey(
            "custom texture",
            [index](auto& candidate)
            {
                candidate.SetCustomTexturePath(
                    index,
                    L"custom.png");
            });
    }
    requireDifferentBatchKey(
        "occlusion strength",
        [](auto& candidate)
        {
            candidate.SetOcclusionStrength(0.25f);
        });
    requireDifferentBatchKey(
        "emissive color",
        [](auto& candidate)
        {
            candidate.SetEmissiveColor({ 1.0f, 0.5f, 0.25f });
        });
    requireDifferentBatchKey(
        "custom parameter",
        [](auto& candidate)
        {
            candidate.SetCustomParameter(
                LamaPon::LitMaterial::CustomParameterCount - 1,
                { 4.0f, 3.0f, 2.0f, 1.0f });
        });
    requireDifferentBatchKey(
        "custom vector",
        [](auto& candidate)
        {
            candidate.SetCustomVector(
                LamaPon::LitMaterial::CustomVectorCount - 1,
                { 1.0f, 2.0f, 3.0f, 4.0f });
        });
    requireDifferentBatchKey(
        "shader keyword",
        [](auto& candidate)
        {
            candidate.EnableShaderKeyword("DETAIL_ON");
        });

    // 可変長文字列は長さもhashし、field境界をまたぐ同じbyte列を
    // 同一materialと誤認しないようにします。
    LamaPon::MeshRendererComponent splitPathsA;
    splitPathsA.SetShaderPath(L"ab");
    splitPathsA.SetAlbedoTexturePath(L"c");
    LamaPon::MeshRendererComponent splitPathsB;
    splitPathsB.SetShaderPath(L"a");
    splitPathsB.SetAlbedoTexturePath(L"bc");
    Require(
        splitPathsA.InstanceBatchKey()
            != splitPathsB.InstanceBatchKey(),
        "Path field boundaries must participate in the batch key.");

    LamaPon::MeshRendererComponent splitKeywordsA;
    splitKeywordsA.SetShaderKeywords(
        LamaPon::ShaderKeywordSet{
            std::vector<std::string>{ "AB", "C" } });
    LamaPon::MeshRendererComponent splitKeywordsB;
    splitKeywordsB.SetShaderKeywords(
        LamaPon::ShaderKeywordSet{
            std::vector<std::string>{ "A", "BC" } });
    Require(
        splitKeywordsA.InstanceBatchKey()
            != splitKeywordsB.InstanceBatchKey(),
        "Keyword boundaries must participate in the batch key.");

    LamaPon::MeshRendererComponent keywordOrderA;
    keywordOrderA.EnableShaderKeyword("SECOND");
    keywordOrderA.EnableShaderKeyword("FIRST");
    LamaPon::MeshRendererComponent keywordOrderB;
    keywordOrderB.EnableShaderKeyword("FIRST");
    keywordOrderB.EnableShaderKeyword("SECOND");
    Require(
        keywordOrderA.InstanceBatchKey()
            == keywordOrderB.InstanceBatchKey(),
        "Keyword insertion order must not split an instance batch.");

    LamaPon::MeshRendererComponent coloredInstance;
    coloredInstance.SetColor({ 0.9f, 0.1f, 0.4f, 1.0f });
    Require(
        coloredInstance.InstanceBatchKey() == baselineKey,
        "Per-instance RGB color must not split an instance batch.");

    LamaPon::MeshRendererComponent customColorA;
    customColorA.SetShaderPath(L"custom.hlsl");
    LamaPon::MeshRendererComponent customColorB;
    customColorB.SetShaderPath(L"custom.hlsl");
    customColorB.SetColor({ 0.9f, 0.1f, 0.4f, 1.0f });
    Require(
        customColorA.InstanceBatchKey()
            != customColorB.InstanceBatchKey(),
        "Custom shader color must participate in the batch key.");

    if (g_failures != 0)
    {
        return EXIT_FAILURE;
    }
    std::cout << "Procedural mesh tests passed.\n";
    return EXIT_SUCCESS;
}
