#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <unordered_map>
#include <vector>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"
#include "constantStruct.h"

using namespace ECS;

class RenderSystem
{
public:
    void renderExecuteIndirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu = {},
        const DepthStencilContext* depthCtx = nullptr);

    CullingStats GetLastCullingStats() const { return mLastCullingStats; }

private:
    struct DrawGroupKey
    {
        Mesh* mesh = nullptr;
        Material* material = nullptr;
        UINT indexCount = 0;
        UINT startIndexLocation = 0;
        INT baseVertexLocation = 0;

        bool operator==(const DrawGroupKey& other) const
        {
            return mesh == other.mesh
                && material == other.material
                && indexCount == other.indexCount
                && startIndexLocation == other.startIndexLocation
                && baseVertexLocation == other.baseVertexLocation;
        }
    };

    struct DrawGroupKeyHash
    {
        size_t operator()(const DrawGroupKey& key) const
        {
            size_t h = std::hash<Mesh*>{}(key.mesh);
            h ^= std::hash<Material*>{}(key.material) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<UINT>{}(key.indexCount) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<UINT>{}(key.startIndexLocation) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<INT>{}(key.baseVertexLocation) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct DrawGroup
    {
        Mesh* mesh = nullptr;
        Material* material = nullptr;
        UINT indexCount = 0;
        UINT startIndexLocation = 0;
        INT baseVertexLocation = 0;
        DirectX::XMFLOAT3 localCenter{};
        DirectX::XMFLOAT3 localExtents{};
        std::vector<DirectX::XMMATRIX> worldMatrices;
    };

    struct BindingRange
    {
        D3D12_GPU_DESCRIPTOR_HANDLE texture{};
        UINT numCommands = 0;
    };

    std::unordered_map<DrawGroupKey, size_t, DrawGroupKeyHash> mGroupIndex;
    std::vector<DrawGroup> mDrawGroups;
    std::vector<GroupDrawData> mGroupDrawList;
    std::vector<BindingRange> mBindingRanges;

    CullingStats mLastCullingStats;
};