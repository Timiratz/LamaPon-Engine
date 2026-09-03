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

    if (g_failures != 0)
    {
        return EXIT_FAILURE;
    }
    std::cout << "Procedural mesh tests passed.\n";
    return EXIT_SUCCESS;
}
