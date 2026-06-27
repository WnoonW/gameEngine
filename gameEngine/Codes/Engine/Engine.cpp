#include "Engine.h"
#include "MeshManager.h"
#include "MaterialManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"
#include "ComponentStruct.h"
#include "Entity.h"

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

    ShaderManager::Get().Initialize();
    RootSignatureManager::Get().Initialize(device);
    PipelineStateManager::Get().Initialize(device);

    return true;
}

void Engine::Update(float deltaTime)
{
    mGravitySystem.Update(mWorld, deltaTime);
    UpdateBounds();
    mCollisionSystem.Update(mWorld);
    UpdateBounds();
}

void Engine::UpdateBounds()
{
    mBoundsSystem.Update(mWorld);
}

Entity Engine::PickObject(int mouseX, int mouseY, float clientWidth, float clientHeight,
                          const XMMATRIX& view, const XMMATRIX& proj)
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
            if (!rend.visible || !rend.mesh) return;

            float dist;
            if (bnds.worldBounds.Intersects(rayOrigin, rayDir, dist))
            {
                if (dist < minDist && dist > 0.001f)
                {
                    minDist = dist;
                    closest = e;
                }
            }
        });

    mSelectedEntity = closest;
    return closest;
}

void Engine::RotateSelected(float dYaw, float dPitch)
{
    if (mSelectedEntity == INVALID_ENTITY) return;
    auto* tf = mWorld.GetComponent<TransformComponent>(mSelectedEntity);
    if (tf) {
        tf->rotation.y += dYaw;
        tf->rotation.x += dPitch;
        tf->rotation.x = MathHelper::Clamp(tf->rotation.x, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
    }
}

void Engine::MoveSelectedViewRelative(float forward, float right, float up, float speed, const XMMATRIX& view)
{
    if (mSelectedEntity == INVALID_ENTITY) return;
    auto* tf = mWorld.GetComponent<TransformComponent>(mSelectedEntity);
    if (!tf) return;

    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMVECTOR cFwd = -invView.r[2];
    XMVECTOR cRight = invView.r[0];
    XMVECTOR cUp = invView.r[1];

    XMVECTOR delta = XMVectorAdd(
        XMVectorScale(cFwd, forward * speed),
        XMVectorScale(cRight, right * speed)
    );
    delta = XMVectorAdd(delta, XMVectorScale(cUp, up * speed));

    XMFLOAT3 d;
    XMStoreFloat3(&d, delta);
    tf->position.x += d.x;
    tf->position.y += d.y;
    tf->position.z += d.z;
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

    Entity entity = mWorld.CreateEntity();

    mWorld.AddComponent(entity, TransformComponent{ .position = position });
    mWorld.AddComponent(entity, RenderableComponent{
        .mesh = mesh,
        .objectCBIndex = mNextObjectCBIndex++
        });
    mWorld.AddComponent(entity, BoundsComponent{});

    if (!materialName.empty())
        MaterialManager::Get().SetEntityMainMaterial(entity, materialName);

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
}

void Engine::SetEntitySubMaterial(Entity entity, const std::string& submeshKey, const std::string& materialName)
{
    if (!GetRenderable(entity))
        return;

    MaterialManager::Get().SetEntitySubMaterial(entity, submeshKey, materialName);
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
    mResourceManager->Shutdown();
    // 필요하면 mRenderSystem, mWorld 관련 정리도 여기에 추가
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