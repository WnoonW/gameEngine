#include "GravitySystem.h"
#include "ComponentStruct.h"

using namespace ECS;
using namespace DirectX;

void GravitySystem::Update(World& world, float deltaTime, bool gpuMotionMirror)
{
    // GravityComponent 없는 아키타입은 ForEach가 바로 끝남 (정적 대량 씬 무부담)
    world.ForEach<TransformComponent, GravityComponent>(
        [&](Entity e, TransformComponent& tf, GravityComponent& gravity)
        {
            if (!gravity.enabled)
                return;

            // GPU CS_UpdateMotion 과 동일 적분 (CPU = 충돌/선택/에디터 소스)
            gravity.velocity.y -= gravity.strength * deltaTime;

            tf.position.x += gravity.velocity.x * deltaTime;
            tf.position.y += gravity.velocity.y * deltaTime;
            tf.position.z += gravity.velocity.z * deltaTime;

            tf.rotation.x += gravity.angularVelocity.x * deltaTime;
            tf.rotation.y += gravity.angularVelocity.y * deltaTime;
            tf.rotation.z += gravity.angularVelocity.z * deltaTime;

            // gpuMotionMirror: bounds/충돌용 dirty, Path3 TRS 업로드는 생략
            // (GPU 버퍼가 같은 식으로 적분 중 — 덮어쓰면 이중 적용/리셋)
            tf.MarkDirty(e, 3, gpuMotionMirror);
        });
}
