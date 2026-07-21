#pragma once
#include "World.h"

using namespace ECS;

class GravitySystem
{
public:
    // gpuMotionMirror: CPU도 동일 적분(충돌/선택) + suppressGpuUpload (Path3 TRS 미업로드)
    void Update(World& world, float deltaTime, bool gpuMotionMirror = false);
};