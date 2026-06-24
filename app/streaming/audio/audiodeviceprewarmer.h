#pragma once

#include <atomic>
#include <thread>

class SdlAudioDevicePrewarmer
{
public:
    SdlAudioDevicePrewarmer();
    ~SdlAudioDevicePrewarmer();

    void start();
    void stop();

private:
    void run();

    std::atomic_bool m_StopRequested;
    std::thread m_Thread;
};
