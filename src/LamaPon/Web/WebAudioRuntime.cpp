#include "LamaPon/Web/WebAudioRuntime.h"

#include <algorithm>
#include <string>

#include <emscripten.h>

namespace
{
    // Web Audioの共有Contextと音源接続処理を用意する。
    EM_JS(void, lamapon_web_audio_initialize, (), {
        // ブラウザーの共有グローバル領域
        const root = globalThis;
        if (root.__lamaponAudioRuntime) return;
        // 利用可能なAudioContext型
        const AudioContextClass = root.AudioContext || root.webkitAudioContext;
        if (!AudioContextClass) {
            if (document.body) document.body.dataset.lamaponAudio = "unsupported";
            return;
        }
        root.__lamaponAudioRuntime = {
            // 共有する音声Context
            context: new AudioContextClass(),
            // 共有する主音量ノード
            master: null,
        };
        // ブラウザー共有の音声実行状態
        const runtime = root.__lamaponAudioRuntime;
        runtime.master = runtime.context.createGain();
        runtime.master.gain.value = 1.0;
        runtime.master.connect(runtime.context.destination);
        // 音源へ定位と主音量の接続を追加する(source: 再生ノード, gain: 音量ノード, options: 定位と減衰の設定)。
        runtime.connectSource = (source, gain, options) => {
            // 3D定位と距離減衰の接続
            let spatialNode = null;
            // ステレオ定位の接続
            let panNode = null;
            if (options.spatial && runtime.context.createPanner) {
                spatialNode = runtime.context.createPanner();
                spatialNode.panningModel = "HRTF";
                spatialNode.distanceModel = "inverse";
                spatialNode.refDistance = Math.max(0.01, options.minimumDistance);
                spatialNode.maxDistance = Math.max(
                    spatialNode.refDistance, options.maximumDistance);
                spatialNode.rolloffFactor = 1.0;
                if (spatialNode.positionX) {
                    spatialNode.positionX.value = options.x;
                    spatialNode.positionY.value = options.y;
                    spatialNode.positionZ.value = options.z;
                } else {
                    spatialNode.setPosition(options.x, options.y, options.z);
                }
                source.connect(spatialNode);
                spatialNode.connect(gain);
            } else if (runtime.context.createStereoPanner) {
                panNode = runtime.context.createStereoPanner();
                panNode.pan.value = Math.max(-1.0, Math.min(1.0, options.pan));
                source.connect(panNode);
                panNode.connect(gain);
            } else {
                source.connect(gain);
            }
            gain.connect(runtime.master);
            return { spatialNode, panNode };
        };
        // Contextの実行状態をDOMへ公開する。
        runtime.publishState = () => {
            if (document.body) {
                document.body.dataset.lamaponAudio = runtime.context.state;
            }
        };
        // 停止中のContextへ非同期の再開を要求する。
        runtime.unlock = () => {
            if (runtime.context.state !== "running") {
                runtime.context.resume()
                    .then(runtime.publishState)
                    .catch(runtime.publishState);
            }
        };
        runtime.context.addEventListener("statechange", runtime.publishState);
        runtime.publishState();
        // 最初のキー・ポインター・タッチ操作でContextの再開を要求する。
        root.addEventListener("keydown", runtime.unlock, { passive: true });
        root.addEventListener("pointerdown", runtime.unlock, { passive: true });
        root.addEventListener("touchstart", runtime.unlock, { passive: true });
    });

    // Web Audioの再開を要求する。
    EM_JS(void, lamapon_web_audio_unlock, (), {
        // ブラウザー共有の音声実行状態
        const runtime = globalThis.__lamaponAudioRuntime;
        if (runtime) runtime.unlock();
    });

    // 共有の主音量を設定する(volume: 音量0～1)。
    EM_JS(void, lamapon_web_audio_set_master_volume, (float volume), {
        // ブラウザー共有の音声実行状態
        const runtime = globalThis.__lamaponAudioRuntime;
        if (runtime && runtime.master) {
            runtime.master.gain.value = Math.max(0.0, Math.min(1.0, volume));
        }
    });

    // 三角波の短音を生成する(frequency: 周波数Hz, duration: 再生秒数, volume: 音量0～1)。
    EM_JS(void, lamapon_web_audio_play_tone,
          (float frequency, float duration, float volume), {
        // ブラウザー共有の音声実行状態
        const runtime = globalThis.__lamaponAudioRuntime;
        if (!runtime || !runtime.master) return;
        // 共有のAudioContext
        const context = runtime.context;
        // 三角波を生成する音源
        const oscillator = context.createOscillator();
        // 音量制御のGain接続
        const gain = context.createGain();
        // 音声時計での開始時刻（秒）
        const start = context.currentTime;
        oscillator.type = "triangle";
        oscillator.frequency.setValueAtTime(Math.max(20.0, frequency), start);
        gain.gain.setValueAtTime(0.0001, start);
        gain.gain.exponentialRampToValueAtTime(
            Math.max(0.0001, volume), start + 0.008);
        gain.gain.exponentialRampToValueAtTime(
            0.0001, start + Math.max(0.02, duration));
        oscillator.connect(gain);
        gain.connect(runtime.master);
        oscillator.start(start);
        oscillator.stop(start + Math.max(0.02, duration) + 0.02);
    });

    // 音声の非同期読込と再生を開始する(url: 音声のURL・仮想パス, volume: 音量0～1, loop: ループ再生するか, pan: 左右定位-1～1, spatial: 3D定位を使うか, x: 音源X位置, y: 音源Y位置, z: 音源Z位置, minimumDistance: 減衰開始距離, maximumDistance: 減衰の最大距離)。
    EM_JS(void, lamapon_web_audio_play_wav,
          (const char* url, float volume, int loop, float pan, int spatial,
           float x, float y, float z, float minimumDistance,
           float maximumDistance), {
        // ブラウザー共有の音声実行状態
        const runtime = globalThis.__lamaponAudioRuntime;
        if (!runtime || !runtime.master) return;
        // JSへ複製した音声パス
        const sourceUrl = UTF8ToString(url);
        // 共有のAudioContext
        const context = runtime.context;
        // FSが利用可能なら仮想ファイルを読み、FS自体が無い場合だけfetchを使う。
        // 音声データの読込Promise
        let load;
        try {
            load = typeof FS !== "undefined"
                ? Promise.resolve(new Blob([FS.readFile(sourceUrl)], { type: "audio/wav" }))
                : fetch(sourceUrl);
        }
        // 同期読込の失敗を記録する(error: 読込エラー)。
        catch (error) {
            console.warn("LamaPon Web Audio asset unavailable", sourceUrl, error);
            return;
        }
        // 音声をデコードして再生し、失敗を記録する(response: 読込応答, data: 音声バイト列, buffer: デコード済み音声, error: 読込・デコードエラー)。
        load
            .then(response => response.arrayBuffer())
            .then(data => context.decodeAudioData(data))
            .then(buffer => {
                if (document.body) {
                    document.body.dataset.lamaponAudioAsset = "loaded";
                    document.body.dataset.lamaponAudioAssetPath = sourceUrl;
                }
                // デコード音声の再生ノード
                const source = context.createBufferSource();
                // 音量制御のGain接続
                const gain = context.createGain();
                source.buffer = buffer;
                source.loop = !!loop;
                gain.gain.value = Math.max(0.0, Math.min(1.0, volume));
                runtime.connectSource(source, gain, {
                    pan, spatial: !!spatial, x, y, z,
                    minimumDistance, maximumDistance,
                });
                source.start();
            })
            .catch(error => {
                if (document.body) {
                    document.body.dataset.lamaponAudioAsset = "error";
                    document.body.dataset.lamaponAudioAssetPath = sourceUrl;
                }
                console.warn("LamaPon Web Audio load failed", error);
            });
    });

    // ループ再生を予約し、取消用IDを返す(url: 音声のURL・仮想パス, volume: 音量0～1, pan: 左右定位-1～1, spatial: 3D定位を使うか, x: 音源X位置, y: 音源Y位置, z: 音源Z位置, minimumDistance: 減衰開始距離, maximumDistance: 減衰の最大距離)。
    EM_JS(int, lamapon_web_audio_play_loop,
          (const char* url, float volume, float pan, int spatial,
           float x, float y, float z, float minimumDistance,
           float maximumDistance), {
        // ブラウザー共有の音声実行状態
        const runtime = globalThis.__lamaponAudioRuntime;
        if (!runtime || !runtime.master) return 0;
        // JSへ複製した音声パス
        const sourceUrl = UTF8ToString(url);
        // 共有のAudioContext
        const context = runtime.context;
        globalThis.__lamaponAudioSources = globalThis.__lamaponAudioSources || {};
        globalThis.__lamaponNextAudioSourceId =
            globalThis.__lamaponNextAudioSourceId || 1;
        // 新規ループ音源の予約ID
        const id = globalThis.__lamaponNextAudioSourceId++;
        // ループ音源の予約と接続状態
        const slot = { source: null, gain: null, spatialNode: null,
            panNode: null, volume: Math.max(0.0, Math.min(1.0, volume)),
            pitch: 0.0, pan: Math.max(-1.0, Math.min(1.0, pan)),
            spatial: !!spatial, x, y, z, minimumDistance, maximumDistance };
        globalThis.__lamaponAudioSources[id] = slot;
        // 音声データの読込Promise
        let load;
        try {
            load = typeof FS !== "undefined"
                ? Promise.resolve(new Blob([FS.readFile(sourceUrl)], { type: "audio/wav" }))
                : fetch(sourceUrl);
        }
        // 同期読込の失敗を記録する(error: 読込エラー)。
        catch (error) {
            console.warn("LamaPon Web Audio loop unavailable", sourceUrl, error);
            delete globalThis.__lamaponAudioSources[id];
            return 0;
        }
        // 未取消の予約を再生し、失敗時は予約を削除する(response: 読込応答, data: 音声バイト列, buffer: デコード済み音声, error: 読込・デコードエラー)。
        load.then(response => response.arrayBuffer())
            .then(data => context.decodeAudioData(data))
            .then(buffer => {
                // 読込後も有効な再生予約
                const current = globalThis.__lamaponAudioSources[id];
                if (!current) return;
                // デコード音声の再生ノード
                const source = context.createBufferSource();
                // 音量制御のGain接続
                const gain = context.createGain();
                if (document.body) {
                    document.body.dataset.lamaponAudioAsset = "loaded";
                    document.body.dataset.lamaponAudioAssetPath = sourceUrl;
                }
                source.buffer = buffer;
                source.loop = true;
                source.playbackRate.value = Math.pow(2.0, current.pitch);
                gain.gain.value = current.volume;
                // 作成した定位接続の組
                const nodes = runtime.connectSource(source, gain, current);
                current.source = source;
                current.gain = gain;
                current.spatialNode = nodes.spatialNode;
                current.panNode = nodes.panNode;
                source.start();
            })
            .catch(error => {
                if (document.body) {
                    document.body.dataset.lamaponAudioAsset = "error";
                    document.body.dataset.lamaponAudioAssetPath = sourceUrl;
                }
                console.warn("LamaPon Web Audio loop load failed", sourceUrl, error);
                delete globalThis.__lamaponAudioSources[id];
            });
        return id;
    });

    // 音源の予約音量と再生音量を更新する(handle: 再生予約ID, volume: 音量0～1)。
    EM_JS(void, lamapon_web_audio_set_loop_volume,
          (int handle, float volume), {
        // ループ音源の予約と接続状態
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        slot.volume = Math.max(0.0, Math.min(1.0, volume));
        if (slot.gain) slot.gain.gain.value = slot.volume;
    });

    // 音源の再生速度を変更する(handle: 再生予約ID, octaves: -1～1の音程差)。
    EM_JS(void, lamapon_web_audio_set_loop_pitch,
          (int handle, float octaves), {
        // ループ音源の予約と接続状態
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        // -1～1へ制限した音程差
        const safe = Math.max(-1.0, Math.min(1.0, octaves));
        slot.pitch = safe;
        if (slot.source) slot.source.playbackRate.value = Math.pow(2.0, safe);
    });

    // 音源のステレオ定位を変更する(handle: 再生予約ID, pan: 左右定位-1～1)。
    EM_JS(void, lamapon_web_audio_set_loop_pan,
          (int handle, float pan), {
        // ループ音源の予約と接続状態
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        slot.pan = Math.max(-1.0, Math.min(1.0, pan));
        if (slot.panNode) slot.panNode.pan.value = slot.pan;
    });

    // 3D音源の位置を変更する(handle: 再生予約ID, x: X位置, y: Y位置, z: Z位置)。
    EM_JS(void, lamapon_web_audio_set_loop_position,
          (int handle, float x, float y, float z), {
        // ループ音源の予約と接続状態
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        slot.x = x; slot.y = y; slot.z = z;
        // 現在の3D定位接続
        const node = slot.spatialNode;
        if (!node) return;
        if (node.positionX) {
            node.positionX.value = x;
            node.positionY.value = y;
            node.positionZ.value = z;
        } else {
            node.setPosition(x, y, z);
        }
    });

    // 共有Contextの聴取位置と姿勢を変更する(x: X位置, y: Y位置, z: Z位置, forwardX: 前方向X成分, forwardY: 前方向Y成分, forwardZ: 前方向Z成分, upX: 上方向X成分, upY: 上方向Y成分, upZ: 上方向Z成分)。
    EM_JS(void, lamapon_web_audio_set_listener,
          (float x, float y, float z,
           float forwardX, float forwardY, float forwardZ,
           float upX, float upY, float upZ), {
        // 共有Contextの聴取者設定
        const listener = globalThis.__lamaponAudioRuntime?.context?.listener;
        if (!listener) return;
        if (listener.positionX) {
            listener.positionX.value = x;
            listener.positionY.value = y;
            listener.positionZ.value = z;
            listener.forwardX.value = forwardX;
            listener.forwardY.value = forwardY;
            listener.forwardZ.value = forwardZ;
            listener.upX.value = upX;
            listener.upY.value = upY;
            listener.upZ.value = upZ;
        } else {
            listener.setPosition(x, y, z);
            listener.setOrientation(
                forwardX, forwardY, forwardZ, upX, upY, upZ);
        }
    });

    // 音源を停止して読込予約も削除する(handle: 再生予約ID)。
    EM_JS(void, lamapon_web_audio_stop_loop, (int handle), {
        // ループ音源の予約ID一覧
        const sources = globalThis.__lamaponAudioSources;
        // ループ音源の予約と接続状態
        const slot = sources?.[handle];
        if (!slot) return;
        // 音源の停止失敗は無視し、予約削除を続ける(error: 停止エラー)。
        try { if (slot.source) slot.source.stop(); } catch (error) {}
        delete sources[handle];
    });
}

namespace LamaPon::Web
{
    void WebAudioRuntime::Initialize() noexcept
    {
        if (!m_initialized)
        {
            lamapon_web_audio_initialize();
            lamapon_web_audio_set_master_volume(m_masterVolume);
            m_initialized = true;
        }
    }

    void WebAudioRuntime::UnlockFromUserGesture() noexcept
    {
        Initialize();
        lamapon_web_audio_unlock();
    }

    void WebAudioRuntime::SetMasterVolume(float volume) noexcept
    {
        m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
        Initialize();
        lamapon_web_audio_set_master_volume(m_masterVolume);
    }

    void WebAudioRuntime::PlayTone(
        float frequency,
        float durationSeconds,
        float volume) noexcept
    {
        UnlockFromUserGesture();
        lamapon_web_audio_play_tone(
            frequency,
            std::max(durationSeconds, 0.02f),
            std::clamp(volume, 0.0f, 1.0f));
    }

    void WebAudioRuntime::PlayWav(
        std::string_view url,
        float volume,
        bool loop,
        float pan,
        bool spatial,
        float x,
        float y,
        float z,
        float minimumDistance,
        float maximumDistance) noexcept
    {
        UnlockFromUserGesture();
        // JSへ渡す終端付き音声パス
        const std::string copiedUrl(url);
        lamapon_web_audio_play_wav(
            copiedUrl.c_str(),
            std::clamp(volume, 0.0f, 1.0f),
            loop ? 1 : 0,
            std::clamp(pan, -1.0f, 1.0f),
            spatial ? 1 : 0,
            x, y, z,
            std::max(minimumDistance, 0.01f),
            std::max(maximumDistance, minimumDistance));
    }

    AudioHandle WebAudioRuntime::PlayLoop(
        std::string_view url,
        float volume,
        float pan,
        bool spatial,
        float x,
        float y,
        float z,
        float minimumDistance,
        float maximumDistance) noexcept
    {
        UnlockFromUserGesture();
        // JSへ渡す終端付き音声パス
        const std::string copiedUrl(url);
        return static_cast<AudioHandle>(lamapon_web_audio_play_loop(
            copiedUrl.c_str(),
            std::clamp(volume, 0.0f, 1.0f),
            std::clamp(pan, -1.0f, 1.0f),
            spatial ? 1 : 0,
            x, y, z,
            std::max(minimumDistance, 0.01f),
            std::max(maximumDistance, minimumDistance)));
    }

    void WebAudioRuntime::SetVolume(AudioHandle handle, float volume) noexcept
    {
        if (handle == 0) return;
        lamapon_web_audio_set_loop_volume(
            static_cast<int>(handle), std::clamp(volume, 0.0f, 1.0f));
    }

    void WebAudioRuntime::SetPitch(AudioHandle handle, float octaves) noexcept
    {
        if (handle == 0) return;
        lamapon_web_audio_set_loop_pitch(
            static_cast<int>(handle), std::clamp(octaves, -1.0f, 1.0f));
    }

    void WebAudioRuntime::SetPan(AudioHandle handle, float pan) noexcept
    {
        if (handle == 0) return;
        lamapon_web_audio_set_loop_pan(
            static_cast<int>(handle), std::clamp(pan, -1.0f, 1.0f));
    }

    void WebAudioRuntime::SetPosition(
        AudioHandle handle,
        float x,
        float y,
        float z) noexcept
    {
        if (handle == 0) return;
        lamapon_web_audio_set_loop_position(
            static_cast<int>(handle), x, y, z);
    }

    void WebAudioRuntime::SetListener(
        float x,
        float y,
        float z,
        float forwardX,
        float forwardY,
        float forwardZ,
        float upX,
        float upY,
        float upZ) noexcept
    {
        Initialize();
        lamapon_web_audio_set_listener(
            x, y, z,
            forwardX, forwardY, forwardZ,
            upX, upY, upZ);
    }

    void WebAudioRuntime::Stop(AudioHandle handle) noexcept
    {
        if (handle == 0) return;
        lamapon_web_audio_stop_loop(static_cast<int>(handle));
    }
}
