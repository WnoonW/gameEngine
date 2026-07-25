#pragma once

#include <DirectXMath.h>
#include "MathHelper.h"

// Shared free-fly / orbit math used by Editor and Play camera controllers.
namespace FlyCameraMath
{
    inline DirectX::XMVECTOR FlattenHorizForward(
        DirectX::XMVECTOR lookForward, DirectX::XMMATRIX yawRot)
    {
        DirectX::XMFLOAT3 hf{};
        DirectX::XMStoreFloat3(&hf, lookForward);
        hf.y = 0.0f;
        DirectX::XMVECTOR horizForward = DirectX::XMLoadFloat3(&hf);
        const float fwdLenSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(horizForward));
        if (fwdLenSq < 1e-6f)
        {
            return DirectX::XMVector3TransformNormal(
                DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), yawRot);
        }
        return DirectX::XMVector3Normalize(horizForward);
    }

    inline DirectX::XMVECTOR ComputeThirdPersonCameraPosition(
        DirectX::XMVECTOR pivot, float objectYaw, float pitch, float theta, float radius)
    {
        const DirectX::XMMATRIX objectRot = DirectX::XMMatrixRotationY(objectYaw);
        const DirectX::XMMATRIX orbitRot = DirectX::XMMatrixRotationRollPitchYaw(pitch, theta, 0.0f);
        const DirectX::XMMATRIX combined = DirectX::XMMatrixMultiply(orbitRot, objectRot);
        const DirectX::XMVECTOR offsetDir = DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), combined);
        return DirectX::XMVectorAdd(pivot, DirectX::XMVectorScale(offsetDir, radius));
    }

    inline void ExtractPitchYawFromView(
        const DirectX::XMMATRIX& view, float& outPitch, float& outYaw)
    {
        DirectX::XMVECTOR det;
        DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(&det, view);
        DirectX::XMVECTOR forward = DirectX::XMVector3Normalize(
            DirectX::XMVector3TransformNormal(
                DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), invView));

        DirectX::XMFLOAT3 f{};
        DirectX::XMStoreFloat3(&f, forward);
        outPitch = asinf(MathHelper::Clamp(f.y, -1.0f, 1.0f));
        outYaw = atan2f(f.x, f.z);
    }

    inline DirectX::XMMATRIX BuildOrbitView(
        DirectX::XMVECTOR camPos, DirectX::XMVECTOR pivot, DirectX::XMVECTOR worldUp,
        float& outCamX, float& outCamY, float& outCamZ)
    {
        DirectX::XMFLOAT3 p{};
        DirectX::XMStoreFloat3(&p, camPos);
        outCamX = p.x;
        outCamY = p.y;
        outCamZ = p.z;
        return DirectX::XMMatrixLookAtLH(camPos, pivot, worldUp);
    }

    struct KeyAxes
    {
        float fwd = 0.f;
        float strafe = 0.f;
        float ascend = 0.f;
    };

    inline KeyAxes BuildNormalizedAxes(bool w, bool s, bool a, bool d, bool space, bool shift)
    {
        KeyAxes axes;
        axes.fwd = (w ? 1.f : 0.f) - (s ? 1.f : 0.f);
        axes.strafe = (d ? 1.f : 0.f) - (a ? 1.f : 0.f);
        axes.ascend = (space ? 1.f : 0.f) - (shift ? 1.f : 0.f);

        const float horizLenSq = axes.fwd * axes.fwd + axes.strafe * axes.strafe;
        if (horizLenSq > 1.0f)
        {
            const float invLen = 1.0f / sqrtf(horizLenSq);
            axes.fwd *= invLen;
            axes.strafe *= invLen;
        }
        return axes;
    }
}
