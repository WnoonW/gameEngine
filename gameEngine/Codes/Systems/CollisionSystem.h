#pragma once
#include "World.h"

using namespace ECS;

class CollisionSystem
{
public:
    // true: 충돌 처리로 위치가 바뀔 수 있음 → bounds 재갱신 필요
    bool Update(World& world);
};