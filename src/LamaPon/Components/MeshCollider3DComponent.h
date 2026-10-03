#pragma once

#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class CollisionMesh;
    class GraphicsDevice;
    struct Bounds3D;
    struct Ray;

    // ワールド空間の三角形1枚（ナローフェーズへ渡す単位）。
    struct MeshColliderTriangle final
    {
        // 第1ワールド頂点
        DirectX::XMFLOAT3 a{};
        // 第2ワールド頂点
        DirectX::XMFLOAT3 b{};
        // 第3ワールド頂点
        DirectX::XMFLOAT3 c{};
    };

    struct MeshColliderRaycastHit final
    {
        // ワールド接触点
        DirectX::XMFLOAT3 point{};
        // 入射側のワールド単位法線
        DirectX::XMFLOAT3 normal{};
        // 始点からのワールド距離
        float distance{};
    };

    // Rigidbodyを持たない地形向けの静的コライダーで、検索には可逆な有限のワールド変換を使います。
    class MeshCollider3DComponent final : public Component
    {
    public:
        // 静的メッシュの衝突設定を作ります(modelPath: モデルパス, offset: ローカル中心位置, isTrigger: 通知だけにする指定, layer: 31以下に制限する所属層, collisionMask: 相手の許可層ビット, material: 制限せず保持する物理材質)。
        explicit MeshCollider3DComponent(
            std::filesystem::path modelPath = {},
            DirectX::XMFLOAT3 offset = {},
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {});

        // パスを設定して初期化済みなら再読込します(path: モデルパスで空なら現メッシュを維持)。
        void SetModelPath(std::filesystem::path path);
        // 直接指定した形状に置換してモデルパスとエラーを消去します(vertices: ローカル頂点列, indices: 三角形の頂点番号列)。
        // 末尾の不完全な三角形番号は除き、範囲外番号・空頂点では空形状になります。
        void SetMesh(
            std::vector<DirectX::XMFLOAT3> vertices,
            std::vector<std::uint32_t> indices);
        // ローカル中心位置を設定します(offset: 中心のXYZ位置)。
        void SetOffset(
            const DirectX::XMFLOAT3& offset) noexcept
        {
            m_offset = offset;
        }
        // 通知だけにする指定を設定します(trigger: 衝突応答を省く指定)。
        void SetTrigger(const bool trigger) noexcept
        {
            m_isTrigger = trigger;
        }
        // 所属層を31以下に制限して設定します(layer: 層番号)。
        void SetLayer(std::uint32_t layer) noexcept;
        // 相手層の許可ビットを設定します(mask: 層ビット集合)。
        void SetCollisionMask(
            const std::uint32_t mask) noexcept
        {
            m_collisionMask = mask;
        }
        // 物理材質を制限せず保持します(material: 物理材質)。
        void SetMaterial(
            const PhysicsMaterial& material) noexcept
        {
            m_material = material;
        }

        // モデルパスを返します。
        [[nodiscard]] const std::filesystem::path&
            ModelPath() const noexcept
        {
            return m_modelPath;
        }
        // ローカル中心位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            Offset() const noexcept
        {
            return m_offset;
        }
        // 衝突応答を行わず通知だけにする指定を返します。
        [[nodiscard]] bool IsTrigger() const noexcept
        {
            return m_isTrigger;
        }
        // 所属層の番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept
        {
            return m_layer;
        }
        // 衝突を許可する相手層のビット集合を返します。
        [[nodiscard]] std::uint32_t
            CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        // 保持している物理材質を返します。
        [[nodiscard]] const PhysicsMaterial&
            Material() const noexcept
        {
            return m_material;
        }
        // 三角形を持つメッシュが設定されているか返します。
        [[nodiscard]] bool HasMesh() const noexcept;
        // 保持する三角形数を返し未設定なら0を返します。
        [[nodiscard]] std::size_t
            TriangleCount() const noexcept;
        // 直近のモデル読み込み失敗の内容を返します。
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }

        // 形状を囲むワールド境界を返し未設定ならローカル半幅0.5の箱を使います。
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;
        // 検索境界に重なる候補のワールド三角形を追加します(worldBounds: ワールド軸平行境界, results: 消去しない追加先)。
        // 逆変換した境界をローカルAABBで囲むため、回転時は余分な候補を含みます。
        void CollectTriangles(
            const Bounds3D& worldBounds,
            std::vector<MeshColliderTriangle>& results)
            const;
        // 距離上限未満の最も近い両面接触を求めます(worldRay: 有限で非ゼロ方向のワールドレイ, maximumDistance: ワールド距離上限, hit: 未検出では変更しない出力)。
        [[nodiscard]] bool Raycast(
            const Ray& worldRay,
            float maximumDistance,
            MeshColliderRaycastHit& hit) const;
        // 逆変換したローカルAABBに三角形が重なるか返します(worldBounds: ワールド軸平行境界)。
        // 回転した検索境界では余分な領域を含む近似判定になります。
        [[nodiscard]] bool OverlapsBounds(
            const Bounds3D& worldBounds) const;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "MeshCollider3D";
        }

    protected:
        // 資産を借用して形状を読み込みます(graphics: 資産を持つ描画装置)。
        // パスと直接指定形状がない場合は同じ物体のModelRendererのパスを使います。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 境界と間引いた三角形の辺を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // 初期化済みでパスがあれば読み込み、標準例外では形状を消去してエラーを記録します。
        void ReloadMesh();
        // ローカル中心位置を先に適用するワールド変換を返します。
        [[nodiscard]] DirectX::XMMATRIX
            WorldMatrixWithOffset() const noexcept;

        // 形状を読み込むモデルパス
        std::filesystem::path m_modelPath;
        // ローカル中心位置
        DirectX::XMFLOAT3 m_offset;
        // 衝突通知だけにする指定
        bool m_isTrigger;
        // 所属層の番号
        std::uint32_t m_layer;
        // 相手層の許可ビット集合
        std::uint32_t m_collisionMask;
        // 制限せず保持する物理材質
        PhysicsMaterial m_material;
        // 共有するローカル衝突形状
        std::shared_ptr<const CollisionMesh> m_mesh;
        // モデル読み込みの失敗内容
        std::string m_lastError;
        // 借用する資産管理
        AssetManager* m_assets{};
    };
}
