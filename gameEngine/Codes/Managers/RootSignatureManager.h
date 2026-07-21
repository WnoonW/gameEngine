#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <unordered_map>
#include "d3dUtil.h"
using namespace DirectX;
using Microsoft::WRL::ComPtr;

enum class RootSignatureType
{
    Scene,          // 일반 오브젝트 렌더링
    ImGui,          // ImGui 전용
    Shadow,         // Shadow Map
    IndirectBuild,  // Path3: cull/compact/build commands
    HiZBuild,       // Path3: hierarchical-Z pyramid
    ComposeWorld,   // Step F1: TRS → world matrix
};

class RootSignatureManager
{
public:
    static RootSignatureManager& Get();

    void Initialize(ID3D12Device* device);
    void Shutdown();

    ID3D12RootSignature* GetRootSignature(RootSignatureType type);
    ID3D12CommandSignature* GetSceneCommandSignature();

private:
    RootSignatureManager() = default;
    ~RootSignatureManager() = default;

    void CreateSceneRootSignature();
    void CreateIndirectBuildRootSignature();
    void CreateHiZBuildRootSignature();
    void CreateComposeWorldRootSignature();
    void CreateSceneCommandSignature();

    ID3D12Device* mDevice = nullptr;
    std::unordered_map<RootSignatureType, Microsoft::WRL::ComPtr<ID3D12RootSignature>> mRootSignatures;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mSceneCommandSignature;
};
