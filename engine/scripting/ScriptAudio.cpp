#include "engine/scripting/ScriptAudio.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <list>
#include <span>
#include <utility>
#include <vector>
#endif

namespace engine::scripting {

#ifdef _WIN32
namespace {

constexpr std::size_t MaxWaveBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaxCacheBytes = 16U * 1024U * 1024U;
constexpr std::size_t MaxClips = 64;
constexpr std::size_t MaxVoices = 24;

struct Clip final {
    WAVEFORMATEX format{};
    std::vector<BYTE> samples;
};

std::uint16_t read16(const BYTE* bytes) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[0]) | (static_cast<unsigned>(bytes[1]) << 8U));
}

std::uint32_t read32(const BYTE* bytes) noexcept {
    return static_cast<std::uint32_t>(read16(bytes)) | (static_cast<std::uint32_t>(read16(bytes + 2)) << 16U);
}

std::shared_ptr<const Clip> loadWave(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size < 12 || size > static_cast<std::streamoff>(MaxWaveBytes)) return {};
    std::vector<BYTE> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size) ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return {};

    const std::size_t riffSize = read32(bytes.data() + 4);
    if (riffSize < 4 || riffSize > bytes.size() - 8U) return {};
    const std::size_t end = 8U + riffSize;
    WAVEFORMATEX format{};
    std::span<const BYTE> samples;
    for (std::size_t offset = 12; offset <= end && end - offset >= 8U;) {
        const BYTE* header = bytes.data() + offset;
        const std::size_t length = read32(header + 4);
        const std::size_t begin = offset + 8U;
        if (length > end - begin) return {};
        const BYTE* content = bytes.data() + begin;
        if (std::memcmp(header, "fmt ", 4) == 0 && length >= 16) {
            format.wFormatTag = read16(content);
            format.nChannels = read16(content + 2);
            format.nSamplesPerSec = read32(content + 4);
            format.nAvgBytesPerSec = read32(content + 8);
            format.nBlockAlign = read16(content + 12);
            format.wBitsPerSample = read16(content + 14);
        } else if (std::memcmp(header, "data", 4) == 0) {
            samples = {content, length};
        }
        offset = begin + length + (length & 1U);
    }

    if (format.wFormatTag != WAVE_FORMAT_PCM || format.nChannels < 1 || format.nChannels > 2 ||
        format.nSamplesPerSec < 8000 || format.nSamplesPerSec > 96000 || format.wBitsPerSample != 16 ||
        format.nBlockAlign != format.nChannels * 2U || format.nAvgBytesPerSec != format.nBlockAlign * format.nSamplesPerSec ||
        samples.empty() || samples.size() % format.nBlockAlign != 0) return {};

    auto clip = std::make_shared<Clip>();
    clip->format = format;
    clip->samples.assign(samples.begin(), samples.end());
    return clip;
}

struct Voice final {
    IXAudio2SourceVoice* handle{};
    std::shared_ptr<const Clip> clip;
    UINT32 sampleRate{};
    WORD channels{};

    void destroy() noexcept {
        if (handle) handle->DestroyVoice();
        handle = nullptr;
        clip.reset();
    }

    void reap() noexcept {
        if (!clip) return;
        XAUDIO2_VOICE_STATE state{};
        handle->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) clip.reset();
    }
};

}

struct ScriptAudio::Impl final {
    IXAudio2* engine{};
    IXAudio2MasteringVoice* master{};
    bool releaseCom{};
    std::array<Voice, MaxVoices> voices;
    std::list<std::pair<std::filesystem::path, std::shared_ptr<const Clip>>> clips;
    std::size_t cachedBytes{};

    ~Impl() {
        for (auto& voice : voices) voice.destroy();
        if (master) master->DestroyVoice();
        if (engine) engine->Release();
        if (releaseCom) CoUninitialize();
    }

    bool open() {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;
        releaseCom = SUCCEEDED(com);
        return SUCCEEDED(XAudio2Create(&engine)) && SUCCEEDED(engine->CreateMasteringVoice(&master));
    }

    std::shared_ptr<const Clip> cache(const std::filesystem::path& path) {
        const auto found = std::find_if(clips.begin(), clips.end(), [&path](const auto& entry) { return entry.first == path; });
        if (found != clips.end()) {
            clips.splice(clips.begin(), clips, found);
            return found->second;
        }

        auto clip = loadWave(path);
        if (!clip) return {};
        while (clips.size() >= MaxClips || cachedBytes + clip->samples.size() > MaxCacheBytes) {
            const auto expired = std::find_if(clips.rbegin(), clips.rend(), [](const auto& entry) { return entry.second.use_count() == 1; });
            if (expired == clips.rend()) return {};
            cachedBytes -= expired->second->samples.size();
            clips.erase(std::prev(expired.base()));
        }
        clips.emplace_front(path, clip);
        cachedBytes += clip->samples.size();
        return clip;
    }

    bool play(const std::filesystem::path& path, const float gain) {
        for (auto& voice : voices) voice.reap();
        const auto idle = std::find_if(voices.begin(), voices.end(), [](const Voice& entry) { return !entry.clip; });
        if (idle == voices.end()) return false;
        auto clip = cache(path);
        if (!clip) return false;

        const auto matched = std::find_if(voices.begin(), voices.end(), [&clip](const Voice& entry) {
            return !entry.clip && entry.handle && entry.channels == clip->format.nChannels && entry.sampleRate == clip->format.nSamplesPerSec;
        });
        auto& voice = matched == voices.end() ? *idle : *matched;
        if (matched == voices.end()) {
            voice.destroy();
            if (FAILED(engine->CreateSourceVoice(&voice.handle, &clip->format))) return false;
            voice.channels = clip->format.nChannels;
            voice.sampleRate = clip->format.nSamplesPerSec;
        }
        voice.clip = std::move(clip);
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(voice.clip->samples.size());
        buffer.pAudioData = voice.clip->samples.data();
        if (SUCCEEDED(voice.handle->SetVolume(gain)) && SUCCEEDED(voice.handle->SubmitSourceBuffer(&buffer)) &&
            SUCCEEDED(voice.handle->Start())) return true;
        voice.destroy();
        return false;
    }
};
#else
struct ScriptAudio::Impl final {};
#endif

ScriptAudio::ScriptAudio() = default;
ScriptAudio::~ScriptAudio() = default;

bool ScriptAudio::play(const std::filesystem::path& resolvedWave, const float gain) {
#ifdef _WIN32
    if (!std::isfinite(gain) || gain <= 0.0F) return false;
    try {
        if (!m_impl) {
            auto candidate = std::make_unique<Impl>();
            if (!candidate->open()) return false;
            m_impl = std::move(candidate);
        }
        return m_impl->play(resolvedWave, std::min(gain, 1.0F));
    } catch (const std::exception&) {
        return false;
    }
#else
    (void)resolvedWave;
    (void)gain;
    return false;
#endif
}

void ScriptAudio::clear() noexcept { m_impl.reset(); }

}
