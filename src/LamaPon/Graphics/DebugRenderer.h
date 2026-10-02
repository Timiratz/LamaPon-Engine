#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <memory>
#include <span>

namespace LamaPon
{
    struct DebugLine final
    {
        // 線分の始点座標
        DirectX::XMFLOAT3 start{};
        // 線分の終点座標
        DirectX::XMFLOAT3 end{};
        // 線分の色
        DirectX::XMFLOAT4 color{};
    };

    // 線分列と行列を同期呼出し中だけ借用して、稼働中の描画APIへ送る。
    class DebugDrawingBackend
    {
    public:
        // 派生した線分描画資源を解放する。
        virtual ~DebugDrawingBackend() = default;

        // 線分描画基盤の基底を生成する。
        DebugDrawingBackend() = default;
        // 線分描画基盤の複製を禁止する。
        DebugDrawingBackend(const DebugDrawingBackend&) = delete;
        // 線分描画基盤の複製代入を禁止する。
        DebugDrawingBackend& operator=(
            const DebugDrawingBackend&) = delete;

        // 線分列を同期的に描画する(lines: 借用する線分列, view: ビュー行列, projection: 射影行列)。
        virtual void DrawLines(
            std::span<const DebugLine> lines,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection) = 0;
    };

    class DebugRenderer final
    {
    public:
        // 非空の線分描画基盤の所有権を受け取る(backend: 線分描画基盤)。
        explicit DebugRenderer(
            std::unique_ptr<DebugDrawingBackend> backend);
        // 所有する線分描画基盤を解放する。
        ~DebugRenderer();

        // 線分描画器の複製を禁止する。
        DebugRenderer(const DebugRenderer&) = delete;
        // 線分描画器の複製代入を禁止する。
        DebugRenderer& operator=(const DebugRenderer&) = delete;

        // XY平面の境界矩形を描く(bounds: 二次元の境界, color: 線の色, view: ビュー行列, projection: 射影行列)。
        void DrawBounds(
            const Bounds2D& bounds,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 境界直方体の辺を描く(bounds: 三次元の境界, color: 線の色, view: ビュー行列, projection: 射影行列)。
        void DrawBounds(
            const Bounds3D& bounds,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // XZ平面の原点中心の格子を描く(spacing: 正の格子間隔, halfExtent: 正の片側要求範囲, view: ビュー行列, projection: 射影行列)。
        // 格子の入力は有限値とし、範囲と間隔の比をintで表せる値にする。
        void DrawGridXZ(
            float spacing,
            float halfExtent,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // XY平面の原点中心の格子を描く(spacing: 正の格子間隔, halfExtent: 正の片側要求範囲, view: ビュー行列, projection: 射影行列)。
        void DrawGridXY(
            float spacing,
            float halfExtent,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 二点ずつを線分にして描き、余った一点を無視する(points: 点の座標列, color: 線の色, view: ビュー行列, projection: 射影行列)。
        void DrawLines(
            std::span<
                const DirectX::XMFLOAT3> points,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 平行光源の進行方向を矢印で描く(lightWorld: 光源のワールド行列, color: 線の色, view: ビュー行列, projection: 射影行列)。
        void DrawDirectionalLight(
            DirectX::FXMMATRIX lightWorld,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 点光源を三平面の円で描く(lightWorld: 光源のワールド行列, radius: 表示半径の要求値, color: 線の色, view: ビュー行列, projection: 射影行列)。
        // 表示半径は0.1〜3へ制限し、光の実際の到達範囲とは独立させる。
        void DrawPointLight(
            DirectX::FXMMATRIX lightWorld,
            float radius,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // スポット光源を円錐の線で描く(lightWorld: 光源のワールド行列, range: 表示距離の要求値, outerConeAngle: 外側円錐の角、ラジアン, color: 線の色, view: ビュー行列, projection: 射影行列)。
        // 光源行列の方向軸は非ゼロとし、表示距離を0.1〜5、角を1〜89度へ制限する。
        void DrawSpotLight(
            DirectX::FXMMATRIX lightWorld,
            float range,
            float outerConeAngle,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // カメラの視錐台をワールド座標へ変換して描く(cameraWorld: カメラのワールド行列, verticalFieldOfView: 縦視野角、ラジアン, aspectRatio: 幅を高さで割った比率, nearDistance: 近面の距離, farDistance: 遠面の距離, color: 線の色, view: ビュー行列, projection: 射影行列)。
        void DrawFrustum(
            DirectX::FXMMATRIX cameraWorld,
            float verticalFieldOfView,
            float aspectRatio,
            float nearDistance,
            float farDistance,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

    private:
        // 行列を保存形式へ変換して線分描画基盤へ送る(lines: 借用する線分列, view: ビュー行列, projection: 射影行列)。
        void Submit(
            std::span<const DebugLine> lines,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

        // 所有する線分描画基盤
        std::unique_ptr<DebugDrawingBackend> m_backend;
    };
}
