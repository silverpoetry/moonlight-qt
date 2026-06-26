#include "input.h"

#include <Limelight.h>

#ifdef Q_OS_WIN32

#include <cmath>
#include <windows.h>

namespace {
typedef BOOL (WINAPI *RegisterTouchpadCapableWindowFn)(HWND hwnd, BOOL touchpadCapable);
typedef BOOL (WINAPI *SkipPointerFrameMessagesFn)(UINT32 pointerId);

constexpr float PINCH_DISTANCE_THRESHOLD = 0.025f;
constexpr float PINCH_CTRL_WHEEL_GUARD_THRESHOLD = 0.010f;
constexpr float SCROLL_CENTER_THRESHOLD = 0.020f;
constexpr Uint32 PINCH_WHEEL_SUPPRESS_MS = 250;
constexpr Uint32 TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS = 180;
constexpr wchar_t MESSAGE_HOOK_HANDLER_PROP[] = L"MoonlightNativeMessageHandler";
constexpr wchar_t MESSAGE_HOOK_PREV_PROC_PROP[] = L"MoonlightNativeMessagePrevProc";

RegisterTouchpadCapableWindowFn pRegisterTouchpadCapableWindow = nullptr;
SkipPointerFrameMessagesFn pSkipPointerFrameMessages = nullptr;
bool s_TouchpadApiLoaded = false;

LRESULT CALLBACK nativeMessageHookWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto handler = reinterpret_cast<SdlInputHandler*>(
                GetPropW(hwnd, MESSAGE_HOOK_HANDLER_PROP));
    auto prevProc = reinterpret_cast<WNDPROC>(
                GetPropW(hwnd, MESSAGE_HOOK_PREV_PROC_PROP));

    if (handler != nullptr && handler->handleNativeTouchpadWheelMessage(message, wParam)) {
        return 0;
    }

    if (prevProc != nullptr) {
        return CallWindowProcW(prevProc, hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

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
}
}

#endif

void SdlInputHandler::installWindowsMessageHook(void* hwndValue)
{
#ifdef Q_OS_WIN32
    auto hwnd = static_cast<HWND>(hwndValue);
    if (!m_EnableTouchpadGestures || hwnd == nullptr || m_WindowsMessageHookHwnd == hwnd) {
        return;
    }

    restoreWindowsMessageHook();

    auto prevProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (prevProc == nativeMessageHookWndProc) {
        m_WindowsMessageHookHwnd = hwnd;
        m_WindowsMessageHookPrevWndProc =
                GetPropW(hwnd, MESSAGE_HOOK_PREV_PROC_PROP);
        SetPropW(hwnd, MESSAGE_HOOK_HANDLER_PROP, this);
        return;
    }

    SetPropW(hwnd, MESSAGE_HOOK_HANDLER_PROP, this);
    SetPropW(hwnd, MESSAGE_HOOK_PREV_PROC_PROP, prevProc);

    SetLastError(0);
    const LONG_PTR oldProc = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                               reinterpret_cast<LONG_PTR>(nativeMessageHookWndProc));
    if (oldProc == 0 && GetLastError() != 0) {
        RemovePropW(hwnd, MESSAGE_HOOK_HANDLER_PROP);
        RemovePropW(hwnd, MESSAGE_HOOK_PREV_PROC_PROP);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to install native Windows touchpad hook: %lu",
                    GetLastError());
        return;
    }

    m_WindowsMessageHookHwnd = hwnd;
    m_WindowsMessageHookPrevWndProc =
            reinterpret_cast<void*>(oldProc != 0 ? oldProc : reinterpret_cast<LONG_PTR>(prevProc));
#else
    Q_UNUSED(hwndValue);
#endif
}

void SdlInputHandler::restoreWindowsMessageHook()
{
#ifdef Q_OS_WIN32
    auto hwnd = static_cast<HWND>(m_WindowsMessageHookHwnd);
    auto prevProc = reinterpret_cast<WNDPROC>(m_WindowsMessageHookPrevWndProc);
    if (hwnd != nullptr) {
        auto currentProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        if (prevProc != nullptr && currentProc == nativeMessageHookWndProc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prevProc));
        }
        RemovePropW(hwnd, MESSAGE_HOOK_HANDLER_PROP);
        RemovePropW(hwnd, MESSAGE_HOOK_PREV_PROC_PROP);
    }

    m_WindowsMessageHookHwnd = nullptr;
    m_WindowsMessageHookPrevWndProc = nullptr;
#endif
}

bool SdlInputHandler::handleNativeTouchpadWheelMessage(unsigned int message, uintptr_t wParam)
{
#ifdef Q_OS_WIN32
    if (!m_EnableTouchpadGestures) {
        return false;
    }

    if (message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) {
        return false;
    }

    if (!isTouchpadCtrlFallbackActive() ||
            (LOWORD(wParam) & MK_CONTROL) == 0 ||
            m_TouchpadSuppressCtrlWheelUntil == 0) {
        return false;
    }

    const Sint32 remainingMs =
            static_cast<Sint32>(m_TouchpadSuppressCtrlWheelUntil - SDL_GetTicks());
    if (remainingMs <= 0) {
        m_TouchpadSuppressCtrlWheelUntil = 0;
        return false;
    }

    return true;
#else
    Q_UNUSED(message);
    Q_UNUSED(wParam);
    return false;
#endif
}

void SdlInputHandler::registerTouchpadWindow()
{
#ifdef Q_OS_WIN32
    if (!m_EnableTouchpadGestures || m_Window == nullptr) {
        return;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(m_Window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return;
    }

    installWindowsMessageHook(info.info.win.window);

    if (m_TouchpadWindowRegistered) {
        return;
    }

    loadTouchpadApi();
    if (pRegisterTouchpadCapableWindow == nullptr) {
        return;
    }

    if (pRegisterTouchpadCapableWindow(info.info.win.window, TRUE)) {
        m_TouchpadWindowRegistered = true;
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
    if (!m_EnableTouchpadGestures) {
        return false;
    }

    if (msg == nullptr || msg->subsystem != SDL_SYSWM_WINDOWS) {
        return false;
    }

    const UINT message = msg->msg.win.msg;
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        if (isTouchpadCtrlFallbackActive() &&
                (LOWORD(msg->msg.win.wParam) & MK_CONTROL) &&
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
        return false;
    }

    UINT32 pointerCount = 0;
    GetPointerFrameInfo(pointerId, &pointerCount, nullptr);
    if (pointerCount == 0 || pointerCount > MAX_TOUCH_COUNT) {
        return m_TouchpadNativeGestureActive;
    }

    POINTER_INFO contacts[MAX_TOUCH_COUNT];
    if (!GetPointerFrameInfo(pointerId, &pointerCount, contacts)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GetPointerFrameInfo failed for touchpad input: %lu",
                    GetLastError());
        return m_TouchpadNativeGestureActive;
    }

    if (pointerCount > 0 && contacts[0].frameId == m_TouchpadLastFrameId) {
        return m_TouchpadNativeGestureActive;
    }
    if (pointerCount > 0) {
        m_TouchpadLastFrameId = contacts[0].frameId;
    }

    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
        return false;
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

        if (m_TouchpadCachedDevice != pointerInfo.sourceDevice ||
                m_TouchpadCachedDeviceWidth == 0 ||
                m_TouchpadCachedDeviceHeight == 0) {
            RECT deviceRect;
            RECT displayRect;
            if (!GetPointerDeviceRects(pointerInfo.sourceDevice, &deviceRect, &displayRect) ||
                    deviceRect.right == deviceRect.left ||
                    deviceRect.bottom == deviceRect.top) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "GetPointerDeviceRects failed for touchpad input: %lu",
                            GetLastError());
                continue;
            }

            m_TouchpadCachedDevice = pointerInfo.sourceDevice;
            m_TouchpadCachedDeviceLeft = deviceRect.left;
            m_TouchpadCachedDeviceTop = deviceRect.top;
            m_TouchpadCachedDeviceWidth = deviceRect.right - deviceRect.left;
            m_TouchpadCachedDeviceHeight = deviceRect.bottom - deviceRect.top;
        }

        frameX[slot] = qBound(0.0f,
                              static_cast<float>(pointerInfo.ptHimetricLocation.x - m_TouchpadCachedDeviceLeft) /
                                      static_cast<float>(m_TouchpadCachedDeviceWidth),
                              1.0f);
        frameY[slot] = qBound(0.0f,
                              static_cast<float>(pointerInfo.ptHimetricLocation.y - m_TouchpadCachedDeviceTop) /
                                      static_cast<float>(m_TouchpadCachedDeviceHeight),
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

            if (distanceDelta >= PINCH_CTRL_WHEEL_GUARD_THRESHOLD &&
                    distanceDelta > centerDelta * 0.75f) {
                m_TouchpadSuppressCtrlWheelUntil = now + TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS;
            }

            if (distanceDelta >= PINCH_DISTANCE_THRESHOLD &&
                    distanceDelta > centerDelta * 1.35f) {
                m_TouchpadNativeGestureActive = true;
                m_TouchpadSuppressWheelUntil = now + PINCH_WHEEL_SUPPRESS_MS;
                m_TouchpadSuppressCtrlWheelUntil = now + TOUCHPAD_CTRL_WHEEL_SUPPRESS_MS;
            }
            else if (centerDelta >= SCROLL_CENTER_THRESHOLD &&
                     centerDelta > distanceDelta * 1.35f) {
                m_TouchpadScrollGestureActive = true;
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
            cancelNativeTouchpadContacts();
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

            LiSendTouchEvent(eventType, slot + 1, frameX[slot], frameY[slot],
                             1.0f, 0.0f, 0.0f, LI_ROT_UNKNOWN);
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

    if (m_TouchpadNativeGestureActive || nativeGestureWasActive) {
        if (pSkipPointerFrameMessages != nullptr) {
            pSkipPointerFrameMessages(pointerId);
        }
        return true;
    }

    return false;
#else
    Q_UNUSED(msg);
    return false;
#endif
}

bool SdlInputHandler::isTouchpadCtrlFallbackActive() const
{
#ifdef Q_OS_WIN32
    if (!m_EnableTouchpadGestures) {
        return false;
    }

    return m_TouchpadGestureTracking ||
            m_TouchpadNativeGestureActive ||
            m_TouchpadSuppressedCtrlDown[0] ||
            m_TouchpadSuppressedCtrlDown[1];
#else
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
    }

    SDL_zero(m_TouchpadHavePosition);
    SDL_zero(m_TouchpadX);
    SDL_zero(m_TouchpadY);
    m_TouchpadGestureTracking = false;
    m_TouchpadNativeGestureActive = false;
    m_TouchpadScrollGestureActive = false;
    m_TouchpadSuppressWheelUntil = 0;
    m_TouchpadSuppressNextCtrlWheel = false;
    m_TouchpadLastFrameId = 0;
}
