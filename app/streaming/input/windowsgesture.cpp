#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include <SDL_syswm.h>

#ifdef Q_OS_WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

namespace {
constexpr int kCtrlKey = 0xA2; // VK_LCONTROL
constexpr float kWheelDeltaPerZoomRatio = 720.0f;
}
#endif

bool SdlInputHandler::handleSystemWindowEvent(SDL_SysWMmsg* msg)
{
#ifdef Q_OS_WIN32
    if (msg == nullptr || msg->subsystem != SDL_SYSWM_WINDOWS) {
        return false;
    }

    if (msg->msg.win.msg != WM_GESTURE) {
        return false;
    }

    GESTUREINFO gestureInfo = {};
    gestureInfo.cbSize = sizeof(gestureInfo);
    if (!GetGestureInfo(reinterpret_cast<HGESTUREINFO>(msg->msg.win.lParam), &gestureInfo)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GetGestureInfo failed for WM_GESTURE");
        return false;
    }

    const bool handled = gestureInfo.dwID == GID_ZOOM && isCaptureActive();
    if (handled) {
        if (gestureInfo.dwFlags & GF_BEGIN) {
            m_PinchZoomActive = true;
            m_PinchZoomSentModifier = !m_KeysDown.contains(kCtrlKey);
            m_LastPinchZoomArgument = gestureInfo.ullArguments;
            m_PinchWheelRemainder = 0.0f;
            if (m_PinchZoomSentModifier) {
                LiSendKeyboardEvent2(0x8000 | kCtrlKey, KEY_ACTION_DOWN, MODIFIER_CTRL, 0);
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Pinch zoom gesture started");
        }
        else if (gestureInfo.dwFlags & GF_END) {
            releasePinchZoomModifier();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Pinch zoom gesture ended");
        }
        else if (m_PinchZoomActive && m_LastPinchZoomArgument != 0 && gestureInfo.ullArguments != 0) {
            const float ratio = static_cast<float>(gestureInfo.ullArguments) /
                    static_cast<float>(m_LastPinchZoomArgument);
            float wheelDelta = (ratio - 1.0f) * kWheelDeltaPerZoomRatio + m_PinchWheelRemainder;
            short sendDelta = static_cast<short>(wheelDelta);

            m_PinchWheelRemainder = wheelDelta - sendDelta;
            m_LastPinchZoomArgument = gestureInfo.ullArguments;

            if (sendDelta != 0) {
                LiSendHighResScrollEvent(sendDelta);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Pinch zoom mapped to Ctrl+wheel delta: %d",
                            sendDelta);
            }
        }
    }

    CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(msg->msg.win.lParam));
    return handled;
#else
    Q_UNUSED(msg);
    return false;
#endif
}

void SdlInputHandler::releasePinchZoomModifier()
{
#ifdef Q_OS_WIN32
    if (!m_PinchZoomActive) {
        return;
    }

    if (m_PinchZoomSentModifier) {
        LiSendKeyboardEvent2(0x8000 | kCtrlKey, KEY_ACTION_UP, 0, 0);
    }
    m_PinchZoomActive = false;
    m_PinchZoomSentModifier = false;
    m_LastPinchZoomArgument = 0;
    m_PinchWheelRemainder = 0.0f;
#else
    m_PinchZoomActive = false;
    m_PinchZoomSentModifier = false;
    m_LastPinchZoomArgument = 0;
    m_PinchWheelRemainder = 0.0f;
#endif
}
