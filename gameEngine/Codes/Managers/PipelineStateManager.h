#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <unordered_map>
#include <string>
#include <functional>
#include "d3dUtil.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;
struct PSOKey
{
    std::string shaderName;                          // 셰이더 이름 (고유 식별자)
    D3D12_BLEND_DESC blendDesc = {};
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    bool operator==(const PSOKey& other) const;
};

// PSOKey 해시 함수
namespace std
{
    template<>
    struct hash<PSOKey>
    {
        size_t operator()(const PSOKey& key) const;
    };
}

class PipelineStateManager
{
public:
    static PipelineStateManager& Get();

    void Initialize(ID3D12Device* device);
    void Shutdown();

    // PSO를 가져오거나 없으면 생성
    ID3D12PipelineState* GetOrCreatePSO(
        const PSOKey& key,
        ID3D12RootSignature* rootSignature,
        ID3D12Device* device = nullptr);   // device가 nullptr이면 내부 mDevice 사용

private:
    PipelineStateManager() = default;

    ID3D12Device* mDevice = nullptr;
    std::unordered_map<PSOKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOCache;
};