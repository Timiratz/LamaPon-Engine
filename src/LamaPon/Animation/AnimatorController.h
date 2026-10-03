#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LamaPon
{
    class AssetDatabase;

    struct AnimatorFloatParameter final
    {
        // 浮動小数点パラメーター名
        std::string name;
        // パラメーターの既定値
        float defaultValue{};
    };

    enum class AnimatorBlendTreeType
    {
        // ブレンドせず単一再生
        None,
        // 一つの値で混合
        OneDimensional,
        // 二つの値で混合
        TwoDimensional
    };

    struct AnimatorBlendChild final
    {
        // 子のクリップ参照パス
        std::filesystem::path clipPath;
        // 子のクリップ参照GUID
        std::string clipGuid;
        // 子のモデル内アニメーション名
        std::string modelClip;
        // 1Dブレンドの閾値
        float threshold{};
        // 2DブレンドのX位置
        float positionX{};
        // 2DブレンドのY位置
        float positionY{};
    };

    struct AnimatorEvent final
    {
        // 発火するイベントの名前
        std::string name;
        // イベントへ渡す文字列
        std::string payload;
        // 発火する0〜1の再生進捗
        float normalizedTime{};
    };

    struct AnimatorState final
    {
        // 状態を識別する名前
        std::string name;
        // 状態のクリップ参照パス
        std::filesystem::path clipPath;
        // 状態のクリップ参照GUID
        std::string clipGuid;
        // モデル内アニメーション名
        std::string modelClip;
        // 子を混合する方式
        AnimatorBlendTreeType blendTreeType{
            AnimatorBlendTreeType::None
        };
        // 1Dまたは2DのX入力変数名
        std::string blendParameter;
        // 2DのY入力変数名
        std::string blendParameterY;
        // 混合する子のクリップ列
        std::vector<AnimatorBlendChild> blendChildren;
        // 再生時刻順のイベント列
        std::vector<AnimatorEvent> events;
        // 再生時間の進行倍率
        float speed{ 1.0f };
        // 状態のループ再生有無
        bool loop{ true };
    };

    // triggerが非空ならトリガーを要し、exitTimeが非負なら時刻条件も同時に満たす必要があります。
    struct AnimatorTransition final
    {
        // 遷移する前の状態名
        std::string from;
        // 遷移した後の状態名
        std::string to;
        // 必要なトリガー名か空
        std::string trigger;
        // 必要な再生進捗か負の無効値
        float exitTime{ -1.0f };
        // 遷移の混合にかける秒数
        float duration{ 0.2f };
    };

    class AnimatorController final
    {
    public:
        // コントローラー文書を読み検証します(path: 読み込むファイル)。
        // 開くことができない場合や形式・値が不正な場合は例外です。
        static AnimatorController LoadFromFile(
            const std::filesystem::path& path);
        // JSONから状態・遷移・ブレンド設定を検証して作ります(json: 文書全体)。
        // 状態は1〜256件、変数は最大64件、遷移は最大1024件で、状態名と変数名は一意です。
        // 1Dの子は閾値順、イベントは正規化時刻順に並べ、開始状態と遷移先は登録済み状態を要します。
        static AnimatorController FromJson(
            std::string_view json);
        // 子の2D位置から混合重みを求めます(children: 有限位置の子列, x: 有限の入力X値, y: 有限の入力Y値)。
        // 逆距離二乗を正の合計で正規化し、距離二乗1e-6以内なら最初の一致子だけを1にします。
        [[nodiscard]] static std::vector<float>
            Calculate2DBlendWeights(
                const std::vector<AnimatorBlendChild>&
                    children,
                float x,
                float y);
        // 状態と子のGUIDに対応する参照パスを更新します(database: 解決するアセット一覧)。
        void ResolveAssetReferences(
            const AssetDatabase& database);

        // 開始する状態の名前を参照します。
        [[nodiscard]] const std::string&
            EntryState() const noexcept
        {
            return m_entryState;
        }
        // 登録済みの状態一覧を参照します。
        [[nodiscard]] const std::vector<AnimatorState>&
            States() const noexcept
        {
            return m_states;
        }
        // 文書順の遷移一覧を参照します。
        [[nodiscard]] const std::vector<AnimatorTransition>&
            Transitions() const noexcept
        {
            return m_transitions;
        }
        // 浮動小数点パラメーターの定義一覧を参照します。
        [[nodiscard]] const std::vector<AnimatorFloatParameter>&
            FloatParameters() const noexcept
        {
            return m_floatParameters;
        }
        // 名前が一致する状態を非所有参照で返します(name: 状態名)。
        // 未定義ならnullptrです。
        [[nodiscard]] const AnimatorState* FindState(
            std::string_view name) const noexcept;
        // 最初に条件へ合致する遷移を返します(state: 現在状態名, normalizedTime: 正規化再生時刻, activeTriggers: 有効なトリガー集合)。
        // 文書順に選び未検出ならnullptrとし、トリガーの消費は呼び出し側が行います。
        [[nodiscard]] const AnimatorTransition*
            FindTransition(
                std::string_view state,
                float normalizedTime,
                const std::unordered_set<std::string>&
                    activeTriggers) const noexcept;

    private:
        // 再生開始時の状態名
        std::string m_entryState;
        // 浮動小数点パラメーターの定義
        std::vector<AnimatorFloatParameter>
            m_floatParameters;
        // 登録順のアニメーション状態
        std::vector<AnimatorState> m_states;
        // 文書順に保持する遷移の定義
        std::vector<AnimatorTransition>
            m_transitions;
    };
}
