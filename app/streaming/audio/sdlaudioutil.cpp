#include "sdlaudioutil.h"

namespace {
QMutex& getSdlAudioMutex()
{
    static QMutex mutex;
    return mutex;
}
}

SdlAudio::LockedSection::LockedSection()
    : m_Locker(&getSdlAudioMutex())
{
}

SdlAudio::SubsystemGuard::SubsystemGuard()
    : m_Initialized(false)
{
    LockedSection lock;
    m_Initialized = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
}

SdlAudio::SubsystemGuard::~SubsystemGuard()
{
    if (m_Initialized) {
        LockedSection lock;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

bool SdlAudio::SubsystemGuard::isInitialized() const
{
    return m_Initialized;
}
