#pragma once

#include "LamaPon/Scene/Component.h"

#include <Audio.h>

namespace LamaPon
{
    class AudioSystem;

    class AudioListenerComponent final : public Component
    {
    public:
        // 位置と向きを持つ音声リスナーを作ります。
        AudioListenerComponent() = default;
        // 音声システムへの登録を解除します。
        ~AudioListenerComponent() override;

        // ワールド位置と向きから速度ゼロのリスナーを返します。
        // 前方・上方の軸は非ゼロかつ平行でない必要があります。
        [[nodiscard]] DirectX::AudioListener
            BuildListener() const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "AudioListener";
        }

    protected:
        // 音声システムに登録します(graphics: 登録先を持つ描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;

    private:
        // 登録先の音声システムはこの部品より長く生存する必要があります。
        // 登録先の借用音声システム
        AudioSystem* m_audio{};
    };
}
