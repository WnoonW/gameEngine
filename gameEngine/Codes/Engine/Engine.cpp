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

	MaterialManager::Get().InitializeTextureTable(descriptorAllocator, 1024); 
    ShaderManager::Get().Initialize();
    RootSignatureManager::Get().Initialize(device);
    PipelineStateManager::Get().Initialize(device);

    mCamera.SetLens(XM_PIDIV4, 16.0f / 9.0f, 0.1f, 1000.0f);

    return true;
}

void Engine::Update()
{
}

void Engine::RenderScene(ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    int currentFrameIndex,
    const DepthStencilContext* depthCtx)
{
    const XMMATRIX viewMatrix = mCamera.GetView();
    const XMMATRIX projMatrix = mCamera.GetProj();

    mRenderSystem.renderExecuteIndirect(mWorld, cmdList, currentFrameResource,
        mDescriptorAllocator, currentFrameIndex, viewMatrix, projMatrix,
        mDepthSrvGpu, depthCtx);
}

static uint32_t CountEstimatedDrawCommands(ECS::World& world)
{
    uint32_t drawCommands = 0;
    world.ForEach<RenderableComponent>(
        [&](Entity, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            drawCommands += static_cast<uint32_t>(rend.mesh->DrawArgs.size());
        });
    return drawCommands;
}

MeshInstanceStats Engine::CollectMeshInstanceStats()
{
    MeshInstanceStats stats;
    stats.objectCBUsed = mNextObjectCBIndex;
    stats.lastCreateError = mLastCreateError;

    if (mDescriptorAllocator)
    {
        stats.descriptorsUsed = mDescriptorAllocator->GetUsedCount();
        stats.descriptorCapacity = mDescriptorAllocator->GetCapacity();
    }

    mWorld.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity, TransformComponent&, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            const std::string meshName = rend.mesh->name.empty() ? "(unnamed)" : rend.mesh->name;
            stats.countByMesh[meshName]++;
            stats.totalInstances++;
            stats.estimatedDrawCommands += static_cast<uint32_t>(rend.mesh->DrawArgs.size());
        });

    return stats;
}

Entity Engine::CreateRenderableEntity(const std::string& meshName,
    const std::string& materialName,
    XMFLOAT3 position)
{
    mLastCreateError.clear();

    Mesh* mesh = mResourceManager->GetMesh(meshName);
    auto material = mResourceManager->GetMaterial(materialName);

    if (!mesh || !material)
    {
        mLastCreateError = "Mesh or material not found.";
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: mesh or material not found\n");
        return INVALID_ENTITY;
    }

    if (mNextObjectCBIndex >= RenderLimits::MaxObjectCount)
    {
        mLastCreateError = "Object constant buffer capacity reached.";
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: object CB full\n");
        return INVALID_ENTITY;
    }

    const uint32_t newDrawCommands = static_cast<uint32_t>(mesh->DrawArgs.size());
    const uint32_t currentDrawCommands = CountEstimatedDrawCommands(mWorld);
    if (currentDrawCommands + newDrawCommands > RenderLimits::MaxDrawCommandCount)
    {
        mLastCreateError = "Draw command buffer capacity would be exceeded.";
        OutputDebugStringA("[Engine] CreateRenderableEntity failed: draw command buffer full\n");
        return INVALID_ENTITY;
    }

    Entity entity = mWorld.CreateEntity();

    mWorld.AddComponent(entity, TransformComponent{ .position = position });
    mWorld.AddComponent(entity, RenderableComponent{
        .mesh = mesh,
        .material = material,
        .objectCBIndex = mNextObjectCBIndex++
        });

    return entity;
}

void Engine::Shutdown()
{
    mResourceManager->Shutdown();

    PipelineStateManager::Get().Shutdown();
    RootSignatureManager::Get().Shutdown();
    ShaderManager::Get().Shutdown();
}