#pragma once
#include <vector>
#include <DirectXMath.h>

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texcoord;
};

struct Model {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
};

struct FaceVertex {
    int pos = -1;
    int tex = -1;
    int nor = -1;
};