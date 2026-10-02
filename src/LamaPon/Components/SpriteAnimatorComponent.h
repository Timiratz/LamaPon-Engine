#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 左上から右へ、次に下へ数える均等シートの連続コマを表す。
    struct SpriteAnimationClip final
    {
        // クリップの識別名
        std::string name;
        // シート内の先頭コマ番号
        int startFrame{};
        // 連続して使うコマ数
        int frameCount{ 1 };
        // 毎秒のコマ送り数
        float framesPerSecond{ 10.0f };
        // 末尾から先頭へ周回する指定
        bool loop{ true };
    };

    // 同じ所有者のSpriteRendererを均等グリッドのスプライトシートでコマ送りする。
    // 数値は有限値で、列数×行数・コマ番号・経過時間からの整数変換がintの範囲に収まることを前提とする。
    class SpriteAnimatorComponent final : public Component
    {
    public:
        // 各軸1以上の均等グリッドでスプライトのコマ送りを作る(columns: シートの列数, rows: シートの行数)。
        explicit SpriteAnimatorComponent(
            int columns = 1,
            int rows = 1) noexcept;

        // シートの列数と行数を各1以上に収める(columns: シートの列数, rows: シートの行数)。
        void SetSheetGrid(int columns, int rows) noexcept;
        // シートの列数を取得する。
        [[nodiscard]] int Columns() const noexcept
        {
            return m_columns;
        }
        // シートの行数を取得する。
        [[nodiscard]] int Rows() const noexcept
        {
            return m_rows;
        }

        // 開始コマ・コマ数・速度を下限に収め、同名クリップを置き換える(clip: 登録するクリップ)。
        void AddClip(SpriteAnimationClip clip);
        // 指定名の全クリップを除去し、再生中なら停止する(name: 除去するクリップ名)。
        void RemoveClip(std::string_view name);
        // 登録クリップの編集用参照を取得する。
        // 直接編集時は開始コマ0以上・コマ数1以上・有限の再生速度0.01以上を維持する。
        [[nodiscard]] std::vector<SpriteAnimationClip>&
            Clips() noexcept
        {
            return m_clips;
        }
        // 登録クリップの一覧を取得する。
        [[nodiscard]] const std::vector<
            SpriteAnimationClip>& Clips() const noexcept
        {
            return m_clips;
        }

        // 指定クリップを先頭から再生し、名前が見つからなければ状態を変えずfalseを返す(clipName: 再生するクリップ名)。
        bool Play(std::string_view clipName);
        // 表示中のコマと再生時刻を保持して再生を停止する。
        void Stop() noexcept { m_playing = false; }
        // コマ送りが再生状態か確認する。
        [[nodiscard]] bool IsPlaying() const noexcept
        {
            return m_playing;
        }
        // 現在選択されているクリップ名を取得する。
        [[nodiscard]] const std::string&
            ActiveClipName() const noexcept
        {
            return m_activeClip;
        }
        // 再生開始前は-1、それ以降はシート周回前のコマ番号を取得する。
        [[nodiscard]] int CurrentFrame() const noexcept
        {
            return m_currentFrame;
        }

        // 再生速度の倍率を設定する(speed: 0で静止し負で逆送りする倍率)。
        void SetSpeed(const float speed) noexcept
        {
            m_speed = speed;
        }
        // 再生速度の倍率を取得する。
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }
        // 初回更新で自動再生するクリップ名を設定する(name: 自動再生名で空は先頭)。
        void SetDefaultClip(std::string name)
        {
            m_defaultClip = std::move(name);
        }
        // 自動再生用のクリップ名を取得する。
        [[nodiscard]] const std::string&
            DefaultClip() const noexcept
        {
            return m_defaultClip;
        }
        // 初回更新での自動再生を設定する(playOnStart: 自動再生する指定)。
        void SetPlayOnStart(const bool playOnStart) noexcept
        {
            m_playOnStart = playOnStart;
        }
        // 初回更新で自動再生する設定か確認する。
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }

        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "SpriteAnimator";
        }

    protected:
        // 初回の自動再生と経過時間に応じたコマ送りを行う(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // 指定名で最初の登録クリップを探し、未登録ならnullptrを返す(name: 検索するクリップ名)。
        [[nodiscard]] const SpriteAnimationClip*
            FindClip(std::string_view name) const noexcept;
        // コマ番号を記録し、シート周回後の矩形を同じ所有者のSpriteRendererへ反映する(sheetFrame: シート周回前のコマ番号)。
        void ApplyFrame(int sheetFrame);

        // シートの列数
        int m_columns{ 1 };
        // シートの行数
        int m_rows{ 1 };
        // 登録済みのクリップ一覧
        std::vector<SpriteAnimationClip> m_clips;
        // 自動再生するクリップ名
        std::string m_defaultClip;
        // 再生対象のクリップ名
        std::string m_activeClip;
        // 再生速度の倍率
        float m_speed{ 1.0f };
        // 速度倍率適用後の経過秒数
        float m_time{};
        // シート周回前の現在コマ番号
        int m_currentFrame{ -1 };
        // コマ送りの再生状態
        bool m_playing{};
        // 初回更新で自動再生する指定
        bool m_playOnStart{ true };
        // 初回更新を実行したか
        bool m_started{};
    };
}
