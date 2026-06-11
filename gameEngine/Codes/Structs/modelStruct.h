#pragma once
#include <vector>
#include <DirectXMath.h>

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texcoord;
};

struct SubMesh
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::string materialName;           // usemtl 이름 저장
};

struct Model {
    std::vector<SubMesh> submeshes;
};


struct FaceVertex {
    int pos = -1;
    int tex = -1;
    int nor = -1;
};