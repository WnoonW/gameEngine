#pragma once
#include <string>
#include <wrl.h>
#include "d3dx12.h"
#include "modelStruct.h"
#include <filesystem>

bool ResourceLoad(const std::wstring& filepath);

bool TextureLoad(const std::wstring& filepath, Microsoft::WRL::ComPtr<ID3D12Resource>& texture, Microsoft::WRL::ComPtr<ID3D12Resource>& UploadHeap, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* commandQueue);

bool MeshLoad(const std::filesystem::path& filepath, Model& outModel);
