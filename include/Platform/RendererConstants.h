#pragma once

#include <cmath>
#include <cstdint>

namespace Platform
{
    struct CameraConstants
    {
        float fovVertical{ 0.0F };
        float aspectRatio{ 0.0F };
        float nearPlane{ 0.0F };
        float farPlane{ 0.0F };
        float eyePosition[3]{};
        float cameraRight[3]{};
        float cameraUp[3]{};
        float cameraForward[3]{};
        float viewToClip[16]{};
        float viewMatrix[16]{};
        float curViewProj[16]{};
        float prevViewProj[16]{};
        bool hasReprojection{ false };
    };

    [[nodiscard]] inline bool AreCameraConstantsValid(const CameraConstants& value) noexcept
    {
        if (!std::isfinite(value.fovVertical) || value.fovVertical <= 0.0F) {
            return false;
        }
        if (!std::isfinite(value.aspectRatio) || value.aspectRatio <= 0.0F) {
            return false;
        }
        if (!std::isfinite(value.nearPlane) || value.nearPlane <= 0.0F) {
            return false;
        }
        if (!std::isfinite(value.farPlane) || value.farPlane <= value.nearPlane) {
            return false;
        }
        for (const float element : value.viewToClip) {
            if (!std::isfinite(element)) {
                return false;
            }
        }
        if (value.viewToClip[0] == 0.0F || value.viewToClip[5] == 0.0F) {
            return false;
        }
        const float* const axes[3] = { value.cameraRight, value.cameraUp, value.cameraForward };
        for (const float* const axis : axes) {
            const float lengthSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
            if (!std::isfinite(lengthSq) || lengthSq < 1e-12F) {
                return false;
            }
        }
        for (const float component : value.eyePosition) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        return true;
    }
}
