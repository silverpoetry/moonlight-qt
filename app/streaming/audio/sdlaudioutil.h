#pragma once

#include "SDL_compat.h"

#include <QMutex>
#include <QMutexLocker>

namespace SdlAudio {

class LockedSection
{
public:
    LockedSection();

private:
    Q_DISABLE_COPY(LockedSection)

    QMutexLocker m_Locker;
};

class SubsystemGuard
{
public:
    SubsystemGuard();
    ~SubsystemGuard();

    bool isInitialized() const;

private:
    Q_DISABLE_COPY(SubsystemGuard)

    bool m_Initialized;
};

}
