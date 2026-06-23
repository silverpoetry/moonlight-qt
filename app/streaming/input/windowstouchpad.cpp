#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include <SDL_syswm.h>

#ifdef Q_OS_WIN32
#include <cmath>
#include <windows.h>

namespace {
typedef BOOL (WINAPI *RegisterTouchpadCapableWindowFn)(HWND hwnd, BOOL touchpadCapable);
typedef BOOL (WINAPI *SkipPointerFrameMessagesFn)(UINT32 pointerId);

constexpr float PINCH_DISTANCE_THRESHOLD = 0.025f;
constexpr float SCROLL_CENTER_THRESHOLD = 0.020f;
constexpr Uint32 PINCH_WHEEL_SUPPRESS_MS = 250;
constexpr Uint32 TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS = 80;

RegisterTouchpadCapableWindowFn pRegisterTouchpadCapableWindow = nullptr;
SkipPointerFrameMessagesFn pSkipPointerFrameMessages = nullptr;
bool s_TouchpadApiLoaded = false;

void loadTouchpadApi()
{
    if (s_TouchpadApiLoaded) {
        return;
    }

    s_TouchpadApiLoaded = true;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        user32 = LoadLibraryW(L"user32.dll");
    }

    if (user32 != nullptr) {
        pRegisterTouchpadCapableWindow = reinterpret_cast<RegisterTouchpadCapableWindowFn>(
                    GetProcAddress(user32, "RegisterTouchpadCapableWindow"));
        pSkipPointerFrameMessages = reinterpret_cast<SkipPointerFrameMessagesFn>(
                    GetProcAddress(user32, "SkipPointerFrameMessages"));
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Windows touchpad API: register=%s skip=%s",
                pRegisterTouchpadCapableWindow ? "yes" : "no",
                pSkipPointerFrameMessages ? "yes" : "no");
}
}
#endif

void SdlInputHandler::registerTouchpadWindow()
{
#ifdef Q_OS_WIN32
    if (m_Window == nullptr || m_TouchpadWindowRegistered) {
        return;
    }

    loadTouchpadApi();
    if (pRegisterTouchpadCapableWindow == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "RegisterTouchpadCapableWindow is unavailable");
        return;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(m_Window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to get HWND for touchpad registration");
        return;
    }

    if (pRegisterTouchpadCapableWindow(info.info.win.window, TRUE)) {
        m_TouchpadWindowRegistered = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Registered SDL window for native touchpad pointer input");
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "RegisterTouchpadCapableWindow failed: %lu",
                    GetLastError());
    }
#endif
}

bool SdlInputHandler::handleSystemWindowEvent(SDL_SysWMmsg* msg)
{
#ifdef Q_OS_WIN32
    if (msg == nullptr || msg->subsystem != SDL_SYSWM_WINDOWS) {
        return false;
    }

    const UINT message = msg->msg.win.msg;
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        if ((LOWORD(msg->msg.win.wParam) & MK_CONTROL) &&
                m_TouchpadSuppressCtrlWheelUntil != 0 &&
                static_cast<Sint32>(m_TouchpadSuppressCtrlWheelUntil - SDL_GetTicks()) > 0) {
            m_TouchpadSuppressNextCtrlWheel = true;
        }
        return false;
    }

    if (message != WM_POINTERDOWN &&
            message != WM_POINTERUPDATE &&
            message != WM_POINTERUP &&
            message != WM_POINTERCAPTURECHANGED) {
        return false;
    }

    const UINT32 pointerId = GET_POINTERID_WPARAM(msg->msg.win.wParam);

    POINTER_INPUT_TYPE pointerType = PT_POINTER;
    if (!GetPointerType(pointerId, &pointerType) || pointerType != PT_TOUCHPAD) {
        return false;
    }

    if (message == WM_POINTERCAPTURECHANGED) {
        cancelNativeTouchpadContacts();
        return true;
    }

    if (!isCaptureActive()) {
        return true;
    }

    UINT32 pointerCount = 0;
    GetPointerFrameInfo(pointerId, &pointerCount, nullptr);
    if (pointerCount == 0 || pointerCount > MAX_TOUCH_COUNT) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Invalid touchpad pointer frame count: %u",
                    pointerCount);
        return true;
    }

    POINTER_INFO contacts[MAX_TOUCH_COUNT];
    if (!GetPointerFrameInfo(pointerId, &pointerCount, contacts)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GetPointerFrameInfo failed for PT_TOUCHPAD: %lu",
                    GetLastError());
        return true;
    }

    if (pointerCount > 0 && contacts[0].frameId == m_TouchpadLastFrameId) {
        return true;
    }
    if (pointerCount > 0) {
        m_TouchpadLastFrameId = contacts[0].frameId;
    }

    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host does not support native touch events; ignoring native touchpad frame");
        return true;
    }

    bool hasNewContact = false;
    for (UINT32 i = 0; i < pointerCount; i++) {
        const POINTER_INFO& pointerInfo = contacts[i];
        if ((pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) &&
                (message == WM_POINTERDOWN || (pointerInfo.pointerFlags & POINTER_FLAG_NEW))) {
            hasNewContact = true;
            break;
        }
    }

    if (hasNewContact &&
            (m_TouchpadGestureTracking || m_TouchpadNativeGestureActive || m_TouchpadScrollGestureActive)) {
        cancelNativeTouchpadContacts();
        m_TouchpadLastFrameId = contacts[0].frameId;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Reset stale native touchpad gesture on new contact");
    }

    bool framePresent[MAX_FINGERS] = {};
    float frameX[MAX_FINGERS] = {};
    float frameY[MAX_FINGERS] = {};
    bool frameSeen[MAX_FINGERS] = {};

    for (UINT32 i = 0; i < pointerCount; i++) {
        const POINTER_INFO& pointerInfo = contacts[i];
        const UINT32 contactId = pointerInfo.pointerId;
        const int slot = static_cast<int>(contactId % MAX_FINGERS);

        if (slot < 0 || slot >= MAX_FINGERS) {
            continue;
        }

        framePresent[slot] = (pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        if (!framePresent[slot] && !m_TouchpadContactDown[slot]) {
            continue;
        }
        frameSeen[slot] = true;

        RECT deviceRect;
        RECT displayRect;
        if (!GetPointerDeviceRects(pointerInfo.sourceDevice, &deviceRect, &displayRect) ||
                deviceRect.right == deviceRect.left ||
                deviceRect.bottom == deviceRect.top) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GetPointerDeviceRects failed for PT_TOUCHPAD: %lu",
                        GetLastError());
            continue;
        }

        framePresent[slot] = (pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        frameX[slot] = qBound(0.0f,
                              static_cast<float>(pointerInfo.ptHimetricLocation.x - deviceRect.left) /
                                   static_cast<float>(deviceRect.right - deviceRect.left),
                              1.0f);
        frameY[slot] = qBound(0.0f,
                              static_cast<float>(pointerInfo.ptHimetricLocation.y - deviceRect.top) /
                                   static_cast<float>(deviceRect.bottom - deviceRect.top),
                              1.0f);
        if (!framePresent[slot] && m_TouchpadHavePosition[slot]) {
            frameX[slot] = m_TouchpadX[slot];
            frameY[slot] = m_TouchpadY[slot];
        }
    }

    const bool twoFingersPresent = framePresent[0] && framePresent[1];
    const bool twoFingersTracked = m_TouchpadHavePosition[0] && m_TouchpadHavePosition[1];
    const Uint32 now = SDL_GetTicks();
    const bool nativeGestureWasActive = m_TouchpadNativeGestureActive;

    if (twoFingersPresent) {
        const float centerX = (frameX[0] + frameX[1]) * 0.5f;
        const float centerY = (frameY[0] + frameY[1]) * 0.5f;
        const float distanceX = frameX[0] - frameX[1];
        const float distanceY = frameY[0] - frameY[1];
        const float distance = std::sqrt(distanceX * distanceX + distanceY * distanceY);

        if (!m_TouchpadGestureTracking || !twoFingersTracked) {
            m_TouchpadGestureTracking = true;
            m_TouchpadNativeGestureActive = false;
            m_TouchpadScrollGestureActive = false;
            m_TouchpadGestureStartCenterX = centerX;
            m_TouchpadGestureStartCenterY = centerY;
            m_TouchpadGestureStartDistance = distance;
        }
        else if (!m_TouchpadNativeGestureActive && !m_TouchpadScrollGestureActive) {
            const float centerDeltaX = centerX - m_TouchpadGestureStartCenterX;
            const float centerDeltaY = centerY - m_TouchpadGestureStartCenterY;
            const float centerDelta = std::sqrt(centerDeltaX * centerDeltaX + centerDeltaY * centerDeltaY);
            const float distanceDelta = std::fabs(distance - m_TouchpadGestureStartDistance);

            if (distanceDelta >= PINCH_DISTANCE_THRESHOLD &&
                    distanceDelta > centerDelta * 1.35f) {
                m_TouchpadNativeGestureActive = true;
                m_TouchpadSuppressWheelUntil = now + PINCH_WHEEL_SUPPRESS_MS;
                m_TouchpadSuppressCtrlWheelUntil = now + TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS;
                m_TouchpadLoggedSuppressedWheel = false;
                m_TouchpadLoggedSuppressedCtrlWheel = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Native touchpad pinch selected distanceDelta=%.4f centerDelta=%.4f",
                            distanceDelta, centerDelta);
            }
            else if (centerDelta >= SCROLL_CENTER_THRESHOLD &&
                     centerDelta > distanceDelta * 1.35f) {
                m_TouchpadScrollGestureActive = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Native touchpad scroll ignored distanceDelta=%.4f centerDelta=%.4f",
                            distanceDelta, centerDelta);
            }
        }

        if (m_TouchpadNativeGestureActive) {
            m_TouchpadSuppressWheelUntil = now + PINCH_WHEEL_SUPPRESS_MS;
            m_TouchpadSuppressCtrlWheelUntil = now + TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS;
        }
    }
    else if (m_TouchpadGestureTracking) {
        if (m_TouchpadNativeGestureActive) {
            m_TouchpadSuppressCtrlWheelUntil = now + TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS;
            m_TouchpadLoggedSuppressedCtrlWheel = false;
            cancelNativeTouchpadContacts();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Ended native touchpad pinch and cleared gesture state");
            if (pSkipPointerFrameMessages != nullptr) {
                pSkipPointerFrameMessages(pointerId);
            }
            return true;
        }
        m_TouchpadGestureTracking = false;
        m_TouchpadNativeGestureActive = false;
        m_TouchpadScrollGestureActive = false;
    }

    if (m_TouchpadNativeGestureActive || nativeGestureWasActive) {
        for (int slot = 0; slot < MAX_FINGERS; slot++) {
            if (!framePresent[slot] && !m_TouchpadContactDown[slot]) {
                continue;
            }

            uint8_t eventType;
            if (framePresent[slot] && !m_TouchpadContactDown[slot]) {
                eventType = LI_TOUCH_EVENT_DOWN;
            }
            else if (framePresent[slot]) {
                eventType = LI_TOUCH_EVENT_MOVE;
            }
            else {
                eventType = LI_TOUCH_EVENT_UP;
            }

            LiSendTouchEvent(eventType, slot + 1, frameX[slot], frameY[slot], 1.0f, 0.0f, 0.0f, LI_ROT_UNKNOWN);
            m_TouchpadContactDown[slot] = framePresent[slot];
        }
    }
    else {
        bool hadContact = false;
        for (int slot = 0; slot < MAX_FINGERS; slot++) {
            if (m_TouchpadContactDown[slot]) {
                hadContact = true;
                m_TouchpadContactDown[slot] = false;
            }
        }

        if (hadContact) {
            LiSendTouchEvent(LI_TOUCH_EVENT_CANCEL_ALL, 0, 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, LI_ROT_UNKNOWN);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Cancelled native touchpad contacts after non-pinch gesture");
        }
    }

    for (int slot = 0; slot < MAX_FINGERS; slot++) {
        if (!frameSeen[slot] && !framePresent[slot]) {
            continue;
        }

        m_TouchpadHavePosition[slot] = framePresent[slot];
        if (framePresent[slot]) {
            m_TouchpadX[slot] = frameX[slot];
            m_TouchpadY[slot] = frameY[slot];
        }
    }

    if (pSkipPointerFrameMessages != nullptr) {
        pSkipPointerFrameMessages(pointerId);
    }

    return true;
#else
    Q_UNUSED(msg);
    return false;
#endif
}

void SdlInputHandler::cancelNativeTouchpadContacts()
{
    bool hadContact = false;
    for (int slot = 0; slot < MAX_FINGERS; slot++) {
        if (m_TouchpadContactDown[slot]) {
            hadContact = true;
            m_TouchpadContactDown[slot] = false;
        }
    }

    if (hadContact && (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
        LiSendTouchEvent(LI_TOUCH_EVENT_CANCEL_ALL, 0, 0.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, LI_ROT_UNKNOWN);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Cancelled native touchpad contacts");
    }

    SDL_zero(m_TouchpadHavePosition);
    SDL_zero(m_TouchpadX);
    SDL_zero(m_TouchpadY);
    m_TouchpadGestureTracking = false;
    m_TouchpadNativeGestureActive = false;
    m_TouchpadScrollGestureActive = false;
    m_TouchpadSuppressWheelUntil = 0;
    m_TouchpadLoggedSuppressedWheel = false;
    m_TouchpadLastFrameId = 0;
    m_TouchpadSuppressNextCtrlWheel = false;
}
