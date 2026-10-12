#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 顔の向きや目の開きなど、キャラクターの部品をまとめて動かす名前付きの値です。
    struct Rig2DParameter final
    {
        // 部品のKeyform2Dから参照する名前(1〜64バイト)
        std::string name;
        // 値の下限
        float minimum{ 0.0f };
        // 値の上限
        float maximum{ 1.0f };
        // 基準姿勢に対応する既定値
        float defaultValue{ 0.0f };
        // 現在の値
        float value{ 0.0f };
        // 現在の値を中心に自動で揺らす幅(呼吸などに使い0で無効)
        float autoAmplitude{ 0.0f };
        // 自動で揺らす周波数(Hz)
        float autoFrequency{ 0.25f };
    };

    // 名前付きのパラメータを持ち、子孫のKeyform2Dがその値に合わせて部品を動かします。
    // Live2Dのパラメータのように、角度Xや目の開きといった1つの値で複数の部品を連動させます。
    class Rig2DComponent final : public Component
    {
    public:
        // パラメータ一覧からリグを作ります(parameters: 補正して登録するパラメータ)。
        explicit Rig2DComponent(
            std::vector<Rig2DParameter> parameters = {});

        // 全パラメータを並び順のまま置き換え、空名・長すぎる名前・重複名は除きます(parameters: 新しいパラメータ)。
        void SetParameters(std::vector<Rig2DParameter> parameters);
        // 名前・範囲・値を補正して同名を置き換えるか末尾へ追加します(parameter: 登録するパラメータ)。
        // 名前が空か64バイトを超える場合はfalseで登録しません。
        bool AddParameter(Rig2DParameter parameter);
        // 指定名のパラメータを除去し、見つかればtrueを返します(name: 除去する名前)。
        bool RemoveParameter(std::string_view name);
        // 登録順のパラメータ一覧を返します。
        [[nodiscard]] const std::vector<Rig2DParameter>&
            Parameters() const noexcept
        {
            return m_parameters;
        }
        // 指定名のパラメータを返し、なければnullptrです(name: 探す名前)。
        [[nodiscard]] const Rig2DParameter* FindParameter(
            std::string_view name) const noexcept;
        // 範囲へ収めて現在の値を設定し、名前が見つからなければfalseです(name: パラメータ名, value: 新しい値)。
        bool SetParameter(std::string_view name, float value) noexcept;
        // 自動の揺れを含めた現在の値を返し、名前が見つからなければ0です(name: パラメータ名)。
        [[nodiscard]] float ParameterValue(
            std::string_view name) const noexcept;
        // 全パラメータを既定値へ戻します。
        void ResetParameters() noexcept;

        // 自身と子孫のKeyform2Dへ現在の値の姿勢と不透明度を適用します。
        // 編集中のプレビューに使い、再生中は各Keyform2Dが毎フレーム自分で適用します。
        void ApplyToHierarchy();
        // 自身と子孫のKeyform2Dの部品を記録した基準姿勢へ戻します。
        void RestoreHierarchyRestPose();

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Rig2D";
        }

    protected:
        // 自動で揺らす時刻を進めます(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // 名前・範囲・値を補正したパラメータを返します(parameter: 補正するパラメータ)。
        [[nodiscard]] static Rig2DParameter Sanitize(
            Rig2DParameter parameter) noexcept;

        // 登録順のパラメータ
        std::vector<Rig2DParameter> m_parameters;
        // 自動の揺れに使う経過秒数
        float m_time{};
    };
}
