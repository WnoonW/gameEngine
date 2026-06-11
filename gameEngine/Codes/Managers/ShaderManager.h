#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <unordered_map>
#include <string>

class ShaderManager
{
public:
    static ShaderManager& Get();

    void Initialize();
    void Shutdown();

    // 셰이더 컴파일 또는 캐시 반환
    Microsoft::WRL::ComPtr<ID3DBlob> GetShader(
        const std::wstring& filePath,
        const std::string& entryPoint,
        const std::string& target,
        const D3D_SHADER_MACRO* defines = nullptr);

    // 편의 함수
    Microsoft::WRL::ComPtr<ID3DBlob> GetVertexShader(const std::wstring& filePath, const std::string& entryPoint = "VS");
    Microsoft::WRL::ComPtr<ID3DBlob> GetPixelShader(const std::wstring& filePath, const std::string& entryPoint = "PS");

private:
    ShaderManager() = default;

    std::wstring MakeKey(const std::wstring& filePath,
        const std::string& entryPoint,
        const std::string& target,
        const D3D_SHADER_MACRO* defines);

    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID3DBlob>> mShaderCache;
};