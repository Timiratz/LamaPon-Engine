#pragma once

#include "LamaPon/Components/SpriteMeshDeformer.h"

#include <DirectXMath.h>

#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class Rig2DComponent;

    // パラメータが特定の値のときの部品の姿勢・不透明度・格子の変形です。
    struct Keyform2DKey final
    {
        // このキーのパラメータ値
        float value{};
        // 基準位置からのXY移動
        DirectX::XMFLOAT2 positionOffset{};
        // 基準回転に足すZ回転(度)
        float rotationDegrees{};
        // 基準拡縮に掛けるXY倍率
        DirectX::XMFLOAT2 scale{ 1.0f, 1.0f };
        // Sprite Rendererの不透明度に掛ける倍率(0〜1)
        float opacity{ 1.0f };
        // 格子頂点のローカル移動量で、空か格子と数が合わなければ動かしません
        std::vector<DirectX::XMFLOAT2> vertexOffsets;
    };

    // 1つのパラメータに対応するキーの列です。
    struct Keyform2DChannel final
    {
        // 祖先のRig2Dで探すパラメータ名
        std::string parameter;
        // パラメータ値の昇順で同じ値を持たないキー
        std::vector<Keyform2DKey> keys;
    };

    // 全チャンネルを合成した基準姿勢からの差です。
    struct Keyform2DPose final
    {
        // 基準位置からのXY移動
        DirectX::XMFLOAT2 positionOffset{};
        // 基準回転に足すZ回転(度)
        float rotationDegrees{};
        // 基準拡縮に掛けるXY倍率
        DirectX::XMFLOAT2 scale{ 1.0f, 1.0f };
        // 不透明度に掛ける倍率
        float opacity{ 1.0f };
    };

    // 祖先のRig2Dのパラメータ値に合わせて、この部品の姿勢・不透明度・格子をキーの間で補間して動かします。
    // チャンネルの移動・回転・頂点移動は足し合わせ、拡縮と不透明度は掛け合わせます。
    // 姿勢は記録した基準姿勢からの差として毎フレーム書くため、Transform Animatorと同じ部品には使いません。
    class Keyform2DComponent final : public SpriteMeshDeformer
    {
    public:
        // チャンネル一覧から部品の動きを作ります(channels: 補正して登録するチャンネル)。
        explicit Keyform2DComponent(
            std::vector<Keyform2DChannel> channels = {});

        // 非有限のキーを除き、値の昇順・重複なしへ整えて置き換えます(channels: 新しいチャンネル)。
        // パラメータ名が空のチャンネルは除きます。
        void SetChannels(std::vector<Keyform2DChannel> channels);
        // 登録順のチャンネルを返します。
        [[nodiscard]] const std::vector<Keyform2DChannel>&
            Channels() const noexcept
        {
            return m_channels;
        }
        // 指定パラメータのチャンネルへキーを追加し、同じ値のキーは置き換えます(parameter: パラメータ名, key: 追加するキー)。
        // 名前が空かキーに非有限値があればfalseです。チャンネルがなければ作ります。
        bool SetKey(std::string_view parameter, Keyform2DKey key);
        // 指定パラメータの同じ値のキーを除去し、空になったチャンネルも除きます(parameter: パラメータ名, value: キーの値)。
        bool RemoveKey(std::string_view parameter, float value);

        // 現在の位置・回転・拡縮・不透明度を基準姿勢として記録します。
        // パラメータが既定値で、キーを適用していない状態で呼びます。
        void CaptureRestPose();
        // 保存した基準姿勢を設定します(position: 位置, rotation: 回転クォータニオン, scale: 拡縮, opacity: 不透明度)。
        void SetRestPose(
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT4& rotation,
            const DirectX::XMFLOAT3& scale,
            float opacity) noexcept;
        // 基準姿勢を記録済みか返します。
        [[nodiscard]] bool HasRestPose() const noexcept
        {
            return m_hasRestPose;
        }
        // 基準位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& RestPosition() const noexcept
        {
            return m_restPosition;
        }
        // 基準回転を返します。
        [[nodiscard]] const DirectX::XMFLOAT4& RestRotation() const noexcept
        {
            return m_restRotation;
        }
        // 基準拡縮を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& RestScale() const noexcept
        {
            return m_restScale;
        }
        // 基準の不透明度を返します。
        [[nodiscard]] float RestOpacity() const noexcept
        {
            return m_restOpacity;
        }

        // 現在の姿勢と不透明度の基準からの差を、指定パラメータの値のキーへ記録します(parameter: パラメータ名, value: キーの値)。
        // 同じ値のキーがあれば頂点移動を保ったまま姿勢だけを置き換えます。基準姿勢がなければfalseです。
        bool RecordPoseKey(std::string_view parameter, float value);
        // この部品以外の変形を適用した格子の差を、指定パラメータの値のキーの頂点移動へ記録します(parameter: パラメータ名, value: キーの値)。
        // Sprite Rendererがなければfalseです。
        bool RecordMeshKey(std::string_view parameter, float value);

        // 祖先のRig2Dの現在の値で全チャンネルを合成した差を返します。
        [[nodiscard]] Keyform2DPose EvaluatePose() const;
        // 基準姿勢へ現在の差を適用し、キーが使う項目だけを書き換えます。
        void ApplyPose();
        // キーが使う項目を基準姿勢へ戻します。
        void RestoreRestPose();

        // 格子頂点へ現在の値の頂点移動を足します(sprite: 描画するSprite Renderer, positions: 頂点位置の入出力)。
        void DeformSpriteMesh(
            const SpriteRendererComponent& sprite,
            std::vector<DirectX::XMFLOAT2>& positions) override;
        // 頂点移動を持つキーがあるか返します。
        [[nodiscard]] bool DeformsSpriteMesh() const override;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Keyform2D";
        }

    protected:
        // 基準姿勢がなければ現在の姿勢を記録します(graphics: 使用しない描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 現在のパラメータ値の姿勢を適用します(deltaTime: 使用しない経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 無効化時は基準姿勢へ戻します(active: 新しい稼働状態)。
        void OnActiveStateChanged(bool active) override;

    private:
        // 自身から祖先へ最初のRig2Dを返します。
        [[nodiscard]] Rig2DComponent* FindRig() const;
        // キーが姿勢と不透明度を使うかを数え直します。
        void RefreshUsage() noexcept;

        // 登録順のチャンネル
        std::vector<Keyform2DChannel> m_channels;
        // 基準位置
        DirectX::XMFLOAT3 m_restPosition{};
        // 基準回転クォータニオン
        DirectX::XMFLOAT4 m_restRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 基準拡縮
        DirectX::XMFLOAT3 m_restScale{ 1.0f, 1.0f, 1.0f };
        // 基準の不透明度
        float m_restOpacity{ 1.0f };
        // 基準姿勢を記録済みか
        bool m_hasRestPose{};
        // いずれかのキーが位置・回転・拡縮を変えるか
        bool m_usesTransform{};
        // いずれかのキーが不透明度を変えるか
        bool m_usesOpacity{};
    };
}
