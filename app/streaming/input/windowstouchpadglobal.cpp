#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"

#ifdef Q_OS_WIN32

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;

constexpr float THREE_FINGER_INITIAL_THRESHOLD = 35.0f;
constexpr float THREE_FINGER_NAV_STEP_THRESHOLD = 45.0f;
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

const char* touchpadGlobalActionName(TouchpadGlobalAction action)
{
    switch (action) {
    case TouchpadGlobalAction::ThreeFingerTap:
        return "ThreeFingerTap";
    case TouchpadGlobalAction::ThreeFingerPressDown:
        return "ThreeFingerPressDown";
    case TouchpadGlobalAction::ThreeFingerPressUp:
        return "ThreeFingerPressUp";
    default:
        return "Unknown";
    }
}

class GlobalTouchpadGestureState
{
public:
    void initialize()
    {
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
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Windows global touchpad gestures are not supported on this system");
                return;
            }

            m_Controller = TouchpadGesturesController::CreateForProcess();
            m_Controller.SupportedGestures(
                        TouchpadGlobalGestureKinds::ThreeFingerManipulations |
                        TouchpadGlobalGestureKinds::ThreeFingerActions);
            m_Controller.Enabled(true);

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

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Registered Windows global touchpad gestures for 3-finger input");
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to register Windows global touchpad gestures: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
        catch (...) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to register Windows global touchpad gestures");
        }
    }

private:
    enum class GestureMode {
        Unknown,
        AltTab,
        ShowDesktop
    };

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
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Started remote Alt+Tab gesture");
    }

    void sendAltTabStep(bool reverse)
    {
        startAltTabGesture();

        if (reverse) {
            sendKey(REMOTE_VK_LSHIFT, true, MODIFIER_ALT | MODIFIER_SHIFT);
            sendKeyTap(REMOTE_VK_TAB, MODIFIER_ALT | MODIFIER_SHIFT);
            sendKey(REMOTE_VK_LSHIFT, false, MODIFIER_ALT);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Mapped 3-finger swipe step to remote Alt+Shift+Tab");
        }
        else {
            sendKeyTap(REMOTE_VK_TAB, MODIFIER_ALT);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Mapped 3-finger swipe step to remote Alt+Tab");
        }
    }

    void sendAltTabArrow(short keyCode, const char* directionName)
    {
        if (!m_AltTabActive) {
            return;
        }

        sendKeyTap(keyCode, MODIFIER_ALT);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Mapped 3-finger Alt+Tab navigation to remote %s",
                    directionName);
    }

    void finishAltTabGesture()
    {
        if (!m_AltTabActive) {
            return;
        }

        sendKey(REMOTE_VK_LMENU, false, 0);
        m_AltTabActive = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Finished remote Alt+Tab gesture");
    }

    void sendShowDesktop()
    {
        sendKey(REMOTE_VK_LWIN, true, MODIFIER_META);
        sendKeyTap(REMOTE_VK_D, MODIFIER_META);
        sendKey(REMOTE_VK_LWIN, false, 0);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Mapped 3-finger vertical swipe to remote Win+D");
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

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Windows global touchpad action: %s",
                    touchpadGlobalActionName(args.Action()));
    }

    void onPointerPressed(TouchpadGesturesController const&,
                          PointerEventArgs const& args)
    {
        try {
            m_ActiveContactCount++;
            m_MaxContactCount = std::max(m_MaxContactCount, m_ActiveContactCount);
            m_Recognizer.ProcessDownEvent(args.CurrentPoint());
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Global touchpad ProcessDownEvent failed: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
    }

    void onPointerMoved(TouchpadGesturesController const&,
                        PointerEventArgs const& args)
    {
        try {
            m_Recognizer.ProcessMoveEvents(args.GetIntermediatePoints());
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Global touchpad ProcessMoveEvents failed: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
    }

    void onPointerReleased(TouchpadGesturesController const&,
                           PointerEventArgs const& args)
    {
        try {
            m_Recognizer.ProcessUpEvent(args.CurrentPoint());
            m_ActiveContactCount = std::max(0, m_ActiveContactCount - 1);
            if (m_ActiveContactCount == 0) {
                finishAltTabGesture();
            }
        }
        catch (hresult_error const& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Global touchpad ProcessUpEvent failed: 0x%08x",
                        static_cast<unsigned int>(e.code()));
        }
    }

    void onManipulationStarted(PhysicalGestureRecognizer const&,
                               ManipulationStartedEventArgs const&)
    {
        m_GestureMode = GestureMode::Unknown;
        m_LastNavStepX = 0;
        m_LastNavStepY = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Windows global touchpad manipulation started contacts=%d",
                    std::max(3, m_MaxContactCount));
    }

    void onManipulationUpdated(PhysicalGestureRecognizer const&,
                               ManipulationUpdatedEventArgs const& args)
    {
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
            // Match precision touchpad behavior: the first horizontal swipe opens
            // Alt+Tab on the most recent window instead of wrapping to the last item.
            sendAltTabStep(false);
            m_LastNavStepX = static_cast<int>(x / THREE_FINGER_NAV_STEP_THRESHOLD);
            m_LastNavStepY = static_cast<int>(y / THREE_FINGER_NAV_STEP_THRESHOLD);
            return;
        }

        if (m_GestureMode != GestureMode::AltTab) {
            return;
        }

        const int stepX = static_cast<int>(x / THREE_FINGER_NAV_STEP_THRESHOLD);
        const int stepY = static_cast<int>(y / THREE_FINGER_NAV_STEP_THRESHOLD);
        const int deltaX = stepX - m_LastNavStepX;
        const int deltaY = stepY - m_LastNavStepY;

        if (std::abs(deltaX) >= std::abs(deltaY) && deltaX != 0) {
            if (deltaX > 0) {
                sendAltTabArrow(REMOTE_VK_RIGHT, "Right");
                m_LastNavStepX++;
            }
            else {
                sendAltTabArrow(REMOTE_VK_LEFT, "Left");
                m_LastNavStepX--;
            }
        }
        else if (deltaY != 0) {
            if (deltaY > 0) {
                sendAltTabArrow(REMOTE_VK_DOWN, "Down");
                m_LastNavStepY++;
            }
            else {
                sendAltTabArrow(REMOTE_VK_UP, "Up");
                m_LastNavStepY--;
            }
        }
    }

    void onManipulationCompleted(PhysicalGestureRecognizer const&,
                                 ManipulationCompletedEventArgs const& args)
    {
        const auto cumulative = args.Cumulative();
        const float x = cumulative.Translation.X;
        const float y = cumulative.Translation.Y;
        const float absX = std::fabs(x);
        const float absY = std::fabs(y);
        const bool horizontal = absX > absY * THREE_FINGER_AXIS_DOMINANCE;
        const bool vertical = absY > absX * THREE_FINGER_AXIS_DOMINANCE;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Windows global touchpad manipulation completed totalX=%.1f totalY=%.1f mode=%d",
                    x, y, static_cast<int>(m_GestureMode));

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

    bool m_Initialized = false;
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
};

GlobalTouchpadGestureState s_GlobalTouchpadGestures;
}

#endif

void SdlInputHandler::registerTouchpadGlobalGestures()
{
#ifdef Q_OS_WIN32
    s_GlobalTouchpadGestures.initialize();
#endif
}
