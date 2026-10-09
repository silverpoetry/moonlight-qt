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

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QMutexLocker<QMutex> m_Locker;
#else
    QMutexLocker m_Locker;
#endif
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
