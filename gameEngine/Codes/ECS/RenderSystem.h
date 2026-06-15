#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"

using namespace ECS;

struct Mesh;
struct SubmeshGeometry;
struct Material;

struct DrawCall
{
    Entity entity;
    Mesh* mesh;
    const SubmeshGeometry* submesh;     // DrawArgs 정보
    Material* material;

    // 정렬용 키
    ID3D12RootSignature* rootSignature;
    ID3D12PipelineState* pso;
};

class RenderSystem {
public:
    void createCBV(ID3D12Device* device,
        std::vector<std::unique_ptr<FrameResource>>& frameResources,
        int gNumFrameResources,
        DescriptorAllocator& descriptorAllocator,
        Entity entity,
        ECS::World& world);

    void render(ECS::World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    std::vector<std::vector<DescriptorAllocator::DescriptorHandle>> mEntityCBVHandles;
};