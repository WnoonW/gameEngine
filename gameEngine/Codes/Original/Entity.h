#pragma once
#include "Mesh.h"
#include "Material.h"
#include <d3d12.h>
#include <string>

class Entity
{
public:
    Entity();
    ~Entity();

    void SetMesh(Mesh* mesh);
    void SetMaterial(Material1* material);

    Mesh* GetMesh() const { return mMesh; }
    Material1* GetMaterial() const { return mMaterial; }
    uint32_t GetID() const { return mID; }

    void Draw(ID3D12GraphicsCommandList* cmdList);

private:
    uint32_t mID = 0;
    Mesh* mMesh = nullptr;
    Material1* mMaterial = nullptr;

    static uint32_t sNextID;
};