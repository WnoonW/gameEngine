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
#include <algorithm>
#include <vector>

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

    return true;
}

void Engine::Update(float deltaTime)
{
    mGravitySystem.Update(mWorld, deltaTime);
    UpdateBounds();
    // 충돌이 실제로 움직인 경우에만 bounds 2차 갱신 (정적 대량 씬에서 이중 순회 제거)
    if (mCollisionSystem.Update(mWorld))
        UpdateBounds();
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

void Engine::SetSelectedEntity(Entity entity)
{
    ClearSelection();

    if (entity == INVALID_ENTITY)
        return;

    if (!mWorld.GetComponent<TransformComponent>(entity))
        return;

    if (!mWorld.GetComponent<SelectedComponent>(entity))
        mWorld.AddComponent(entity, SelectedComponent{});
}

void Engine::AddSelectedEntity(Entity entity)
{
    if (entity == INVALID_ENTITY)
        return;
    if (!mWorld.GetComponent<TransformComponent>(entity))
        return;
    if (mWorld.GetComponent<SelectedComponent>(entity))
        return;
    mWorld.AddComponent(entity, SelectedComponent{});
}

void Engine::ToggleSelectedEntity(Entity entity)
{
    if (entity == INVALID_ENTITY)
        return;
    if (!mWorld.GetComponent<TransformComponent>(entity))
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

void Engine::Render(ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    mRenderSystem.render(mWorld, cmdList, currentFrameResource,
        mDescriptorAllocator, currentFrameIndex, viewMatrix, projMatrix);
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

    // 스폰 시 선택한 Main Material 적용 (비어 있으면 메시 init / Default 사용)
    if (!materialName.empty())
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

    pc.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };

    currentFrame->PassCB->CopyData(0, pc);
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