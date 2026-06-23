#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include <SDL_syswm.h>

#ifdef Q_OS_WIN32
#include "streaming/streamutils.h"

#include <windows.h>

namespace {
typedef BOOL (WINAPI *RegisterTouchpadCapableWindowFn)(HWND hwnd, BOOL touchpadCapable);
typedef BOOL (WINAPI *SkipPointerFrameMessagesFn)(UINT32 pointerId);

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

    if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host does not support native touch events; ignoring native touchpad frame");
        return true;
    }

    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    SDL_Rect src, dst;
    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    bool present[MAX_FINGERS] = {};
    for (UINT32 i = 0; i < pointerCount; i++) {
        const POINTER_INFO& contact = contacts[i];
        const UINT32 contactId = contact.pointerId;
        const int slot = static_cast<int>(contactId % MAX_FINGERS);

        if (slot < 0 || slot >= MAX_FINGERS) {
            continue;
        }

        present[slot] = (contact.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        if (!present[slot] && !m_TouchpadContactDown[slot]) {
            continue;
        }

        const float x = qBound(0.0f,
                               static_cast<float>(contact.ptPixelLocation.x - dst.x) / dst.w,
                               1.0f);
        const float y = qBound(0.0f,
                               static_cast<float>(contact.ptPixelLocation.y - dst.y) / dst.h,
                               1.0f);

        uint8_t eventType;
        if (present[slot] && !m_TouchpadContactDown[slot]) {
            eventType = LI_TOUCH_EVENT_DOWN;
        }
        else if (present[slot]) {
            eventType = LI_TOUCH_EVENT_MOVE;
        }
        else {
            eventType = LI_TOUCH_EVENT_UP;
        }

        LiSendTouchEvent(eventType, slot + 1, x, y, 1.0f, 0.0f, 0.0f, LI_ROT_UNKNOWN);
        m_TouchpadContactDown[slot] = present[slot];

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Native touchpad contact event=%u slot=%d x=%.4f y=%.4f raw=(%ld,%ld) frameContacts=%u",
                    eventType, slot, x, y,
                    contact.ptPixelLocation.x, contact.ptPixelLocation.y,
                    pointerCount);
    }

    for (int slot = 0; slot < MAX_FINGERS; slot++) {
        if (m_TouchpadContactDown[slot] && !present[slot] && message == WM_POINTERUP) {
            LiSendTouchEvent(LI_TOUCH_EVENT_UP, slot + 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, LI_ROT_UNKNOWN);
            m_TouchpadContactDown[slot] = false;
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

void SdlInputHandler::releasePinchZoomModifier()
{
    m_PinchZoomActive = false;
    m_PinchZoomSentModifier = false;
    m_LastPinchZoomArgument = 0;
    m_PinchWheelRemainder = 0.0f;
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
}
