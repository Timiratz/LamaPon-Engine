#pragma once

#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>

#include <cstdint>
#include <filesystem>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11InputLayout;

namespace DirectX
{
    inline namespace DX11
    {
        class CommonStates;
    }
}

namespace LamaPon
{
    struct Bounds3D;
    struct ShaderRenderState;

    class AssetManager;
    class LitEffect;
    struct LitTextureRequest;
    class SkeletalModel;
    struct ModelAsset;
    struct TextureAsset;
    struct SkeletalPoseSample;

    struct AnimationEventNotification final
    {
        // 通知元の制御状態名
        std::string state;
        // 通知イベント名
        std::string name;
        // 通知の付随文字列
        std::string payload;
        // 定義中の正規化通知時刻
        float normalizedTime{};
    };

    // 数値入力は有限値とし、描画機器と資産管理器はコンポーネントより長く保持する。
    class ModelRendererComponent final : public Component
    {
    public:
        // モデルと再生の初期設定を保持する(modelPath: モデルパス, wireframe: 線表示フラグ, materialOverrideEnabled: 材質上書きフラグ, color: 上書きRGBA色, albedoTexture: 色画像パス, normalTexture: 法線画像パス, roughness: 表面の粗さ, normalStrength: 法線マップ強度, materialAsset: 材質資産パス, animationIndex: 再生クリップ番号, animationSpeed: 再生速度倍率, animationLoop: クリップ循環フラグ, animationPlayOnStart: 自動再生フラグ, animationController: 再生制御器パス, applyRootMotion: ルート移動適用フラグ, rootMotionNode: 移動抽出ノード名, preserveEmbeddedMaterialColor: 埋込材質色の乗算フラグ)。
        explicit ModelRendererComponent(
            std::filesystem::path modelPath = {},
            bool wireframe = false,
            bool materialOverrideEnabled = false,
            DirectX::XMFLOAT4 color = {
                1.0f,
                1.0f,
                1.0f,
                1.0f
            },
            std::filesystem::path albedoTexture = {},
            std::filesystem::path normalTexture = {},
            float roughness = 0.5f,
            float normalStrength = 1.0f,
            std::filesystem::path materialAsset = {},
            std::size_t animationIndex = 0,
            float animationSpeed = 1.0f,
            bool animationLoop = true,
            bool animationPlayOnStart = true,
            std::filesystem::path animationController = {},
            bool applyRootMotion = false,
            std::string rootMotionNode = {},
            bool preserveEmbeddedMaterialColor = false);
        // 保持するモデルと描画資源を解放する。
        ~ModelRendererComponent() override;

        // モデルを差し替えて再生時刻と描画参照を作り直す(modelPath: モデルパス)。
        void SetModelPath(std::filesystem::path modelPath);
        // 設定中のモデルパスを返す。
        [[nodiscard]] const std::filesystem::path& ModelPath() const noexcept
        {
            return m_modelPath;
        }
        // 取得可能なモデル空間の境界を出力する(bounds: 出力境界)。
        [[nodiscard]] bool TryGetLocalBounds(
            Bounds3D& bounds) const noexcept;

        // モデルの線表示を設定する(wireframe: 線表示フラグ)。
        void SetWireframe(const bool wireframe) noexcept { m_wireframe = wireframe; }
        // 線表示の設定を返す。
        [[nodiscard]] bool IsWireframe() const noexcept { return m_wireframe; }

        // クリップ番号を有効範囲に制限し時刻を先頭へ戻す(index: 再生クリップ番号)。
        void SetAnimationIndex(std::size_t index) noexcept;
        // 現在の再生クリップ番号を返す。
        [[nodiscard]] std::size_t AnimationIndex() const noexcept
        {
            return m_animationIndex;
        }
        // モデルの再生クリップ数を返す。
        [[nodiscard]] std::size_t AnimationCount() const noexcept;
        // モデル内の骨格ノード名を列挙する。
        [[nodiscard]] std::vector<std::string>
            SkeletonNodeNames() const;
        // クリップ名を借用し、範囲外なら空を返す(index: クリップ番号)。
        // 返す文字列はモデルの差し替えや再読込で無効になる。
        [[nodiscard]] std::string_view AnimationName(
            std::size_t index) const noexcept;
        // 現在の状態またはクリップの再生区間長を返す。
        [[nodiscard]] float AnimationDuration() const;
        // 再生倍率を設定し、非有限値は1にする(speed: 再生速度倍率)。
        void SetAnimationSpeed(float speed) noexcept;
        // 符号付きの再生速度倍率を返す。
        [[nodiscard]] float AnimationSpeed() const noexcept
        {
            return m_animationSpeed;
        }
        // 単独クリップの循環を設定する(loop: 循環フラグ)。
        void SetAnimationLoop(bool loop) noexcept
        {
            m_animationLoop = loop;
        }
        // 単独クリップの循環設定を返す。
        [[nodiscard]] bool AnimationLoop() const noexcept
        {
            return m_animationLoop;
        }
        // 初期化時の自動再生を設定する(enabled: 自動再生フラグ)。
        void SetAnimationPlayOnStart(bool enabled) noexcept
        {
            m_animationPlayOnStart = enabled;
        }
        // 自動再生の設定を返す。
        [[nodiscard]] bool AnimationPlayOnStart() const noexcept
        {
            return m_animationPlayOnStart;
        }
        // クリップがあれば再生し、終端なら先頭へ戻す。
        void PlayAnimation() noexcept;
        // 時刻と遷移状態を保持して再生を止める。
        void PauseAnimation() noexcept;
        // 再生を止め、時刻・遷移・トリガー・通知を消去する。
        void StopAnimation() noexcept;
        // 再生と制御状態を進める(deltaTime: 正の経過秒数, allowRootMotion: ルート移動許可フラグ)。
        // 同一フレームの姿勢を描画間で共有するため、時刻の進行は描画前に行う。
        void AdvanceAnimation(
            float deltaTime,
            bool allowRootMotion = true);
        // 現在の状態の時刻を区間内に制限する(time: 再生時刻秒)。
        void SetAnimationTime(float time) noexcept;
        // 現在の状態またはクリップの再生時刻秒を返す。
        [[nodiscard]] float AnimationTime() const noexcept
        {
            return m_animationTime;
        }
        // 再生中かどうかを返す。
        [[nodiscard]] bool IsAnimationPlaying() const noexcept
        {
            return m_animationPlaying;
        }
        // 制御器を切り替えて再読込する(path: 制御器パス)。
        // 読込前に旧制御器を破棄するため、失敗時に旧状態へは戻らない。
        void SetAnimationControllerPath(
            std::filesystem::path path);
        // 設定中の再生制御器パスを返す。
        [[nodiscard]] const std::filesystem::path&
            AnimationControllerPath() const noexcept
        {
            return m_animationControllerPath;
        }
        // 制御器を破棄して資産を再読込する。
        void ReloadAnimationController();
        // 1〜96バイトのトリガーを登録する(trigger: 遷移トリガー名)。
        void SetAnimationTrigger(std::string trigger);
        // 現在の制御状態名を返す。
        [[nodiscard]] const std::string&
            CurrentAnimationState() const noexcept
        {
            return m_currentAnimationState;
        }
        // 遷移先の状態が設定されているか返す。
        [[nodiscard]] bool IsAnimationTransitioning() const noexcept
        {
            return !m_nextAnimationState.empty();
        }
        // 制御器の遷移トリガーを重複なしで列挙する。
        [[nodiscard]] std::vector<std::string>
            AnimationTriggers() const;
        // 定義済みパラメーターへ有限値を設定する(parameter: パラメーター名, value: 設定値)。
        void SetAnimationFloat(
            std::string parameter,
            float value);
        // パラメーター値を取得し、未定義なら0を返す(parameter: パラメーター名)。
        [[nodiscard]] float AnimationFloat(
            std::string_view parameter) const noexcept;
        // 制御器の浮動小数点パラメーター定義を返す。
        [[nodiscard]] std::vector<AnimatorFloatParameter>
            AnimationFloatParameters() const;
        // ルート移動の反映を設定する(enabled: 移動適用フラグ)。
        void SetApplyRootMotion(bool enabled) noexcept
        {
            m_applyRootMotion = enabled;
        }
        // ルート移動の反映設定を返す。
        [[nodiscard]] bool ApplyRootMotion() const noexcept
        {
            return m_applyRootMotion;
        }
        // 移動を抽出するノードを指定する(node: 空なら自動選択のノード名)。
        void SetRootMotionNode(std::string node)
        {
            m_rootMotionNode = std::move(node);
        }
        // 移動を抽出するノード名を返す。
        [[nodiscard]] const std::string&
            RootMotionNode() const noexcept
        {
            return m_rootMotionNode;
        }
        // 最古の通知を取り出し、空なら出力を保持する(event: 出力する通知)。
        [[nodiscard]] bool PollAnimationEvent(
            AnimationEventNotification& event);
        // 未取得のアニメーション通知数を返す。
        [[nodiscard]] std::size_t
            PendingAnimationEventCount() const noexcept
        {
            return m_animationEventQueue.size();
        }

        // 材質上書きを切り替え、解除時はモデルを再読込する(enabled: 上書きフラグ)。
        void SetMaterialOverrideEnabled(bool enabled);
        // 材質上書きの設定を返す。
        [[nodiscard]] bool IsMaterialOverrideEnabled() const noexcept
        {
            return m_materialOverrideEnabled;
        }

        // 従来のDirectXTK照明へ切り替える(enabled: 従来照明フラグ)。
        void SetUseLegacyShading(bool enabled);
        // 従来のDirectXTK照明の設定を返す。
        [[nodiscard]] bool UsesLegacyShading() const noexcept
        {
            return m_useLegacyShading;
        }

        // 上書き時の埋込材質色の乗算を設定する(enabled: 乗算フラグ)。
        void SetPreserveEmbeddedMaterialColor(
            const bool enabled) noexcept
        {
            m_preserveEmbeddedMaterialColor = enabled;
        }
        // 埋込材質色の乗算設定を返す。
        [[nodiscard]] bool PreserveEmbeddedMaterialColor() const noexcept
        {
            return m_preserveEmbeddedMaterialColor;
        }

        // 上書き色の各成分を0〜1に制限する(color: 上書きRGBA色)。
        void SetColor(const DirectX::XMFLOAT4& color) noexcept
        {
            m_material.SetBaseColor(color);
        }
        // 描画パスとモデル部品から半透明ソートの対象を判定する。
        [[nodiscard]] bool IsAlphaBlended3D() const override;
        // 上書き用のRGBA色を返す。
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept
        {
            return m_material.BaseColor();
        }

        // 上書き色画像を読み込み、解除時はモデルを再読込する(texturePath: 色画像パス)。
        void SetAlbedoTexturePath(std::filesystem::path texturePath);
        // 上書き色画像のパスを返す。
        [[nodiscard]] const std::filesystem::path&
            AlbedoTexturePath() const noexcept
        {
            return m_material.AlbedoTexture();
        }

        // 追加画像を読み込み、範囲外の番号は無視する(index: 0〜3の追加画像番号, path: 空なら解除の画像パス)。
        void SetCustomTexturePath(
            std::size_t index,
            std::filesystem::path path);
        // 上書き法線画像を読み込み、解除時はモデルを再読込する(texturePath: 法線画像パス)。
        void SetNormalTexturePath(std::filesystem::path texturePath);
        // 上書き法線画像のパスを返す。
        [[nodiscard]] const std::filesystem::path&
            NormalTexturePath() const noexcept
        {
            return m_material.NormalTexture();
        }

        // 上書き粗さ画像を読み込む(texturePath: 空なら解除の画像パス)。
        void SetRoughnessTexturePath(
            std::filesystem::path texturePath);
        // 上書き粗さ画像のパスを返す。
        [[nodiscard]] const std::filesystem::path&
            RoughnessTexturePath() const noexcept
        {
            return m_material.RoughnessTexture();
        }
        // 上書き金属度画像を読み込む(texturePath: 空なら解除の画像パス)。
        void SetMetallicTexturePath(
            std::filesystem::path texturePath);
        // 上書き金属度画像のパスを返す。
        [[nodiscard]] const std::filesystem::path&
            MetallicTexturePath() const noexcept
        {
            return m_material.MetallicTexture();
        }
        // 上書き遮蔽画像を読み込む(texturePath: 空なら解除の画像パス)。
        void SetOcclusionTexturePath(
            std::filesystem::path texturePath);
        // 上書き遮蔽画像のパスを返す。
        [[nodiscard]] const std::filesystem::path&
            OcclusionTexturePath() const noexcept
        {
            return m_material.OcclusionTexture();
        }
        // 上書き発光画像を読み込む(texturePath: 空なら解除の画像パス)。
        void SetEmissiveTexturePath(
            std::filesystem::path texturePath);
        // 上書き発光画像のパスを返す。
        [[nodiscard]] const std::filesystem::path&
            EmissiveTexturePath() const noexcept
        {
            return m_material.EmissiveTexture();
        }
        // 遮蔽の強度を0〜1に制限する(strength: 遮蔽強度)。
        void SetOcclusionStrength(
            const float strength) noexcept
        {
            m_material.SetOcclusionStrength(strength);
        }
        // 上書き材質の遮蔽強度を返す。
        [[nodiscard]] float OcclusionStrength() const noexcept
        {
            return m_material.OcclusionStrength();
        }
        // 発光色の各成分を0以上に制限する(color: HDRのRGB発光色)。
        void SetEmissiveColor(
            const DirectX::XMFLOAT3& color) noexcept
        {
            m_material.SetEmissiveColor(color);
        }
        // 上書き材質のHDR発光色を返す。
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmissiveColor() const noexcept
        {
            return m_material.EmissiveColor();
        }

        // 粗さを0.02〜1に制限する(roughness: 表面の粗さ)。
        void SetRoughness(const float roughness) noexcept
        {
            m_material.SetRoughness(roughness);
        }
        // 上書き材質の粗さを返す。
        [[nodiscard]] float Roughness() const noexcept
        {
            return m_material.Roughness();
        }

        // 法線強度を0〜2に制限する(strength: 法線マップの強度)。
        void SetNormalStrength(const float strength) noexcept
        {
            m_material.SetNormalStrength(strength);
        }
        // 上書き材質の法線強度を返す。
        [[nodiscard]] float NormalStrength() const noexcept
        {
            return m_material.NormalStrength();
        }
        // 金属度を0〜1に制限する(metallic: 金属の割合)。
        void SetMetallic(const float metallic) noexcept
        {
            m_material.SetMetallic(metallic);
        }
        // 上書き材質の金属度を返す。
        [[nodiscard]] float Metallic() const noexcept
        {
            return m_material.Metallic();
        }
        // 自作シェーダーを指定してD3D11の描画効果を更新する(path: 空なら既定のシェーダーパス)。
        void SetShaderPath(std::filesystem::path path);
        // 設定中の自作シェーダーパスを返す。
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_material.Shader();
        }
        // バリアントのキーワードを有効にする(keyword: 有効化するキーワード)。
        // シェーダーが宣言していないキーワードは描画時の正規化で除外される。
        void EnableShaderKeyword(std::string keyword)
        {
            m_material.EnableShaderKeyword(std::move(keyword));
        }
        // バリアントのキーワードを無効にする(keyword: 無効化するキーワード)。
        void DisableShaderKeyword(
            const std::string_view keyword)
        {
            m_material.DisableShaderKeyword(keyword);
        }
        // キーワードの保存状態を返す(keyword: 確認するキーワード)。
        [[nodiscard]] bool IsShaderKeywordEnabled(
            const std::string_view keyword) const noexcept
        {
            return m_material.IsShaderKeywordEnabled(keyword);
        }
        // バリアントのキーワード集合を置き換える(keywords: 新しいキーワード集合)。
        void SetShaderKeywords(ShaderKeywordSet keywords)
        {
            m_material.SetShaderKeywords(std::move(keywords));
        }
        // 保存中のキーワード集合を返す。
        [[nodiscard]] const ShaderKeywordSet&
            ShaderKeywords() const noexcept
        {
            return m_material.ShaderKeywords();
        }

        // 範囲内の追加パラメーターを設定する(index: 0〜7のパラメーター番号, value: 設定する4成分値)。
        void SetCustomParameter(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            m_material.SetCustomParameter(index, value);
        }
        // 追加パラメーターを取得し、範囲外なら末尾を返す(index: パラメーター番号)。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(std::size_t index) const noexcept
        {
            return m_material.CustomParameter(index);
        }
        // D3D11のシェーダーキャッシュを無効化して再取得する。
        void ReloadShader();
        // 最新のシェーダーまたは頂点配置の診断を返す。
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }

        // 上書き用の材質設定を返す。
        [[nodiscard]] const LitMaterial& Material() const noexcept
        {
            return m_material;
        }
        // 材質資産を読み込み、空でなければ上書きを有効にする(path: 空なら関連解除の材質パス)。
        void SetMaterialAssetPath(std::filesystem::path path);
        // 設定中の材質資産を再読込して上書きを有効にする。
        void ReloadMaterialAsset();
        // 設定中の材質資産パスを返す。
        [[nodiscard]] const std::filesystem::path&
            MaterialAssetPath() const noexcept
        {
            return m_materialAssetPath;
        }
        // 従来形式のモデルで材質上書き用の共通照明が使えるか返す。
        [[nodiscard]] bool UsesCommonLit() const noexcept;
        // 骨格または従来形式のモデルで共通照明が選択されているか返す。
        [[nodiscard]] bool UsesLamaPonLit() const noexcept;
        // 共通照明の対応診断を借用する。
        // 診断の借用文字列は描画資源の再構築で無効になる。
        [[nodiscard]] std::string_view
            CommonLitStatus() const noexcept;
        // 再読込後の描画能力から静的で不透明なまとめ描画の可否を返す。
        [[nodiscard]] bool CanBeInstanced() const;
        // モデル・材質・共有効果からプロセス内のバッチ識別値を返す。
        [[nodiscard]] std::uint64_t
            InstanceBatchKey() const noexcept;
        // 同一バッチの描画器を詳細度ごとにまとめて描く(batch: 借用するモデル描画器一覧, view: ビュー行列, projection: 射影行列)。
        // 代表はこの描画器で、描画途中の失敗では一部の描画済み部品が残る。
        bool RenderInstancedBatch(
            const std::vector<ModelRendererComponent*>& batch,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        // 画面上の大きさと品質設定から詳細度を選ぶ(view: ビュー行列, projection: 射影行列)。
        [[nodiscard]] std::size_t AutomaticLodLevel(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) const noexcept;
        // 指定した詳細度の三角形数を返す(lodLevel: 詳細度の段階)。
        [[nodiscard]] std::uint64_t TriangleCount(
            std::size_t lodLevel) const noexcept;
        // 個別描画の診断を出力し、まとめ描画済みなら除外する(description: 出力する描画診断)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // コンポーネントの種別名を返す。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "ModelRenderer";
        }

    protected:
        // モデル・材質・描画効果を初期化する(graphics: 寿命を共有する描画機器)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 再生とルート移動を進める(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 従来形式の自作シェーダーで遮蔽の事前描画が有効か返す。
        [[nodiscard]] bool HasPreRender3DPass() override;
        // 通常描画に先行して従来形式の遮蔽部分を描く(view: ビュー行列, projection: 射影行列)。
        void OnPreRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;
        // 選択済みの描画方式でモデルを描く(view: ビュー行列, projection: 射影行列)。
        void OnRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // 従来形式に合成・深度・カリング状態を適用する(state: 宣言された描画状態)。
        void ApplyShaderRenderState(
            const ShaderRenderState& state) const;
        // 外部画像と追加画像を資源を保持する描画要求へまとめる。
        [[nodiscard]] LitTextureRequest
            BuildLitTextureRequest() const noexcept;
        // 姿勢と材質を反映してD3D12で描く(view: ビュー行列, projection: 射影行列, occludedOnly: 従来形式の遮蔽のみ描画)。
        void DrawD3D12Model(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            bool occludedOnly = false);
        // 検証済みバッチをD3D12の組込照明でまとめ描画する(batch: 同じ識別値の描画器一覧, view: ビュー行列, projection: 射影行列)。
        [[nodiscard]] bool RenderD3D12InstancedBatch(
            const std::vector<ModelRendererComponent*>& batch,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        struct CommonLitResources;

        // モデルを再読込し、時刻と部品参照を作り直す。
        void ReloadModel();
        // 再生制御を初期状態に戻して参照クリップを検証する。
        void LoadAnimationController();
        // 状態の代表クリップを解決し、未解決なら最大値を返す(state: 再生制御状態)。
        [[nodiscard]] std::size_t ResolveAnimationIndex(
            const AnimatorState& state) const;
        // クリップ名を解決し、未解決なら最大値を返す(modelClip: 再生クリップ名)。
        [[nodiscard]] std::size_t ResolveAnimationIndex(
            std::string_view modelClip) const noexcept;
        // 状態のクリップ長を混合重みで平均する(state: 再生制御状態)。
        [[nodiscard]] float StateAnimationDuration(
            const AnimatorState& state) const;
        // 状態内の子クリップの混合重みを出力する(state: 再生制御状態, weights: 出力する重み配列)。
        void CalculateBlendWeights(
            const AnimatorState& state,
            std::vector<float>& weights) const;
        // 解決可能な状態へ入り、時刻と遷移情報を初期化する(state: 新しい再生制御状態)。
        void EnterAnimationState(
            const AnimatorState& state);
        // 遷移先を解決してトリガーを消費し遷移を開始する(transition: 条件を満たした遷移定義)。
        void StartAnimationTransition(
            const AnimatorTransition& transition);
        // 状態の再生時刻・イベント・遷移を進める(deltaTime: 正の経過秒数)。
        void AdvanceControllerAnimation(
            float deltaTime);
        // 状態遷移と混合木から姿勢標本を再構築する(samples: 出力する姿勢標本配列)。
        void CollectAnimationPoseSamples(
            std::vector<SkeletalPoseSample>& samples) const;
        // 指定名または先頭の親なしノードを選び、未解決なら最大値を返す。
        [[nodiscard]] std::size_t
            ResolveRootMotionNode() const noexcept;
        // 位置差分とY軸回転を所有物のローカル変換へ反映する(before: 進行前の姿勢標本, after: 進行後の姿勢標本)。
        // 時刻の巻戻りは一回の循環とみなすため、多重循環を跨ぐ刻みを避ける。
        void ApplyRootMotionDelta(
            const std::vector<SkeletalPoseSample>& before,
            const std::vector<SkeletalPoseSample>& after);
        // 通過した通知を最大256件の待ち行列へ追加する(state: 通知元の制御状態, previousTime: 進行前の時刻秒, currentTime: 進行後の時刻秒, duration: 状態の再生区間長秒, looped: 循環検出フラグ, forward: 順再生フラグ)。
        void DispatchAnimationEvents(
            const AnimatorState& state,
            float previousTime,
            float currentTime,
            float duration,
            bool looped,
            bool forward);
        // 従来形式の部品参照とパス別頂点配置を再構築する。
        void RebuildCommonLitResources();
        // D3D11の共有効果と頂点配置を世代に合わせて同期する(forceReload: キャッシュ無効化フラグ)。
        void RefreshShader(bool forceReload);
        // 外部画像を読込後に材質を差し替える(material: 新しい材質設定)。
        void ApplyMaterial(const LitMaterial& material);
        // 従来形式のモデルを共通照明で描く(view: ビュー行列, projection: 射影行列, occludedOnly: 遮蔽部分のみ描画)。
        void DrawCommonLit(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            bool occludedOnly = false);

        // 設定中のモデルパス
        std::filesystem::path m_modelPath;
        // 共有するモデル資産
        std::shared_ptr<const ModelAsset> m_model;
        // 上書き用の材質設定
        LitMaterial m_material;
        // 関連する材質資産パス
        std::filesystem::path m_materialAssetPath;
        // 上書き色画像の共有資産
        std::shared_ptr<const TextureAsset> m_albedoTexture;
        // 上書き法線画像の共有資産
        std::shared_ptr<const TextureAsset> m_normalTexture;

        // 上書き粗さ画像の共有資産
        std::shared_ptr<const TextureAsset> m_roughnessTexture;
        // 上書き金属度画像の共有資産
        std::shared_ptr<const TextureAsset> m_metallicTexture;
        // 上書き遮蔽画像の共有資産
        std::shared_ptr<const TextureAsset> m_occlusionTexture;
        // 上書き発光画像の共有資産
        std::shared_ptr<const TextureAsset> m_emissiveTexture;
        // t7〜t10の追加画像共有資産
        std::array<
            std::shared_ptr<const TextureAsset>,
            LitMaterial::CustomTextureCount>
            m_customTextures{};
        // 借用する資産管理器
        AssetManager* m_assets{};
        // 借用する描画機器
        GraphicsDevice* m_graphics{};
        // 借用する描画コンテキスト
        ID3D11DeviceContext* m_context{};
        // 借用する標準描画状態
        DirectX::CommonStates* m_states{};
        // 従来形式の部品と頂点配置資源
        std::unique_ptr<CommonLitResources>
            m_commonLitResources;
        // 従来形式の共有描画効果
        LitEffect* m_effect{};
        // 骨格形式の主共有描画効果
        LitEffect* m_skinnedEffect{};
        // 骨格の有無が混在するモデルでは部品ごとに共有効果を使い分ける。
        // 骨格なし部品の共有描画効果
        LitEffect* m_skeletalForwardEffect{};
        // 直接HLSLの骨格用頂点配置
        Microsoft::WRL::ComPtr<ID3D11InputLayout>
            m_skinnedInputLayout;
        // 骨格形式の色パス順の頂点配置
        std::vector<Microsoft::WRL::ComPtr<ID3D11InputLayout>>
            m_skeletalColorInputLayouts;
        // 骨格形式の輪郭パス順の頂点配置
        std::vector<Microsoft::WRL::ComPtr<ID3D11InputLayout>>
            m_skeletalOutlineInputLayouts;
        // 骨格なしの色パス順の頂点配置
        std::vector<Microsoft::WRL::ComPtr<ID3D11InputLayout>>
            m_skeletalForwardColorInputLayouts;
        // 骨格なしの輪郭パス順の頂点配置
        std::vector<Microsoft::WRL::ComPtr<ID3D11InputLayout>>
            m_skeletalForwardOutlineInputLayouts;
        // まとめ描画のパス順の頂点配置
        std::vector<Microsoft::WRL::ComPtr<ID3D11InputLayout>>
            m_instancedInputLayouts;
        // 選択済みのシェーダーパス
        std::filesystem::path m_activeShaderPath;
        // 主描画効果の世代
        std::uint64_t m_shaderGeneration{};
        // 骨格なし描画効果の世代
        std::uint64_t m_skeletalForwardShaderGeneration{};
        // シェーダーと頂点配置の診断
        std::string m_shaderError;
        // 線表示フラグ
        bool m_wireframe{};
        // 材質上書きフラグ
        bool m_materialOverrideEnabled{};
        // 従来照明の選択フラグ
        bool m_useLegacyShading{};
        // 埋込材質色の乗算フラグ
        bool m_preserveEmbeddedMaterialColor{};
        // 現在の再生クリップ番号
        std::size_t m_animationIndex{};
        // 符号付きの再生速度倍率
        float m_animationSpeed{ 1.0f };
        // 現在の再生時刻秒
        float m_animationTime{};
        // 単独クリップの循環フラグ
        bool m_animationLoop{ true };
        // 初期化時の自動再生フラグ
        bool m_animationPlayOnStart{ true };
        // 再生中フラグ
        bool m_animationPlaying{};
        // 再生制御器の資産パス
        std::filesystem::path m_animationControllerPath;
        // 共有する再生制御器資産
        std::shared_ptr<const AnimatorController>
            m_animationController;
        // 現在の制御状態名
        std::string m_currentAnimationState;
        // 遷移先の制御状態名
        std::string m_nextAnimationState;
        // 遷移先の代表クリップ番号
        std::size_t m_nextAnimationIndex{};
        // 遷移先の再生時刻秒
        float m_nextAnimationTime{};
        // 速度倍率なしの遷移経過秒
        float m_animationTransitionTime{};
        // 遷移に掛ける秒数
        float m_animationTransitionDuration{};
        // 消費前の遷移トリガー集合
        std::unordered_set<std::string>
            m_activeAnimationTriggers;
        // 名前別の浮動小数点制御値
        std::unordered_map<std::string, float>
            m_animationFloatValues;
        // ルート移動の反映フラグ
        bool m_applyRootMotion{};
        // ルート移動の抽出ノード名
        std::string m_rootMotionNode;
        // 最大256件の未取得通知
        std::deque<AnimationEventNotification>
            m_animationEventQueue;
        // 影・深度・色描画の間ではフレーム番号とモデル参照が同じ姿勢を共有する。
        // 評価済みのモデル空間姿勢
        std::vector<DirectX::XMFLOAT4X4> m_cachedGlobalPose;
        // 姿勢を評価したフレーム番号
        std::uint64_t m_cachedPoseFrame{ ~std::uint64_t{} };
        // 姿勢を評価したモデル参照
        const SkeletalModel* m_cachedPoseModel{};
        // 個別描画を一回省くフラグ
        bool m_instancedThisPass{};
    };
}
