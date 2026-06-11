#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include <vector>
#include "DataLoader.h"

using Microsoft::WRL::ComPtr;

class Material1
{
public:
    Material1();
    ~Material1();

    // DataLoader + DX12 통합 버전
    bool CreateMaterial(ID3D12Device* device,
                        ID3D12CommandQueue* commandQueue,
                        DataLoader& loader,
                        const std::wstring& vsPath,
                        const std::wstring& psPath,
                        const std::vector<std::wstring>& texturePaths = {});

    ID3D12PipelineState* GetPSO() const { return mPSO.Get(); }
    ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }
    ID3D12DescriptorHeap* GetDescriptorHeap() const { return mSrvHeap.Get(); }

private:
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePSO(ID3D12Device* device, ID3DBlob* vsBlob, ID3DBlob* psBlob);

    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12PipelineState> mPSO;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    std::vector<ComPtr<ID3D12Resource>> mTextures;
};
