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
    // 失敗したassertionの件数
    int g_failures{};

    // 条件不成立を失敗一覧へ追加します。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const std::string& message)
    {
        // assertion失敗を集計する
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // leftとrightの差が1e-5以内か返します。
    // NearlyEqual(left: 左値, right: 右値)
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right) noexcept
    {
        return std::abs(left - right) <= 1.0e-5f;
    }
}

// ProceduralMeshとinstance batch keyを検証します。
int main()
{
    // 検証対象のmesh renderer
    LamaPon::MeshRendererComponent mesh;
    // 法線再計算とbounds検証に使うmesh vertices
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
    // vertex: 生成meshに格納された頂点
    for (const auto& vertex : mesh.ProceduralVertices())
    {
        Require(
            NearlyEqual(vertex.normal.x, 0.0f)
                && NearlyEqual(vertex.normal.y, 1.0f)
                && NearlyEqual(vertex.normal.z, 0.0f),
            "Recalculated normals must be normalized and face upward.");
    }
    // procedural meshのbounds出力先
    LamaPon::Bounds3D bounds{};
    Require(
        mesh.TryGetLocalBounds(bounds)
            && NearlyEqual(bounds.minimum.x, -2.0f)
            && NearlyEqual(bounds.minimum.z, -1.0f)
            && NearlyEqual(bounds.maximum.x, 3.0f)
            && NearlyEqual(bounds.maximum.z, 4.0f),
        "Procedural bounds must cover every vertex for culling.");

    // 範囲外indexを拒否したか
    bool rejectedInvalidIndex = false;
    // 不正geometryを受け付けないことを確認
    try
    {
        mesh.SetProceduralMesh(vertices, { 0u, 1u, 9u });
    }
    // 範囲外index例外を拒否結果にする
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

    // material属性がbatch keyを分け、通常instance色は除外する
    // 既定materialの比較元
    const LamaPon::MeshRendererComponent batchBaseline;
    // 比較元materialのbatch key
    const auto baselineKey = batchBaseline.InstanceBatchKey();
    // 同一属性ならkeyを共有するmaterial
    const LamaPon::MeshRendererComponent matchingBatchMaterial;
    Require(
        matchingBatchMaterial.InstanceBatchKey() == baselineKey,
        "Matching mesh materials must share an instance batch key.");
    // field: material属性名, mutate: 候補へ属性を設定
    const auto requireDifferentBatchKey =
        [baselineKey](const char* const field, auto&& mutate)
    {
        // 比較元から属性を変更する候補material
        LamaPon::MeshRendererComponent candidate;
        mutate(candidate);
        Require(
            candidate.InstanceBatchKey() != baselineKey,
            std::string{ field }
                + " must participate in the mesh instance batch key.");
    };
    // candidate: roughness textureだけを変更
    requireDifferentBatchKey(
        "roughness texture",
        [](auto& candidate)
        {
            candidate.SetRoughnessTexturePath(L"roughness.png");
        });
    // candidate: metallic textureだけを変更
    requireDifferentBatchKey(
        "metallic texture",
        [](auto& candidate)
        {
            candidate.SetMetallicTexturePath(L"metallic.png");
        });
    // candidate: occlusion textureだけを変更
    requireDifferentBatchKey(
        "occlusion texture",
        [](auto& candidate)
        {
            candidate.SetOcclusionTexturePath(L"occlusion.png");
        });
    // candidate: emissive textureだけを変更
    requireDifferentBatchKey(
        "emissive texture",
        [](auto& candidate)
        {
            candidate.SetEmissiveTexturePath(L"emissive.png");
        });
    // index: 各custom texture slot
    for (std::size_t index{};
        index < LamaPon::LitMaterial::CustomTextureCount;
        ++index)
    {
        // candidate: index slotのtextureを変更
        requireDifferentBatchKey(
            "custom texture",
            [index](auto& candidate)
            {
                candidate.SetCustomTexturePath(
                    index,
                    L"custom.png");
            });
    }
    // candidate: occlusion strengthだけを変更
    requireDifferentBatchKey(
        "occlusion strength",
        [](auto& candidate)
        {
            candidate.SetOcclusionStrength(0.25f);
        });
    // candidate: emissive colorだけを変更
    requireDifferentBatchKey(
        "emissive color",
        [](auto& candidate)
        {
            candidate.SetEmissiveColor({ 1.0f, 0.5f, 0.25f });
        });
    // candidate: custom parameterだけを変更
    requireDifferentBatchKey(
        "custom parameter",
        [](auto& candidate)
        {
            candidate.SetCustomParameter(
                LamaPon::LitMaterial::CustomParameterCount - 1,
                { 4.0f, 3.0f, 2.0f, 1.0f });
        });
    // candidate: custom vectorだけを変更
    requireDifferentBatchKey(
        "custom vector",
        [](auto& candidate)
        {
            candidate.SetCustomVector(
                LamaPon::LitMaterial::CustomVectorCount - 1,
                { 1.0f, 2.0f, 3.0f, 4.0f });
        });
    // candidate: shader keywordだけを変更
    requireDifferentBatchKey(
        "shader keyword",
        [](auto& candidate)
        {
            candidate.EnableShaderKeyword("DETAIL_ON");
        });

    // fieldの長さもkeyへ含め、境界衝突を避ける
    // shader/albedo path境界が異なる組A
    LamaPon::MeshRendererComponent splitPathsA;
    splitPathsA.SetShaderPath(L"ab");
    splitPathsA.SetAlbedoTexturePath(L"c");
    // shader/albedo path境界が異なる組B
    LamaPon::MeshRendererComponent splitPathsB;
    splitPathsB.SetShaderPath(L"a");
    splitPathsB.SetAlbedoTexturePath(L"bc");
    Require(
        splitPathsA.InstanceBatchKey()
            != splitPathsB.InstanceBatchKey(),
        "Path field boundaries must participate in the batch key.");

    // keyword境界が異なる組A
    LamaPon::MeshRendererComponent splitKeywordsA;
    splitKeywordsA.SetShaderKeywords(
        LamaPon::ShaderKeywordSet{
            std::vector<std::string>{ "AB", "C" } });
    // keyword境界が異なる組B
    LamaPon::MeshRendererComponent splitKeywordsB;
    splitKeywordsB.SetShaderKeywords(
        LamaPon::ShaderKeywordSet{
            std::vector<std::string>{ "A", "BC" } });
    Require(
        splitKeywordsA.InstanceBatchKey()
            != splitKeywordsB.InstanceBatchKey(),
        "Keyword boundaries must participate in the batch key.");

    // keyword追加順だけを変える組A
    LamaPon::MeshRendererComponent keywordOrderA;
    keywordOrderA.EnableShaderKeyword("SECOND");
    keywordOrderA.EnableShaderKeyword("FIRST");
    // keyword追加順だけを変える組B
    LamaPon::MeshRendererComponent keywordOrderB;
    keywordOrderB.EnableShaderKeyword("FIRST");
    keywordOrderB.EnableShaderKeyword("SECOND");
    Require(
        keywordOrderA.InstanceBatchKey()
            == keywordOrderB.InstanceBatchKey(),
        "Keyword insertion order must not split an instance batch.");

    // 標準shaderでinstance colorだけを変えるmaterial
    LamaPon::MeshRendererComponent coloredInstance;
    coloredInstance.SetColor({ 0.9f, 0.1f, 0.4f, 1.0f });
    Require(
        coloredInstance.InstanceBatchKey() == baselineKey,
        "Per-instance RGB color must not split an instance batch.");

    // custom shaderの色比較元
    LamaPon::MeshRendererComponent customColorA;
    customColorA.SetShaderPath(L"custom.hlsl");
    // custom shader colorを変えた比較先
    LamaPon::MeshRendererComponent customColorB;
    customColorB.SetShaderPath(L"custom.hlsl");
    customColorB.SetColor({ 0.9f, 0.1f, 0.4f, 1.0f });
    Require(
        customColorA.InstanceBatchKey()
            != customColorB.InstanceBatchKey(),
        "Custom shader color must participate in the batch key.");

    // assertion失敗があれば異常終了
    if (g_failures != 0)
    {
        return EXIT_FAILURE;
    }
    std::cout << "Procedural mesh tests passed.\n";
    return EXIT_SUCCESS;
}
