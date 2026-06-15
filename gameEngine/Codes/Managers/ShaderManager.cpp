#include "ShaderManager.h"
#include "d3dUtil.h"
#include <sstream>

ShaderManager& ShaderManager::Get()
{
    static ShaderManager instance;
    return instance;
}

void ShaderManager::Initialize()
{}

void ShaderManager::Shutdown()
{
    mShaderCache.clear();
}

// 키를 wstring으로 통일 (변환 경고 제거)
std::wstring ShaderManager::MakeKey(const std::wstring& filePath,
    const std::string& entryPoint,
    const std::string& target,
    const D3D_SHADER_MACRO* defines)
{
    std::wstring wEntryPoint(entryPoint.begin(), entryPoint.end());
    std::wstring wTarget(target.begin(), target.end());

    std::wstringstream wss;
    wss << filePath << L"_" << wEntryPoint << L"_" << wTarget;

    if (defines)
        wss << L"_with_defines";

    return wss.str();
}

ComPtr<ID3DBlob> ShaderManager::GetShader(
    const std::wstring& filePath,
    const std::string& entryPoint,
    const std::string& target,
    const D3D_SHADER_MACRO* defines)
{
    std::wstring key = MakeKey(filePath, entryPoint, target, defines);

    auto it = mShaderCache.find(key);
    if (it != mShaderCache.end())
        return it->second;

    ComPtr<ID3DBlob> shaderBlob =
        d3dUtil::CompileShader(filePath.c_str(), defines, entryPoint.c_str(), target.c_str());

    if (!shaderBlob)
    {
        OutputDebugStringA("Shader Compile Failed!\n");
        return nullptr;
    }

    mShaderCache[key] = shaderBlob;
    return shaderBlob;
}

ComPtr<ID3DBlob> ShaderManager::GetVertexShader(const std::wstring& filePath, const std::string& entryPoint)
{
    return GetShader(filePath, entryPoint, "vs_5_0");
}

ComPtr<ID3DBlob> ShaderManager::GetPixelShader(const std::wstring& filePath, const std::string& entryPoint)
{
    return GetShader(filePath, entryPoint, "ps_5_0");
}