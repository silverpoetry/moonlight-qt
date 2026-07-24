#include "input.h"

#include <Limelight.h>

#ifdef Q_OS_WIN32

#include <winrt/base.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
using namespace winrt;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;

constexpr float THREE_FINGER_INITIAL_THRESHOLD = 35.0f;
constexpr float THREE_FINGER_NAV_STEP_THRESHOLD = 45.0f;
constexpr float THREE_FINGER_NAV_VERTICAL_STEP_THRESHOLD = 90.0f;
constexpr float THREE_FINGER_ACTION_THRESHOLD = 70.0f;
constexpr float THREE_FINGER_AXIS_DOMINANCE = 1.35f;
constexpr short REMOTE_VK_TAB = 0x09;
constexpr short REMOTE_VK_D = 0x44;
constexpr short REMOTE_VK_LWIN = 0x5B;
constexpr short REMOTE_VK_LSHIFT = 0xA0;
constexpr short REMOTE_VK_LMENU = 0xA4;
constexpr short REMOTE_VK_LEFT = 0x25;
constexpr short REMOTE_VK_UP = 0x26;
constexpr short REMOTE_VK_RIGHT = 0x27;
constexpr short REMOTE_VK_DOWN = 0x28;
constexpr float PHYSICAL_UNITS_PER_INCH = 96.0f;
constexpr float MILLIMETERS_PER_INCH = 25.4f;

struct NativeTouchpadPoint
{
    uint32_t pointerId;
    float x;
    float y;
    float pressure;
    uint16_t deviceWidthMm;
    uint16_t deviceHeightMm;
    uint8_t buttonState;
};

bool nativeTouchpadProtocolSupported()
{
    const uint32_t hostFeatures = LiGetHostFeatureFlags();
    return (hostFeatures & (LI_FF_TOUCHPAD_FRAME_EVENTS | LI_FF_TOUCHPAD_EVENTS)) != 0;
}

bool getNativeTouchpadPoint(PointerPoint const& point, NativeTouchpadPoint& nativePoint)
{
    try {
        auto physicalPosition = point.try_as<IPointerPointPhysicalPosition>();
        if (!physicalPosition || !physicalPosition.IsPhysicalPositionSupported()) {
            return false;
        }

        const auto pointerDevice = point.PointerDevice();
        if (!pointerDevice) {
            return false;
        }

        const auto deviceRect = pointerDevice.PhysicalDeviceRect();
        if (deviceRect.Width <= 0.0f || deviceRect.Height <= 0.0f) {
            return false;
        }

        const auto position = physicalPosition.PhysicalPosition();
        const auto properties = point.Properties();
        const auto physicalUnitsToMillimeters = [](float physicalUnits) {
            const long millimeters = std::lround(
                        std::fabs(physicalUnits) *
                        MILLIMETERS_PER_INCH /
                        PHYSICAL_UNITS_PER_INCH);
            return static_cast<uint16_t>(
                        std::clamp<long>(millimeters,
                                         0,
                                         std::numeric_limits<uint16_t>::max()));
        };

        nativePoint.pointerId = point.PointerId();
        nativePoint.x = std::clamp(
                    (position.X - deviceRect.X) / deviceRect.Width,
                    0.0f,
                    1.0f);
        nativePoint.y = std::clamp(
                    (position.Y - deviceRect.Y) / deviceRect.Height,
                    0.0f,
                    1.0f);
        nativePoint.pressure = std::clamp(properties.Pressure(), 0.0f, 1.0f);
        nativePoint.deviceWidthMm = physicalUnitsToMillimeters(deviceRect.Width);
        nativePoint.deviceHeightMm = physicalUnitsToMillimeters(deviceRect.Height);
        nativePoint.buttonState = properties.IsLeftButtonPressed() ?
                    LI_TOUCHPAD_BUTTON_PRIMARY :
                    0;
        return true;
    }
    catch (hresult_error const& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to read physical touchpad position: 0x%08x",
                    static_cast<unsigned int>(e.code()));
        return false;
    }
}

int sendNativeTouchpadContact(uint8_t eventType, const NativeTouchpadPoint& point)
{
    const uint32_t hostFeatures = LiGetHostFeatureFlags();
    if (!(hostFeatures & LI_FF_TOUCHPAD_EVENTS)) {
        return LI_ERR_UNSUPPORTED;
    }

    return LiSendTouchpadEvent(
                eventType,
                point.pointerId,
                point.x,
                point.y,
                point.pressure,
                0.0f,
                0.0f,
                LI_ROT_UNKNOWN,
                point.deviceWidthMm,
                point.deviceHeightMm,
                point.buttonState);
}

class GlobalTouchpadGestureState
{
public:
    void initialize(SdlInputHandler* handler)
    {
        m_Handler = handler;

        if (m_Initialized) {
            return;
        }

        m_Initialized = true;

        try {
            try {
                init_apartment(apartment_type::multi_threaded);
            }
            catch (hresult_error const& e) {
                if (e.code() != RPC_E_CHANGED_MODE) {
                    throw;
                }
            }

            if (!TouchpadGesturesController::IsSupported()) {
                return;
            }

            m_Controller = TouchpadGesturesController::CreateForProcess();
            m_Controller.SupportedGestures(
                        TouchpadGlobalGestureKinds::ThreeFingerManipulations |
                        TouchpadGlobalGestureKinds::ThreeFingerActions);
            m_Controller.Enabled(false);

            m_Recognizer = PhysicalGestureRecognizer();
            m_Recognizer.GestureSettings(
                        GestureSettings::ManipulationTranslateX |
                        GestureSettings::ManipulationTranslateY);
            m_Recognizer.TranslationMinContactCount(3);
            m_Recognizer.TranslationMaxContactCount(3);
            m_Recognizer.TapMinContactCount(3);
            m_Recognizer.TapMaxContactCount(3);
            m_Recognizer.HoldMinContactCount(3);
            m_Recognizer.HoldMaxContactCount(3);

            m_ActionToken = m_Controller.GlobalActionPerformed(
                        { this, &GlobalTouchpadGestureState::onGlobalActionPerformed });
            m_PressedToken = m_Controller.PointerPressed(
                        { this, &GlobalTouchpadGestureState::onPointerPressed });
            m_MovedToken = m_Controller.PointerMoved(
                        { this, &GlobalTouchpadGestureState::onPointerMoved });
            m_ReleasedToken = m_Controller.PointerReleased(
                        { this, &GlobalTouchpadGestureState::onPointerReleased });
            m_ManipulationStartedToken = m_Recognizer.ManipulationStarted(
                        { this, &GlobalTouchpadGestureState::onManipulationStarted });
            m_ManipulationUpdatedToken = m_Recognizer.ManipulationUpdated(
                        { this, &GlobalTouchpadGestureState::onManipulationUpdated });
            m_ManipulationCompletedToken = m_Recognizer.ManipulationCompleted(
                        { this, &GlobalTouchpadGestureState::onManipulationCompleted });
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to register Windows touchpad gestures: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
        catch (...) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to register Windows touchpad gestures");
        }
    }

    void setEnabled(SdlInputHandler* handler, bool enabled)
    {
        if (enabled) {
            initialize(handler);
        }
        else if (handler == m_Handler) {
            m_Handler = nullptr;
        }

        if (!m_Controller) {
            cancelNativeForwarding();
            resetGestureState();
            m_Enabled = false;
            return;
        }

        const bool active = enabled && m_Handler != nullptr;
        const bool useNativeProtocol = active && nativeTouchpadProtocolSupported();
        TouchpadGlobalGestureKinds supportedGestures =
                TouchpadGlobalGestureKinds::ThreeFingerManipulations;
        if (useNativeProtocol) {
            // Manipulation events expose physical positions for every contact,
            // so they can be forwarded natively. Global action events do not
            // expose contact positions; retain the existing three-finger action
            // behavior only when system-key capture is active.
            supportedGestures = supportedGestures |
                    TouchpadGlobalGestureKinds::FourFingerManipulations |
                    TouchpadGlobalGestureKinds::FiveFingerManipulations;
            if (m_Handler->isSystemKeyCaptureActive()) {
                supportedGestures = supportedGestures |
                        TouchpadGlobalGestureKinds::ThreeFingerActions;
            }
        }
        else {
            supportedGestures = supportedGestures |
                    TouchpadGlobalGestureKinds::ThreeFingerActions;
        }

        if (m_Enabled == active &&
                m_UseNativeProtocol == useNativeProtocol &&
                m_SupportedGestures == supportedGestures) {
            return;
        }

        if (!active ||
                m_UseNativeProtocol != useNativeProtocol ||
                m_SupportedGestures != supportedGestures) {
            cancelNativeForwarding();
            resetGestureState();
        }

        try {
            if (m_Enabled) {
                m_Controller.Enabled(false);
            }
            m_Controller.SupportedGestures(supportedGestures);
            m_Controller.Enabled(active);
            m_Enabled = active;
            m_UseNativeProtocol = useNativeProtocol;
            m_SupportedGestures = supportedGestures;
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to update Windows touchpad gestures: 0x%08x",
                        static_cast<unsigned int>(e.code()));
            cancelNativeForwarding();
            resetGestureState();
            m_Enabled = false;
            m_UseNativeProtocol = false;
        }
    }

private:
    enum class GestureMode {
        Unknown,
        AltTab,
        ShowDesktop
    };

    struct NativeContactState {
        NativeTouchpadPoint point;
        bool active;
    };

    int findNativeContact(uint32_t pointerId) const
    {
        for (int slot = 0; slot < MAX_NATIVE_TOUCHPAD_CONTACTS; slot++) {
            if (m_NativeContacts[slot].active &&
                    m_NativeContacts[slot].point.pointerId == pointerId) {
                return slot;
            }
        }

        return -1;
    }

    int findUnusedNativeContact() const
    {
        for (int slot = 0; slot < MAX_NATIVE_TOUCHPAD_CONTACTS; slot++) {
            if (!m_NativeContacts[slot].active) {
                return slot;
            }
        }

        return -1;
    }

    int activeNativeContactCount() const
    {
        int count = 0;
        for (const auto& contact : m_NativeContacts) {
            if (contact.active) {
                count++;
            }
        }
        return count;
    }

    bool sendNativeFrame(const uint8_t* eventTypes,
                         const NativeTouchpadPoint* points,
                         uint8_t contactCount,
                         uint8_t buttonState)
    {
        if (contactCount == 0) {
            return true;
        }

        uint32_t pointerIds[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        float x[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        float y[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        float pressure[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        for (uint8_t i = 0; i < contactCount; i++) {
            pointerIds[i] = points[i].pointerId;
            x[i] = points[i].x;
            y[i] = points[i].y;
            pressure[i] = points[i].pressure;
        }

        const uint32_t hostFeatures = LiGetHostFeatureFlags();
        int result = LI_ERR_UNSUPPORTED;
        if (hostFeatures & LI_FF_TOUCHPAD_FRAME_EVENTS) {
            result = LiSendTouchpadFrameEvent(
                        contactCount,
                        eventTypes,
                        pointerIds,
                        x,
                        y,
                        pressure,
                        LI_ROT_UNKNOWN,
                        points[0].deviceWidthMm,
                        points[0].deviceHeightMm,
                        buttonState);
        }

        if (result == LI_ERR_UNSUPPORTED &&
                (hostFeatures & LI_FF_TOUCHPAD_EVENTS)) {
            result = 0;
            for (uint8_t i = 0; i < contactCount; i++) {
                NativeTouchpadPoint point = points[i];
                point.buttonState = buttonState;
                const int contactResult =
                        sendNativeTouchpadContact(eventTypes[i], point);
                if (contactResult != 0) {
                    result = contactResult;
                    break;
                }
            }
        }

        if (result != 0 && result != LI_ERR_UNSUPPORTED) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to queue global native touchpad frame: %d",
                        result);
        }

        return result != LI_ERR_UNSUPPORTED;
    }

    bool forwardNativePoint(PointerPoint const& point, uint8_t requestedEventType)
    {
        NativeTouchpadPoint nativePoint;
        if (!getNativeTouchpadPoint(point, nativePoint)) {
            return false;
        }

        int slot = findNativeContact(nativePoint.pointerId);
        uint8_t eventType = requestedEventType;

        if (eventType == LI_TOUCH_EVENT_DOWN) {
            if (slot >= 0) {
                eventType = LI_TOUCH_EVENT_MOVE;
            }
            else {
                if (activeNativeContactCount() == 0 && m_Handler != nullptr) {
                    // A global 3+ finger gesture takes over from the window's
                    // two-finger stream. End the old stream before reusing IDs.
                    m_Handler->cancelNativeTouchpadContacts();
                }
                slot = findUnusedNativeContact();
            }
        }
        else if (eventType == LI_TOUCH_EVENT_MOVE && slot < 0) {
            eventType = LI_TOUCH_EVENT_DOWN;
            if (activeNativeContactCount() == 0 && m_Handler != nullptr) {
                m_Handler->cancelNativeTouchpadContacts();
            }
            slot = findUnusedNativeContact();
        }
        else if ((eventType == LI_TOUCH_EVENT_UP ||
                  eventType == LI_TOUCH_EVENT_CANCEL) &&
                 slot < 0) {
            return true;
        }

        if (slot < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Global native touchpad gesture exceeded %d contacts",
                        MAX_NATIVE_TOUCHPAD_CONTACTS);
            cancelNativeForwarding();
            return true;
        }

        m_NativeContacts[slot].point = nativePoint;
        m_NativeContacts[slot].active = true;

        uint8_t eventTypes[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        NativeTouchpadPoint points[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        uint8_t contactCount = 0;

        if (eventType == LI_TOUCH_EVENT_DOWN) {
            eventTypes[contactCount] = eventType;
            points[contactCount] = nativePoint;
            contactCount++;
        }

        for (int candidate = 0; candidate < MAX_NATIVE_TOUCHPAD_CONTACTS; candidate++) {
            if (!m_NativeContacts[candidate].active ||
                    (eventType == LI_TOUCH_EVENT_DOWN && candidate == slot) ||
                    ((eventType == LI_TOUCH_EVENT_UP ||
                      eventType == LI_TOUCH_EVENT_CANCEL) &&
                     candidate == slot)) {
                continue;
            }

            eventTypes[contactCount] = LI_TOUCH_EVENT_MOVE;
            points[contactCount] = m_NativeContacts[candidate].point;
            contactCount++;
        }

        if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
            eventTypes[contactCount] = eventType;
            points[contactCount] = nativePoint;
            contactCount++;
        }

        const bool handled = sendNativeFrame(
                    eventTypes,
                    points,
                    contactCount,
                    nativePoint.buttonState);
        if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
            m_NativeContacts[slot] = {};
        }

        return handled;
    }

    void cancelNativeForwarding()
    {
        uint8_t eventTypes[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        NativeTouchpadPoint points[MAX_NATIVE_TOUCHPAD_CONTACTS] = {};
        uint8_t contactCount = 0;

        for (const auto& contact : m_NativeContacts) {
            if (!contact.active) {
                continue;
            }

            eventTypes[contactCount] = LI_TOUCH_EVENT_CANCEL;
            points[contactCount] = contact.point;
            points[contactCount].pressure = 0.0f;
            points[contactCount].buttonState = 0;
            contactCount++;
        }

        if (contactCount != 0) {
            sendNativeFrame(eventTypes, points, contactCount, 0);
        }
        m_NativeContacts = {};
    }

    bool shouldForwardNativeGesture() const
    {
        return m_Enabled &&
                m_UseNativeProtocol &&
                m_Handler != nullptr &&
                m_Handler->isCaptureActive();
    }

    void sendKey(short keyCode, bool down, char modifiers)
    {
        const short remoteKeyCode = static_cast<short>(0x8000 | keyCode);
        LiSendKeyboardEvent(remoteKeyCode, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers);
    }

    void sendKeyTap(short keyCode, char modifiers)
    {
        sendKey(keyCode, true, modifiers);
        sendKey(keyCode, false, modifiers);
    }

    void startAltTabGesture()
    {
        if (m_AltTabActive) {
            return;
        }

        sendKey(REMOTE_VK_LMENU, true, MODIFIER_ALT);
        m_AltTabActive = true;
    }

    void sendAltTabStep(bool reverse)
    {
        startAltTabGesture();

        if (reverse) {
            sendKey(REMOTE_VK_LSHIFT, true, MODIFIER_ALT | MODIFIER_SHIFT);
            sendKeyTap(REMOTE_VK_TAB, MODIFIER_ALT | MODIFIER_SHIFT);
            sendKey(REMOTE_VK_LSHIFT, false, MODIFIER_ALT);
        }
        else {
            sendKeyTap(REMOTE_VK_TAB, MODIFIER_ALT);
        }
    }

    void sendAltTabArrow(short keyCode)
    {
        if (m_AltTabActive) {
            sendKeyTap(keyCode, MODIFIER_ALT);
        }
    }

    void finishAltTabGesture()
    {
        if (!m_AltTabActive) {
            return;
        }

        sendKey(REMOTE_VK_LMENU, false, 0);
        m_AltTabActive = false;
    }

    void sendShowDesktop()
    {
        sendKey(REMOTE_VK_LWIN, true, MODIFIER_META);
        sendKeyTap(REMOTE_VK_D, MODIFIER_META);
        sendKey(REMOTE_VK_LWIN, false, 0);
    }

    void resetGestureState()
    {
        finishAltTabGesture();
        m_GestureMode = GestureMode::Unknown;
        m_ActiveContactCount = 0;
        m_MaxContactCount = 0;
        m_LastNavStepX = 0;
        m_LastNavStepY = 0;
    }

    void onGlobalActionPerformed(TouchpadGesturesController const&,
                                 TouchpadGlobalActionEventArgs const& args)
    {
        if (!shouldHandleLegacyGesture()) {
            return;
        }

        switch (args.Action()) {
        case TouchpadGlobalAction::ThreeFingerPressDown:
            m_ActiveContactCount = 3;
            m_MaxContactCount = std::max(m_MaxContactCount, 3);
            break;
        case TouchpadGlobalAction::ThreeFingerPressUp:
            if (m_ActiveContactCount == 0) {
                finishAltTabGesture();
            }
            break;
        default:
            break;
        }
    }

    void onPointerPressed(TouchpadGesturesController const&,
                          PointerEventArgs const& args)
    {
        if (shouldForwardNativeGesture()) {
            try {
                if (!forwardNativePoint(args.CurrentPoint(), LI_TOUCH_EVENT_DOWN)) {
                    cancelNativeForwarding();
                }
            }
            catch (hresult_error const& e) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to read global touchpad down event: 0x%08x",
                            static_cast<unsigned int>(e.code()));
                cancelNativeForwarding();
            }
            return;
        }

        if (!shouldHandleLegacyGesture()) {
            return;
        }

        try {
            m_ActiveContactCount++;
            m_MaxContactCount = std::max(m_MaxContactCount, m_ActiveContactCount);
            m_Recognizer.ProcessDownEvent(args.CurrentPoint());
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Touchpad gesture ProcessDownEvent failed: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
    }

    void onPointerMoved(TouchpadGesturesController const&,
                        PointerEventArgs const& args)
    {
        if (shouldForwardNativeGesture()) {
            try {
                const auto points = args.GetIntermediatePoints();
                for (int index = static_cast<int>(points.Size()) - 1;
                     index >= 0;
                     index--) {
                    if (!forwardNativePoint(points.GetAt(index), LI_TOUCH_EVENT_MOVE)) {
                        cancelNativeForwarding();
                        break;
                    }
                }
            }
            catch (hresult_error const& e) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to read global touchpad move event: 0x%08x",
                            static_cast<unsigned int>(e.code()));
                cancelNativeForwarding();
            }
            return;
        }

        if (!shouldHandleLegacyGesture()) {
            return;
        }

        try {
            m_Recognizer.ProcessMoveEvents(args.GetIntermediatePoints());
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Touchpad gesture ProcessMoveEvents failed: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
    }

    void onPointerReleased(TouchpadGesturesController const&,
                           PointerEventArgs const& args)
    {
        if (shouldForwardNativeGesture()) {
            try {
                if (!forwardNativePoint(args.CurrentPoint(), LI_TOUCH_EVENT_UP)) {
                    cancelNativeForwarding();
                }
            }
            catch (hresult_error const& e) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to read global touchpad up event: 0x%08x",
                            static_cast<unsigned int>(e.code()));
                cancelNativeForwarding();
            }
            return;
        }

        if (!shouldHandleLegacyGesture()) {
            return;
        }

        try {
            m_Recognizer.ProcessUpEvent(args.CurrentPoint());
            m_ActiveContactCount = std::max(0, m_ActiveContactCount - 1);
            if (m_ActiveContactCount == 0) {
                finishAltTabGesture();
            }
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Touchpad gesture ProcessUpEvent failed: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
    }

    void onManipulationStarted(PhysicalGestureRecognizer const&,
                               ManipulationStartedEventArgs const&)
    {
        if (!shouldHandleLegacyGesture()) {
            return;
        }

        m_GestureMode = GestureMode::Unknown;
        m_LastNavStepX = 0;
        m_LastNavStepY = 0;
    }

    void onManipulationUpdated(PhysicalGestureRecognizer const&,
                               ManipulationUpdatedEventArgs const& args)
    {
        if (!shouldHandleLegacyGesture()) {
            return;
        }

        const auto cumulative = args.Cumulative();
        const float x = cumulative.Translation.X;
        const float y = cumulative.Translation.Y;
        const float absX = std::fabs(x);
        const float absY = std::fabs(y);

        if (m_GestureMode == GestureMode::ShowDesktop) {
            return;
        }

        if (m_GestureMode == GestureMode::Unknown &&
                absY >= THREE_FINGER_ACTION_THRESHOLD &&
                absY > absX * THREE_FINGER_AXIS_DOMINANCE) {
            m_GestureMode = GestureMode::ShowDesktop;
            return;
        }

        if (m_GestureMode == GestureMode::Unknown &&
                absX >= THREE_FINGER_INITIAL_THRESHOLD &&
                absX > absY * THREE_FINGER_AXIS_DOMINANCE) {
            m_GestureMode = GestureMode::AltTab;
            sendAltTabStep(false);
            m_LastNavStepX = static_cast<int>(x / THREE_FINGER_NAV_STEP_THRESHOLD);
            m_LastNavStepY = static_cast<int>(y / THREE_FINGER_NAV_STEP_THRESHOLD);
            return;
        }

        if (m_GestureMode != GestureMode::AltTab) {
            return;
        }

        const int stepX = static_cast<int>(x / THREE_FINGER_NAV_STEP_THRESHOLD);
        const int stepY = static_cast<int>(y / THREE_FINGER_NAV_VERTICAL_STEP_THRESHOLD);
        const int deltaX = stepX - m_LastNavStepX;
        const int deltaY = stepY - m_LastNavStepY;

        if (std::abs(deltaX) >= std::abs(deltaY) && deltaX != 0) {
            if (deltaX > 0) {
                sendAltTabArrow(REMOTE_VK_RIGHT);
                m_LastNavStepX++;
            }
            else {
                sendAltTabArrow(REMOTE_VK_LEFT);
                m_LastNavStepX--;
            }
        }
        else if (deltaY != 0) {
            if (deltaY > 0) {
                sendAltTabArrow(REMOTE_VK_DOWN);
                m_LastNavStepY++;
            }
            else {
                sendAltTabArrow(REMOTE_VK_UP);
                m_LastNavStepY--;
            }
        }
    }

    void onManipulationCompleted(PhysicalGestureRecognizer const&,
                                 ManipulationCompletedEventArgs const& args)
    {
        if (!shouldHandleLegacyGesture()) {
            resetGestureState();
            return;
        }

        const auto cumulative = args.Cumulative();
        const float x = cumulative.Translation.X;
        const float y = cumulative.Translation.Y;
        const float absX = std::fabs(x);
        const float absY = std::fabs(y);
        const bool horizontal = absX > absY * THREE_FINGER_AXIS_DOMINANCE;
        const bool vertical = absY > absX * THREE_FINGER_AXIS_DOMINANCE;

        if (m_GestureMode == GestureMode::AltTab) {
            finishAltTabGesture();
        }
        else if (m_GestureMode == GestureMode::ShowDesktop ||
                 (vertical && absY >= THREE_FINGER_ACTION_THRESHOLD)) {
            sendShowDesktop();
        }
        else if (horizontal && absX >= THREE_FINGER_ACTION_THRESHOLD) {
            sendAltTabStep(false);
            finishAltTabGesture();
        }

        resetGestureState();
    }

    bool shouldHandleLegacyGesture()
    {
        if (!m_Enabled || m_Handler == nullptr || !m_Handler->isSystemKeyCaptureActive()) {
            resetGestureState();
            return false;
        }

        return true;
    }

    bool m_Initialized = false;
    bool m_Enabled = false;
    bool m_UseNativeProtocol = false;
    SdlInputHandler* m_Handler = nullptr;
    TouchpadGesturesController m_Controller{ nullptr };
    PhysicalGestureRecognizer m_Recognizer{ nullptr };
    event_token m_ActionToken{};
    event_token m_PressedToken{};
    event_token m_MovedToken{};
    event_token m_ReleasedToken{};
    event_token m_ManipulationStartedToken{};
    event_token m_ManipulationUpdatedToken{};
    event_token m_ManipulationCompletedToken{};
    GestureMode m_GestureMode = GestureMode::Unknown;
    int m_ActiveContactCount = 0;
    int m_MaxContactCount = 0;
    int m_LastNavStepX = 0;
    int m_LastNavStepY = 0;
    bool m_AltTabActive = false;
    TouchpadGlobalGestureKinds m_SupportedGestures =
            TouchpadGlobalGestureKinds::None;
    std::array<NativeContactState, MAX_NATIVE_TOUCHPAD_CONTACTS> m_NativeContacts{};
};

GlobalTouchpadGestureState s_GlobalTouchpadGestures;
}

#endif

void SdlInputHandler::registerTouchpadGlobalGestures()
{
#ifdef Q_OS_WIN32
    s_GlobalTouchpadGestures.initialize(this);
#endif
}

void SdlInputHandler::updateTouchpadGlobalGesturesEnabled(bool enabled)
{
#ifdef Q_OS_WIN32
    s_GlobalTouchpadGestures.setEnabled(this, enabled);
#else
    Q_UNUSED(enabled);
#endif
}
