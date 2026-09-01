#include "audio.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace bf {
namespace {

namespace fs = std::filesystem;

constexpr unsigned kSampleRate = 48000;
constexpr unsigned kChannels = 2;
constexpr int kPcmPlayback = 0;
constexpr int kPcmAccessRwInterleaved = 3;
constexpr int kPcmFormatS16Le = 2;
constexpr int kPcmNonblock = 1;
constexpr double kPi = 3.14159265358979323846;

bool isCardputerCodec(const std::string& id, const std::string& name) {
    return id == "ES8388Audio" || id == "ES8389Audio" ||
           name.find("ES8388-Audio") != std::string::npos ||
           name.find("ES8389-Audio") != std::string::npos;
}

bool isEs8389(const std::string& id, const std::string& name) {
    return id == "ES8389Audio" || name.find("ES8389-Audio") != std::string::npos;
}

std::string trim(std::string value) {
    const std::string whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

int firstPlaybackDevice(const fs::path& root, int card) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return -1;
    const std::string prefix = "pcmC" + std::to_string(card) + "D";
    int selected = -1;
    for (const auto& entry :
         fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || name.empty() || name.back() != 'p') continue;
        const std::string number =
            name.substr(prefix.size(), name.size() - prefix.size() - 1);
        int device = -1;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), device);
        if (number.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != number.data() + number.size() || device < 0) continue;
        if (selected < 0 || device < selected) selected = device;
    }
    return selected;
}

struct Segment {
    double startHz;
    double endHz;
    int durationMs;
    int gapMs;
    double amplitude;
};

std::vector<Segment> cueSegments(HudCue cue) {
    switch (cue) {
    case HudCue::Startup:
        return {{420, 760, 60, 14, .18}, {980, 1540, 95, 0, .25}};
    case HudCue::Navigate:
        return {{1550, 1870, 18, 0, .12}};
    case HudCue::Select:
        return {{860, 1040, 30, 7, .18}, {1320, 1510, 42, 0, .23}};
    case HudCue::Back:
        return {{940, 560, 58, 0, .18}};
    case HudCue::Prompt:
        return {{760, 760, 34, 24, .20}, {760, 760, 34, 0, .20}};
    case HudCue::Success:
        return {{560, 680, 42, 8, .18}, {850, 980, 46, 8, .21},
                {1210, 1460, 68, 0, .25}};
    case HudCue::Error:
        return {{360, 290, 82, 22, .24}, {255, 220, 112, 0, .27}};
    case HudCue::LinkUp:
        return {{440, 900, 70, 10, .20}, {1120, 1480, 70, 0, .24}};
    case HudCue::LinkDown:
        return {{1320, 760, 82, 12, .22}, {560, 350, 92, 0, .25}};
    case HudCue::Critical:
        return {{980, 980, 70, 24, .28}, {610, 610, 70, 24, .28},
                {980, 980, 70, 24, .28}, {610, 610, 85, 0, .28}};
    case HudCue::Command:
        return {{1720, 2050, 22, 0, .14}};
    }
    return {};
}

} // namespace

AudioDeviceInfo discoverCardputerZeroAudio(const std::string& procAsoundRoot,
                                            const std::string& devSndRoot) {
    AudioDeviceInfo fallback;
    std::ifstream cards(fs::path(procAsoundRoot) / "cards");
    std::string line;
    while (std::getline(cards, line)) {
        std::istringstream parts(line);
        int number = -1;
        if (!(parts >> number)) continue;
        const auto open = line.find('[');
        const auto close = line.find(']', open == std::string::npos ? 0 : open);
        if (open == std::string::npos || close == std::string::npos || close <= open) continue;
        const std::string id = trim(line.substr(open + 1, close - open - 1));
        const std::string name = trim(line.substr(close + 1));
        if (!isCardputerCodec(id, name)) continue;
        AudioDeviceInfo candidate;
        candidate.cardPresent = true;
        candidate.cardNumber = number;
        candidate.cardId = id;
        candidate.cardName = name;
        candidate.mixerName = "hw:" + id;
        candidate.mixerElements = isEs8389(id, name)
                                      ? std::vector<std::string>{"DACL", "DACR"}
                                      : std::vector<std::string>{"Headphone"};
        candidate.playbackDevice = firstPlaybackDevice(devSndRoot, number);
        candidate.playbackPresent = candidate.playbackDevice >= 0;
        if (candidate.playbackPresent) {
            candidate.pcmName = candidate.mixerName + "," +
                                std::to_string(candidate.playbackDevice);
            return candidate;
        }
        if (!fallback.cardPresent) fallback = std::move(candidate);
    }
    return fallback;
}

std::vector<std::int16_t> synthesizeHudCue(HudCue cue) {
    const std::vector<Segment> segments = cueSegments(cue);
    std::size_t totalFrames = 0;
    for (const Segment& segment : segments) {
        totalFrames += static_cast<std::size_t>(
            (static_cast<unsigned long long>(kSampleRate) *
             static_cast<unsigned>(segment.durationMs + segment.gapMs)) /
            1000U);
    }
    std::vector<std::int16_t> pcm;
    pcm.reserve(totalFrames * kChannels);
    double phase = 0.0;
    for (const Segment& segment : segments) {
        const int frames = static_cast<int>(
            (static_cast<unsigned long long>(kSampleRate) * segment.durationMs) / 1000U);
        const int rampFrames = std::max(1, std::min(frames / 2, static_cast<int>(kSampleRate / 250)));
        for (int frame = 0; frame < frames; ++frame) {
            const double position = frames > 1 ? static_cast<double>(frame) / (frames - 1) : 0.0;
            const double frequency = segment.startHz + (segment.endHz - segment.startHz) * position;
            phase += 2.0 * kPi * frequency / kSampleRate;
            if (phase > 2.0 * kPi) phase = std::fmod(phase, 2.0 * kPi);
            const double attack = std::min(1.0, static_cast<double>(frame + 1) / rampFrames);
            const double release = std::min(1.0, static_cast<double>(frames - frame) / rampFrames);
            const double envelope = attack * release;
            const double hudWave = std::sin(phase) + 0.16 * std::sin(phase * 2.0);
            const auto sample = static_cast<std::int16_t>(
                std::clamp(hudWave * segment.amplitude * envelope, -0.92, 0.92) * 32767.0);
            pcm.push_back(sample);
            pcm.push_back(sample);
        }
        const int gapFrames = static_cast<int>(
            (static_cast<unsigned long long>(kSampleRate) * segment.gapMs) / 1000U);
        pcm.insert(pcm.end(), static_cast<std::size_t>(gapFrames) * kChannels, 0);
    }
    return pcm;
}

struct Audio::Impl {
    AudioDeviceInfo device;
    std::string backend = "silent";
    std::atomic<bool> isAvailable{false};
    std::atomic<bool> isEnabled{true};
    std::atomic<int> volumePercent{70};
    std::atomic<int> appliedVolume{-1};
    std::atomic<bool> stopping{false};
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable idleReady;
    std::deque<HudCue> queue;
    std::atomic<bool> playing{false};
    std::thread worker;
    mutable std::mutex errorMutex;
    std::string error;

#if defined(__linux__)
    struct AlsaApi {
        struct Pcm;
        struct Mixer;
        struct MixerElem;
        using Frames = long;
        using UFrames = unsigned long;

        void* library = nullptr;
        bool ready = false;
        int (*pcmOpen)(Pcm**, const char*, int, int) = nullptr;
        int (*pcmSetParams)(Pcm*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
        int (*pcmNonblock)(Pcm*, int) = nullptr;
        Frames (*pcmWritei)(Pcm*, const void*, UFrames) = nullptr;
        int (*pcmState)(Pcm*) = nullptr;
        int (*pcmRecover)(Pcm*, int, int) = nullptr;
        int (*pcmDrain)(Pcm*) = nullptr;
        int (*pcmDrop)(Pcm*) = nullptr;
        int (*pcmClose)(Pcm*) = nullptr;
        const char* (*errorText)(int) = nullptr;
        int (*mixerOpen)(Mixer**, int) = nullptr;
        int (*mixerAttach)(Mixer*, const char*) = nullptr;
        int (*mixerRegister)(Mixer*, void*, void**) = nullptr;
        int (*mixerLoad)(Mixer*) = nullptr;
        MixerElem* (*mixerFirstElem)(Mixer*) = nullptr;
        MixerElem* (*mixerElemNext)(MixerElem*) = nullptr;
        int (*mixerElemActive)(MixerElem*) = nullptr;
        const char* (*mixerElemName)(MixerElem*) = nullptr;
        int (*mixerHasPlaybackVolume)(MixerElem*) = nullptr;
        int (*mixerPlaybackRange)(MixerElem*, long*, long*) = nullptr;
        int (*mixerSetPlaybackAll)(MixerElem*, long) = nullptr;
        int (*mixerClose)(Mixer*) = nullptr;

        template <typename Function>
        bool symbol(Function& target, const char* name) {
            target = reinterpret_cast<Function>(dlsym(library, name));
            return target != nullptr;
        }

        bool load() {
            if (ready) return true;
            if (library != nullptr) {
                dlclose(library);
                library = nullptr;
            }
            library = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
            ready = library != nullptr &&
                    symbol(pcmOpen, "snd_pcm_open") &&
                    symbol(pcmSetParams, "snd_pcm_set_params") &&
                    symbol(pcmNonblock, "snd_pcm_nonblock") &&
                    symbol(pcmWritei, "snd_pcm_writei") &&
                    symbol(pcmState, "snd_pcm_state") &&
                    symbol(pcmRecover, "snd_pcm_recover") &&
                    symbol(pcmDrain, "snd_pcm_drain") &&
                    symbol(pcmDrop, "snd_pcm_drop") &&
                    symbol(pcmClose, "snd_pcm_close") &&
                    symbol(errorText, "snd_strerror") &&
                    symbol(mixerOpen, "snd_mixer_open") &&
                    symbol(mixerAttach, "snd_mixer_attach") &&
                    symbol(mixerRegister, "snd_mixer_selem_register") &&
                    symbol(mixerLoad, "snd_mixer_load") &&
                    symbol(mixerFirstElem, "snd_mixer_first_elem") &&
                    symbol(mixerElemNext, "snd_mixer_elem_next") &&
                    symbol(mixerElemActive, "snd_mixer_selem_is_active") &&
                    symbol(mixerElemName, "snd_mixer_selem_get_name") &&
                    symbol(mixerHasPlaybackVolume, "snd_mixer_selem_has_playback_volume") &&
                    symbol(mixerPlaybackRange, "snd_mixer_selem_get_playback_volume_range") &&
                    symbol(mixerSetPlaybackAll, "snd_mixer_selem_set_playback_volume_all") &&
                    symbol(mixerClose, "snd_mixer_close");
            if (!ready && library != nullptr) {
                dlclose(library);
                library = nullptr;
            }
            return ready;
        }

        std::string describe(int code) const {
            const char* text = errorText == nullptr ? nullptr : errorText(code);
            return text == nullptr ? std::to_string(code) : std::string(text);
        }

        ~AlsaApi() {
            if (library != nullptr) dlclose(library);
        }
    } alsa;
#endif

    void setError(const std::string& text) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = text;
    }

    void fail(const std::string& text) {
        setError(text);
        isAvailable.store(false);
    }

#if defined(__linux__)
    bool applyMixerVolume(int percent) {
        if (appliedVolume.load() == percent) return true;
        AlsaApi::Mixer* mixer = nullptr;
        if (alsa.mixerOpen(&mixer, 0) < 0 || mixer == nullptr) {
            fail("cannot open the " + device.cardId + " mixer");
            return false;
        }
        bool ok = alsa.mixerAttach(mixer, device.mixerName.c_str()) >= 0 &&
                  alsa.mixerRegister(mixer, nullptr, nullptr) >= 0 &&
                  alsa.mixerLoad(mixer) >= 0;
        std::vector<AlsaApi::MixerElem*> selected(device.mixerElements.size(), nullptr);
        if (ok) {
            for (auto* elem = alsa.mixerFirstElem(mixer); elem != nullptr;
                 elem = alsa.mixerElemNext(elem)) {
                const char* name = alsa.mixerElemName(elem);
                if (name == nullptr || alsa.mixerElemActive(elem) == 0 ||
                    alsa.mixerHasPlaybackVolume(elem) == 0) continue;
                const auto found = std::find(device.mixerElements.begin(),
                                             device.mixerElements.end(), name);
                if (found != device.mixerElements.end()) {
                    selected[static_cast<std::size_t>(
                        std::distance(device.mixerElements.begin(), found))] = elem;
                }
            }
        }
        ok = ok && !selected.empty() &&
             std::find(selected.begin(), selected.end(), nullptr) == selected.end();
        for (auto* elem : selected) {
            if (!ok) break;
            long minimum = 0;
            long maximum = 0;
            if (alsa.mixerPlaybackRange(elem, &minimum, &maximum) < 0 || maximum <= minimum) {
                ok = false;
                break;
            }
            // ES8389 raw 75% is its 0 dB point. Keep the hardware below that,
            // then let the bounded PCM envelope provide the HUD character.
            const long floor = minimum + ((maximum - minimum) * 30L) / 100L;
            const long ceiling = minimum + ((maximum - minimum) * 75L) / 100L;
            const long raw = percent <= 0
                                 ? minimum
                                 : floor + ((ceiling - floor) * (percent - 1L)) / 99L;
            if (alsa.mixerSetPlaybackAll(elem, raw) < 0) ok = false;
        }
        const bool closed = alsa.mixerClose(mixer) >= 0;
        if (!ok || !closed) {
            fail("required " + device.cardId + " playback controls are unavailable");
            return false;
        }
        appliedVolume.store(percent);
        return true;
    }

    void playOne(HudCue cue) {
        if (!isAvailable.load() || !isEnabled.load() || stopping.load()) return;
        const int percent = volumePercent.load();
        if (percent <= 0 || !applyMixerVolume(percent)) return;
        if (!isAvailable.load() || !isEnabled.load() || stopping.load()) return;
        const std::vector<std::int16_t> pcm = synthesizeHudCue(cue);
        AlsaApi::Pcm* handle = nullptr;
        int code = alsa.pcmOpen(&handle, device.pcmName.c_str(), kPcmPlayback, kPcmNonblock);
        if (code < 0 || handle == nullptr) {
            fail("cannot open " + device.pcmName + ": " + alsa.describe(code));
            return;
        }
        code = alsa.pcmSetParams(handle, kPcmFormatS16Le, kPcmAccessRwInterleaved,
                                 kChannels, kSampleRate, 1, 70000);
        if (code < 0 || alsa.pcmNonblock(handle, 1) < 0) {
            fail("cannot configure " + device.pcmName + ": " + alsa.describe(code));
            alsa.pcmClose(handle);
            return;
        }
        std::size_t offset = 0;
        const std::size_t totalFrames = pcm.size() / kChannels;
        bool ok = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (offset < totalFrames && !stopping.load() && isEnabled.load()) {
            const std::size_t request = std::min<std::size_t>(512, totalFrames - offset);
            long written = alsa.pcmWritei(handle, pcm.data() + offset * kChannels,
                                          static_cast<unsigned long>(request));
            if (written == -EAGAIN) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    fail(device.cardId + " playback timed out while submitting frames");
                    ok = false;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (written < 0) {
                const int recovered = alsa.pcmRecover(handle, static_cast<int>(written), 1);
                if (recovered < 0) {
                    fail(device.cardId + " playback failed: " + alsa.describe(recovered));
                    ok = false;
                    break;
                }
                continue;
            }
            if (written == 0) {
                ok = false;
                fail(device.cardId + " playback stopped without consuming frames");
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        if (ok && offset == totalFrames && !stopping.load() && isEnabled.load()) {
            // This driver returns EAGAIN while it drains. Calling drain again
            // restarts its silence tail, so start exactly once and poll state.
            code = finishNonblockingAudioDrain(
                [&] { return alsa.pcmDrain(handle); },
                [&] { return alsa.pcmState(handle); },
                [&] {
                    return !stopping.load() && isEnabled.load() &&
                           std::chrono::steady_clock::now() < deadline;
                },
                [] { std::this_thread::sleep_for(std::chrono::milliseconds(3)); });
            if (code < 0) {
                fail(device.cardId + " drain failed: " + alsa.describe(code));
                alsa.pcmDrop(handle);
            }
        } else {
            alsa.pcmDrop(handle);
        }
        alsa.pcmClose(handle);
    }
#endif

    void run() {
        for (;;) {
            HudCue cue = HudCue::Navigate;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load()) break;
                cue = queue.front();
                queue.pop_front();
                playing.store(true);
            }
#if defined(__linux__)
            playOne(cue);
#else
            (void)cue;
#endif
            playing.store(false);
            idleReady.notify_all();
        }
    }
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {}
Audio::~Audio() { shutdown(); }

bool Audio::start(const std::string& procAsoundRoot, const std::string& devSndRoot) {
    shutdown();
    impl_->setError({});
    impl_->appliedVolume.store(-1);
    impl_->playing.store(false);
    impl_->device = discoverCardputerZeroAudio(procAsoundRoot, devSndRoot);
    impl_->backend = "silent";
    impl_->stopping.store(false);
#if defined(__linux__)
    if (!impl_->device.cardPresent || !impl_->device.playbackPresent) {
        impl_->setError("Cardputer Zero ES8388/ES8389 playback was not found");
        return false;
    }
    if (!impl_->alsa.load()) {
        impl_->backend = isEs8389(impl_->device.cardId, impl_->device.cardName)
                             ? "alsa-es8389-library-missing"
                             : "alsa-es8388-library-missing";
        impl_->setError("libasound.so.2 is unavailable");
        return false;
    }
    impl_->backend = isEs8389(impl_->device.cardId, impl_->device.cardName)
                         ? "alsa-es8389"
                         : "alsa-es8388";
    impl_->isAvailable.store(true);
    impl_->worker = std::thread([this] { impl_->run(); });
    return true;
#else
    impl_->setError("ALSA is unavailable on this host");
    return false;
#endif
}

void Audio::shutdown() {
    impl_->stopping.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->queue.clear();
    }
    impl_->queueReady.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->isAvailable.store(false);
}

void Audio::setEnabled(bool enabled) {
    impl_->isEnabled.store(enabled);
    if (!enabled) {
        {
            std::lock_guard<std::mutex> lock(impl_->queueMutex);
            impl_->queue.clear();
        }
    }
}

void Audio::setVolume(int percent) {
    impl_->volumePercent.store(std::clamp(percent, 0, 100));
    impl_->appliedVolume.store(-1);
}

void Audio::play(HudCue cue) {
    if (!impl_->isAvailable.load() || !impl_->isEnabled.load() ||
        impl_->volumePercent.load() <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    if (cue == HudCue::Critical || cue == HudCue::Error || cue == HudCue::LinkDown) {
        impl_->queue.clear();
    } else if (cue == HudCue::Navigate && !impl_->queue.empty() &&
               impl_->queue.back() == HudCue::Navigate) {
        return;
    }
    if (impl_->queue.size() >= 6) impl_->queue.pop_front();
    impl_->queue.push_back(cue);
    impl_->queueReady.notify_one();
}

bool Audio::waitIdle(int timeoutMs) {
    std::unique_lock<std::mutex> lock(impl_->queueMutex);
    return impl_->idleReady.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
        [&] { return impl_->queue.empty() && !impl_->playing.load(); });
}

bool Audio::available() const { return impl_->isAvailable.load(); }
bool Audio::enabled() const { return impl_->isEnabled.load(); }
int Audio::volume() const { return impl_->volumePercent.load(); }
std::string Audio::backendName() const { return impl_->backend; }
std::string Audio::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->error;
}

} // namespace bf
