#include "audiodeviceprewarmer.h"

#include "sdlaudioutil.h"

namespace {
constexpr int kPrewarmSampleRate = 48000;
constexpr int kPrewarmChannels = 2;
constexpr int kPrewarmSamples = 720;
constexpr int kPrewarmTickMs = 100;
constexpr int kPrewarmQueuedBuffers = 2;
}

SdlAudioDevicePrewarmer::SdlAudioDevicePrewarmer()
    : m_StopRequested(false)
{
}

SdlAudioDevicePrewarmer::~SdlAudioDevicePrewarmer()
{
    stop();
}

void SdlAudioDevicePrewarmer::start()
{
    if (m_Thread.joinable()) {
        return;
    }

    m_StopRequested = false;
    m_Thread = std::thread(&SdlAudioDevicePrewarmer::run, this);
}

void SdlAudioDevicePrewarmer::stop()
{
    m_StopRequested = true;

    if (m_Thread.joinable()) {
        m_Thread.join();
    }
}

void SdlAudioDevicePrewarmer::run()
{
    Uint32 startTime = SDL_GetTicks();

    SdlAudio::SubsystemGuard audioSubsystem;
    if (!audioSubsystem.isInitialized()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Background audio prewarm failed to initialize SDL audio: %s",
                    SDL_GetError());
        return;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = kPrewarmSampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = kPrewarmChannels;
    want.samples = kPrewarmSamples;

    SDL_AudioDeviceID audioDevice;
    {
        SdlAudio::LockedSection lock;
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    }

    if (audioDevice == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Background audio prewarm failed to open audio device: %s",
                    SDL_GetError());
        return;
    }

    Uint32 openTime = SDL_GetTicks() - startTime;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Background audio device prewarmed in %u ms using %s",
                openTime,
                SDL_GetCurrentAudioDriver());

    void* silence = SDL_calloc(1, have.size);
    if (silence != nullptr) {
        SDL_PauseAudioDevice(audioDevice, 0);

        while (!m_StopRequested) {
            if (SDL_GetAudioDeviceStatus(audioDevice) == SDL_AUDIO_STOPPED) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Background audio prewarm device stopped");
                break;
            }

            if (SDL_GetQueuedAudioSize(audioDevice) < have.size * kPrewarmQueuedBuffers) {
                SDL_QueueAudio(audioDevice, silence, have.size);
            }

            SDL_Delay(kPrewarmTickMs);
        }

        SDL_ClearQueuedAudio(audioDevice);
        SDL_free(silence);
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Background audio prewarm failed to allocate silence buffer");
        while (!m_StopRequested) {
            SDL_Delay(kPrewarmTickMs);
        }
    }

    {
        SdlAudio::LockedSection lock;
        SDL_CloseAudioDevice(audioDevice);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Background audio prewarm stopped");
}
