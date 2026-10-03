#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;

    // ローカル位置と初速方向の生成分布を指定する。
    enum class ParticleEmitterShape
    {
        // 原点からY軸中心の円錐内へ放射
        Cone,
        // 原点から球面上の全方向へ放射
        Sphere,
        // 直方体の内部から全方向へ放射
        Box
    };

    // 粒子の板を置く平面を指定する。
    enum class ParticleRenderMode
    {
        // カメラの右軸と上軸の平面
        Billboard,
        // ワールドXZ平面
        Horizontal
    };

    // 数値設定は有限値、生成時の所有者の方向変換は非退化を前提とし、生成後はワールド座標で移動する。
    class ParticleSystemComponent final
        : public Component
    {
    public:
        // カスタムシェーダー定数の個数
        static constexpr std::size_t
            CustomParameterCount = 8;
        // カスタムシェーダー定数の配列型
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        // ワールド座標で移動する板状粒子の生成・再生・描画を作る(maxParticles: 同時生存数の上限, emissionRate: 毎秒の自動生成数, lifetime: 寿命の秒数範囲, speed: 毎秒の初速距離範囲, startSize: 初期の一辺の長さ範囲, startColor: 寿命開始時のRGBA, endColor: 寿命終了時のRGBA, shape: 初期位置と方向の生成形状, texturePath: 粒子画像のパス)。
        explicit ParticleSystemComponent(
            std::uint32_t maxParticles = 512,
            float emissionRate = 36.0f,
            DirectX::XMFLOAT2 lifetime =
                { 0.8f, 1.8f },
            DirectX::XMFLOAT2 speed =
                { 1.0f, 3.0f },
            DirectX::XMFLOAT2 startSize =
                { 0.08f, 0.22f },
            DirectX::XMFLOAT4 startColor =
                { 0.20f, 0.72f, 1.0f, 1.0f },
            DirectX::XMFLOAT4 endColor =
                { 0.04f, 0.18f, 0.55f, 0.0f },
            ParticleEmitterShape shape =
                ParticleEmitterShape::Cone,
            std::filesystem::path texturePath = {});
        // 粒子とCPU側の描画キャッシュを破棄する。
        ~ParticleSystemComponent() override;

        // 上限を1〜4096に収め、末尾の超過粒子を削除して描画容量も確保する(count: 同時生存数の上限)。
        // noexcept内でメモリーを確保するため、確保失敗時はプログラムを終了する。
        void SetMaxParticles(
            std::uint32_t count) noexcept;
        // 毎秒の自動生成数を0〜10000に収める(rate: 毎秒の生成粒子数)。
        void SetEmissionRate(float rate) noexcept;
        // 寿命範囲を並べ替えて0.01〜120秒に収める(range: 下限と上限の秒数)。
        void SetLifetime(
            const DirectX::XMFLOAT2& range) noexcept;
        // 初速範囲を並べ替えて0〜1000に収める(range: 毎秒の移動距離の範囲)。
        void SetStartSpeed(
            const DirectX::XMFLOAT2& range) noexcept;
        // 初期サイズ範囲を並べ替えて0.001〜100に収める(range: 一辺の長さの範囲)。
        void SetStartSize(
            const DirectX::XMFLOAT2& range) noexcept;
        // 寿命終了時のサイズ倍率を0〜10に収める(multiplier: 初期サイズに対する倍率)。
        void SetEndSizeMultiplier(
            float multiplier) noexcept;
        // 開始色のRGBを0〜16、アルファを0〜1に収める(color: アルファ乗算前のRGBA)。
        void SetStartColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 終了色のRGBを0〜16、アルファを0〜1に収める(color: アルファ乗算前のRGBA)。
        void SetEndColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 全粒子への加速度の各軸を−1000〜1000に収める(gravity: ワールド座標の加速度)。
        void SetGravity(
            const DirectX::XMFLOAT3& gravity) noexcept;
        // 生成時のローカル位置と方向の分布を設定する(shape: 粒子の生成形状)。
        void SetEmitterShape(
            ParticleEmitterShape shape) noexcept
        {
            m_shape = shape;
        }
        // 粒子の板をカメラ平面かワールドXZ平面へ向ける(mode: 板の向きを決める描画方式)。
        void SetRenderMode(
            ParticleRenderMode mode) noexcept
        {
            m_renderMode = mode;
        }
        // Box生成範囲の各軸の絶対値を0〜1000に収める(size: ローカルの生成範囲の全幅)。
        void SetEmitterSize(
            const DirectX::XMFLOAT3& size) noexcept;
        // Cone生成の半角の絶対値を0〜π/2に収める(radians: ローカルY軸からの半角)。
        void SetConeAngle(float radians) noexcept;
        // 自動生成期間を0.01〜3600秒に収める(duration: 一周期の生成秒数)。
        void SetDuration(float duration) noexcept;
        // 自動生成期間の末尾で周回するか設定する(looping: 生成期間を周回する指定)。
        void SetLooping(bool looping) noexcept
        {
            m_looping = looping;
        }
        // 初期化時の再生を設定し、粒子と生成時刻が空なら現在の再生状態も変える(enabled: 自動再生する指定)。
        void SetPlayOnStart(bool enabled) noexcept
        {
            m_playOnStart = enabled;
            if (m_particles.empty()
                && m_emittingTime == 0.0f)
            {
                m_playing = enabled;
            }
        }
        // エディターからのプレビュー更新を受け付けるか設定する(enabled: プレビューを更新する指定)。
        void SetPreviewInEditor(
            bool enabled) noexcept
        {
            m_previewInEditor = enabled;
        }
        // 加算合成を切り替え、通常アルファ合成なら粒子を遠い順に描く(additive: 加算合成する指定)。
        void SetAdditive(bool additive) noexcept
        {
            m_additive = additive;
        }
        // 画像を読み込んで差し替え、空なら白色画像を使う(path: 粒子画像のパス)。
        // 読み込み失敗時は以前のパスと画像を保持する。
        void SetTexturePath(
            std::filesystem::path path);
        // 頂点生成は維持してピクセルシェーダーを次の描画で差し替える(path: HLSLパスで空は既定描画)。
        void SetShaderPath(
            std::filesystem::path path);
        // 補助画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: 補助画像のパスで空は解除)。
        void SetAuxiliaryTexturePath(
            std::filesystem::path path);
        // 範囲内のカスタム定数を設定し、範囲外なら無視する(index: 0〜7の定数番号, value: 設定する四成分の定数)。
        void SetCustomParameter(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept;
        // 設定したシェーダーのキャッシュを無効化して次回描画で再読み込みさせる。
        void ReloadShader();

        // 同時生存数の上限を取得する。
        [[nodiscard]] std::uint32_t
            MaxParticles() const noexcept
        {
            return m_maxParticles;
        }
        // 毎秒の自動生成数を取得する。
        [[nodiscard]] float
            EmissionRate() const noexcept
        {
            return m_emissionRate;
        }
        // 生成時に選ぶ寿命の秒数範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            Lifetime() const noexcept
        {
            return m_lifetime;
        }
        // 生成時に選ぶ毎秒の初速距離範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            StartSpeed() const noexcept
        {
            return m_startSpeed;
        }
        // 生成時に選ぶ初期の一辺の長さ範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            StartSize() const noexcept
        {
            return m_startSize;
        }
        // 寿命終了時の初期サイズに対する倍率を取得する。
        [[nodiscard]] float
            EndSizeMultiplier() const noexcept
        {
            return m_endSizeMultiplier;
        }
        // 全粒子の寿命開始時に使うアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            StartColor() const noexcept
        {
            return m_startColor;
        }
        // 全粒子の寿命終了時に使うアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            EndColor() const noexcept
        {
            return m_endColor;
        }
        // 全粒子に適用するワールド座標の加速度を取得する。
        [[nodiscard]] const DirectX::XMFLOAT3&
            Gravity() const noexcept
        {
            return m_gravity;
        }
        // 初期位置と方向を決める生成形状を取得する。
        [[nodiscard]] ParticleEmitterShape
            EmitterShape() const noexcept
        {
            return m_shape;
        }
        // 板の向きを決める描画方式を取得する。
        [[nodiscard]] ParticleRenderMode
            RenderMode() const noexcept
        {
            return m_renderMode;
        }
        // Box生成時のローカル範囲の全幅を取得する。
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmitterSize() const noexcept
        {
            return m_emitterSize;
        }
        // Cone生成時のローカルY軸からの半角をラジアンで取得する。
        [[nodiscard]] float
            ConeAngle() const noexcept
        {
            return m_coneAngle;
        }
        // 一周期の自動生成期間を秒で取得する。
        [[nodiscard]] float Duration() const noexcept
        {
            return m_duration;
        }
        // 自動生成期間を周回する設定か確認する。
        [[nodiscard]] bool Looping() const noexcept
        {
            return m_looping;
        }
        // 初期化時に自動再生する設定か確認する。
        [[nodiscard]] bool
            PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }
        // プレビュー更新を受け付ける設定か確認する。
        [[nodiscard]] bool
            PreviewInEditor() const noexcept
        {
            return m_previewInEditor;
        }
        // 加算合成で描画する設定か確認する。
        [[nodiscard]] bool Additive() const noexcept
        {
            return m_additive;
        }
        // 粒子画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        // カスタムピクセルシェーダーのパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_shaderPath;
        }
        // 補助画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            AuxiliaryTexturePath() const noexcept
        {
            return m_auxiliaryTexturePath;
        }
        // 設定したカスタム定数の配列を取得する。
        [[nodiscard]] const CustomParameters&
            CustomShaderParameters() const noexcept
        {
            return m_customParameters;
        }
        // 指定のカスタム定数を取得し、範囲外なら0を返す(index: 0〜7の定数番号)。
        [[nodiscard]] DirectX::XMFLOAT4
            CustomParameter(
                const std::size_t index) const noexcept
        {
            return index < m_customParameters.size()
                ? m_customParameters[index]
                : DirectX::XMFLOAT4{};
        }
        // 最後の描画で記録したカスタムシェーダーの診断を取得する。
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }
        // 現在生存している粒子数を取得する。
        [[nodiscard]] std::size_t
            ActiveParticleCount() const noexcept
        {
            return m_particles.size();
        }
        // 自動生成が再生状態か確認する。
        [[nodiscard]] bool IsPlaying() const noexcept
        {
            return m_playing;
        }

        // 自動生成を再開し、非周回で期間終了済みなら生成時刻を先頭へ戻す。
        void Play() noexcept;
        // 自動生成と未満一個分の蓄積を停止し、指定があれば既存粒子も消去する(clearParticles: 生存粒子を消去する指定)。
        // falseなら既存粒子の移動と寿命更新は続く。
        void Stop(bool clearParticles = true) noexcept;
        // 粒子・生成時刻・蓄積・乱数を初期化して自動生成を開始する。
        void Restart() noexcept;
        // 再生状態によらず、設定した生成形状で上限まで粒子を追加する(count: 追加を要求する粒子数)。
        void Emit(std::uint32_t count);
        // 上限に空きがあれば指定のワールド位置と初速で一粒を追加する(position: ワールド座標の位置, velocity: ワールド座標の毎秒の速度, lifetime: 0.01〜120に収める寿命秒数, startSize: 0.001〜100に収める一辺, rotation: 板の面内のラジアン角, angularVelocity: 面内の毎秒のラジアン角)。
        void EmitParticle(
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& velocity,
            float lifetime,
            float startSize,
            float rotation = 0.0f,
            float angularVelocity = 0.0f);
        // プレビューが有効なら通常と同じ生成・移動更新を行う(deltaTime: 有限な経過秒数)。
        void UpdatePreview(float deltaTime);

        // 粒子数・画像・合成方式の描画情報を記録する(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "ParticleSystem";
        }

    protected:
        // 描画機器とアセットを借用して画像を読み、自動再生設定を適用する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 粒子の移動・寿命と自動生成を更新する(deltaTime: 有限な経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 寿命割合で色とサイズを補間して板状粒子を描く(view: 有限で逆行列を持つビュー, projection: 射影行列)。
        void OnRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX
                projection) override;

    private:
        struct Particle final
        {
            // ワールド座標の粒子位置
            DirectX::XMFLOAT3 position;
            // ワールド座標の毎秒の速度
            DirectX::XMFLOAT3 velocity;
            // 生成からの経過秒数
            float age{};
            // 生成時に選んだ寿命の秒数
            float lifetime{ 1.0f };
            // 生成時に選んだ一辺の長さ
            float startSize{ 0.1f };
            // 板の面内のラジアン角
            float rotation{};
            // 面内の毎秒のラジアン角
            float angularVelocity{};
        };

        // 経過時間を0〜0.1秒に収めて既存粒子を更新した後に新粒子を生成する(deltaTime: 有限な経過秒数)。
        void Simulate(float deltaTime);
        // 設定した形状の位置と方向をワールドへ変換して一粒を生成する。
        void SpawnOne();
        // 内部のXorshift乱数を進めて0以上1未満の値を取得する。
        [[nodiscard]] float Random01() noexcept;
        // 内部乱数から指定範囲の値を選ぶ(minimum: 範囲の下限, maximum: 範囲の上限)。
        [[nodiscard]] float RandomRange(
            float minimum,
            float maximum) noexcept;
        // 球面上に一様な単位方向を選ぶ。
        [[nodiscard]] DirectX::XMFLOAT3
            RandomUnitVector() noexcept;

        // 同時生存数の絶対上限
        static constexpr std::uint32_t
            AbsoluteMaximumParticles = 4096;

        // 同時生存数の上限
        std::uint32_t m_maxParticles;
        // 毎秒の自動生成数
        float m_emissionRate;
        // 生成時の寿命秒数範囲
        DirectX::XMFLOAT2 m_lifetime;
        // 生成時の初速距離範囲
        DirectX::XMFLOAT2 m_startSpeed;
        // 生成時の一辺の長さ範囲
        DirectX::XMFLOAT2 m_startSize;
        // 寿命終了時のサイズ倍率
        float m_endSizeMultiplier{ 0.15f };
        // 全粒子の寿命開始時のRGBA
        DirectX::XMFLOAT4 m_startColor;
        // 全粒子の寿命終了時のRGBA
        DirectX::XMFLOAT4 m_endColor;
        // 全粒子へのワールド加速度
        DirectX::XMFLOAT3 m_gravity{
            0.0f,
            -1.2f,
            0.0f
        };
        // 初期位置と方向の生成形状
        ParticleEmitterShape m_shape;
        // 粒子の板を置く平面
        ParticleRenderMode m_renderMode{
            ParticleRenderMode::Billboard
        };
        // Box生成範囲のローカル全幅
        DirectX::XMFLOAT3 m_emitterSize{
            1.0f,
            1.0f,
            1.0f
        };
        // Cone生成のラジアン半角
        float m_coneAngle{
            DirectX::XMConvertToRadians(25.0f)
        };
        // 一周期の自動生成秒数
        float m_duration{ 5.0f };
        // 生成期間を周回する指定
        bool m_looping{ true };
        // 初期化時に自動再生する指定
        bool m_playOnStart{ true };
        // プレビューを更新する指定
        bool m_previewInEditor{ true };
        // 加算合成する指定
        bool m_additive{ true };
        // 自動生成が再生状態か
        bool m_playing{ true };
        // 生成周期内の経過秒数
        float m_emittingTime{};
        // 一個未満の自動生成数の蓄積
        float m_spawnAccumulator{};
        // Xorshift乱数の現在状態
        std::uint32_t m_randomState{
            0x7f4a7c15u
        };
        // 粒子画像のパス
        std::filesystem::path m_texturePath;
        // 共有する粒子画像
        std::shared_ptr<const TextureAsset> m_texture;
        // カスタムHLSLのパス
        std::filesystem::path m_shaderPath;
        // 補助画像のパス
        std::filesystem::path m_auxiliaryTexturePath;
        // 共有する補助画像
        std::shared_ptr<const TextureAsset>
            m_auxiliaryTexture;
        // 8個のカスタムシェーダー定数
        CustomParameters m_customParameters{};
        // 最後の描画時のシェーダー世代
        std::uint64_t m_shaderGeneration{};
        // 最後の描画時の診断文字列
        std::string m_shaderError;
        // 生存中の粒子一覧
        std::vector<Particle> m_particles;
        // 描画順の粒子番号一覧
        std::vector<std::size_t> m_renderOrder;
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
        // 借用したアセット管理
        AssetManager* m_assets{};
        // 頂点のCPUキャッシュを所有し、GPU描画資源はGraphicsRenderServicesに委ねる。
        struct RenderData;
        // 所有するCPU頂点キャッシュ
        std::unique_ptr<RenderData> m_renderData;
    };
}
