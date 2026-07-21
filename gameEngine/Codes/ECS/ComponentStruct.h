#pragma once
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include "Entity.h"
#include "TransformDirtyTracker.h"
struct Mesh;
struct Material;

using namespace DirectX;


struct TransformComponent {
    XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 rotation{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

    // ObjectCB / 인스턴스 버퍼에 다시 올려야 하는 프레임 수 (보통 gNumFrameResources)
    int dirtyFrames = 0;

    // 엔티티를 알면 MarkDirty(entity) 권장 — dirty 리스트 기반 패치
    void MarkDirty(int frames = 3)
    {
        if (frames > dirtyFrames)
            dirtyFrames = frames;
        mWorldValid = false;
        TransformDirtyTracker::NotifyUnknown();
    }

    void MarkDirty(ECS::Entity entity, int frames = 3)
    {
        if (frames > dirtyFrames)
            dirtyFrames = frames;
        mWorldValid = false;
        TransformDirtyTracker::Notify(entity);
    }

    // position/rotation/scale 변경 후 MarkDirty 호출 전제. 유효하면 재계산 없이 캐시 반환.
    XMMATRIX GetWorldMatrix() const
    {
        if (!mWorldValid)
        {
            const XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
            const XMMATRIX R = XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z);
            const XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
            const XMMATRIX W = S * R * T;
            XMStoreFloat4x4(&mWorldCache, W);
            mWorldValid = true;
            return W;
        }
        return XMLoadFloat4x4(&mWorldCache);
    }

    // designated initializer / aggregate 유지용 (외부에서 직접 쓰지 말 것)
    mutable bool mWorldValid = false;
    mutable XMFLOAT4X4 mWorldCache{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
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