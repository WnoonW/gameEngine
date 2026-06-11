#pragma once
#include <DirectXMath.h>
#include <string>
using namespace DirectX;


struct TransformComponent {
    XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 rotation{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

    XMMATRIX GetWorldMatrix() const {
        XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z);
        XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
        return S * R * T;
    }
};


struct RenderableComponent {
    std::string meshName;   // MeshManager::Get().GetMesh()
    std::string materialName;     // MatarialManager::Get().GetMatarial()
    uint32_t    objectCBIndex = 0;         // FrameResource ObjectCB 인덱스
    bool        visible = true;
};


struct CameraComponent {
    float fov = DirectX::XM_PIDIV4;      // 45도 (기본값)
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    bool useWindowAspect = true;         // true면 창 크기에 맞춰 aspect 자동 계산
    float aspectRatio = 16.0f / 9.0f;    // useWindowAspect가 false일 때 사용
    bool isMainCamera = true;            // 여러 카메라 중 메인 카메라 구분용
};