#pragma once
#include "World.h"

using namespace ECS;

class GravitySystem
{
public:
    // skipCpuIntegrate: Path3 GPU motion (F2) owns integration — avoid double-apply
    void Update(World& world, float deltaTime, bool skipCpuIntegrate = false);
};