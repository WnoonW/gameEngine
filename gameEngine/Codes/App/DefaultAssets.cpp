#include "DefaultAssets.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "GeometryGenerator.h"
#include <vector>
#include <string>
#include <Windows.h>

namespace DefaultAssets
{
    void Load(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12CommandQueue* cmdQueue,
        DescriptorAllocator& descriptorAllocator)
    {
        MaterialManager::Get().EnsureMissingTextureMaterial(
            device, cmdList, descriptorAllocator);

        MaterialManager::Get().CreateMaterial("Default", L"Resources/Textures/bricks.dds",
            device, cmdList, cmdQueue, descriptorAllocator);
        MaterialManager::Get().CreateMaterial("Test", L"Resources/Textures/e.png",
            device, cmdList, cmdQueue, descriptorAllocator);

        // Character materials (shared textures per material group)
        const wchar_t* faceTex = L"Resources/Textures/颜.png";
        for (const char* name : { "颜", "颜2", "眉睫", "目", "目光", "白目", "口线", "口舌", "齿", "目影" })
            MaterialManager::Get().CreateMaterial(name, faceTex, device, cmdList, cmdQueue, descriptorAllocator);

        MaterialManager::Get().CreateMaterial("体", L"Resources/Textures/体.png",
            device, cmdList, cmdQueue, descriptorAllocator);
        MaterialManager::Get().CreateMaterial("肌", L"Resources/Textures/体.png",
            device, cmdList, cmdQueue, descriptorAllocator);
        for (const char* name : { "体2", "足", "髮" })
            MaterialManager::Get().CreateMaterial(name, L"Resources/Textures/髮.png",
                device, cmdList, cmdQueue, descriptorAllocator);
        MaterialManager::Get().CreateMaterial("髮+", L"Resources/Textures/spa_h.png",
            device, cmdList, cmdQueue, descriptorAllocator);

        const bool meshOk =
            MeshManager::Get().CreateMesh("bibian", L"Resources/Assets/bibian.obj", device, cmdList)
            && MeshManager::Get().CreateMesh("box", L"Resources/Assets/square.obj", device, cmdList);

        GeometryGenerator geo;
        bool primOk = true;
        primOk &= MeshManager::Get().CreateMeshFromGeometry(
            "cube", geo.CreateBox(1.0f, 1.0f, 1.0f, 0), device, cmdList);
        primOk &= MeshManager::Get().CreateMeshFromGeometry(
            "sphere", geo.CreateSphere(1.0f, 20, 20), device, cmdList);
        primOk &= MeshManager::Get().CreateMeshFromGeometry(
            "geosphere", geo.CreateGeosphere(1.0f, 2), device, cmdList);
        primOk &= MeshManager::Get().CreateMeshFromGeometry(
            "cylinder", geo.CreateCylinder(0.5f, 0.5f, 2.0f, 20, 4), device, cmdList);
        primOk &= MeshManager::Get().CreateMeshFromGeometry(
            "cone", geo.CreateCylinder(0.5f, 0.0f, 2.0f, 20, 4), device, cmdList);
        primOk &= MeshManager::Get().CreateMeshFromGeometry(
            "grid", geo.CreateGrid(10.0f, 10.0f, 10, 10), device, cmdList);

        if (!meshOk || !primOk)
        {
            MessageBoxA(nullptr,
                "Mesh Creation Failed!\n\n"
                "Check that the Resources folder is next to the .exe\n"
                "(Working directory must be the game folder).",
                "Error", MB_OK | MB_ICONERROR);
        }

        // Auto LOD variants for all non-lod base meshes
        auto names = MeshManager::Get().GetLoadedMeshNames();
        std::vector<std::string> bases;
        bases.reserve(names.size());
        for (const auto& n : names)
        {
            if (n.find("_lod") != std::string::npos)
                continue;
            bases.push_back(n);
        }
        for (const auto& n : bases)
        {
            MeshManager::Get().EnsureAutoLodVariants(
                n, device, cmdList, /*forceRebuild=*/true);
        }
    }
}
