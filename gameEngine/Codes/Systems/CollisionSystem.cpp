#include "CollisionSystem.h"
#include "ComponentStruct.h"
#include <DirectXCollision.h>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace ECS;
using namespace DirectX;

namespace
{
    constexpr int kMaxSolverIterations = 4;

    struct ColliderEntry
    {
        Entity entity;
        TransformComponent* transform;
        BoundsComponent* bounds;
        CollisionComponent* collision;
        GravityComponent* gravity;
    };

    void GetAABBMinMax(const BoundingBox& box, XMFLOAT3& outMin, XMFLOAT3& outMax)
    {
        outMin = {
            box.Center.x - box.Extents.x,
            box.Center.y - box.Extents.y,
            box.Center.z - box.Extents.z
        };
        outMax = {
            box.Center.x + box.Extents.x,
            box.Center.y + box.Extents.y,
            box.Center.z + box.Extents.z
        };
    }

    bool AABBsOverlap(const XMFLOAT3& aMin, const XMFLOAT3& aMax,
        const XMFLOAT3& bMin, const XMFLOAT3& bMax,
        float& overlapX, float& overlapY, float& overlapZ)
    {
        if (aMax.x < bMin.x || aMin.x > bMax.x) return false;
        if (aMax.y < bMin.y || aMin.y > bMax.y) return false;
        if (aMax.z < bMin.z || aMin.z > bMax.z) return false;

        overlapX = std::min(aMax.x, bMax.x) - std::max(aMin.x, bMin.x);
        overlapY = std::min(aMax.y, bMax.y) - std::max(aMin.y, bMin.y);
        overlapZ = std::min(aMax.z, bMax.z) - std::max(aMin.z, bMin.z);
        return overlapX > 0.0f && overlapY > 0.0f && overlapZ > 0.0f;
    }

    void ApplyAxisMove(ColliderEntry& entry, int axis, float move)
    {
        if (move == 0.0f)
            return;

        if (axis == 0)
        {
            entry.transform->position.x += move;
            entry.bounds->worldBounds.Center.x += move;
            if (entry.gravity && !entry.collision->isStatic)
                entry.gravity->velocity.x = 0.0f;
        }
        else if (axis == 1)
        {
            entry.transform->position.y += move;
            entry.bounds->worldBounds.Center.y += move;
            if (entry.gravity && !entry.collision->isStatic)
                entry.gravity->velocity.y = 0.0f;
        }
        else
        {
            entry.transform->position.z += move;
            entry.bounds->worldBounds.Center.z += move;
            if (entry.gravity && !entry.collision->isStatic)
                entry.gravity->velocity.z = 0.0f;
        }
        entry.transform->MarkDirty();
    }

    float GetAxisCenter(const ColliderEntry& entry, int axis)
    {
        if (axis == 0) return entry.bounds->worldBounds.Center.x;
        if (axis == 1) return entry.bounds->worldBounds.Center.y;
        return entry.bounds->worldBounds.Center.z;
    }

    void ResolveOverlap(ColliderEntry& a, ColliderEntry& b, float overlapX, float overlapY, float overlapZ)
    {
        float penetration = overlapX;
        int axis = 0;
        if (overlapY < penetration) { penetration = overlapY; axis = 1; }
        if (overlapZ < penetration) { penetration = overlapZ; axis = 2; }

        const float aCenter = GetAxisCenter(a, axis);
        const float bCenter = GetAxisCenter(b, axis);
        const float sign = (aCenter >= bCenter) ? 1.0f : -1.0f;

        const bool aStatic = a.collision->isStatic;
        const bool bStatic = b.collision->isStatic;
        if (aStatic && bStatic)
            return;

        float moveA = 0.0f;
        float moveB = 0.0f;
        if (aStatic)
            moveB = -sign * penetration;
        else if (bStatic)
            moveA = sign * penetration;
        else
        {
            moveA = sign * penetration * 0.5f;
            moveB = -sign * penetration * 0.5f;
        }

        ApplyAxisMove(a, axis, moveA);
        ApplyAxisMove(b, axis, moveB);
    }

    void ResolveGroundPlane(ColliderEntry& entry)
    {
        XMFLOAT3 minPt, maxPt;
        GetAABBMinMax(entry.bounds->worldBounds, minPt, maxPt);
        if (minPt.y >= 0.0f)
            return;

        const float pushUp = -minPt.y;
        entry.transform->position.y += pushUp;
        entry.bounds->worldBounds.Center.y += pushUp;
        entry.transform->MarkDirty();

        if (entry.gravity && entry.gravity->velocity.y < 0.0f)
            entry.gravity->velocity.y = 0.0f;
    }

    bool SolveCollisions(std::vector<ColliderEntry>& colliders)
    {
        bool anyResolved = false;

        for (size_t i = 0; i < colliders.size(); ++i)
        {
            if (!colliders[i].collision->isStatic)
                ResolveGroundPlane(colliders[i]);
        }

        for (size_t i = 0; i < colliders.size(); ++i)
        {
            for (size_t j = i + 1; j < colliders.size(); ++j)
            {
                auto& a = colliders[i];
                auto& b = colliders[j];

                if (a.collision->isStatic && b.collision->isStatic)
                    continue;

                XMFLOAT3 aMin, aMax, bMin, bMax;
                GetAABBMinMax(a.bounds->worldBounds, aMin, aMax);
                GetAABBMinMax(b.bounds->worldBounds, bMin, bMax);

                float overlapX = 0.0f, overlapY = 0.0f, overlapZ = 0.0f;
                if (!AABBsOverlap(aMin, aMax, bMin, bMax, overlapX, overlapY, overlapZ))
                    continue;

                ResolveOverlap(a, b, overlapX, overlapY, overlapZ);
                anyResolved = true;
            }
        }

        return anyResolved;
    }
}

void CollisionSystem::Update(World& world)
{
    std::vector<ColliderEntry> colliders;
    colliders.reserve(64);

    world.ForEach<TransformComponent, BoundsComponent, CollisionComponent>(
        [&](Entity e, TransformComponent& tf, BoundsComponent& bounds, CollisionComponent& collision)
        {
            if (!collision.enabled)
                return;

            colliders.push_back({
                e,
                &tf,
                &bounds,
                &collision,
                world.GetComponent<GravityComponent>(e)
            });
        });

    if (colliders.size() <= 1)
        return;

    for (int iter = 0; iter < kMaxSolverIterations; ++iter)
    {
        if (!SolveCollisions(colliders))
            break;
    }
}