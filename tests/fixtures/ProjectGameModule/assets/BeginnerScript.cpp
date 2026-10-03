#include "LamaPon/LamaPon.h"

class BeginnerScript final : public LamaPon::Script
{
public:
    // 所有ObjectのXを2にして初心者Scriptの登録と起動を検査する。
    void Start() override
    {
        Owner().GetTransform().position.x = 2.0f;
    }

    // 所有ObjectをY方向へ毎秒1動かして更新引数を検査する(deltaTime: 前の更新からの秒数)。
    void Update(const float deltaTime) override
    {
        Owner().GetTransform().position.y += deltaTime;
    }
};

LAMAPON_SCRIPT(BeginnerScript);
