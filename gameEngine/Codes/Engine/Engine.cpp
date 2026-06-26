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

void Engine::Update()
{
    // 나중에 TransformSystem, AnimationSystem 등 추가 예정
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
    auto material = mResourceManager->GetMaterial(materialName);

    if (!mesh || !material)
    {
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: mesh or material not found\n");
        return INVALID_ENTITY;
    }

    Entity entity = mWorld.CreateEntity();

    mWorld.AddComponent(entity, TransformComponent{ .position = position });
    mWorld.AddComponent(entity, RenderableComponent{
        .mesh = mesh,
        .material = material,
        .objectCBIndex = mNextObjectCBIndex++
        });

    // createCBV 제거됨 (이제 Root CBV 직접 바인딩 사용)
    return entity;
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
            // XMMatrixRotationRollPitchYaw(Roll, Pitch, Yaw)
            XMMATRIX rot = XMMatrixRotationRollPitchYaw(0.0f, tf.rotation.x, tf.rotation.y);

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