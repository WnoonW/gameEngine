#pragma once
#include <string>
#include <DirectXMath.h>

#include "World.h"
#include "RenderSystem.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"
#include "constantStruct.h"
#include "ResourceManager.h"
#include "Camera.h"

using namespace DirectX;

class Engine
{
public:
    Engine();
    ~Engine();

    // 초기화
    bool Initialize(ID3D12Device* device,
        std::vector<std::unique_ptr<FrameResource>>& frameResources,
        int gNumFrameResources,
        DescriptorAllocator& descriptorAllocator);

    // 매 프레임 업데이트 (나중에 시스템들 추가 예정)
    void Update();

    Camera& GetCamera() { return mCamera; }
    const Camera& GetCamera() const { return mCamera; }

    void RenderScene(ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        int currentFrameIndex,
        const DepthStencilContext* depthCtx);

    // 엔티티 생성 (이름 기반)
    Entity CreateRenderableEntity(const std::string& meshName,
        const std::string& materialName,
        XMFLOAT3 position = { 0.0f, 0.0f, 0.0f });

    MeshInstanceStats CollectMeshInstanceStats();
    CullingStats GetLastCullingStats() const;

    void Shutdown();
private:
    ECS::World mWorld;
    RenderSystem mRenderSystem;
    ResourceManager* mResourceManager = nullptr;

    ID3D12Device* mDevice = nullptr;
    std::vector<std::unique_ptr<FrameResource>>* mFrameResources = nullptr;
    int mGNumFrameResources = 0;
    DescriptorAllocator* mDescriptorAllocator = nullptr;

    uint32_t mNextObjectCBIndex = 0;
    std::string mLastCreateError;
    Camera mCamera;

    D3D12_GPU_DESCRIPTOR_HANDLE mDepthSrvGpu{};  // set from app for occlusion

public:
    void SetDepthSrvGpu(D3D12_GPU_DESCRIPTOR_HANDLE srv) { mDepthSrvGpu = srv; }
};