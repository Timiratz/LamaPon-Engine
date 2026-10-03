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

        // リスナーのワールド変換
        const XMMATRIX world = Owner().WorldMatrix();
        // リスナーのワールド位置
        const XMVECTOR position = world.r[3];
        // 正規化した前方軸
        const XMVECTOR forward =
            XMVector3Normalize(XMVectorNegate(world.r[2]));
        // 正規化した上方軸
        const XMVECTOR up =
            XMVector3Normalize(world.r[1]);

        // 出力する音声リスナー
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
