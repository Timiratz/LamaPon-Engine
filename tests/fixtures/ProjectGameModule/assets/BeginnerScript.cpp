#include "LamaPon/LamaPon.h"

class BeginnerScript final : public LamaPon::Script
{
public:
    void Start() override
    {
        Owner().GetTransform().position.x = 2.0f;
    }

    void Update(const float deltaTime) override
    {
        Owner().GetTransform().position.y += deltaTime;
    }
};

LAMAPON_SCRIPT(BeginnerScript);
