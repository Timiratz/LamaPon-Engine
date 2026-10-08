#pragma once

#include "LamaPon/Components/SpriteMeshDeformer.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    class GameObject;

    // 1頂点に影響するボーンと重みです。
    struct SpriteSkinWeight final
    {
        // 1頂点に影響できるボーンの最大数
        static constexpr std::size_t MaximumInfluences = 4;

        // 影響するボーンのBones()内の番号
        std::array<std::uint16_t, MaximumInfluences> bones{};
        // 各ボーンの重みで合計1
        std::array<float, MaximumInfluences> weights{
            1.0f, 0.0f, 0.0f, 0.0f };
    };

    // 同じGameObjectのSprite Rendererの格子を、ボーンにしたGameObjectの動きに合わせて曲げます。
    // バインドした時点のボーンとSpriteのワールド姿勢を基準にし、頂点ごとに最大4本のボーンを重みで混ぜます。
    // ボーンは任意のGameObjectで、Sway2Dを付けると髪や服がしなるように揺れます。
    class SpriteSkin2DComponent final : public SpriteMeshDeformer
    {
    public:
        // 自動で付ける重みの距離減衰の既定値
        static constexpr float DefaultWeightFalloff = 4.0f;

        // ボーン一覧からスキンを作ります(bones: ボーンにするGameObjectのID列)。
        explicit SpriteSkin2DComponent(
            std::vector<std::uint64_t> bones = {});

        // ボーン一覧を置き換えてバインドを解除します(bones: ボーンにするGameObjectのID列)。
        void SetBones(std::vector<std::uint64_t> bones);
        // ボーンにするGameObjectのID列を返します。
        [[nodiscard]] const std::vector<std::uint64_t>&
            Bones() const noexcept
        {
            return m_bones;
        }
        // バインド済みのボーンIDを複製先などのIDへ置き換えます(bones: 同数の新しいID列)。
        // 数が合わなければfalseで、バインドと重みは保持します。
        bool RemapBones(std::vector<std::uint64_t> bones);

        // 自動の重みの距離減衰を0.5〜16へ収めて設定します(falloff: 大きいほど近いボーンだけが効く指数)。
        void SetWeightFalloff(float falloff) noexcept;
        // 自動の重みの距離減衰を返します。
        [[nodiscard]] float WeightFalloff() const noexcept
        {
            return m_weightFalloff;
        }

        // 現在のボーンとSpriteの姿勢を基準にし、重みが格子と合わなければ自動で付けます。
        // Sprite Renderer・ボーン・可逆な姿勢がそろわなければfalseで、以前のバインドを保持します。
        bool Bind();
        // 現在のバインド姿勢で、ボーンからの距離に応じた重みを付け直します。
        bool ComputeAutomaticWeights();
        // バインドを解除して格子を動かさない状態へ戻します。
        void Unbind() noexcept;
        // バインド済みか返します。
        [[nodiscard]] bool IsBound() const noexcept
        {
            return m_bound;
        }
        // 頂点ごとの重みを返します。
        [[nodiscard]] const std::vector<SpriteSkinWeight>&
            Weights() const noexcept
        {
            return m_weights;
        }
        // バインド済みの格子と同数の重みを正規化して設定します(weights: 頂点ごとの重み)。
        // 数・ボーン番号・値が不正ならfalseで変更しません。
        bool SetWeights(std::vector<SpriteSkinWeight> weights);
        // バインドした時点の各ボーンのワールド行列を返します。
        [[nodiscard]] const std::vector<DirectX::XMFLOAT4X4>&
            BoneBindPoses() const noexcept
        {
            return m_boneBindPoses;
        }
        // バインドした時点のSpriteのワールド行列を返します。
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            SpriteBindPose() const noexcept
        {
            return m_spriteBindPose;
        }
        // バインドした格子の列数を返します。
        [[nodiscard]] int BoundColumns() const noexcept
        {
            return m_boundColumns;
        }
        // バインドした格子の行数を返します。
        [[nodiscard]] int BoundRows() const noexcept
        {
            return m_boundRows;
        }
        // 保存したバインドを復元します(boneBindPoses: ボーンと同数の行列, spriteBindPose: Spriteの行列, weights: 頂点ごとの重み, columns: 格子の列数, rows: 格子の行数)。
        // 数や値が不正ならfalseで、バインドを解除します。
        bool RestoreBinding(
            std::vector<DirectX::XMFLOAT4X4> boneBindPoses,
            const DirectX::XMFLOAT4X4& spriteBindPose,
            std::vector<SpriteSkinWeight> weights,
            int columns,
            int rows);

        // Spriteの基準点から反対側の端まで、子の鎖としてボーンを作りバインドします(boneCount: 1〜16のボーン数, addSway: ボーンにSway2Dを付けるか)。
        // 2本以上なら根元のボーンは揺らさず、1本なら根元ごと揺らします。
        // ボーンはこのGameObjectの子になり、作ったボーンを根元から順に返します。Sprite Rendererがなければ空です。
        std::vector<GameObject*> CreateBoneChain(
            int boneCount,
            bool addSway);

        // 格子頂点をボーンの動きに合わせて変形します(sprite: 描画するSprite Renderer, positions: 頂点位置の入出力)。
        void DeformSpriteMesh(
            const SpriteRendererComponent& sprite,
            std::vector<DirectX::XMFLOAT2>& positions) override;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpriteSkin2D";
        }

    protected:
        // ボーンがありバインドされていなければ、現在の姿勢でバインドします(graphics: 使用しない描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;

    private:
        // ボーンにするGameObjectのID列
        std::vector<std::uint64_t> m_bones;
        // バインドした時点のボーンのワールド行列
        std::vector<DirectX::XMFLOAT4X4> m_boneBindPoses;
        // バインドした時点のSpriteのワールド行列
        DirectX::XMFLOAT4X4 m_spriteBindPose{};
        // 頂点ごとの重み
        std::vector<SpriteSkinWeight> m_weights;
        // 自動の重みの距離減衰の指数
        float m_weightFalloff{ DefaultWeightFalloff };
        // バインドした格子の列数
        int m_boundColumns{};
        // バインドした格子の行数
        int m_boundRows{};
        // バインド済みか
        bool m_bound{};
    };
}
