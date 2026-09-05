#include "LamaPon/Web/WebAudioRuntime.h"

#include <algorithm>
#include <string>

#include <emscripten.h>

namespace
{
    EM_JS(void, lamapon_web_audio_initialize, (), {
        const root = globalThis;
        if (root.__lamaponAudioRuntime) return;
        const AudioContextClass = root.AudioContext || root.webkitAudioContext;
        if (!AudioContextClass) {
            if (document.body) document.body.dataset.lamaponAudio = "unsupported";
            return;
        }
        root.__lamaponAudioRuntime = {
            context: new AudioContextClass(),
            master: null,
        };
        const runtime = root.__lamaponAudioRuntime;
        runtime.master = runtime.context.createGain();
        runtime.master.gain.value = 1.0;
        runtime.master.connect(runtime.context.destination);
        runtime.connectSource = (source, gain, options) => {
            let spatialNode = null;
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
        runtime.publishState = () => {
            if (document.body) {
                document.body.dataset.lamaponAudio = runtime.context.state;
            }
        };
        runtime.unlock = () => {
            if (runtime.context.state !== "running") {
                runtime.context.resume()
                    .then(runtime.publishState)
                    .catch(runtime.publishState);
            }
        };
        runtime.context.addEventListener("statechange", runtime.publishState);
        runtime.publishState();
        // ブラウザーはユーザー操作まで音声を停止します。最初のキー、
        // ポインター、タッチ操作で再開し、ゲーム固有の有効化ボタンを
        // 必須にしません。
        root.addEventListener("keydown", runtime.unlock, { passive: true });
        root.addEventListener("pointerdown", runtime.unlock, { passive: true });
        root.addEventListener("touchstart", runtime.unlock, { passive: true });
    });

    EM_JS(void, lamapon_web_audio_unlock, (), {
        const runtime = globalThis.__lamaponAudioRuntime;
        if (runtime) runtime.unlock();
    });

    EM_JS(void, lamapon_web_audio_set_master_volume, (float volume), {
        const runtime = globalThis.__lamaponAudioRuntime;
        if (runtime && runtime.master) {
            runtime.master.gain.value = Math.max(0.0, Math.min(1.0, volume));
        }
    });

    EM_JS(void, lamapon_web_audio_play_tone,
          (float frequency, float duration, float volume), {
        const runtime = globalThis.__lamaponAudioRuntime;
        if (!runtime || !runtime.master) return;
        const context = runtime.context;
        const oscillator = context.createOscillator();
        const gain = context.createGain();
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

    EM_JS(void, lamapon_web_audio_play_wav,
          (const char* url, float volume, int loop, float pan, int spatial,
           float x, float y, float z, float minimumDistance,
           float maximumDistance), {
        const runtime = globalThis.__lamaponAudioRuntime;
        if (!runtime || !runtime.master) return;
        const sourceUrl = UTF8ToString(url);
        const context = runtime.context;
        // エクスポート時にアセットをEmscriptenの仮想ファイルシステムへ
        // 埋め込みます。埋め込みデータを優先し、複数ファイル構成では
        // fetchを代替手段として使います。
        let load;
        try {
            load = typeof FS !== "undefined"
                ? Promise.resolve(new Blob([FS.readFile(sourceUrl)], { type: "audio/wav" }))
                : fetch(sourceUrl);
        } catch (error) {
            console.warn("LamaPon Web Audio asset unavailable", sourceUrl, error);
            return;
        }
        load
            .then(response => response.arrayBuffer())
            .then(data => context.decodeAudioData(data))
            .then(buffer => {
                if (document.body) {
                    document.body.dataset.lamaponAudioAsset = "loaded";
                    document.body.dataset.lamaponAudioAssetPath = sourceUrl;
                }
                const source = context.createBufferSource();
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

    EM_JS(int, lamapon_web_audio_play_loop,
          (const char* url, float volume, float pan, int spatial,
           float x, float y, float z, float minimumDistance,
           float maximumDistance), {
        const runtime = globalThis.__lamaponAudioRuntime;
        if (!runtime || !runtime.master) return 0;
        const sourceUrl = UTF8ToString(url);
        const context = runtime.context;
        globalThis.__lamaponAudioSources = globalThis.__lamaponAudioSources || {};
        globalThis.__lamaponNextAudioSourceId =
            globalThis.__lamaponNextAudioSourceId || 1;
        const id = globalThis.__lamaponNextAudioSourceId++;
        const slot = { source: null, gain: null, spatialNode: null,
            panNode: null, volume: Math.max(0.0, Math.min(1.0, volume)),
            pitch: 0.0, pan: Math.max(-1.0, Math.min(1.0, pan)),
            spatial: !!spatial, x, y, z, minimumDistance, maximumDistance };
        globalThis.__lamaponAudioSources[id] = slot;
        let load;
        try {
            load = typeof FS !== "undefined"
                ? Promise.resolve(new Blob([FS.readFile(sourceUrl)], { type: "audio/wav" }))
                : fetch(sourceUrl);
        } catch (error) {
            console.warn("LamaPon Web Audio loop unavailable", sourceUrl, error);
            delete globalThis.__lamaponAudioSources[id];
            return 0;
        }
        load.then(response => response.arrayBuffer())
            .then(data => context.decodeAudioData(data))
            .then(buffer => {
                const current = globalThis.__lamaponAudioSources[id];
                if (!current) return;
                const source = context.createBufferSource();
                const gain = context.createGain();
                if (document.body) {
                    document.body.dataset.lamaponAudioAsset = "loaded";
                    document.body.dataset.lamaponAudioAssetPath = sourceUrl;
                }
                source.buffer = buffer;
                source.loop = true;
                source.playbackRate.value = Math.pow(2.0, current.pitch);
                gain.gain.value = current.volume;
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

    EM_JS(void, lamapon_web_audio_set_loop_volume,
          (int handle, float volume), {
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        slot.volume = Math.max(0.0, Math.min(1.0, volume));
        if (slot.gain) slot.gain.gain.value = slot.volume;
    });

    EM_JS(void, lamapon_web_audio_set_loop_pitch,
          (int handle, float octaves), {
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        const safe = Math.max(-1.0, Math.min(1.0, octaves));
        slot.pitch = safe;
        if (slot.source) slot.source.playbackRate.value = Math.pow(2.0, safe);
    });

    EM_JS(void, lamapon_web_audio_set_loop_pan,
          (int handle, float pan), {
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        slot.pan = Math.max(-1.0, Math.min(1.0, pan));
        if (slot.panNode) slot.panNode.pan.value = slot.pan;
    });

    EM_JS(void, lamapon_web_audio_set_loop_position,
          (int handle, float x, float y, float z), {
        const slot = globalThis.__lamaponAudioSources?.[handle];
        if (!slot) return;
        slot.x = x; slot.y = y; slot.z = z;
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

    EM_JS(void, lamapon_web_audio_set_listener,
          (float x, float y, float z,
           float forwardX, float forwardY, float forwardZ,
           float upX, float upY, float upZ), {
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

    EM_JS(void, lamapon_web_audio_stop_loop, (int handle), {
        const sources = globalThis.__lamaponAudioSources;
        const slot = sources?.[handle];
        if (!slot) return;
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
