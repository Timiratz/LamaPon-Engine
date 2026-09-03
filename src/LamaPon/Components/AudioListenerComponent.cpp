#include "LamaPon/Components/AudioListenerComponent.h"

#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

namespace LamaPon
{
    AudioListenerComponent::~AudioListenerComponent()
    {
        if (m_audio != nullptr)
        {
            m_audio->UnregisterListener(*this);
        }
    }

    DirectX::AudioListener
        AudioListenerComponent::BuildListener() const noexcept
    {
        using namespace DirectX;

        const XMMATRIX world = Owner().WorldMatrix();
        const XMVECTOR position = world.r[3];
        const XMVECTOR forward =
            XMVector3Normalize(XMVectorNegate(world.r[2]));
        const XMVECTOR up =
            XMVector3Normalize(world.r[1]);

        AudioListener listener;
        listener.SetPosition(position);
        listener.SetOrientation(forward, up);
        return listener;
    }

    void AudioListenerComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_audio = &graphics.Audio();
        m_audio->RegisterListener(*this);
    }
}
