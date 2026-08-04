#include "Engine.h"
#include "MeshManager.h"
#include "MaterialManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"
#include "ComponentStruct.h"
#include "Entity.h"
#include "RenderLimits.h"
#include "MathHelper.h"
#include "SceneSerializer.h"
#include "UiPresetSerializer.h"
#include <algorithm>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <climits>
#include <cmath>

using namespace DirectX;

Engine::Engine()
{}

Engine::~Engine()
{}

bool Engine::Initialize(ID3D12Device* device,
    std::vector<std::unique_ptr<FrameResource>>& frameResources,
    int gNumFrameResources,
    DescriptorAllocator& descriptorAllocator)
{
    mDevice = device;
    mFrameResources = &frameResources;
    mGNumFrameResources = gNumFrameResources;
    mDescriptorAllocator = &descriptorAllocator;

    mResourceManager = &ResourceManager::Get();
    mResourceManager->Initialize();

    mMaxObjectCBSlots = kMaxSceneObjects;

    ShaderManager::Get().Initialize();
    RootSignatureManager::Get().Initialize(device);
    PipelineStateManager::Get().Initialize(device);
    mRenderSystem.Initialize(device);
    mUiSystem.Initialize(device);
    mUiSystem.SetSampleCount(4);

    return true;
}

void Engine::Update(float deltaTime)
{
    mRenderSystem.SetFrameDeltaTime(deltaTime);
    // F2: GPU motion ON → CPU도 동일 적분(충돌/선택) + suppressGpuUpload
    //     GPU는 별도 버퍼에서 적분 (TRS 업로드로 덮지 않음)
    mGravitySystem.Update(mWorld, deltaTime, mRenderSystem.ShouldSkipCpuGravity());
    UpdateBounds();
    // 충돌이 움직이면 full MarkDirty → GPU TRS/motion 재시드
    if (mCollisionSystem.Update(mWorld))
        UpdateBounds();

    std::vector<Entity> toKill;
    mWorld.ForEach<EffectBillboardComponent>(
        [&](Entity e, EffectBillboardComponent& fx)
        {
            if (fx.lifetime < 0.f)
                return;
            fx.age += deltaTime;
            if (fx.age >= fx.lifetime)
                toKill.push_back(e);
        });
    for (Entity e : toKill)
    {
        mWorld.DestroyEntity(e);
        mUiListDirty = true;
    }
}

void Engine::UpdateBounds()
{
    mBoundsSystem.Update(mWorld);
}

Entity Engine::GetSelectedEntity()
{
    Entity selected = INVALID_ENTITY;
    mWorld.ForEach<SelectedComponent>(
        [&](Entity e, SelectedComponent&)
        {
            if (selected == INVALID_ENTITY)
                selected = e;
        });
    return selected;
}

std::vector<Entity> Engine::GetSelectedEntities()
{
    std::vector<Entity> out;
    mWorld.ForEach<SelectedComponent>(
        [&](Entity e, SelectedComponent&)
        {
            out.push_back(e);
        });
    std::sort(out.begin(), out.end());
    return out;
}

size_t Engine::GetSelectedCount()
{
    size_t n = 0;
    mWorld.ForEach<SelectedComponent>(
        [&](Entity, SelectedComponent&)
        {
            ++n;
        });
    return n;
}

void Engine::ClearSelection()
{
    std::vector<Entity> selectedEntities;
    selectedEntities.reserve(8);

    mWorld.ForEach<SelectedComponent>(
        [&](Entity e, SelectedComponent&)
        {
            selectedEntities.push_back(e);
        });

    for (Entity e : selectedEntities)
        mWorld.RemoveComponent<SelectedComponent>(e);
}

// 3D mesh objects need Transform; screen UI objects have UiElement only (no Transform).
// WorldBillboard UI may have both. Without this, Hierarchy/Scene UI pick never sticks.
static bool CanSelectEntity(ECS::World& world, Entity entity)
{
    if (entity == INVALID_ENTITY)
        return false;
    return world.GetComponent<TransformComponent>(entity) != nullptr
        || world.GetComponent<UiElementComponent>(entity) != nullptr;
}

void Engine::SetSelectedEntity(Entity entity)
{
    ClearSelection();

    if (!CanSelectEntity(mWorld, entity))
        return;

    if (!mWorld.GetComponent<SelectedComponent>(entity))
        mWorld.AddComponent(entity, SelectedComponent{});
}

void Engine::AddSelectedEntity(Entity entity)
{
    if (!CanSelectEntity(mWorld, entity))
        return;
    if (mWorld.GetComponent<SelectedComponent>(entity))
        return;
    mWorld.AddComponent(entity, SelectedComponent{});
}

void Engine::ToggleSelectedEntity(Entity entity)
{
    if (!CanSelectEntity(mWorld, entity))
        return;

    if (mWorld.GetComponent<SelectedComponent>(entity))
        mWorld.RemoveComponent<SelectedComponent>(entity);
    else
        mWorld.AddComponent(entity, SelectedComponent{});
}

bool Engine::IsEntitySelected(Entity entity)
{
    return mWorld.GetComponent<SelectedComponent>(entity) != nullptr;
}

Entity Engine::PickObject(int mouseX, int mouseY, float clientWidth, float clientHeight,
                          const XMMATRIX& view, const XMMATRIX& proj,
                          bool setSelection, bool additive)
{
    // ensure bounds fresh
    mBoundsSystem.Update(mWorld);

    // Generate ray from mouse
    float vx = (2.0f * mouseX / clientWidth) - 1.0f;
    float vy = 1.0f - (2.0f * mouseY / clientHeight);

    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);

    XMVECTOR clipNear = XMVectorSet(vx, vy, 0.0f, 1.0f);
    XMVECTOR clipFar  = XMVectorSet(vx, vy, 1.0f, 1.0f);

    XMVECTOR nearPoint = XMVector3TransformCoord(clipNear, invProj);
    nearPoint = XMVector3TransformCoord(nearPoint, invView);

    XMVECTOR farPoint = XMVector3TransformCoord(clipFar, invProj);
    farPoint = XMVector3TransformCoord(farPoint, invView);

    XMVECTOR rayOrigin = nearPoint;
    XMVECTOR rayDir = XMVector3Normalize(farPoint - nearPoint);

    Entity closest = INVALID_ENTITY;
    float minDist = FLT_MAX;

    mWorld.ForEach<TransformComponent, RenderableComponent, BoundsComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend, BoundsComponent& bnds)
        {
            (void)tf;
            if (!rend.visible || !rend.mesh) return;

            const auto& wb = bnds.worldBounds;
            if (wb.Extents.x <= 0.0f && wb.Extents.y <= 0.0f && wb.Extents.z <= 0.0f)
                return;

            float dist = 0.0f;
            bool hit = wb.Intersects(rayOrigin, rayDir, dist);
            if (!hit && wb.Contains(rayOrigin) != DISJOINT)
            {
                hit = true;
                dist = 0.0f;
            }

            if (hit && dist >= 0.0f && dist < minDist)
            {
                minDist = dist;
                closest = e;
            }
        });

    if (setSelection)
    {
        if (closest == INVALID_ENTITY)
        {
            if (!additive)
                ClearSelection();
        }
        else if (additive)
            ToggleSelectedEntity(closest);
        else
            SetSelectedEntity(closest);
    }
    return closest;
}

size_t Engine::SelectObjectsInRect(
    float rectMinX, float rectMinY, float rectMaxX, float rectMaxY,
    float sceneWidth, float sceneHeight,
    const XMMATRIX& view, const XMMATRIX& proj,
    bool additive)
{
    if (sceneWidth < 1.f || sceneHeight < 1.f)
        return 0;

    // normalize rect
    if (rectMinX > rectMaxX) std::swap(rectMinX, rectMaxX);
    if (rectMinY > rectMaxY) std::swap(rectMinY, rectMaxY);

    mBoundsSystem.Update(mWorld);

    const XMMATRIX viewProj = view * proj;

    if (!additive)
        ClearSelection();

    size_t count = 0;
    mWorld.ForEach<TransformComponent, RenderableComponent, BoundsComponent>(
        [&](Entity e, TransformComponent&, RenderableComponent& rend, BoundsComponent& bnds)
        {
            if (!rend.visible || !rend.mesh) return;

            const auto& wb = bnds.worldBounds;
            if (wb.Extents.x <= 0.0f && wb.Extents.y <= 0.0f && wb.Extents.z <= 0.0f)
                return;

            // Project AABB 8 corners → scene pixel AABB
            float minSX = 1e30f, minSY = 1e30f;
            float maxSX = -1e30f, maxSY = -1e30f;
            int valid = 0;

            for (int i = 0; i < 8; ++i)
            {
                const float sx = (i & 1) ? wb.Extents.x : -wb.Extents.x;
                const float sy = (i & 2) ? wb.Extents.y : -wb.Extents.y;
                const float sz = (i & 4) ? wb.Extents.z : -wb.Extents.z;
                XMVECTOR corner = XMVectorSet(
                    wb.Center.x + sx,
                    wb.Center.y + sy,
                    wb.Center.z + sz,
                    1.0f);

                XMVECTOR clip = XMVector4Transform(corner, viewProj);
                float w = XMVectorGetW(clip);
                if (w <= 1e-5f)
                    continue;

                float ndcX = XMVectorGetX(clip) / w;
                float ndcY = XMVectorGetY(clip) / w;
                // clip space roughly in front
                float ndcZ = XMVectorGetZ(clip) / w;
                if (ndcZ < 0.0f || ndcZ > 1.0f)
                    continue;

                float px = (ndcX * 0.5f + 0.5f) * sceneWidth;
                float py = (1.0f - (ndcY * 0.5f + 0.5f)) * sceneHeight;

                minSX = (std::min)(minSX, px);
                minSY = (std::min)(minSY, py);
                maxSX = (std::max)(maxSX, px);
                maxSY = (std::max)(maxSY, py);
                ++valid;
            }

            if (valid == 0)
                return;

            // 2D AABB intersection (inclusive)
            const bool overlap =
                !(maxSX < rectMinX || minSX > rectMaxX || maxSY < rectMinY || minSY > rectMaxY);
            if (!overlap)
                return;

            if (!IsEntitySelected(e))
                AddSelectedEntity(e);
            ++count;
        });

    // Screen UI objects (no Transform) — rect pick in same scene-pixel space as layout.
    float gw = 0.f, gh = 0.f;
    mUiSystem.GetGlobalDesignResolution(gw, gh);
    mWorld.ForEach<UiElementComponent, UiImageComponent>(
        [&](Entity e, UiElementComponent& el, UiImageComponent&)
        {
            if (!UiElementShouldDraw(el) || el.mode == UiSpaceMode::WorldBillboard)
                return;
            if (IsEntitySelected(e))
                return;

            DirectX::XMFLOAT2 posPx{}, sizePx{};
            UiSystem::ResolveScreenLayout(
                el, sceneWidth, sceneHeight, el.scaleMode, gw, gh, posPx, sizePx);

            const float ax = el.anchor.x * sceneWidth + posPx.x;
            const float ay = el.anchor.y * sceneHeight + posPx.y;
            const float l = ax - el.pivot.x * sizePx.x;
            const float t = ay - el.pivot.y * sizePx.y;
            const float r = l + sizePx.x;
            const float b = t + sizePx.y;

            const bool overlap =
                !(r < rectMinX || l > rectMaxX || b < rectMinY || t > rectMaxY);
            if (!overlap)
                return;

            AddSelectedEntity(e);
            ++count;
        });

    return count;
}

void Engine::RotateSelected(float dYaw, float dPitch)
{
    const Entity selected = GetSelectedEntity();
    if (selected == INVALID_ENTITY) return;
    auto* tf = mWorld.GetComponent<TransformComponent>(selected);
    if (tf) {
        tf->rotation.y += dYaw;
        tf->rotation.x += dPitch;
        tf->rotation.x = MathHelper::Clamp(tf->rotation.x, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
        tf->MarkDirty(selected);
    }
}

namespace
{
    XMVECTOR FlattenToXZ(XMVECTOR v, XMVECTOR fallback)
    {
        XMFLOAT3 f{};
        XMStoreFloat3(&f, v);
        f.y = 0.0f;
        XMVECTOR flat = XMLoadFloat3(&f);
        const float lenSq = XMVectorGetX(XMVector3LengthSq(flat));
        if (lenSq < 1e-6f)
            return fallback;
        return XMVector3Normalize(flat);
    }
}

void Engine::MoveSelectedViewRelative(float forward, float right, float up, float speed, const XMMATRIX& view)
{
    const Entity selected = GetSelectedEntity();
    if (selected == INVALID_ENTITY) return;
    auto* tf = mWorld.GetComponent<TransformComponent>(selected);
    if (!tf) return;

    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    const XMVECTOR cFwd = FlattenToXZ(-invView.r[2], XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
    const XMVECTOR cRight = FlattenToXZ(invView.r[0], XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR delta = XMVectorAdd(
        XMVectorScale(cFwd, forward * speed),
        XMVectorScale(cRight, right * speed)
    );
    delta = XMVectorAdd(delta, XMVectorScale(worldUp, up * speed));

    XMFLOAT3 d;
    XMStoreFloat3(&d, delta);
    tf->position.x += d.x;
    tf->position.y += d.y;
    tf->position.z += d.z;
    tf->MarkDirty(selected);
}

void Engine::MoveSelectedPlanar(float forward, float right, float up, float speed,
    const XMFLOAT3& horizForward, const XMFLOAT3& horizRight)
{
    const Entity selected = GetSelectedEntity();
    if (selected == INVALID_ENTITY)
        return;

    auto* tf = mWorld.GetComponent<TransformComponent>(selected);
    if (!tf)
        return;

    tf->position.x += horizForward.x * forward * speed + horizRight.x * right * speed;
    tf->position.y += up * speed;
    tf->position.z += horizForward.z * forward * speed + horizRight.z * right * speed;
    tf->MarkDirty(selected);
}

void Engine::MoveSelectedUi(float dPosX, float dPosY)
{
    if (dPosX == 0.f && dPosY == 0.f)
        return;

    for (Entity e : GetSelectedEntities())
    {
        UiElementComponent* el = mWorld.GetComponent<UiElementComponent>(e);
        if (!el || el->mode == UiSpaceMode::WorldBillboard)
            continue;
        el->position.x += dPosX;
        el->position.y += dPosY;
        // Free drag breaks edge lock (re-snap on drop may set again).
        el->snapLeft = el->snapRight = el->snapTop = el->snapBottom = false;
    }
}

void Engine::SnapSelectedMesh(float threshold)
{
    if (threshold <= 0.f)
        return;

    const Entity selected = GetSelectedEntity();
    if (selected == INVALID_ENTITY)
        return;
    if (!GetRenderable(selected))
        return;
    TransformComponent* tf = mWorld.GetComponent<TransformComponent>(selected);
    BoundsComponent* sb = mWorld.GetComponent<BoundsComponent>(selected);
    if (!tf || !sb)
        return;

    // Fresh world AABB (union of submeshes)
    mBoundsSystem.Update(mWorld);
    sb = mWorld.GetComponent<BoundsComponent>(selected);
    if (!sb)
        return;

    const BoundingBox& sBox = sb->worldBounds;
    if (sBox.Extents.x <= 0.f && sBox.Extents.y <= 0.f && sBox.Extents.z <= 0.f)
        return;

    const float sMin[3] = {
        sBox.Center.x - sBox.Extents.x,
        sBox.Center.y - sBox.Extents.y,
        sBox.Center.z - sBox.Extents.z
    };
    const float sMax[3] = {
        sBox.Center.x + sBox.Extents.x,
        sBox.Center.y + sBox.Extents.y,
        sBox.Center.z + sBox.Extents.z
    };
    const float sCtr[3] = { sBox.Center.x, sBox.Center.y, sBox.Center.z };

    float bestDelta[3] = { 0.f, 0.f, 0.f };
    float bestAbs[3] = { threshold + 1.f, threshold + 1.f, threshold + 1.f };
    bool any = false;

    auto consider = [&](int axis, float delta)
    {
        const float ad = fabsf(delta);
        if (ad <= threshold && ad < bestAbs[axis])
        {
            bestAbs[axis] = ad;
            bestDelta[axis] = delta;
            any = true;
        }
    };

    mWorld.ForEach<TransformComponent, RenderableComponent, BoundsComponent>(
        [&](Entity e, TransformComponent&, RenderableComponent& rend, BoundsComponent& bnds)
        {
            if (e == selected || !rend.visible || !rend.mesh)
                return;
            const BoundingBox& o = bnds.worldBounds;
            if (o.Extents.x <= 0.f && o.Extents.y <= 0.f && o.Extents.z <= 0.f)
                return;

            const float oMin[3] = {
                o.Center.x - o.Extents.x,
                o.Center.y - o.Extents.y,
                o.Center.z - o.Extents.z
            };
            const float oMax[3] = {
                o.Center.x + o.Extents.x,
                o.Center.y + o.Extents.y,
                o.Center.z + o.Extents.z
            };
            const float oCtr[3] = { o.Center.x, o.Center.y, o.Center.z };

            for (int a = 0; a < 3; ++a)
            {
                // Face-to-face (adjacent / touch)
                consider(a, oMin[a] - sMax[a]); // selected max → other min
                consider(a, oMax[a] - sMin[a]); // selected min → other max
                // Coplanar faces (same side align)
                consider(a, oMin[a] - sMin[a]);
                consider(a, oMax[a] - sMax[a]);
                // Centers on axis
                consider(a, oCtr[a] - sCtr[a]);
            }
        });

    if (!any)
        return;

    // Only apply axes that actually found a snap within threshold
    XMFLOAT3 delta{ 0, 0, 0 };
    if (bestAbs[0] <= threshold) delta.x = bestDelta[0];
    if (bestAbs[1] <= threshold) delta.y = bestDelta[1];
    if (bestAbs[2] <= threshold) delta.z = bestDelta[2];
    if (delta.x == 0.f && delta.y == 0.f && delta.z == 0.f)
        return;

    tf->position.x += delta.x;
    tf->position.y += delta.y;
    tf->position.z += delta.z;
    tf->MarkDirty(selected);
    mBoundsSystem.Update(mWorld);
}

bool Engine::SnapSelectedUi(float thresholdPercent, float canvasW, float canvasH)
{
    // Editor canvas only: snap widget edges to window left / right / top / bottom.
    // thresholdPercent is % of canvas width (X) / height (Y).
    if (thresholdPercent <= 0.f || canvasW < 1.f || canvasH < 1.f)
        return false;

    const float thresholdPxX = (thresholdPercent * 0.01f) * canvasW;
    const float thresholdPxY = (thresholdPercent * 0.01f) * canvasH;

    float gw = 0.f, gh = 0.f;
    mUiSystem.GetGlobalDesignResolution(gw, gh);

    struct UiRect
    {
        Entity e = INVALID_ENTITY;
        UiElementComponent* el = nullptr;
        float l = 0, t = 0, r = 0, b = 0;
    };

    std::vector<UiRect> rects;
    rects.reserve(16);
    mWorld.ForEach<UiElementComponent, UiImageComponent>(
        [&](Entity e, UiElementComponent& el, UiImageComponent&)
        {
            if (!UiElementShouldDraw(el) || el.mode == UiSpaceMode::WorldBillboard)
                return;
            XMFLOAT2 posPx{}, sizePx{};
            UiSystem::ResolveScreenLayout(
                el, canvasW, canvasH, el.scaleMode, gw, gh, posPx, sizePx);
            const float ax = el.anchor.x * canvasW + posPx.x;
            const float ay = el.anchor.y * canvasH + posPx.y;
            UiRect ur;
            ur.e = e;
            ur.el = &el;
            ur.l = ax - el.pivot.x * sizePx.x;
            ur.t = ay - el.pivot.y * sizePx.y;
            ur.r = ur.l + sizePx.x;
            ur.b = ur.t + sizePx.y;
            rects.push_back(ur);
        });

    bool anySnapped = false;

    auto snapOne = [&](UiRect& self) -> bool
    {
        float bestDx = 0.f, bestDy = 0.f;
        float bestAx = thresholdPxX + 1.f, bestAy = thresholdPxY + 1.f;
        bool bestIsLeft = false, bestIsTop = false;

        auto considerX = [&](float edge, float guide, bool isLeft)
        {
            const float d = guide - edge;
            const float ad = fabsf(d);
            if (ad <= thresholdPxX && ad < bestAx)
            {
                bestAx = ad;
                bestDx = d;
                bestIsLeft = isLeft;
            }
        };
        auto considerY = [&](float edge, float guide, bool isTop)
        {
            const float d = guide - edge;
            const float ad = fabsf(d);
            if (ad <= thresholdPxY && ad < bestAy)
            {
                bestAy = ad;
                bestDy = d;
                bestIsTop = isTop;
            }
        };

        // Left / right of canvas
        considerX(self.l, 0.f, true);
        considerX(self.r, canvasW, false);
        // Top / bottom of canvas
        considerY(self.t, 0.f, true);
        considerY(self.b, canvasH, false);

        const bool xOk = bestAx <= thresholdPxX;
        const bool yOk = bestAy <= thresholdPxY;
        if (!xOk && !yOk)
            return false;

        // Priority: single edge (L/R/T/B line) > corner (two edges at once).
        constexpr float kCornerPriorityScale = 0.5f;
        bool applyX = false;
        bool applyY = false;

        if (xOk && yOk)
        {
            if (bestAx <= bestAy)
            {
                applyX = true;
                applyY = (bestAy <= thresholdPxY * kCornerPriorityScale);
            }
            else
            {
                applyY = true;
                applyX = (bestAx <= thresholdPxX * kCornerPriorityScale);
            }
        }
        else if (xOk)
        {
            applyX = true;
        }
        else
        {
            applyY = true;
        }

        if (!applyX && !applyY)
            return false;

        if (self.el->layoutPercent)
        {
            if (applyX)
                self.el->position.x += bestDx / canvasW;
            if (applyY)
                self.el->position.y += bestDy / canvasH;
        }
        else
        {
            if (applyX)
                self.el->position.x += bestDx;
            if (applyY)
                self.el->position.y += bestDy;
        }

        // Persist edge locks for Keep Aspect Fit resize maintenance.
        self.el->snapLeft = self.el->snapRight = self.el->snapTop = self.el->snapBottom = false;
        if (applyX)
        {
            self.el->snapLeft = bestIsLeft;
            self.el->snapRight = !bestIsLeft;
        }
        if (applyY)
        {
            self.el->snapTop = bestIsTop;
            self.el->snapBottom = !bestIsTop;
        }

        // Immediately re-lock in Keep Aspect Fit so size/pos stay consistent this frame.
        if (self.el->scaleMode == UiScaleMode::UniformMin)
        {
            UiSystem::MaintainKeepAspectFitEdgeLocks(
                *self.el, canvasW, canvasH, gw, gh);
        }
        return true;
    };

    for (Entity e : GetSelectedEntities())
    {
        for (UiRect& ur : rects)
        {
            if (ur.e == e)
            {
                if (snapOne(ur))
                    anySnapped = true;
                break;
            }
        }
    }
    return anySnapped;
}

void Engine::Render(ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    mRenderSystem.render(mWorld, cmdList, currentFrameResource,
        mDescriptorAllocator, currentFrameIndex, viewMatrix, projMatrix);
}

void Engine::RenderUi(
    ID3D12GraphicsCommandList* cmdList,
    UINT screenWidth, UINT screenHeight,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    XMFLOAT3 eye{};
    mWorld.ForEach<TransformComponent, CameraComponent>(
        [&](Entity, TransformComponent& tf, CameraComponent& cam)
        {
            if (cam.isMainCamera)
                eye = tf.position;
        });
    mUiSystem.Render(mWorld, cmdList, mDescriptorAllocator,
        screenWidth, screenHeight, viewMatrix, projMatrix, eye);
}

Entity Engine::CreateUiImage(
    UiSpaceMode mode,
    const std::string& materialName,
    XMFLOAT2 size,
    XMFLOAT3 worldOrScreenPos,
    XMFLOAT2 anchor)
{
    if (!mResourceManager || !mResourceManager->GetMaterial(materialName))
    {
        OutputDebugStringA("[Engine] CreateUiImage: material not found\n");
        return INVALID_ENTITY;
    }

    Entity e = mWorld.CreateEntity();
    UiElementComponent el{};
    el.mode = mode;
    el.scaleMode = mUiSystem.GetScaleMode(); // inherit editor default for new UI
    el.active = true;
    el.visible = true;
    el.size = size;
    el.anchor = anchor;
    el.pivot = { 0.5f, 0.5f };

    UiImageComponent img{};
    img.materialName = materialName;

    if (mode == UiSpaceMode::WorldBillboard)
    {
        TransformComponent tf{};
        tf.position = worldOrScreenPos;
        tf.MarkDirty(e);
        mWorld.AddComponent(e, std::move(tf));
    }
    else
    {
        el.position = { worldOrScreenPos.x, worldOrScreenPos.y };
    }

    mWorld.AddComponent(e, std::move(el));
    mWorld.AddComponent(e, std::move(img));
    mUiListDirty = true;
    return e;
}

Entity Engine::CreateUiImageScreenRect(
    const std::string& materialName,
    float minX, float minY, float maxX, float maxY,
    float designW, float designH)
{
    if (maxX < minX)
        std::swap(minX, maxX);
    if (maxY < minY)
        std::swap(minY, maxY);

    float w = maxX - minX;
    float h = maxY - minY;
    constexpr float kMin = 8.0f;
    if (w < kMin)
    {
        const float c = 0.5f * (minX + maxX);
        minX = c - 0.5f * kMin;
        maxX = c + 0.5f * kMin;
        w = kMin;
    }
    if (h < kMin)
    {
        const float c = 0.5f * (minY + maxY);
        minY = c - 0.5f * kMin;
        maxY = c + 0.5f * kMin;
        h = kMin;
    }

    std::string mat = materialName;
    if (mat.empty() || !mResourceManager || !mResourceManager->GetMaterial(mat))
    {
        // Fall back to Default if selection is empty / missing
        mat = "Default";
        if (!mResourceManager || !mResourceManager->GetMaterial(mat))
        {
            OutputDebugStringA("[Engine] CreateUiImageScreenRect: no usable material\n");
            return INVALID_ENTITY;
        }
    }

    float dw = designW;
    float dh = designH;
    if (dw < 1.0f || dh < 1.0f)
        mUiSystem.GetGlobalDesignResolution(dw, dh);
    // Authoring canvas (Scene panel / window) for percent conversion
    if (dw < 1.0f) dw = (std::max)(w, 1.0f);
    if (dh < 1.0f) dh = (std::max)(h, 1.0f);

    // Center of rect as % of canvas; size as % of canvas. Pivot = center.
    const float centerX = (minX + maxX) * 0.5f;
    const float centerY = (minY + maxY) * 0.5f;
    const float centerPctX = centerX / dw;
    const float centerPctY = centerY / dh;
    const float sizePctX = w / dw;
    const float sizePctY = h / dh;

    Entity e = mWorld.CreateEntity();
    UiElementComponent el{};
    el.mode = UiSpaceMode::ScreenAlways;
    el.scaleMode = mUiSystem.GetScaleMode(); // inherit Canvas & Scale default
    el.active = true;
    el.visible = true;
    el.anchor = { 0.0f, 0.0f }; // canvas top-left origin
    el.pivot = { 0.5f, 0.5f };  // position is the CENTER of the widget
    el.layoutPercent = true;
    el.position = { centerPctX, centerPctY };
    el.size = { sizePctX, sizePctY };
    el.zOrder = 0;
    el.designW = dw;
    el.designH = dh;

    UiImageComponent img{};
    img.materialName = mat;

    mWorld.AddComponent(e, std::move(el));
    mWorld.AddComponent(e, std::move(img));
    mUiListDirty = true;

    char buf[256];
    sprintf_s(buf,
        "[UI] CreateUiImageScreenRect mat=%s rectPx=(%.0f,%.0f)-(%.0f,%.0f) "
        "center%%=(%.4f,%.4f) size%%=(%.4f,%.4f) canvas=%.0fx%.0f e=%u\n",
        mat.c_str(), minX, minY, maxX, maxY,
        centerPctX, centerPctY, sizePctX, sizePctY,
        dw, dh, static_cast<unsigned>(e));
    OutputDebugStringA(buf);
    return e;
}

void Engine::SetUiScaleMode(UiScaleMode mode)
{
    // Default for newly created UI only — existing widgets keep el.scaleMode.
    mUiSystem.SetScaleMode(mode);
}

UiScaleMode Engine::GetUiScaleMode() const
{
    return mUiSystem.GetScaleMode();
}

void Engine::SetUiDesignResolution(float width, float height)
{
    mUiSystem.SetGlobalDesignResolution(width, height);
}

void Engine::GetUiDesignResolution(float& outW, float& outH) const
{
    mUiSystem.GetGlobalDesignResolution(outW, outH);
}

void Engine::DestroyUiEntity(Entity e)
{
    if (e == INVALID_ENTITY)
        return;
    mWorld.DestroyEntity(e);
    mUiListDirty = true;
}

void Engine::SetUiActive(Entity e, bool active)
{
    if (auto* el = mWorld.GetComponent<UiElementComponent>(e))
        el->active = active;
}

void Engine::SetUiVisible(Entity e, bool visible)
{
    if (auto* el = mWorld.GetComponent<UiElementComponent>(e))
        el->visible = visible;
}

UiElementComponent* Engine::GetUiElement(Entity e)
{
    return mWorld.GetComponent<UiElementComponent>(e);
}

UiImageComponent* Engine::GetUiImage(Entity e)
{
    return mWorld.GetComponent<UiImageComponent>(e);
}

void Engine::RebuildUiListCacheIfNeeded()
{
    if (!mUiListDirty)
        return;
    mUiListCache.clear();
    mWorld.ForEach<UiElementComponent>(
        [&](Entity e, UiElementComponent&)
        {
            mUiListCache.push_back(e);
        });
    std::sort(mUiListCache.begin(), mUiListCache.end());
    mUiListDirty = false;
}

const std::vector<Entity>& Engine::GetUiEntities()
{
    RebuildUiListCacheIfNeeded();
    return mUiListCache;
}

void Engine::DestroyAllUiEntities()
{
    DestroyAllUiPresetInstances();

    std::vector<Entity> toDestroy;
    mWorld.ForEach<UiElementComponent>(
        [&](Entity e, UiElementComponent&)
        {
            toDestroy.push_back(e);
        });
    for (Entity e : toDestroy)
        mWorld.DestroyEntity(e);
    mUiListDirty = true;
}

Entity Engine::PickUiScreen(float pixelX, float pixelY, UINT screenW, UINT screenH)
{
    if (screenW == 0 || screenH == 0)
        return INVALID_ENTITY;

    const float sw = static_cast<float>(screenW);
    const float sh = static_cast<float>(screenH);
    float gw = 0.f, gh = 0.f;
    mUiSystem.GetGlobalDesignResolution(gw, gh);

    mWorld.ForEach<UiElementComponent>(
        [&](Entity, UiElementComponent& el)
        {
            UiSystem::MaintainKeepAspectFitEdgeLocks(el, sw, sh, gw, gh);
        });

    Entity best = INVALID_ENTITY;
    int bestZ = INT_MIN;

    mWorld.ForEach<UiElementComponent, UiImageComponent>(
        [&](Entity e, UiElementComponent& el, UiImageComponent&)
        {
            // Editor pick: same draw gate as render; screen UI only.
            if (!UiElementShouldDraw(el) || el.mode == UiSpaceMode::WorldBillboard)
                return;

            DirectX::XMFLOAT2 posPx{}, sizePx{};
            UiSystem::ResolveScreenLayout(
                el, sw, sh, el.scaleMode, gw, gh, posPx, sizePx);

            const float ax = el.anchor.x * sw + posPx.x;
            const float ay = el.anchor.y * sh + posPx.y;
            const float l = ax - el.pivot.x * sizePx.x;
            const float t = ay - el.pivot.y * sizePx.y;
            const float r = l + sizePx.x;
            const float b = t + sizePx.y;
            if (pixelX >= l && pixelX <= r && pixelY >= t && pixelY <= b)
            {
                if (el.zOrder >= bestZ)
                {
                    bestZ = el.zOrder;
                    best = e;
                }
            }
        });

    return best;
}

bool Engine::DeleteUiPreset(const std::string& name, std::string* outError)
{
    if (name.empty())
    {
        if (outError) *outError = "Empty preset name";
        return false;
    }

    RemoveSceneUiPreset(name);
    UnregisterUiPreset(name);

    if (!UiPresetSerializer::DeletePresetFile(name))
    {
        // Still success if only registry/scene entry existed
        if (outError) *outError = "Preset file missing or already deleted (registry cleared)";
        OutputDebugStringA(("[UI] DeleteUiPreset file missing: " + name + "\n").c_str());
        return true;
    }

    OutputDebugStringA(("[UI] DeleteUiPreset ok: " + name + "\n").c_str());
    return true;
}

UiPresetData Engine::CaptureCurrentUiAsPreset(const std::string& presetName)
{
    UiPresetData data;
    data.version = UiPresetData::kCurrentVersion;
    data.name = presetName;
    mUiSystem.GetGlobalDesignResolution(data.designW, data.designH);

    mWorld.ForEach<UiElementComponent, UiImageComponent>(
        [&](Entity e, UiElementComponent& el, UiImageComponent& img)
        {
            UiPresetElementData d;
            d.mode = el.mode;
            d.scaleMode = el.scaleMode;
            d.active = el.active;
            d.visible = el.visible;
            d.zOrder = el.zOrder;
            d.anchor = el.anchor;
            d.pivot = el.pivot;
            d.position = el.position;
            d.size = el.size;
            d.rotationRad = el.rotationRad;
            d.layoutPercent = el.layoutPercent;
            d.designW = el.designW;
            d.designH = el.designH;
            d.snapLeft = el.snapLeft;
            d.snapRight = el.snapRight;
            d.snapTop = el.snapTop;
            d.snapBottom = el.snapBottom;
            // Normalize legacy pixel (top-left) → center percent when design canvas known
            if (!d.layoutPercent && d.designW > 1.0f && d.designH > 1.0f
                && el.mode != UiSpaceMode::WorldBillboard)
            {
                const float cx = el.position.x + el.size.x * el.pivot.x;
                const float cy = el.position.y + el.size.y * el.pivot.y;
                d.layoutPercent = true;
                d.position = { cx / d.designW, cy / d.designH };
                d.size = { el.size.x / d.designW, el.size.y / d.designH };
                d.pivot = { 0.5f, 0.5f };
                d.anchor = { 0.0f, 0.0f };
            }
            // Older percent used top-left pivot — convert to center percent
            else if (d.layoutPercent && el.mode != UiSpaceMode::WorldBillboard
                && (el.pivot.x < 0.25f || el.pivot.y < 0.25f))
            {
                d.position.x = el.position.x + el.size.x * 0.5f;
                d.position.y = el.position.y + el.size.y * 0.5f;
                d.pivot = { 0.5f, 0.5f };
                d.anchor = { 0.0f, 0.0f };
            }
            d.materialName = img.materialName;
            d.color = img.color;
            d.uvRect = img.uvRect;

            if (el.mode == UiSpaceMode::WorldBillboard)
            {
                if (TransformComponent* tf = mWorld.GetComponent<TransformComponent>(e))
                    d.worldPos = tf->position;
            }

            if (UiButtonComponent* btn = mWorld.GetComponent<UiButtonComponent>(e))
            {
                d.hasButton = true;
                d.actionId = btn->actionId;
                d.interactable = btn->interactable;
            }

            // Prefer a shared design res from elements if global was empty
            if (data.designW < 1.0f && el.designW > 1.0f)
                data.designW = el.designW;
            if (data.designH < 1.0f && el.designH > 1.0f)
                data.designH = el.designH;

            data.elements.push_back(std::move(d));
        });

    return data;
}

bool Engine::SaveCurrentUiAsPreset(const std::string& presetName, std::string* outError)
{
    if (presetName.empty())
    {
        if (outError) *outError = "Preset name is empty";
        return false;
    }

    UiPresetData data = CaptureCurrentUiAsPreset(presetName);
    if (data.elements.empty())
    {
        if (outError) *outError = "No UI elements to save (create UI first)";
        return false;
    }

    const std::string path = UiPresetSerializer::ResolvePresetPath(presetName);
    if (!UiPresetSerializer::SaveToFile(path, data))
    {
        if (outError) *outError = "Failed to write: " + path;
        return false;
    }

    RegisterUiPreset(presetName, data);

    char buf[256];
    sprintf_s(buf, "[UI] saved preset '%s' (%zu elements) -> %s\n",
        presetName.c_str(), data.elements.size(), path.c_str());
    OutputDebugStringA(buf);
    return true;
}

bool Engine::RegisterUiPreset(const std::string& name, const UiPresetData& data)
{
    if (name.empty())
        return false;
    UiPresetData copy = data;
    copy.name = name;
    mUiPresetRegistry[name] = std::move(copy);
    return true;
}

bool Engine::RegisterUiPresetFromFile(const std::string& nameOrPath, std::string* outError)
{
    const std::string path = UiPresetSerializer::ResolvePresetPath(nameOrPath);
    UiPresetData data;
    if (!UiPresetSerializer::LoadFromFile(path, data, outError))
        return false;

    // Registry key = caller name (stem), not whatever is inside the file.
    std::string key = nameOrPath;
    try
    {
        std::filesystem::path p(nameOrPath);
        if (p.has_filename())
            key = p.stem().string();
    }
    catch (...) {}
    if (key.empty())
        key = data.name;
    if (data.name.empty())
        data.name = key;

    return RegisterUiPreset(key, data);
}

void Engine::UnregisterUiPreset(const std::string& name)
{
    mUiPresetRegistry.erase(name);
}

void Engine::ClearUiPresetRegistry()
{
    mUiPresetRegistry.clear();
}

bool Engine::HasUiPreset(const std::string& name) const
{
    return mUiPresetRegistry.find(name) != mUiPresetRegistry.end();
}

std::vector<std::string> Engine::GetRegisteredUiPresetNames() const
{
    std::vector<std::string> names;
    names.reserve(mUiPresetRegistry.size());
    for (const auto& kv : mUiPresetRegistry)
        names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

SceneUiPresetEntry* Engine::FindSceneUiPreset(const std::string& name)
{
    for (auto& e : mSceneUiPresets)
    {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

const SceneUiPresetEntry* Engine::FindSceneUiPreset(const std::string& name) const
{
    for (const auto& e : mSceneUiPresets)
    {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

void Engine::SetSceneUiPresetList(std::vector<SceneUiPresetEntry> entries)
{
    // Drop runtime instances for entries that disappear
    for (auto& old : mSceneUiPresets)
    {
        if (old.instanceId == 0)
            continue;
        const bool keep = std::any_of(entries.begin(), entries.end(),
            [&](const SceneUiPresetEntry& n) { return n.name == old.name; });
        if (!keep)
            DestroyUiPresetInstance(old.instanceId);
    }
    mSceneUiPresets = std::move(entries);
    for (auto& e : mSceneUiPresets)
        e.instanceId = 0;
}

void Engine::AddSceneUiPreset(const std::string& name, bool visible)
{
    if (name.empty())
        return;
    if (FindSceneUiPreset(name))
        return;
    SceneUiPresetEntry e;
    e.name = name;
    e.visible = visible;
    e.instanceId = 0;
    mSceneUiPresets.push_back(std::move(e));
}

void Engine::RemoveSceneUiPreset(const std::string& name)
{
    for (auto it = mSceneUiPresets.begin(); it != mSceneUiPresets.end(); ++it)
    {
        if (it->name != name)
            continue;
        if (it->instanceId != 0)
            DestroyUiPresetInstance(it->instanceId);
        mSceneUiPresets.erase(it);
        return;
    }
}

bool Engine::GetSceneUiPresetVisible(const std::string& name) const
{
    if (const SceneUiPresetEntry* e = FindSceneUiPreset(name))
        return e->visible;
    return false;
}

bool Engine::SetSceneUiPresetVisible(const std::string& name, bool visible)
{
    SceneUiPresetEntry* e = FindSceneUiPreset(name);
    if (!e)
        return false;

    e->visible = visible;

    if (!visible)
    {
        if (e->instanceId != 0)
        {
            DestroyUiPresetInstance(e->instanceId);
            e->instanceId = 0;
        }
        return true;
    }

    if (!HasUiPreset(name))
    {
        std::string err;
        RegisterUiPresetFromFile(name, &err);
    }

    if (e->instanceId == 0)
    {
        e->instanceId = SpawnUiPreset(name, true);
        if (e->instanceId == 0)
            return false;
    }

    SetUiPresetInstanceVisible(e->instanceId, true);
    SetUiPresetInstanceActive(e->instanceId, true);

    char buf[128];
    sprintf_s(buf, "[UI] scene preset '%s' visible=%d instance=%u\n",
        name.c_str(), visible ? 1 : 0, e->instanceId);
    OutputDebugStringA(buf);
    return true;
}

bool Engine::PreloadSceneUiPresets(std::string* outError)
{
    std::string combined;
    bool allOk = true;
    for (const auto& e : mSceneUiPresets)
    {
        if (e.name.empty() || HasUiPreset(e.name))
            continue;
        std::string err;
        if (!RegisterUiPresetFromFile(e.name, &err))
        {
            allOk = false;
            if (!combined.empty())
                combined += "; ";
            combined += e.name + ": " + err;
        }
    }
    if (!allOk && outError)
        *outError = combined;
    return allOk || mSceneUiPresets.empty();
}

void Engine::ApplySceneUiPresetVisibility()
{
    for (auto& e : mSceneUiPresets)
    {
        if (e.name.empty())
            continue;

        // Only spawn when the scene marks this preset visible — no surprise UI.
        if (!e.visible)
        {
            if (e.instanceId != 0)
            {
                DestroyUiPresetInstance(e.instanceId);
                e.instanceId = 0;
            }
            continue;
        }

        if (e.instanceId == 0)
            e.instanceId = SpawnUiPreset(e.name, true);
        if (e.instanceId != 0)
        {
            SetUiPresetInstanceActive(e.instanceId, true);
            SetUiPresetInstanceVisible(e.instanceId, true);
            auto it = mUiPresetInstances.find(e.instanceId);
            if (it != mUiPresetInstances.end())
            {
                for (Entity ent : it->second.entities)
                {
                    if (UiElementComponent* el = mWorld.GetComponent<UiElementComponent>(ent))
                    {
                        el->visible = true;
                        el->active = true;
                    }
                }
            }
        }
        else
        {
            char buf[160];
            sprintf_s(buf, "[UI] ApplySceneUiPresetVisibility: spawn failed for '%s'\n",
                e.name.c_str());
            OutputDebugStringA(buf);
        }
    }

    char summary[128];
    sprintf_s(summary, "[UI] ApplySceneUiPresetVisibility: %zu scene presets, %zu live UI\n",
        mSceneUiPresets.size(), GetUiEntities().size());
    OutputDebugStringA(summary);
}

Entity Engine::SpawnUiPresetElement(const UiPresetElementData& src, uint32_t instanceId, bool startActive)
{
    std::string mat = src.materialName;
    if (mat.empty() || !mResourceManager || !mResourceManager->GetMaterial(mat))
    {
        mat = "Default";
        if (!mResourceManager || !mResourceManager->GetMaterial(mat))
        {
            OutputDebugStringA("[UI] SpawnUiPresetElement: material missing\n");
            return INVALID_ENTITY;
        }
    }

    Entity e = mWorld.CreateEntity();

    UiElementComponent el{};
    el.mode = src.mode;
    el.scaleMode = src.scaleMode;
    el.active = startActive ? src.active : false;
    // Prefer component visible flag; ApplySceneUiPresetVisibility may force true after spawn.
    el.visible = src.visible;
    el.zOrder = src.zOrder;
    el.anchor = src.anchor;
    el.pivot = src.pivot;
    el.position = src.position;
    el.size = src.size;
    el.rotationRad = src.rotationRad;
    el.layoutPercent = src.layoutPercent;
    el.designW = src.designW;
    el.designH = src.designH;
    el.snapLeft = src.snapLeft;
    el.snapRight = src.snapRight;
    el.snapTop = src.snapTop;
    el.snapBottom = src.snapBottom;
    // Screen percent UI always uses center pivot for correct resize sync
    if (el.layoutPercent && el.mode != UiSpaceMode::WorldBillboard)
    {
        if (el.pivot.x < 0.25f || el.pivot.y < 0.25f)
        {
            el.position.x = src.position.x + src.size.x * 0.5f;
            el.position.y = src.position.y + src.size.y * 0.5f;
        }
        el.pivot = { 0.5f, 0.5f };
        el.anchor = { 0.0f, 0.0f };
    }
    if (el.designW < 1.0f || el.designH < 1.0f)
    {
        float gw = 0.f, gh = 0.f;
        mUiSystem.GetGlobalDesignResolution(gw, gh);
        if (el.designW < 1.0f) el.designW = gw;
        if (el.designH < 1.0f) el.designH = gh;
    }

    UiImageComponent img{};
    img.materialName = mat;
    img.color = src.color;
    img.uvRect = src.uvRect;

    if (src.mode == UiSpaceMode::WorldBillboard)
    {
        TransformComponent tf{};
        tf.position = src.worldPos;
        tf.MarkDirty(e);
        mWorld.AddComponent(e, std::move(tf));
    }

    mWorld.AddComponent(e, std::move(el));
    mWorld.AddComponent(e, std::move(img));

    if (src.hasButton)
    {
        UiButtonComponent btn{};
        btn.actionId = src.actionId;
        btn.interactable = src.interactable;
        mWorld.AddComponent(e, std::move(btn));
    }

    UiPresetInstanceTag tag{};
    tag.instanceId = instanceId;
    mWorld.AddComponent(e, std::move(tag));

    mUiListDirty = true;
    return e;
}

uint32_t Engine::SpawnUiPreset(const std::string& presetName, bool startActive)
{
    if (presetName.empty())
        return 0;

    if (!HasUiPreset(presetName))
    {
        std::string err;
        if (!RegisterUiPresetFromFile(presetName, &err))
        {
            char buf[256];
            sprintf_s(buf, "[UI] SpawnUiPreset: not registered and load failed '%s' (%s)\n",
                presetName.c_str(), err.c_str());
            OutputDebugStringA(buf);
            return 0;
        }
    }

    auto it = mUiPresetRegistry.find(presetName);
    if (it == mUiPresetRegistry.end() || it->second.elements.empty())
        return 0;

    const UiPresetData& data = it->second;
    const uint32_t id = mNextUiPresetInstanceId++;
    UiPresetInstance inst;
    inst.id = id;
    inst.presetName = presetName;
    inst.entities.reserve(data.elements.size());

    for (const auto& elSrc : data.elements)
    {
        UiPresetElementData el = elSrc;
        if (el.designW < 1.0f && data.designW > 1.0f)
            el.designW = data.designW;
        if (el.designH < 1.0f && data.designH > 1.0f)
            el.designH = data.designH;
        Entity e = SpawnUiPresetElement(el, id, startActive);
        if (e != INVALID_ENTITY)
            inst.entities.push_back(e);
    }

    if (inst.entities.empty())
        return 0;

    const size_t count = inst.entities.size();
    mUiPresetInstances[id] = std::move(inst);

    char buf[160];
    sprintf_s(buf, "[UI] SpawnUiPreset '%s' instance=%u entities=%zu\n",
        presetName.c_str(), id, count);
    OutputDebugStringA(buf);
    return id;
}

void Engine::SetUiPresetInstanceActive(uint32_t instanceId, bool active)
{
    auto it = mUiPresetInstances.find(instanceId);
    if (it == mUiPresetInstances.end())
        return;
    for (Entity e : it->second.entities)
        SetUiActive(e, active);
}

void Engine::SetUiPresetInstanceVisible(uint32_t instanceId, bool visible)
{
    auto it = mUiPresetInstances.find(instanceId);
    if (it == mUiPresetInstances.end())
        return;
    for (Entity e : it->second.entities)
        SetUiVisible(e, visible);
}

void Engine::DestroyUiPresetInstance(uint32_t instanceId)
{
    auto it = mUiPresetInstances.find(instanceId);
    if (it == mUiPresetInstances.end())
        return;
    for (Entity e : it->second.entities)
    {
        if (e != INVALID_ENTITY)
            mWorld.DestroyEntity(e);
    }
    mUiPresetInstances.erase(it);
    for (auto& e : mSceneUiPresets)
    {
        if (e.instanceId == instanceId)
            e.instanceId = 0;
    }
    mUiListDirty = true;
}

void Engine::DestroyAllUiPresetInstances()
{
    std::vector<uint32_t> ids;
    ids.reserve(mUiPresetInstances.size());
    for (const auto& kv : mUiPresetInstances)
        ids.push_back(kv.first);
    for (uint32_t id : ids)
        DestroyUiPresetInstance(id);
    for (auto& e : mSceneUiPresets)
        e.instanceId = 0;
}

size_t Engine::CountUiPresetInstances(const std::string& presetName) const
{
    if (presetName.empty())
        return mUiPresetInstances.size();
    size_t n = 0;
    for (const auto& kv : mUiPresetInstances)
    {
        if (kv.second.presetName == presetName)
            ++n;
    }
    return n;
}

Entity Engine::CreateEffectBillboard(
    const std::string& materialName,
    XMFLOAT3 worldPos,
    float size,
    XMFLOAT4 color,
    bool additive,
    float lifetime)
{
    if (!mResourceManager || !mResourceManager->GetMaterial(materialName))
    {
        OutputDebugStringA("[Engine] CreateEffectBillboard: material not found\n");
        return INVALID_ENTITY;
    }
    Entity e = mWorld.CreateEntity();
    TransformComponent tf{};
    tf.position = worldPos;
    tf.MarkDirty(e);
    mWorld.AddComponent(e, std::move(tf));
    EffectBillboardComponent fx{};
    fx.materialName = materialName;
    fx.color = color;
    fx.size = size;
    fx.additive = additive;
    fx.lifetime = lifetime;
    mWorld.AddComponent(e, std::move(fx));
    return e;
}

bool Engine::HandleUiPointer(
    float scenePixelX, float scenePixelY,
    UINT screenW, UINT screenH,
    bool leftDown, bool leftPressedThisFrame,
    std::vector<UiClickEvent>* outClicks)
{
    return mUiSystem.HandlePointer(
        mWorld, scenePixelX, scenePixelY, screenW, screenH,
        leftDown, leftPressedThisFrame, outClicks);
}

void Engine::SetRenderPath(RenderPath path)
{
    mRenderSystem.SetRenderPath(path);
}

RenderPath Engine::GetRenderPath() const
{
    return mRenderSystem.GetRenderPath();
}

bool Engine::IsComputeIndirectReady() const
{
    return mRenderSystem.IsComputeIndirectReady();
}

void Engine::SetAutoRenderPathEnabled(bool enabled)
{
    mRenderSystem.SetAutoRenderPathEnabled(enabled);
}

bool Engine::IsAutoRenderPathEnabled() const
{
    return mRenderSystem.IsAutoRenderPathEnabled();
}

void Engine::SetGpuFrustumCullEnabled(bool enabled)
{
    mRenderSystem.SetGpuFrustumCullEnabled(enabled);
}

bool Engine::IsGpuFrustumCullEnabled() const
{
    return mRenderSystem.IsGpuFrustumCullEnabled();
}

const GpuDrivenFrameStats& Engine::GetLastFrameStats() const
{
    return mRenderSystem.GetLastFrameStats();
}

void Engine::SetGpuOcclusionEnabled(bool enabled)
{
    mRenderSystem.SetGpuOcclusionEnabled(enabled);
}

bool Engine::IsGpuOcclusionEnabled() const
{
    return mRenderSystem.IsGpuOcclusionEnabled();
}

void Engine::SetGpuMotionEnabled(bool enabled)
{
    mRenderSystem.SetGpuMotionEnabled(enabled);
    // Path3 may be mid-frame; motion seed version bumped inside SetGpuMotionEnabled
}

bool Engine::IsGpuMotionEnabled() const
{
    return mRenderSystem.IsGpuMotionEnabled();
}

void Engine::SetLodEnabled(bool enabled)
{
    mRenderSystem.SetLodEnabled(enabled);
}

bool Engine::IsLodEnabled() const
{
    return mRenderSystem.IsLodEnabled();
}

void Engine::SetLodDistanceCullEnabled(bool enabled)
{
    mRenderSystem.SetLodDistanceCullEnabled(enabled);
}

bool Engine::IsLodDistanceCullEnabled() const
{
    return mRenderSystem.IsLodDistanceCullEnabled();
}

void Engine::SetLodBias(float bias)
{
    mRenderSystem.SetLodBias(bias);
}

float Engine::GetLodBias() const
{
    return mRenderSystem.GetLodBias();
}

void Engine::SetLodCullDistance(float d)
{
    mRenderSystem.SetLodCullDistance(d);
}

float Engine::GetLodCullDistance() const
{
    return mRenderSystem.GetLodCullDistance();
}

void Engine::PrepareHiZForSceneSize(UINT width, UINT height)
{
    if (!mDescriptorAllocator)
        return;
    mRenderSystem.PrepareHiZForSceneSize(mDescriptorAllocator, width, height);
}

void Engine::BuildHiZ(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* sceneDepth,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
    UINT width, UINT height)
{
    if (!mDescriptorAllocator)
        return;
    mRenderSystem.BuildHiZ(
        cmdList, mDescriptorAllocator,
        sceneDepth, sceneDepthSrvCpu, sceneDepthSrvGpu,
        width, height);
}

void Engine::NotifyRenderableListChanged()
{
    mRenderableListDirty = true;
    mRenderSystem.InvalidateDrawCache();
}

void Engine::RebuildRenderableListCacheIfNeeded()
{
    if (!mRenderableListDirty)
        return;

    mRenderableListCache.clear();
    mWorld.ForEach<RenderableComponent>(
        [&](Entity e, RenderableComponent&)
        {
            mRenderableListCache.push_back(e);
        });
    std::sort(mRenderableListCache.begin(), mRenderableListCache.end());
    mRenderableListDirty = false;
}

size_t Engine::GetRenderableObjectCount()
{
    RebuildRenderableListCacheIfNeeded();
    return mRenderableListCache.size();
}

const std::vector<Entity>& Engine::GetRenderableEntities()
{
    RebuildRenderableListCacheIfNeeded();
    return mRenderableListCache;
}

Entity Engine::CreateRenderableEntity(const std::string& meshName,
    const std::string& materialName,
    XMFLOAT3 position)
{
    Mesh* mesh = mResourceManager->GetMesh(meshName);
    if (!mesh)
    {
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: mesh not found\n");
        return INVALID_ENTITY;
    }

    if (!materialName.empty() && !mResourceManager->GetMaterial(materialName))
    {
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: material not found\n");
        return INVALID_ENTITY;
    }

    if (mNextObjectCBIndex >= mMaxObjectCBSlots)
    {
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: object CB slots full\n");
        return INVALID_ENTITY;
    }

    Entity entity = mWorld.CreateEntity();

    TransformComponent tf{};
    tf.position = position;
    tf.MarkDirty(entity);
    mWorld.AddComponent(entity, std::move(tf));
    mWorld.AddComponent(entity, RenderableComponent{
        .mesh = mesh,
        .objectCBIndex = mNextObjectCBIndex++
        });
    mWorld.AddComponent(entity, BoundsComponent{});

    // Step H: LOD chain (base + auto _lod1/_lod2 if present)
    {
        LodComponent lod{};
        lod.levelCount = MeshManager::Get().BuildLodLevelList(
            meshName, lod.levels, LodComponent::kMaxLevels);
        if (lod.levelCount <= 0)
        {
            lod.levels[0] = mesh;
            lod.levelCount = 1;
        }
        mWorld.AddComponent(entity, std::move(lod));
    }

    // Main material: empty string = None (no main override; draw uses mesh init / Default).
    // Callers that want a specific material must pass it explicitly (not a hidden default like "Test").
    SetEntityMainMaterial(entity, materialName);

    NotifyRenderableListChanged();
    return entity;
}

RenderableComponent* Engine::GetRenderable(Entity entity)
{
    return mWorld.GetComponent<RenderableComponent>(entity);
}

void Engine::SetEntityMainMaterial(Entity entity, const std::string& materialName)
{
    if (!GetRenderable(entity))
        return;

    MaterialManager::Get().SetEntityMainMaterial(entity, materialName);
    mRenderSystem.InvalidateDrawCache();
}

void Engine::SetEntitySubMaterial(Entity entity, const std::string& submeshKey, const std::string& materialName)
{
    if (!GetRenderable(entity))
        return;

    MaterialManager::Get().SetEntitySubMaterial(entity, submeshKey, materialName);
    mRenderSystem.InvalidateDrawCache();
}

std::string Engine::GetEntityMainMaterial(Entity entity) const
{
    if (const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(entity))
        return data->mainMaterialName;
    return {};
}

std::string Engine::GetEntitySubMaterial(Entity entity, const std::string& submeshKey) const
{
    if (const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(entity))
    {
        auto it = data->subMaterialNames.find(submeshKey);
        if (it != data->subMaterialNames.end())
            return it->second;
    }
    return {};
}

void Engine::Shutdown()
{
    mUiSystem.Shutdown();
    mRenderSystem.Shutdown();
    PipelineStateManager::Get().Shutdown();
    RootSignatureManager::Get().Shutdown();
    ShaderManager::Get().Shutdown();
    if (mResourceManager)
        mResourceManager->Shutdown();
}

bool Engine::GetMainCameraViewProj(XMMATRIX& outView, XMMATRIX& outProj, float aspectRatio)
{
    bool found = false;
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX proj = XMMatrixIdentity();

    // ECS World에서 Transform + CameraComponent를 가진 메인 카메라 찾기
    mWorld.ForEach<TransformComponent, CameraComponent>(
        [&](Entity e, TransformComponent& tf, CameraComponent& cam)
        {
            if (found || !cam.isMainCamera)
                return;

            // View 행렬: Transform의 position + rotation 사용
            XMVECTOR camPos = XMLoadFloat3(&tf.position);
            // rotation.x = pitch, rotation.y = yaw 로 저장한다고 가정
            XMMATRIX rot = XMMatrixRotationRollPitchYaw(tf.rotation.x, tf.rotation.y, 0.0f);

            // 기본 forward = +Z 
            XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rot);
            XMVECTOR up      = XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rot);

            view = XMMatrixLookToLH(camPos, forward, up);

            // Proj 행렬
            float aspect = cam.useWindowAspect ? aspectRatio : cam.aspectRatio;
            proj = XMMatrixPerspectiveFovLH(cam.fov, aspect, cam.nearZ, cam.farZ);

            found = true;
        });

    if (found)
    {
        outView = view;
        outProj = proj;
    }
    return found;
}

void Engine::FillPassCB(FrameResource* currentFrame,
    const XMMATRIX& view,
    const XMMATRIX& proj,
    float clientWidth, float clientHeight,
    float aspectRatio,
    float totalTime, float deltaTime)
{
    if (!currentFrame || !currentFrame->PassCB)
        return;

    XMMATRIX viewProj = XMMatrixMultiply(proj, view);  // correct order for combined gViewProj storage (transpose(proj*view) == tv * tp)

    XMVECTOR det;
    XMMATRIX invView     = XMMatrixInverse(&det, view);
    XMMATRIX invProj     = XMMatrixInverse(&det, proj);
    XMMATRIX invViewProj = XMMatrixInverse(&det, viewProj);

    // ECS CameraComponent에서 EyePos, Near, Far 등을 가져옴
    XMFLOAT3 eyePos = { 0.0f, 0.0f, 0.0f };
    float nearZ = 0.1f;
    float farZ = 1000.0f;

    mWorld.ForEach<TransformComponent, CameraComponent>(
        [&](Entity e, TransformComponent& tf, CameraComponent& cam)
        {
            if (cam.isMainCamera)
            {
                eyePos = tf.position;
                nearZ = cam.nearZ;
                farZ = cam.farZ;
            }
        });

    PassConstants pc{};
    XMStoreFloat4x4(&pc.View,        XMMatrixTranspose(view));
    XMStoreFloat4x4(&pc.InvView,     XMMatrixTranspose(invView));
    XMStoreFloat4x4(&pc.Proj,        XMMatrixTranspose(proj));
    XMStoreFloat4x4(&pc.InvProj,     XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&pc.ViewProj,    XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&pc.InvViewProj, XMMatrixTranspose(invViewProj));

    pc.EyePosW = eyePos;
    pc.NearZ = nearZ;
    pc.FarZ  = farZ;

    pc.RenderTargetSize    = { clientWidth, clientHeight };
    pc.InvRenderTargetSize = { 1.0f / clientWidth, 1.0f / clientHeight };

    pc.TotalTime = totalTime;
    pc.DeltaTime = deltaTime;

    pc.AmbientLight = { mAmbientRgb.x, mAmbientRgb.y, mAmbientRgb.z, 1.0f };

    // Primary directional light (Lights[0])
    XMFLOAT3 sunDir = mSunDirection;
    {
        XMVECTOR d = XMLoadFloat3(&sunDir);
        d = XMVector3Normalize(d);
        if (XMVectorGetX(XMVector3LengthSq(d)) < 1e-8f)
            d = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
        XMStoreFloat3(&sunDir, d);
    }
    pc.Lights[0].Direction = sunDir;
    pc.Lights[0].Strength = mSunStrength;

    pc.GraphicsStyle = (mGraphicsStyle == GraphicsStyle::Toon) ? 1 : 0;
    pc.ToonBands = mToonBands;
    pc.OutlineWidth = mOutlineWidth;
    pc.SpecularPower = mSpecularPower;

    // Directional shadow ortho around scene origin, looking along sun ray travel dir
    {
        XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&sunDir));
        if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-8f)
            dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);

        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (fabsf(XMVectorGetX(XMVector3Dot(dir, up))) > 0.95f)
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

        // Center cascade near camera eye for better local density
        XMVECTOR focus = XMLoadFloat3(&eyePos);
        const float cascadeHalf = 35.0f;
        const float cascadeDist = 50.0f;
        XMVECTOR lightPos = XMVectorSubtract(focus, XMVectorScale(dir, cascadeDist));
        XMMATRIX lightView = XMMatrixLookAtLH(lightPos, focus, up);
        XMMATRIX lightProj = XMMatrixOrthographicLH(
            cascadeHalf * 2.0f, cascadeHalf * 2.0f, 1.0f, cascadeDist * 2.0f);
        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);
        XMStoreFloat4x4(&pc.LightViewProj, XMMatrixTranspose(lightVP));
    }
    pc.ShadowBias = mShadowBias;
    pc.ShadowEnabled = mShadowsEnabled ? 1.0f : 0.0f;
    pc.ShadowSoftness = 1.0f;

    currentFrame->PassCB->CopyData(0, pc);
}

void Engine::SetShadowsEnabled(bool enabled)
{
    mShadowsEnabled = enabled;
    mRenderSystem.SetShadowsEnabled(enabled);
}

void Engine::SetShadowBias(float bias)
{
    mShadowBias = MathHelper::Clamp(bias, 0.0001f, 0.05f);
}

void Engine::RenderShadowMap(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    if (!mDescriptorAllocator)
        return;
    mRenderSystem.RenderShadowMap(
        mWorld, cmdList, currentFrameResource, mDescriptorAllocator,
        currentFrameIndex, viewMatrix, projMatrix);
}

void Engine::SyncOutlinePassFlag()
{
    const bool outline =
        (mGraphicsStyle == GraphicsStyle::Toon) && mToonOutlineEnabled;
    mRenderSystem.SetOutlinePassEnabled(outline);
}

void Engine::SetGraphicsStyle(GraphicsStyle style)
{
    mGraphicsStyle = style;
    SyncOutlinePassFlag();
}

void Engine::SetToonBands(float bands)
{
    mToonBands = MathHelper::Clamp(bands, 1.0f, 8.0f);
}

void Engine::SetToonOutlineEnabled(bool enabled)
{
    mToonOutlineEnabled = enabled;
    SyncOutlinePassFlag();
}

void Engine::SetOutlineWidth(float w)
{
    mOutlineWidth = MathHelper::Clamp(w, 0.001f, 0.2f);
}

void Engine::SetSunDirection(XMFLOAT3 dir)
{
    mSunDirection = dir;
}

void Engine::SetSunStrength(XMFLOAT3 rgb)
{
    mSunStrength = rgb;
}

void Engine::SetAmbientLight(XMFLOAT3 rgb)
{
    mAmbientRgb = rgb;
}

void Engine::SetSpecularPower(float p)
{
    mSpecularPower = MathHelper::Clamp(p, 1.0f, 256.0f);
}

Entity Engine::CreateMainCamera(XMFLOAT3 position, float fov, float nearZ, float farZ)
{
    Entity camEntity = mWorld.CreateEntity();

    mWorld.AddComponent(camEntity, TransformComponent{
        .position = position,
        .rotation = { 0.0f, 0.0f, 0.0f },
        .scale    = { 1.0f, 1.0f, 1.0f }
    });

    mWorld.AddComponent(camEntity, CameraComponent{
        .fov = fov,
        .nearZ = nearZ,
        .farZ = farZ,
        .useWindowAspect = true,
        .aspectRatio = 16.0f / 9.0f,
        .isMainCamera = true
    });

    return camEntity;
}

void Engine::SetCameraTransform(Entity camEntity, const XMFLOAT3& position, const XMFLOAT3& rotation)
{
    TransformComponent* tf = mWorld.GetComponent<TransformComponent>(camEntity);
    if (tf)
    {
        tf->position = position;
        tf->rotation = rotation;
    }
}

TransformComponent* Engine::GetTransform(Entity entity)
{
    return mWorld.GetComponent<TransformComponent>(entity);
}

bool Engine::HasGravityComponent(Entity entity)
{
    return mWorld.GetComponent<GravityComponent>(entity) != nullptr;
}

GravityComponent* Engine::GetGravityComponent(Entity entity)
{
    return mWorld.GetComponent<GravityComponent>(entity);
}

void Engine::SetEntityGravityEnabled(Entity entity, bool enabled)
{
    if (entity == INVALID_ENTITY)
        return;

    if (enabled)
    {
        if (!HasGravityComponent(entity))
            mWorld.AddComponent(entity, GravityComponent{});
    }
    else if (HasGravityComponent(entity))
    {
        mWorld.RemoveComponent<GravityComponent>(entity);
    }

    // F2: Path3 motion buffer must reseed without waiting for scene rebuild
    mRenderSystem.NotifyEntityMotionChanged(mWorld, entity);
}

void Engine::NotifyEntityMotionChanged(Entity entity)
{
    if (entity == INVALID_ENTITY)
        return;
    mRenderSystem.NotifyEntityMotionChanged(mWorld, entity);
}

bool Engine::HasCollisionComponent(Entity entity)
{
    return mWorld.GetComponent<CollisionComponent>(entity) != nullptr;
}

CollisionComponent* Engine::GetCollisionComponent(Entity entity)
{
    return mWorld.GetComponent<CollisionComponent>(entity);
}

void Engine::SetEntityCollisionEnabled(Entity entity, bool enabled)
{
    if (entity == INVALID_ENTITY)
        return;

    if (enabled)
    {
        if (!mWorld.GetComponent<BoundsComponent>(entity) && GetRenderable(entity))
            mWorld.AddComponent(entity, BoundsComponent{});

        if (!HasCollisionComponent(entity))
            mWorld.AddComponent(entity, CollisionComponent{});
    }
    else if (HasCollisionComponent(entity))
    {
        mWorld.RemoveComponent<CollisionComponent>(entity);
    }
}

void Engine::DestroyRenderableEntity(Entity entity)
{
    if (entity == INVALID_ENTITY)
        return;
    if (!GetRenderable(entity))
        return;

    if (IsEntitySelected(entity))
        ToggleSelectedEntity(entity); // remove selection tag

    MaterialManager::Get().ClearEntityMaterialData(entity);
    mWorld.DestroyEntity(entity);
    NotifyRenderableListChanged();
    mRenderSystem.InvalidateDrawCache();
}

void Engine::ClearRenderableEntities()
{
    ClearSelection();

    // 복사본 — Destroy 중 캐시가 바뀌므로
    std::vector<Entity> list = GetRenderableEntities();
    for (Entity e : list)
    {
        MaterialManager::Get().ClearEntityMaterialData(e);
        mWorld.DestroyEntity(e);
    }

    mNextObjectCBIndex = 0;
    NotifyRenderableListChanged();
    mRenderSystem.InvalidateDrawCache();
}

bool Engine::SaveSceneToFile(const std::string& path, const std::string& sceneName, bool packLiveUiIntoFile)
{
    SceneFileData scene;
    scene.version = SceneFileData::kCurrentVersion;
    if (!sceneName.empty())
        scene.name = sceneName;
    else
    {
        try
        {
            scene.name = std::filesystem::path(path).stem().string();
        }
        catch (...)
        {
            scene.name = "scene";
        }
    }

    const auto& entities = GetRenderableEntities();
    scene.entities.reserve(entities.size());

    for (Entity e : entities)
    {
        RenderableComponent* rend = GetRenderable(e);
        TransformComponent* tf = GetTransform(e);
        if (!rend || !rend->mesh || !tf)
            continue;

        SceneEntityData data;
        data.meshName = rend->mesh->name;
        {
            const auto lodPos = data.meshName.find("_lod");
            if (lodPos != std::string::npos)
                data.meshName = data.meshName.substr(0, lodPos);
        }
        data.mainMaterial = GetEntityMainMaterial(e);
        data.position = tf->position;
        data.rotation = tf->rotation;
        data.scale = tf->scale;
        data.visible = rend->visible;

        if (GravityComponent* g = GetGravityComponent(e))
        {
            data.hasGravity = true;
            data.gravityEnabled = g->enabled;
            data.gravityStrength = g->strength;
            data.gravityVelocity = g->velocity;
            data.gravityAngularVelocity = g->angularVelocity;
        }

        if (CollisionComponent* c = GetCollisionComponent(e))
        {
            data.hasCollision = true;
            data.collisionEnabled = c->enabled;
            data.collisionStatic = c->isStatic;
            data.collisionRestitution = c->restitution;
        }

        if (rend->mesh)
        {
            for (const auto& pair : rend->mesh->DrawArgs)
            {
                const std::string sub = GetEntitySubMaterial(e, pair.first);
                if (!sub.empty())
                    data.subMaterials[pair.first] = sub;
            }
        }

        scene.entities.push_back(std::move(data));
    }

    // Only what the user put on the scene preload list (no auto presets / no disk spam)
    scene.uiPresets.clear();
    scene.uiPresets.reserve(mSceneUiPresets.size() + (packLiveUiIntoFile ? 1u : 0u));
    scene.uiPresetPayloads.clear();

    for (const auto& e : mSceneUiPresets)
    {
        if (e.name.empty())
            continue;
        if (!HasUiPreset(e.name))
        {
            std::string err;
            RegisterUiPresetFromFile(e.name, &err);
        }

        SceneUiPresetEntry saveE;
        saveE.name = e.name;
        saveE.visible = e.visible;
        saveE.instanceId = 0;
        scene.uiPresets.push_back(std::move(saveE));

        auto it = mUiPresetRegistry.find(e.name);
        if (it != mUiPresetRegistry.end())
            scene.uiPresetPayloads.push_back(it->second);
    }

    // Optional: embed live UI into THIS file only (export / temp play snapshot).
    // Does not write UiPresets/*.uipreset and does not change mSceneUiPresets.
    if (packLiveUiIntoFile)
    {
        UiPresetData live = CaptureCurrentUiAsPreset("__live_ui__");
        for (auto& el : live.elements)
        {
            el.visible = true;
            el.active = true;
            if (el.layoutPercent && el.mode != UiSpaceMode::WorldBillboard)
            {
                el.pivot = { 0.5f, 0.5f };
                el.anchor = { 0.0f, 0.0f };
            }
            // Ensure size is never zero after pack (avoids invisible quads in Game.exe)
            if (el.layoutPercent)
            {
                if (el.size.x < 0.001f) el.size.x = 0.05f;
                if (el.size.y < 0.001f) el.size.y = 0.05f;
            }
        }
        if (!live.elements.empty())
        {
            live.name = "__live_ui__";
            // Replace any previous __live_ui__ entry
            scene.uiPresets.erase(
                std::remove_if(scene.uiPresets.begin(), scene.uiPresets.end(),
                    [](const SceneUiPresetEntry& e) { return e.name == "__live_ui__"; }),
                scene.uiPresets.end());
            scene.uiPresetPayloads.erase(
                std::remove_if(scene.uiPresetPayloads.begin(), scene.uiPresetPayloads.end(),
                    [](const UiPresetData& p) { return p.name == "__live_ui__"; }),
                scene.uiPresetPayloads.end());

            SceneUiPresetEntry liveEntry;
            liveEntry.name = "__live_ui__";
            liveEntry.visible = true;
            scene.uiPresets.insert(scene.uiPresets.begin(), liveEntry);
            scene.uiPresetPayloads.insert(scene.uiPresetPayloads.begin(), std::move(live));
        }
        else
        {
            OutputDebugStringA("[UI] packLiveUi: CaptureCurrentUiAsPreset returned 0 elements\n");
        }
    }

    char saveLog[192];
    sprintf_s(saveLog, "[UI] scene save: %zu preset(s), %zu payload(s) packLive=%d path=%s\n",
        scene.uiPresets.size(), scene.uiPresetPayloads.size(), packLiveUiIntoFile ? 1 : 0,
        path.c_str());
    OutputDebugStringA(saveLog);

    const bool ok = SceneSerializer::SaveToFile(path, scene);
    if (!ok)
        OutputDebugStringA("[UI] scene save FAILED to write file\n");
    return ok;
}

bool Engine::LoadSceneFromFile(const std::string& path, std::string* outError)
{
    SceneFileData scene;
    if (!SceneSerializer::LoadFromFile(path, scene, outError))
        return false;

    ClearRenderableEntities();
    DestroyAllUiEntities();
    ClearUiPresetRegistry();
    mSceneUiPresets = scene.uiPresets;
    for (auto& e : mSceneUiPresets)
        e.instanceId = 0;

    // Register embedded payloads in memory only — never write UiPresets/ as a side effect.
    // Scene preload list stays exactly as written in the file (ui_preset= lines).
    for (const auto& payload : scene.uiPresetPayloads)
    {
        if (payload.name.empty())
            continue;
        RegisterUiPreset(payload.name, payload);
    }

    // Fall back to disk files only for names already listed on the scene
    {
        std::string presetErr;
        if (!PreloadSceneUiPresets(&presetErr) && !presetErr.empty())
        {
            char buf[512];
            sprintf_s(buf, "[UI] scene preset preload: %s\n", presetErr.c_str());
            OutputDebugStringA(buf);
        }
    }

    // Spawn only presets marked visible on this scene
    ApplySceneUiPresetVisibility();

    {
        char buf[256];
        sprintf_s(buf,
            "[UI] LoadScene '%s': scenePresets=%zu payloads=%zu liveUi=%zu\n",
            path.c_str(),
            mSceneUiPresets.size(),
            scene.uiPresetPayloads.size(),
            GetUiEntities().size());
        OutputDebugStringA(buf);
        for (const auto& e : mSceneUiPresets)
        {
            sprintf_s(buf, "  ui_preset name=%s visible=%d instance=%u hasReg=%d\n",
                e.name.c_str(), e.visible ? 1 : 0, e.instanceId,
                HasUiPreset(e.name) ? 1 : 0);
            OutputDebugStringA(buf);
        }
    }

    size_t created = 0;
    for (const auto& data : scene.entities)
    {
        if (data.meshName.empty())
            continue;

        Entity e = CreateRenderableEntity(data.meshName, data.mainMaterial, data.position);
        if (e == INVALID_ENTITY)
        {
            // 메시/머티리얼 없으면 스킵하고 계속
            continue;
        }

        if (TransformComponent* tf = GetTransform(e))
        {
            tf->rotation = data.rotation;
            tf->scale = data.scale;
            tf->MarkDirty(e);
        }

        if (RenderableComponent* r = GetRenderable(e))
            r->visible = data.visible;

        if (data.hasGravity)
        {
            SetEntityGravityEnabled(e, true);
            if (GravityComponent* g = GetGravityComponent(e))
            {
                g->enabled = data.gravityEnabled;
                g->strength = data.gravityStrength;
                g->velocity = data.gravityVelocity;
                g->angularVelocity = data.gravityAngularVelocity;
            }
            NotifyEntityMotionChanged(e);
        }

        if (data.hasCollision)
        {
            SetEntityCollisionEnabled(e, true);
            if (CollisionComponent* c = GetCollisionComponent(e))
            {
                c->enabled = data.collisionEnabled;
                c->isStatic = data.collisionStatic;
                c->restitution = data.collisionRestitution;
            }
        }

        for (const auto& sub : data.subMaterials)
            SetEntitySubMaterial(e, sub.first, sub.second);

        ++created;
    }

    if (created == 0 && !scene.entities.empty() && outError)
        *outError = "No entities could be created (missing meshes/materials?)";

    return true;
}