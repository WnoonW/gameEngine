#include "GravitySystem.h"
#include "ComponentStruct.h"

using namespace ECS;
using namespace DirectX;

void GravitySystem::Update(World& world, float deltaTime)
{
    // GravityComponent 없는 아키타입은 ForEach가 바로 끝남 (정적 대량 씬 무부담)
    world.ForEach<TransformComponent, GravityComponent>(
        [&](Entity e, TransformComponent& tf, GravityComponent& gravity)
        {
            if (!gravity.enabled)
                return;

            gravity.velocity.y -= gravity.strength * deltaTime;

            tf.position.x += gravity.velocity.x * deltaTime;
            tf.position.y += gravity.velocity.y * deltaTime;
            tf.position.z += gravity.velocity.z * deltaTime;
            tf.MarkDirty(e);
        });
}