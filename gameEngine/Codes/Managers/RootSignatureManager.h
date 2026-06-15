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
    // Transparent, PostProcess 등 나중에 추가
};

class RootSignatureManager
{
public:
    static RootSignatureManager& Get();

    void Initialize(ID3D12Device* device);
    void Shutdown();

    ID3D12RootSignature* GetRootSignature(RootSignatureType type);

private:
    RootSignatureManager() = default;
    ~RootSignatureManager() = default;

    void CreateSceneRootSignature();
    // void CreateImGuiRootSignature();
    // void CreateShadowRootSignature();

    ID3D12Device* mDevice = nullptr;
    std::unordered_map<RootSignatureType, Microsoft::WRL::ComPtr<ID3D12RootSignature>> mRootSignatures;
};