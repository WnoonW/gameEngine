#pragma once
#include <DirectXMath.h>
#include <DirectXCollision.h>
struct Mesh;
struct Material;

using namespace DirectX;


struct TransformComponent {
    XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 rotation{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

    // ObjectCB / 인스턴스 버퍼에 다시 올려야 하는 프레임 수 (보통 gNumFrameResources)
    int dirtyFrames = 0;

    void MarkDirty(int frames = 3) { dirtyFrames = frames; }

    XMMATRIX GetWorldMatrix() const {
        XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
        XMMATRIX R = XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z);
        XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
        return S * R * T;
    }
};


struct RenderableComponent {
    Mesh* mesh = nullptr;
    uint32_t objectCBIndex = 0;
    bool visible = true;
};


struct CameraComponent {
    float fov = DirectX::XM_PIDIV4;      // 45도 (기본값)
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    bool useWindowAspect = true;         // true면 창 크기에 맞춰 aspect 자동 계산
    float aspectRatio = 16.0f / 9.0f;    // useWindowAspect가 false일 때 사용
    bool isMainCamera = true;            // 여러 카메라 중 메인 카메라 구분용
};

struct BoundsComponent {
    DirectX::BoundingBox localBounds;   // per-object union of all submeshes (local space)
    DirectX::BoundingBox worldBounds;   // transformed to world space
};

struct GravityComponent {
    bool enabled = true;
    float strength = 9.81f;
    XMFLOAT3 velocity{ 0.0f, 0.0f, 0.0f };
};

struct CollisionComponent {
    bool enabled = true;
    bool isStatic = false;
    float restitution = 0.0f;
};

struct SelectedComponent {
    // Editor tag: this entity is the active viewport selection.
};