#include "BoundsSystem.h"
#include "ComponentStruct.h"
#include "Managers/MeshManager.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <algorithm>
#include <cfloat>

using namespace ECS;
using namespace DirectX;

void BoundsSystem::Update(World& world)
{
    world.ForEach<TransformComponent, RenderableComponent, BoundsComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend, BoundsComponent& bounds)
        {
            if (!rend.mesh || !rend.visible) return;

            XMMATRIX worldMat = tf.GetWorldMatrix();

            // Compute union of all submeshes' world AABBs (main object AABB)
            XMFLOAT3 objMin = { FLT_MAX, FLT_MAX, FLT_MAX };
            XMFLOAT3 objMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            bool hasAny = false;

            Mesh* pMesh = rend.mesh;
            for (auto it = pMesh->DrawArgs.begin(); it != pMesh->DrawArgs.end(); ++it)
            {
                const auto& subMesh = it->second;
                const auto& local = subMesh.Bounds;

                if (local.Extents.x <= 0 && local.Extents.y <= 0 && local.Extents.z <= 0)
                    continue; // invalid/empty

                // Generate 8 corners of local AABB
                XMFLOAT3 corners[8] = {
                    { local.Center.x - local.Extents.x, local.Center.y - local.Extents.y, local.Center.z - local.Extents.z },
                    { local.Center.x + local.Extents.x, local.Center.y - local.Extents.y, local.Center.z - local.Extents.z },
                    { local.Center.x - local.Extents.x, local.Center.y + local.Extents.y, local.Center.z - local.Extents.z },
                    { local.Center.x + local.Extents.x, local.Center.y + local.Extents.y, local.Center.z - local.Extents.z },
                    { local.Center.x - local.Extents.x, local.Center.y - local.Extents.y, local.Center.z + local.Extents.z },
                    { local.Center.x + local.Extents.x, local.Center.y - local.Extents.y, local.Center.z + local.Extents.z },
                    { local.Center.x - local.Extents.x, local.Center.y + local.Extents.y, local.Center.z + local.Extents.z },
                    { local.Center.x + local.Extents.x, local.Center.y + local.Extents.y, local.Center.z + local.Extents.z }
                };

                for (int i = 0; i < 8; ++i)
                {
                    XMVECTOR localCorner = XMLoadFloat3(&corners[i]);
                    XMVECTOR worldCorner = XMVector3TransformCoord(localCorner, worldMat);
                    XMFLOAT3 wc;
                    XMStoreFloat3(&wc, worldCorner);

                    objMin.x = (objMin.x < wc.x ? objMin.x : wc.x);
                    objMin.y = (objMin.y < wc.y ? objMin.y : wc.y);
                    objMin.z = (objMin.z < wc.z ? objMin.z : wc.z);

                    objMax.x = (objMax.x > wc.x ? objMax.x : wc.x);
                    objMax.y = (objMax.y > wc.y ? objMax.y : wc.y);
                    objMax.z = (objMax.z > wc.z ? objMax.z : wc.z);
                    hasAny = true;
                }
            }

            if (hasAny)
            {
                bounds.worldBounds.Center = {
                    (objMin.x + objMax.x) * 0.5f,
                    (objMin.y + objMax.y) * 0.5f,
                    (objMin.z + objMax.z) * 0.5f
                };
                bounds.worldBounds.Extents = {
                    (objMax.x - objMin.x) * 0.5f,
                    (objMax.y - objMin.y) * 0.5f,
                    (objMax.z - objMin.z) * 0.5f
                };
            }
            else
            {
                bounds.worldBounds = {};
            }
        });
}