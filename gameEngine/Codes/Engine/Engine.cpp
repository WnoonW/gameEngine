#include "Engine.h"
#include "MeshManager.h"
#include "MaterialManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"

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

    mRenderSystem.createCBV(mDevice, *mFrameResources, mGNumFrameResources,
        *mDescriptorAllocator, entity, mWorld);

    return entity;
}

void Engine::Shutdown()
{
    mResourceManager->Shutdown();
    // 필요하면 mRenderSystem, mWorld 관련 정리도 여기에 추가
}