#pragma once

// Portable版の2Dキャラクター部品です。LamaPon.hの末尾から読み込まれます。
// Windows版と同じ名前と主なメソッドを持ち、Web書き出しでも同じスクリプトを使えます。

namespace LamaPon
{
    class CharacterRig2DRuntime;

    // 髪・服・飾りなど、回転中心から垂れ下がる2Dパーツの揺れ設定です。
    struct Sway2DSettings final
    {
        // 回転中心から揺れの先端までのローカルXY(拡縮前の単位)
        DirectX::XMFLOAT2 tipOffset{ 0.0f, 100.0f };
        // 先端を静止姿勢へ戻すばねの強さ(毎秒毎秒)
        float stiffness{ 60.0f };
        // 揺れを弱める減衰の強さ(毎秒)
        float damping{ 8.0f };
        // 回転中心の移動に先端が取り残される割合(0で追従・1で完全に残る)
        float inertia{ 1.0f };
        // 先端へ掛かるワールド加速度(毎秒毎秒、Yは下向き)
        DirectX::XMFLOAT2 gravity{ 0.0f, 0.0f };
        // 静止姿勢から振れる最大角度(度、0〜180)
        float maxAngleDegrees{ 45.0f };
        // 風で加える周期的な揺れ幅(度、0〜180)
        float windAmplitudeDegrees{ 0.0f };
        // 風の揺れの周波数(Hz、0〜60)
        float windFrequency{ 0.5f };
        // 風の揺れの位相(度)
        float windPhaseDegrees{ 0.0f };
    };

    // 先端をばね・減衰・重力で追わせ、その角度だけZ回転を足す揺れ物です。
    class Sway2DComponent final : public Component
    {
    public:
        // 揺れ物を作ります(settings: 揺れ設定で範囲外の値は補正)。
        explicit Sway2DComponent(
            const Sway2DSettings& settings = {}) noexcept;

        // 範囲外の値を補正して揺れ設定を置き換えます(settings: 新しい揺れ設定)。
        void SetSettings(const Sway2DSettings& settings) noexcept;
        // 補正済みの揺れ設定を返します。
        [[nodiscard]] const Sway2DSettings& Settings() const noexcept
        {
            return m_settings;
        }
        // 非有限値を既定値へ戻し、各値を許容範囲へ収めた設定を返します(settings: 補正する設定)。
        [[nodiscard]] static Sway2DSettings Sanitize(
            Sway2DSettings settings) noexcept;
        // 揺れを止め、次の更新で現在の姿勢から計算をやり直します。
        void ResetSimulation() noexcept;
        // 直近に静止姿勢へ足したZ回転をラジアンで返します。
        [[nodiscard]] float CurrentAngle() const noexcept
        {
            return m_angle;
        }

    private:
        friend class CharacterRig2DRuntime;

        // 補正済みの揺れ設定
        Sway2DSettings m_settings;
        // 先端の点のワールドXY
        DirectX::XMFLOAT2 m_tip{};
        // 先端の点のワールド速度
        DirectX::XMFLOAT2 m_velocity{};
        // 前フレームの回転中心のワールドXY
        DirectX::XMFLOAT2 m_previousPivot{};
        // 前フレームの静止先端のワールドXY
        DirectX::XMFLOAT2 m_previousRestTip{};
        // 静止姿勢のZ回転ラジアン
        float m_restRotation{};
        // 直近に書き込んだZ回転ラジアン
        float m_appliedRotation{};
        // 静止姿勢へ足したZ回転ラジアン
        float m_angle{};
        // 風の揺れの累積位相ラジアン
        float m_windPhase{};
        // 今フレームで計算する経過秒数
        float m_pendingDeltaTime{};
        // 先端の点を初期化済みか
        bool m_simulationReady{};
        // 書き込んだ回転が残っている可能性があるか
        bool m_rotationApplied{};
        // 今フレームの計算を済ませたか
        bool m_solvedThisFrame{ true };
    };

    // 目のスプライトシートと瞬きの間隔を表します。
    struct Blink2DSettings final
    {
        // シートの列数(1〜256)
        int columns{ 3 };
        // シートの行数(1〜256)
        int rows{ 1 };
        // 目を開いたコマ番号
        int openFrame{ 0 };
        // 閉じ始めのコマ番号
        int closingStartFrame{ 1 };
        // 閉じ始めから閉じきるまでのコマ数(1〜64)
        int closingFrameCount{ 2 };
        // 途中のコマを表示する秒数
        float frameSeconds{ 0.04f };
        // 閉じた目を表示する秒数
        float closedSeconds{ 0.06f };
        // 次の瞬きまでの最短秒数
        float intervalMinSeconds{ 2.0f };
        // 次の瞬きまでの最長秒数
        float intervalMaxSeconds{ 6.0f };
        // 瞬きの直後にもう一度瞬く確率(0〜1)
        float doubleBlinkChance{ 0.15f };
        // ランダムな間隔で自動的に瞬くか
        bool autoBlink{ true };
        // 子孫のSprite Rendererも同じコマにするか
        bool includeChildren{ true };
    };

    // 自身と子孫のSprite Rendererを「開→半目→閉→半目→開」のコマで切り替えて瞬きさせます。
    class Blink2DComponent final : public Component
    {
    public:
        // 瞬きを作ります(settings: 瞬き設定で範囲外の値は補正)。
        explicit Blink2DComponent(
            const Blink2DSettings& settings = {}) noexcept;

        // 範囲外の値を補正して置き換えます(settings: 新しい瞬き設定)。
        void SetSettings(const Blink2DSettings& settings) noexcept;
        // 補正済みの瞬き設定を返します。
        [[nodiscard]] const Blink2DSettings& Settings() const noexcept
        {
            return m_settings;
        }
        // 非有限値を既定値へ戻し、各値を許容範囲へ収めた設定を返します(settings: 補正する設定)。
        [[nodiscard]] static Blink2DSettings Sanitize(
            Blink2DSettings settings) noexcept;
        // 目が開いていれば、次の更新から瞬きを始めます。
        void Blink() noexcept;
        // 目を閉じたままにするかを設定します(holdClosed: 閉じたままにする指定)。
        void SetHoldClosed(const bool holdClosed) noexcept
        {
            m_holdClosed = holdClosed;
        }
        // 目を閉じたままにする指定を返します。
        [[nodiscard]] bool HoldClosed() const noexcept
        {
            return m_holdClosed;
        }
        // 間隔の乱数を固定し、次の瞬きまでの時間を引き直します(seed: 乱数の種)。
        void SetRandomSeed(std::uint32_t seed) noexcept;
        // 閉じ始めてから開ききるまでの間か返します。
        [[nodiscard]] bool IsBlinking() const noexcept
        {
            return m_phase != 0;
        }
        // 現在表示するシートのコマ番号を返します。
        [[nodiscard]] int CurrentFrame() const noexcept;
        // 次の瞬きまでの残り秒数を返します。
        [[nodiscard]] float SecondsUntilNextBlink() const noexcept
        {
            return m_secondsUntilBlink;
        }

    private:
        friend class CharacterRig2DRuntime;

        // 経過秒数だけ段階を進めます(deltaTime: 0以上の経過秒数)。
        void Advance(float deltaTime) noexcept;
        // 次の瞬きまでの秒数を引きます(allowDouble: 二度瞬きを許す指定)。
        void ScheduleNextBlink(bool allowDouble) noexcept;
        // 最小値と最大値の間の乱数を返します(minimum: 最小値, maximum: 最大値)。
        [[nodiscard]] float RandomRange(float minimum, float maximum) noexcept;

        // 補正済みの瞬き設定
        Blink2DSettings m_settings;
        // 線形合同法の乱数の状態
        std::uint32_t m_random{ 1u };
        // 瞬きの段階(0=開, 1=閉じ途中, 2=閉, 3=開き途中)
        int m_phase{};
        // 現在の段階の経過秒数
        float m_phaseSeconds{};
        // 次の瞬きまでの残り秒数
        float m_secondsUntilBlink{};
        // 直前の間隔が二度瞬きだったか
        bool m_lastWasDouble{};
        // 目を閉じたままにする指定
        bool m_holdClosed{};
        // 乱数の種を明示したか
        bool m_seeded{};
        // 初期化を済ませたか
        bool m_ready{};
    };

    // 同じGameObjectのSprite Rendererの格子頂点を描画の直前に変形する部品の基底です。
    class SpriteMeshDeformer : public Component
    {
    public:
        // 格子頂点のローカル位置を変形します(sprite: 描画するSprite Renderer, positions: 頂点位置の入出力)。
        virtual void DeformSpriteMesh(
            const SpriteRendererComponent& sprite,
            std::vector<DirectX::XMFLOAT2>& positions) = 0;
        // 頂点を動かす設定を持ち、分割のないSpriteもメッシュで描く必要があるか返します。
        [[nodiscard]] virtual bool DeformsSpriteMesh() const
        {
            return true;
        }
    };

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

    // ボーンにしたGameObjectの動きに合わせてSpriteのメッシュを曲げます。
    // Web版ではSetBonesで指定したボーンを次の更新で探すため、Bindはその後に有効になります。
    class SpriteSkin2DComponent final : public SpriteMeshDeformer
    {
    public:
        // 自動で付ける重みの距離減衰の既定値
        static constexpr float DefaultWeightFalloff = 4.0f;

        // ボーン一覧からスキンを作ります(bones: ボーンにするGameObjectのID列)。
        explicit SpriteSkin2DComponent(
            std::vector<std::uint64_t> bones = {});

        // ボーン一覧を置き換えてバインドを解除します(bones: ボーンのID列)。
        void SetBones(std::vector<std::uint64_t> bones);
        // ボーンのID列を返します。
        [[nodiscard]] const std::vector<std::uint64_t>& Bones() const noexcept
        {
            return m_bones;
        }
        // バインドを保ったままボーンIDを置き換えます(bones: 同数の新しいID列)。
        bool RemapBones(std::vector<std::uint64_t> bones);
        // 自動の重みの距離減衰を0.5〜16へ収めて設定します(falloff: 距離減衰の指数)。
        void SetWeightFalloff(float falloff) noexcept;
        // 自動の重みの距離減衰を返します。
        [[nodiscard]] float WeightFalloff() const noexcept
        {
            return m_weightFalloff;
        }
        // 現在のボーンとSpriteの姿勢を基準にし、重みが格子と合わなければ自動で付けます。
        bool Bind();
        // 現在のバインド姿勢で、ボーンからの距離に応じた重みを付け直します。
        bool ComputeAutomaticWeights();
        // バインドを解除します。
        void Unbind() noexcept;
        // バインド済みか返します。
        [[nodiscard]] bool IsBound() const noexcept
        {
            return m_bound;
        }
        // 頂点ごとの重みを返します。
        [[nodiscard]] const std::vector<SpriteSkinWeight>& Weights() const noexcept
        {
            return m_weights;
        }
        // バインド済みの格子と同数の重みを正規化して設定します(weights: 頂点ごとの重み)。
        bool SetWeights(std::vector<SpriteSkinWeight> weights);
        // バインドした時点の各ボーンのワールド行列を返します。
        [[nodiscard]] const std::vector<DirectX::XMFLOAT4X4>&
            BoneBindPoses() const noexcept
        {
            return m_boneBindPoses;
        }
        // バインドした時点のSpriteのワールド行列を返します。
        [[nodiscard]] const DirectX::XMFLOAT4X4& SpriteBindPose() const noexcept
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
        bool RestoreBinding(
            std::vector<DirectX::XMFLOAT4X4> boneBindPoses,
            const DirectX::XMFLOAT4X4& spriteBindPose,
            std::vector<SpriteSkinWeight> weights,
            int columns,
            int rows);
        // Spriteの基準点から反対側の端まで子の鎖としてボーンを作りバインドします(boneCount: 1〜16のボーン数, addSway: ボーンにSway2Dを付けるか)。
        std::vector<GameObject*> CreateBoneChain(int boneCount, bool addSway);
        // 格子頂点をボーンの動きに合わせて変形します(sprite: 描画するSprite Renderer, positions: 頂点位置の入出力)。
        void DeformSpriteMesh(
            const SpriteRendererComponent& sprite,
            std::vector<DirectX::XMFLOAT2>& positions) override;

    private:
        friend class CharacterRig2DRuntime;

        // ボーンのID列
        std::vector<std::uint64_t> m_bones;
        // IDから見つけたボーンで、見つからなければnullptr
        std::vector<GameObject*> m_boneObjects;
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

    // キャラクターの部品をまとめて動かす名前付きの値です。
    struct Rig2DParameter final
    {
        // Keyform2Dから参照する名前(1〜64バイト)
        std::string name;
        // 値の下限
        float minimum{ 0.0f };
        // 値の上限
        float maximum{ 1.0f };
        // 基準姿勢に対応する既定値
        float defaultValue{ 0.0f };
        // 現在の値
        float value{ 0.0f };
        // 現在の値を中心に自動で揺らす幅
        float autoAmplitude{ 0.0f };
        // 自動で揺らす周波数(Hz)
        float autoFrequency{ 0.25f };
    };

    // 名前付きのパラメータを持ち、子孫のKeyform2Dがその値に合わせて部品を動かします。
    class Rig2DComponent final : public Component
    {
    public:
        // パラメータ一覧からリグを作ります(parameters: 補正して登録するパラメータ)。
        explicit Rig2DComponent(std::vector<Rig2DParameter> parameters = {});

        // 全パラメータを並び順のまま置き換え、空名・長すぎる名前・重複名は除きます(parameters: 新しいパラメータ)。
        void SetParameters(std::vector<Rig2DParameter> parameters);
        // 名前・範囲・値を補正して同名を置き換えるか末尾へ追加します(parameter: 登録するパラメータ)。
        bool AddParameter(Rig2DParameter parameter);
        // 指定名のパラメータを除去します(name: 除去する名前)。
        bool RemoveParameter(std::string_view name);
        // 登録順のパラメータ一覧を返します。
        [[nodiscard]] const std::vector<Rig2DParameter>& Parameters() const noexcept
        {
            return m_parameters;
        }
        // 指定名のパラメータを返し、なければnullptrです(name: 探す名前)。
        [[nodiscard]] const Rig2DParameter* FindParameter(
            std::string_view name) const noexcept;
        // 範囲へ収めて現在の値を設定します(name: パラメータ名, value: 新しい値)。
        bool SetParameter(std::string_view name, float value) noexcept;
        // 自動の揺れを含めた現在の値を返し、名前が見つからなければ0です(name: パラメータ名)。
        [[nodiscard]] float ParameterValue(std::string_view name) const noexcept;
        // 全パラメータを既定値へ戻します。
        void ResetParameters() noexcept;
        // 自身と子孫のKeyform2Dへ現在の値の姿勢と不透明度を適用します。
        void ApplyToHierarchy();
        // 自身と子孫のKeyform2Dの部品を記録した基準姿勢へ戻します。
        void RestoreHierarchyRestPose();

    private:
        friend class CharacterRig2DRuntime;

        // 登録順のパラメータ
        std::vector<Rig2DParameter> m_parameters;
        // 自動の揺れに使う経過秒数
        float m_time{};
    };

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
        // 格子頂点のローカル移動量
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

    // 祖先のRig2Dのパラメータ値に合わせて、部品の姿勢・不透明度・格子をキーの間で補間して動かします。
    class Keyform2DComponent final : public SpriteMeshDeformer
    {
    public:
        // チャンネル一覧から部品の動きを作ります(channels: 補正して登録するチャンネル)。
        explicit Keyform2DComponent(std::vector<Keyform2DChannel> channels = {});
        // Rig2Dの一括適用の対象から外して破棄します。
        ~Keyform2DComponent() override;
        // 登録の重複を防ぐため複製を禁止します。
        Keyform2DComponent(const Keyform2DComponent&) = delete;
        // 登録の重複を防ぐため複製代入を禁止します。
        Keyform2DComponent& operator=(const Keyform2DComponent&) = delete;

        // 非有限のキーを除き、値の昇順・重複なしへ整えて置き換えます(channels: 新しいチャンネル)。
        void SetChannels(std::vector<Keyform2DChannel> channels);
        // 登録順のチャンネルを返します。
        [[nodiscard]] const std::vector<Keyform2DChannel>& Channels() const noexcept
        {
            return m_channels;
        }
        // 指定パラメータのチャンネルへキーを追加し、同じ値のキーは置き換えます(parameter: パラメータ名, key: 追加するキー)。
        bool SetKey(std::string_view parameter, Keyform2DKey key);
        // 指定パラメータの同じ値のキーを除去します(parameter: パラメータ名, value: キーの値)。
        bool RemoveKey(std::string_view parameter, float value);
        // 現在の位置・回転・拡縮・不透明度を基準姿勢として記録します。
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
        // 基準回転のクォータニオンを返します。
        [[nodiscard]] DirectX::XMFLOAT4 RestRotation() const noexcept;
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
        // 現在の姿勢と不透明度の基準からの差をキーへ記録します(parameter: パラメータ名, value: キーの値)。
        bool RecordPoseKey(std::string_view parameter, float value);
        // この部品以外の変形を適用した格子の差をキーの頂点移動へ記録します(parameter: パラメータ名, value: キーの値)。
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

    private:
        friend class CharacterRig2DRuntime;
        friend class Rig2DComponent;

        // 自身から祖先へ最初のRig2Dを返します。
        [[nodiscard]] Rig2DComponent* FindRig() const;
        // キーが姿勢と不透明度を使うかを数え直します。
        void RefreshUsage() noexcept;

        // 登録順のチャンネル
        std::vector<Keyform2DChannel> m_channels;
        // 基準位置
        DirectX::XMFLOAT3 m_restPosition{};
        // 基準回転のオイラー角(ラジアン)
        DirectX::XMFLOAT3 m_restEuler{};
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
