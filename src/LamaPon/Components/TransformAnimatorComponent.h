#pragma once

#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Scene/Component.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

namespace LamaPon
{
    class AssetManager;

    // 制御器では状態側の速度・周回を使い、ブレンド時間には全体の速度倍率を掛けない。
    class TransformAnimatorComponent final : public Component
    {
    public:
        // クリップまたは制御器でローカルTransformを再生する(clipPath: 単体再生クリップのパス, speed: 0超100以下の速度倍率, loop: 単体再生で周回する指定, playOnStart: 初期化時に再生する指定, controllerPath: 制御器のパスで空は単体再生)。
        explicit TransformAnimatorComponent(
            std::filesystem::path clipPath = {},
            float speed = 1.0f,
            bool loop = true,
            bool playOnStart = true,
            std::filesystem::path controllerPath = {});

        // クリップを読み込んで単体再生へ切り替え、時刻と遷移を初期化する(path: クリップのパスで空は解除)。
        // 新しいクリップの読み込みに失敗した場合は切り替え前の状態を保持する。
        void SetClipPath(
            std::filesystem::path path);
        // 単体再生用に設定されたクリップのパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            ClipPath() const noexcept
        {
            return m_clipPath;
        }
        // 有限かつ0超100以下の速度倍率を設定し、不正なら例外を返す(speed: 再生速度の倍率)。
        void SetSpeed(float speed);
        // 制御器を設定して入口状態から再生し、空なら単体再生設定へ戻す(path: 制御器のパスで空は解除)。
        void SetControllerPath(
            std::filesystem::path path);
        // 制御器のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            ControllerPath() const noexcept
        {
            return m_controllerPath;
        }
        // コンポーネント全体の再生速度倍率を取得する。
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }
        // 単体再生時の周回を設定する(loop: 末尾から先頭へ周回する指定)。
        void SetLoop(bool loop) noexcept
        {
            m_loop = loop;
        }
        // 単体再生時の周回設定を取得する。
        [[nodiscard]] bool Loop() const noexcept
        {
            return m_loop;
        }
        // 初期化時の自動再生を設定する(enabled: 自動再生する指定)。
        void SetPlayOnStart(bool enabled) noexcept
        {
            m_playOnStart = enabled;
        }
        // 初期化時に自動再生する設定か確認する。
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }

        // クリップがあれば再生を開始し、末尾に達していれば先頭へ戻す。
        void Play() noexcept;
        // 時刻と遷移状態を保持して再生を一時停止する。
        void Pause() noexcept;
        // 再生を停止し、時刻・遷移・トリガーを初期化して先頭の姿勢を適用する。
        void Stop() noexcept;
        // 単体クリップを再読み込みし、制御器の指定があれば制御器を再読み込みする。
        // 以前のクリップを先に解除するため、読み込み失敗時は以前の再生状態へ戻らない。
        void ReloadClip();
        // 制御器と入口クリップを再読み込みして入口状態の姿勢を適用する。
        // 以前の制御器とクリップを先に解除し、失敗時は読み込み済みの段階の状態が残る。
        void ReloadController();
        // 1〜96バイトの非空トリガーを記録し、不正なら例外を返す(trigger: 遷移を要求するトリガー名)。
        void SetTrigger(std::string trigger);
        // 時刻をクリップの範囲に収めて姿勢を適用し、非有限値は0とする(time: 設定する再生時刻の秒数)。
        void SetTime(float time) noexcept;
        // 再生が有効か確認する。
        [[nodiscard]] bool IsPlaying() const noexcept
        {
            return m_playing;
        }
        // 現在クリップの再生時刻を秒で取得する。
        [[nodiscard]] float Time() const noexcept
        {
            return m_time;
        }
        // 現在クリップの長さを秒で取得し、未読込なら0を返す。
        [[nodiscard]] float Duration() const noexcept
        {
            return m_clip != nullptr
                ? m_clip->Duration()
                : 0.0f;
        }
        // 現在クリップのキーフレーム数を取得し、未読込なら0を返す。
        [[nodiscard]] std::size_t KeyframeCount() const noexcept
        {
            return m_clip != nullptr
                ? m_clip->Keyframes().size()
                : 0;
        }
        // 制御器の現在状態名を取得する。
        [[nodiscard]] const std::string&
            CurrentState() const noexcept
        {
            return m_currentState;
        }
        // 遷移先クリップを保持しているか確認する。
        [[nodiscard]] bool IsTransitioning() const noexcept
        {
            return m_nextClip != nullptr;
        }

        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "TransformAnimator";
        }

    protected:
        // アセットを借用して制御器優先で読み込み、自動再生と初期姿勢を適用する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 正の経過時間でクリップまたは制御器の再生を進める(deltaTime: 有限な経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // 単体クリップを読み込み、現在クリップと時刻を初期化する。
        void LoadClip();
        // 制御器と入口クリップを読み込み、入口状態を選択する。
        void LoadController();
        // 状態のクリップを読み込み、時刻と遷移を初期化して選択する(state: 再生を開始する状態)。
        void EnterState(
            const AnimatorState& state);
        // 遷移先を読み込み、トリガーを消費して即時変更またはブレンドを始める(transition: 遷移先とブレンド設定)。
        void StartTransition(
            const AnimatorTransition& transition);
        // 状態の時刻と遷移を更新し、位置・回転・拡縮をブレンドする(deltaTime: 有限で正の経過秒数)。
        void UpdateController(float deltaTime);
        // 現在クリップの位置・回転・拡縮をローカルTransformへ適用する。
        void ApplyCurrentSample() noexcept;

        // 単体再生用のクリップパス
        std::filesystem::path m_clipPath;
        // 制御器のパス
        std::filesystem::path m_controllerPath;
        // 全体の再生速度倍率
        float m_speed{ 1.0f };
        // 単体再生で周回する指定
        bool m_loop{ true };
        // 初期化時に自動再生する指定
        bool m_playOnStart{ true };
        // 再生が有効か
        bool m_playing{};
        // 現在クリップの再生秒数
        float m_time{};
        // 借用したアセット管理
        AssetManager* m_assets{};
        // 現在再生する共有クリップ
        std::shared_ptr<const AnimationClip> m_clip;
        // 状態と遷移を定義する共有制御器
        std::shared_ptr<const AnimatorController>
            m_controller;
        // ブレンド先の共有クリップ
        std::shared_ptr<const AnimationClip> m_nextClip;
        // 制御器の現在状態名
        std::string m_currentState;
        // 制御器の遷移先状態名
        std::string m_nextState;
        // 遷移先クリップの再生秒数
        float m_nextTime{};
        // ブレンド開始からの実経過秒数
        float m_transitionTime{};
        // ブレンドに使う実経過秒数
        float m_transitionDuration{};
        // 遷移で消費される未処理トリガー
        std::unordered_set<std::string>
            m_activeTriggers;
    };
}
