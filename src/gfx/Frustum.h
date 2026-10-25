#pragma once

#include <DirectXMath.h>

#include <algorithm>

namespace ex::gfx {

// Six frustum planes extracted from a row-vector view-projection matrix.
//
// DirectX::BoundingFrustum expects a pure projection matrix; feeding it the
// combined view-projection yields a wrong, camera-angle-dependent frustum that
// culls objects which are clearly on screen. Extracting the planes directly
// avoids that pitfall and is cheap enough for a per-frame test.
struct FrustumPlanes {
    DirectX::XMFLOAT4 planes[6]{};

    void FromViewProj(const DirectX::XMFLOAT4X4& m) {
        using namespace DirectX;
        // Columns of the row-major matrix hold the plane coefficients.
        const XMVECTOR c0 = XMVectorSet(m._11, m._21, m._31, m._41);
        const XMVECTOR c1 = XMVectorSet(m._12, m._22, m._32, m._42);
        const XMVECTOR c2 = XMVectorSet(m._13, m._23, m._33, m._43);
        const XMVECTOR c3 = XMVectorSet(m._14, m._24, m._34, m._44);
        const XMVECTOR raw[6] = {c3 + c0, c3 - c0, c3 + c1, c3 - c1, c2, c3 - c2};
        for (int i = 0; i < 6; ++i) {
            const float length = XMVectorGetX(XMVector3Length(raw[i]));
            XMStoreFloat4(&planes[i],
                          XMVectorScale(raw[i], 1.0f / std::max(length, 1e-6f)));
        }
    }

    bool ContainsSphere(const DirectX::XMFLOAT3& center, float radius) const {
        using namespace DirectX;
        const XMVECTOR c = XMLoadFloat3(&center);
        for (int i = 0; i < 6; ++i) {
            const XMVECTOR plane = XMLoadFloat4(&planes[i]);
            const float distance =
                XMVectorGetX(XMVector3Dot(plane, c)) + XMVectorGetW(plane);
            if (distance < -radius) {
                return false;
            }
        }
        return true;
    }
};

} // namespace ex::gfx
