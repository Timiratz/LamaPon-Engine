#pragma once

#include <cstdint>
#include <string_view>

namespace LamaPon::Web
{
    using AudioHandle = std::uint32_t;

    // 音声はブラウザー全体で共有し、破棄時の一括停止やContextの破棄は行わない。
    class WebAudioRuntime final
    {
    public:
        // 未初期化・最大主音量のWeb音声アダプターを用意する。
        WebAudioRuntime() = default;

        // ブラウザー共有のWeb Audio接続と主音量を初期化する。
        void Initialize() noexcept;
        // ユーザー操作外の再開はブラウザーに拒否され得る。
        // Web Audioの再開を要求する。
        void UnlockFromUserGesture() noexcept;
        // ブラウザー共有の主音量を設定する(volume: 音量0～1)。
        void SetMasterVolume(float volume) noexcept;
        // 三角波の短い音を鳴らす(frequency: 最小20Hzの周波数, durationSeconds: 最小0.02秒の長さ, volume: 音量0～1)。
        void PlayTone(
            float frequency,
            float durationSeconds,
            float volume = 0.12f) noexcept;
        // 音声を非同期に読み込んで再生する(url: 音声のURL・仮想パス, volume: 音量0～1, loop: ループ再生するか, pan: 左右定位-1～1, spatial: 3D定位を使うか, x: 音源X位置, y: 音源Y位置, z: 音源Z位置, minimumDistance: 減衰開始距離, maximumDistance: 減衰の最大距離)。
        void PlayWav(
            std::string_view url,
            float volume = 1.0f,
            bool loop = false,
            float pan = 0.0f,
            bool spatial = false,
            float x = 0.0f,
            float y = 0.0f,
            float z = 0.0f,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f) noexcept;
        // 返すIDは再生開始を保証せず、非同期読込失敗時は無効になる。
        // 非同期のループ再生を予約し、準備不可なら0を返す(url: 音声のURL・仮想パス, volume: 音量0～1, pan: 左右定位-1～1, spatial: 3D定位を使うか, x: 音源X位置, y: 音源Y位置, z: 音源Z位置, minimumDistance: 減衰開始距離, maximumDistance: 減衰の最大距離)。
        [[nodiscard]] AudioHandle PlayLoop(
            std::string_view url,
            float volume = 1.0f,
            float pan = 0.0f,
            bool spatial = false,
            float x = 0.0f,
            float y = 0.0f,
            float z = 0.0f,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f) noexcept;
        // 再生予約または再生中の音量を更新する(handle: PlayLoopのID, volume: 音量0～1)。
        void SetVolume(AudioHandle handle, float volume) noexcept;
        // 再生速度をオクターブ比で更新する(handle: PlayLoopのID, octaves: -1～1の音程差)。
        void SetPitch(AudioHandle handle, float octaves) noexcept;
        // 3D定位を使う音源にはpanの変更を適用しない。
        // ステレオ定位を更新する(handle: PlayLoopのID, pan: 左右定位-1～1)。
        void SetPan(AudioHandle handle, float pan) noexcept;
        // 3D音源の位置を更新する(handle: PlayLoopのID, x: X位置, y: Y位置, z: Z位置)。
        void SetPosition(
            AudioHandle handle,
            float x,
            float y,
            float z) noexcept;
        // ブラウザー共有の聴取位置と姿勢を更新する(x: X位置, y: Y位置, z: Z位置, forwardX: 前方向X成分, forwardY: 前方向Y成分, forwardZ: 前方向Z成分, upX: 上方向X成分, upY: 上方向Y成分, upZ: 上方向Z成分)。
        void SetListener(
            float x,
            float y,
            float z,
            float forwardX,
            float forwardY,
            float forwardZ,
            float upX,
            float upY,
            float upZ) noexcept;
        // ループ再生を停止し、読込中なら開始を取り消す(handle: PlayLoopのID)。
        void Stop(AudioHandle handle) noexcept;

    private:
        // このアダプターの初期化実行済み
        bool m_initialized{};
        // 次回初期化にも使う主音量
        float m_masterVolume{ 1.0f };
    };
}
