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
    std::string shaderName;                          
    D3D12_BLEND_DESC blendDesc = {};
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    bool operator==(const PSOKey& other) const;

    bool isComputeShader = false;
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


    ID3D12PipelineState* GetOrCreatePSO(
        const PSOKey& key,
        ID3D12RootSignature* rootSignature,
        ID3D12Device* device = nullptr);

    ID3D12PipelineState* GetOrCreateComputePSO(
        const PSOKey& key,
        ID3D12RootSignature* rootSignature,
        ID3D12Device* device = nullptr);

private:
    PipelineStateManager() = default;

    ID3D12Device* mDevice = nullptr;
    std::unordered_map<PSOKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOCache;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mComputePSOCache;
    // ==================== Indirect Drawing 지원 (Phase 1) ====================
private:
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDrawIndexedIndirectSignature;

public:
    void InitializeCommandSignatures(ID3D12Device* device);
    ID3D12CommandSignature* GetDrawIndexedIndirectSignature() const;

    // ==================== Phase 2: Compute Shader 지원 ====================
public:
    void InitializeComputePipeline(ID3D12Device* device);
    ID3D12RootSignature* GetComputeRootSignature() const;
    ID3D12PipelineState* GetFrustumCullingPSO() const;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mFrustumCullingPSO;
};