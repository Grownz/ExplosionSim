#include "gfx/Camera.h"

#include <cmath>

using namespace DirectX;

namespace ex::gfx {

void Camera::SetViewport(int width, int height) {
    aspect_ = height > 0 ? float(width) / float(height) : 1.0f;
    viewportHeight_ = std::max(1, height);
}

void Camera::Orbit(float dxPixels, float dyPixels) {
    yawDesired_ += dxPixels * 0.28f;
    yaw_ += dxPixels * 0.28f;
    pitchDesired_ = std::clamp(pitchDesired_ + dyPixels * 0.24f, -85.0f, 85.0f);
    pitch_ = std::clamp(pitch_ + dyPixels * 0.24f, -85.0f, 85.0f);
}

void Camera::Pan(float dxPixels, float dyPixels) {
    const float worldPerPixel = 2.0f * distanceDesired_ * std::tan(fovY_ * 0.5f) / float(viewportHeight_);
    const float yawRad = XMConvertToRadians(yaw_);
    const XMVECTOR right = XMVectorSet(std::cos(yawRad), 0.0f, -std::sin(yawRad), 0.0f);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR offset = XMVectorScale(right, -dxPixels * worldPerPixel);
    offset = XMVectorAdd(offset, XMVectorScale(up, dyPixels * worldPerPixel));

    XMVECTOR target = XMLoadFloat3(&targetDesired_);
    target = XMVectorAdd(target, offset);
    XMStoreFloat3(&targetDesired_, target);
}

void Camera::Zoom(float wheelDelta) {
    distanceDesired_ = std::clamp(distanceDesired_ * std::pow(1.12f, -wheelDelta), 2.0f, 200000.0f);
}

void Camera::FrameSphere(const XMFLOAT3& center, float radius) {
    targetDesired_ = center;
    const float fitDistance = radius / std::tan(fovY_ * 0.5f) * 1.35f;
    distanceDesired_ = std::clamp(fitDistance, 5.0f, 200000.0f);
}

void Camera::SetTarget(const XMFLOAT3& center, float distance) {
    targetDesired_ = center;
    distanceDesired_ = std::max(1.0f, distance);
}

void Camera::SetOrientation(float yawDeg, float pitchDeg) {
    yaw_ = yawDesired_ = yawDeg;
    pitch_ = pitchDesired_ = std::clamp(pitchDeg, -89.0f, 89.0f);
}

void Camera::Update(float dt) {
    const float k = 1.0f - std::exp(-dt * 8.0f);
    const float kFast = 1.0f - std::exp(-dt * 20.0f);

    yaw_ += (yawDesired_ - yaw_) * kFast;
    pitch_ += (pitchDesired_ - pitch_) * kFast;
    distance_ += (distanceDesired_ - distance_) * k;

    const XMVECTOR target = XMLoadFloat3(&target_);
    const XMVECTOR targetGoal = XMLoadFloat3(&targetDesired_);
    XMStoreFloat3(&target_, XMVectorLerp(target, targetGoal, k));

    // Keep the clip range generous but stable for very different scene scales.
    nearPlane_ = std::max(0.1f, distance_ * 0.001f);
    farPlane_ = std::max(5000.0f, distance_ * 40.0f);

    position_ = ComputePosition();
}

void Camera::Snap() {
    yaw_ = yawDesired_;
    pitch_ = pitchDesired_;
    distance_ = distanceDesired_;
    target_ = targetDesired_;
    nearPlane_ = std::max(0.1f, distance_ * 0.001f);
    farPlane_ = std::max(5000.0f, distance_ * 40.0f);
    position_ = ComputePosition();
}

void Camera::Basis(XMFLOAT3& right, XMFLOAT3& up) const {
    const float yawRad = XMConvertToRadians(yaw_);
    const float pitchRad = XMConvertToRadians(pitch_);
    const float cy = std::cos(yawRad);
    const float sy = std::sin(yawRad);
    const float cp = std::cos(pitchRad);
    const float sp = std::sin(pitchRad);

    right = XMFLOAT3(-cy, 0.0f, sy);
    up = XMFLOAT3(-sp * sy, cp, -sp * cy);
}

XMFLOAT3 Camera::ComputePosition() const {    const float yawRad = XMConvertToRadians(yaw_);
    const float pitchRad = XMConvertToRadians(pitch_);
    const float cp = std::cos(pitchRad);

    XMFLOAT3 pos;
    pos.x = target_.x + distance_ * cp * std::sin(yawRad);
    pos.y = target_.y + distance_ * std::sin(pitchRad);
    pos.z = target_.z + distance_ * cp * std::cos(yawRad);
    return pos;
}

XMMATRIX Camera::ViewMatrix() const {
    const XMVECTOR eye = XMLoadFloat3(&position_);
    const XMVECTOR focus = XMLoadFloat3(&target_);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookAtLH(eye, focus, up);
}

XMMATRIX Camera::ProjectionMatrix() const {
    return XMMatrixPerspectiveFovLH(fovY_, aspect_, nearPlane_, farPlane_);
}

XMMATRIX Camera::ViewProjectionMatrix() const {
    return ViewMatrix() * ProjectionMatrix();
}

void Camera::SetClipPlanes(float nearPlane, float farPlane) {
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
}

} // namespace ex::gfx
