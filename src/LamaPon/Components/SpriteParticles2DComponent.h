#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;

    // ワールドXYで生成・移動する粒子を通常のスプライト描画で表示する。
    // 数値設定は有限値を前提とし、UIRectTransformがある所有者では描画しない。
    class SpriteParticles2DComponent final : public Component
    {
    public:
        // 所有者のワールドXY位置から放射状に生成する2D粒子を作る(maxParticles: 同時生存数の上限, lifetime: 寿命の秒数範囲, speed: 毎秒の初速距離範囲, startSize: 初期の一辺の長さ範囲, startColor: 寿命開始時のRGBA, endColor: 寿命終了時のRGBA)。
        explicit SpriteParticles2DComponent(
            std::uint32_t maxParticles = 128,
            DirectX::XMFLOAT2 lifetime = { 0.25f, 0.65f },
            DirectX::XMFLOAT2 speed = { 24.0f, 72.0f },
            DirectX::XMFLOAT2 startSize = { 4.0f, 12.0f },
            DirectX::XMFLOAT4 startColor =
                { 1.0f, 0.85f, 0.35f, 1.0f },
            DirectX::XMFLOAT4 endColor =
                { 1.0f, 0.25f, 0.05f, 0.0f });

        // 上限を1〜4096に収め、超過粒子を末尾から削除して容量を確保する(count: 同時生存数の上限)。
        // noexcept内でメモリーを確保するため、確保失敗時はプログラムを終了する。
        void SetMaxParticles(std::uint32_t count) noexcept;
        // 寿命範囲を並べ替えて0.01〜120秒に収める(range: 下限と上限の秒数)。
        void SetLifetime(
            const DirectX::XMFLOAT2& range) noexcept;
        // 初速範囲を並べ替えて0〜10000に収める(range: 毎秒の移動距離の範囲)。
        void SetStartSpeed(
            const DirectX::XMFLOAT2& range) noexcept;
        // 初期サイズ範囲を並べ替えて0.001〜1000に収める(range: 一辺の長さの範囲)。
        void SetStartSize(
            const DirectX::XMFLOAT2& range) noexcept;
        // 毎秒のサイズ変化を−10000〜10000に収める(growth: 一辺の毎秒の変化量)。
        void SetSizeGrowth(float growth) noexcept;
        // XY加速度の各軸を−10000〜10000に収める(gravity: 毎秒の速度変化量)。
        void SetGravity(
            const DirectX::XMFLOAT2& gravity) noexcept;
        // 速度の線形減衰係数を0〜100に収める(drag: 毎秒の速度減衰係数)。
        void SetDrag(float drag) noexcept;
        // 開始色のRGBを0〜16、アルファを0〜1に収める(color: アルファ乗算前のRGBA)。
        void SetStartColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 終了色のRGBを0〜16、アルファを0〜1に収める(color: アルファ乗算前のRGBA)。
        void SetEndColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 画像パスを更新して旧画像を解除し、空なら白色の単色表示にする(path: 粒子画像のパス)。
        // 読み込み失敗時は新しいパスを保持し、以前の画像は復元しない。
        void SetTexturePath(std::filesystem::path path);
        // 2D描画の並び順を設定する(sortOrder: 小さいほど先に描く順番)。
        void SetSortOrder(int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        // 同時生存数の上限を取得する。
        [[nodiscard]] std::uint32_t MaxParticles() const noexcept
        {
            return m_maxParticles;
        }
        // 生成時に選ぶ寿命の秒数範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2& Lifetime() const noexcept
        {
            return m_lifetime;
        }
        // 生成時に選ぶ毎秒の初速距離範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2& StartSpeed() const noexcept
        {
            return m_startSpeed;
        }
        // 生成時に選ぶ初期の一辺の長さ範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2& StartSize() const noexcept
        {
            return m_startSize;
        }
        // 一辺の長さの毎秒の変化量を取得する。
        [[nodiscard]] float SizeGrowth() const noexcept
        {
            return m_sizeGrowth;
        }
        // 全粒子に適用するXY加速度を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2& Gravity() const noexcept
        {
            return m_gravity;
        }
        // 全粒子に適用する速度の線形減衰係数を取得する。
        [[nodiscard]] float Drag() const noexcept
        {
            return m_drag;
        }
        // 全粒子の寿命開始時に使うアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4& StartColor() const noexcept
        {
            return m_startColor;
        }
        // 全粒子の寿命終了時に使うアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4& EndColor() const noexcept
        {
            return m_endColor;
        }
        // 粒子画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        // 設定された2D描画順を取得する。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }
        // 現在生存している粒子数を取得する。
        [[nodiscard]] std::size_t ActiveParticleCount() const noexcept
        {
            return m_particles.size();
        }

        // 所有者の現在のワールドXYに上限まで粒子を追加する(count: 追加を要求する粒子数)。
        void Emit(std::uint32_t count);
        // 生存粒子をすべて消去し、乱数の状態は保持する。
        void Clear() noexcept
        {
            m_particles.clear();
        }

        // 粒子数と画像などの描画情報を記録する(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpriteParticles2D";
        }
        // 2D描画を整列するための順番を取得する。
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画機器とアセットを借用して粒子画像を読む(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 負の時間を0として移動と回転を更新し、寿命切れ粒子を除去する(deltaTime: 有限な経過秒数)。
        void OnUpdate(float deltaTime) override;
        // UI矩形のない所有者の粒子をワールドXYのスプライトで描く(sprites: スプライト描画先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        struct Particle final
        {
            // ワールドXYの粒子位置
            DirectX::XMFLOAT2 position;
            // ワールドXYの毎秒の速度
            DirectX::XMFLOAT2 velocity;
            // 生成からの経過秒数
            float age{};
            // 生成時に選んだ寿命の秒数
            float lifetime{ 0.5f };
            // 生成時に選んだ一辺の長さ
            float size{ 8.0f };
            // 粒子のZ回転角のラジアン
            float rotation{};
            // 毎秒のZ回転角のラジアン
            float angularVelocity{};
        };

        // 内部の線形合同乱数を進めて0以上1未満の値を取得する。
        [[nodiscard]] float Random01() noexcept;
        // 内部乱数から指定範囲の値を選ぶ(minimum: 範囲の下限, maximum: 範囲の上限)。
        [[nodiscard]] float RandomRange(
            float minimum,
            float maximum) noexcept;
        // 以前の画像を解除して設定された粒子画像を読む。
        void RefreshTexture();

        // 同時生存数の絶対上限
        static constexpr std::uint32_t AbsoluteMaximumParticles = 4096;

        // 同時生存数の設定上限
        std::uint32_t m_maxParticles{ 128 };
        // 生成時の寿命秒数範囲
        DirectX::XMFLOAT2 m_lifetime{ 0.25f, 0.65f };
        // 生成時の初速距離範囲
        DirectX::XMFLOAT2 m_startSpeed{ 24.0f, 72.0f };
        // 生成時の一辺の長さ範囲
        DirectX::XMFLOAT2 m_startSize{ 4.0f, 12.0f };
        // 一辺の毎秒の変化量
        float m_sizeGrowth{ -4.0f };
        // 全粒子に適用するXY加速度
        DirectX::XMFLOAT2 m_gravity{ 0.0f, 180.0f };
        // 毎秒の線形速度減衰係数
        float m_drag{ 0.8f };
        // 全粒子の寿命開始時のRGBA
        DirectX::XMFLOAT4 m_startColor{ 1.0f, 0.85f, 0.35f, 1.0f };
        // 全粒子の寿命終了時のRGBA
        DirectX::XMFLOAT4 m_endColor{ 1.0f, 0.25f, 0.05f, 0.0f };
        // 粒子画像のパス
        std::filesystem::path m_texturePath;
        // 共有する粒子画像
        std::shared_ptr<const TextureAsset> m_texture;
        // 生存中の粒子一覧
        std::vector<Particle> m_particles;
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
        // 借用したアセット管理
        AssetManager* m_assets{};
        // 2D描画の並び順
        int m_sortOrder{};
        // 線形合同乱数の現在状態
        std::uint32_t m_randomState{ 0x91e10da5u };
    };
}
