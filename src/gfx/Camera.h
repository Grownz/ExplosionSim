#pragma once

#include <DirectXMath.h>

#include <algorithm>

namespace ex::gfx {

// Orbit camera with smooth transitions. World space is left-handed, +Y is up,
// distances are given in metres.
class Camera {
public:
    void SetViewport(int width, int height);

    void Orbit(float dxPixels, float dyPixels);
    void Pan(float dxPixels, float dyPixels);
    void Zoom(float wheelDelta);

    // Smoothly moves the camera to frame a sphere.
    void FrameSphere(const DirectX::XMFLOAT3& center, float radius);
    void SetTarget(const DirectX::XMFLOAT3& center, float distance);
    void SetOrientation(float yawDeg, float pitchDeg);
    void SetFovY(float radians) { fovY_ = radians; }
    void Update(float dt);
    void Snap();

    DirectX::XMMATRIX ViewMatrix() const;
    DirectX::XMMATRIX ProjectionMatrix() const;
    DirectX::XMMATRIX ViewProjectionMatrix() const;

    DirectX::XMFLOAT3 Position() const { return position_; }
    DirectX::XMFLOAT3 Target() const { return target_; }
    // World-space billboard axes (right, up) for the current orientation.
    void Basis(DirectX::XMFLOAT3& right, DirectX::XMFLOAT3& up) const;
    float Distance() const { return distance_; }
    float NearPlane() const { return nearPlane_; }
    float FarPlane() const { return farPlane_; }
    float FovY() const { return fovY_; }

    void SetClipPlanes(float nearPlane, float farPlane);

private:
    DirectX::XMFLOAT3 ComputePosition() const;
    // Clamps the desired target height and pitch so the orbit stays above the
    // ground plane.
    void ClampDesired();

    DirectX::XMFLOAT3 target_{0.0f, 18.0f, 0.0f};
    DirectX::XMFLOAT3 targetDesired_{0.0f, 18.0f, 0.0f};
    DirectX::XMFLOAT3 position_{0.0f, 0.0f, -100.0f};

    float distance_ = 160.0f;
    float distanceDesired_ = 160.0f;
    float yaw_ = 35.0f;
    float yawDesired_ = 35.0f;
    float pitch_ = 12.0f;
    float pitchDesired_ = 12.0f;

    float fovY_ = DirectX::XMConvertToRadians(50.0f);
    float aspect_ = 16.0f / 9.0f;
    float nearPlane_ = 0.5f;
    float farPlane_ = 20000.0f;
    int viewportHeight_ = 900;
};

} // namespace ex::gfx
