#pragma once

#include <DirectXMath.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    struct TransformAnimationSample final
    {
        // 位置の三軸座標
        DirectX::XMFLOAT3 position{};
        // Euler回転の三軸ラジアン
        DirectX::XMFLOAT3 rotation{};
        // 三軸の拡縮倍率
        DirectX::XMFLOAT3 scale{
            1.0f,
            1.0f,
            1.0f
        };
    };

    struct TransformKeyframe final
    {
        // クリップ開始からの秒数
        float time{};
        // この時刻のTransform値
        TransformAnimationSample transform;
    };

    class AnimationClip final
    {
    public:
        // クリップ文書を読み込み検証します(path: 読み込むファイル)。
        // 開くことができない場合や形式・値が不正な場合は例外です。
        static AnimationClip LoadFromFile(
            const std::filesystem::path& path);
        // JSONから検証済みクリップを作ります(json: 文書全体)。
        // キーは1〜4096件で時刻は非負かつ厳密昇順とし、所要時間は正で全キーを含めます。
        static AnimationClip FromJson(
            std::string_view json);
        // 値を文書と同じ規則で検証してクリップを作ります(name: クリップ名, duration: 所要秒数, loop: ループ有無, keyframes: 所有するキー列)。
        static AnimationClip Create(
            std::string name,
            float duration,
            bool loop,
            std::vector<TransformKeyframe> keyframes);

        // 現在のクリップをJSON文書へ書き出します。
        [[nodiscard]] std::string
            SerializeToJson() const;
        // JSON文書を書いて保存先を置き換えます(path: 保存ファイル)。
        // 同名.tmpへ書いた後にWRITE_THROUGHで改名し、失敗は例外です。
        void SaveToFile(
            const std::filesystem::path& path) const;

        // 時刻に対応するTransformを補間します(time: 有限の再生秒数)。
        // 位置・拡縮は線形、Euler角は軸別の最短差で補間し、範囲外は端のキーを返します。
        // Loop設定による時刻の周回は呼び出し側で行います。
        [[nodiscard]] TransformAnimationSample
            Sample(float time) const noexcept;
        // 回転をクォータニオンで補間します(time: 有限の再生秒数)。
        // 保存したEuler角を変換し、短い回転側でSlerpして正規化します。
        [[nodiscard]] DirectX::XMFLOAT4
            SampleRotationQuaternion(float time) const noexcept;
        // クリップ名を非所有参照で返します。
        [[nodiscard]] const std::string& Name() const noexcept
        {
            return m_name;
        }
        // クリップの所要秒数を返します。
        [[nodiscard]] float Duration() const noexcept
        {
            return m_duration;
        }
        // 再生側へ渡すループ設定を返します。
        [[nodiscard]] bool Loop() const noexcept
        {
            return m_loop;
        }
        // 時刻順のキー列を非所有参照で返します。
        [[nodiscard]] const std::vector<TransformKeyframe>&
            Keyframes() const noexcept
        {
            return m_keyframes;
        }

    private:
        // クリップの名前
        std::string m_name;
        // クリップの所要秒数
        float m_duration{};
        // 再生側へ渡すループ設定
        bool m_loop{ true };
        // 時刻順のTransformキー列
        std::vector<TransformKeyframe> m_keyframes;
    };
}
