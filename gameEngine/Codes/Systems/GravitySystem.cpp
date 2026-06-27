#include "GravitySystem.h"
#include "ComponentStruct.h"

using namespace ECS;
using namespace DirectX;

void GravitySystem::Update(World& world, float deltaTime)
{
    world.ForEach<TransformComponent, GravityComponent>(
        [&](Entity, TransformComponent& tf, GravityComponent& gravity)
        {
            if (!gravity.enabled)
                return;

            gravity.velocity.y -= gravity.strength * deltaTime;

            tf.position.x += gravity.velocity.x * deltaTime;
            tf.position.y += gravity.velocity.y * deltaTime;
            tf.position.z += gravity.velocity.z * deltaTime;
        });
}