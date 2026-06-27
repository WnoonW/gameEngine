#pragma once
#include "World.h"

using namespace ECS;

class GravitySystem
{
public:
    void Update(World& world, float deltaTime);
};