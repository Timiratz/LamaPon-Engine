#pragma once

#include "LamaPon/Scene/Component.h"

#include <Audio.h>

namespace LamaPon
{
    class AudioSystem;

    class AudioListenerComponent final : public Component
    {
    public:
        AudioListenerComponent() = default;
        ~AudioListenerComponent() override;

        [[nodiscard]] DirectX::AudioListener
            BuildListener() const noexcept;

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "AudioListener";
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;

    private:
        AudioSystem* m_audio{};
    };
}
