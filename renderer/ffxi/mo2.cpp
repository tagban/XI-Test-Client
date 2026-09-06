#include "mo2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>

namespace ffxi
{
namespace
{
// Two-byte packed, like everything else here.
constexpr size_t kHeaderSize = 10;
constexpr size_t kElementSize = 84;

template <typename T> T read(const std::span<const uint8_t>& data, size_t offset)
{
    if (offset + sizeof(T) > data.size())
    {
        throw std::runtime_error("MO2: read past the end of the chunk");
    }
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
} // namespace

float Animation::frameSeconds() const
{
    // Speed is a multiplier on thirty frames a second. A speed of zero would
    // be a still frame rather than an infinitely long one.
    return speed > 0.0f ? 1.0f / (30.0f * speed) : 1.0f / 30.0f;
}

Animation parseMo2(const Chunk& chunk)
{
    const std::span<const uint8_t>& data = chunk.data;

    Animation animation;
    animation.name = std::string(chunk.id, 4);
    const uint16_t elements = read<uint16_t>(data, 2);
    const uint16_t frames = read<uint16_t>(data, 4);
    animation.speed = read<float>(data, 6);

    if (frames < 2 || elements == 0)
    {
        throw std::runtime_error("MO2: nothing to animate");
    }

    // Channel indices count floats from the start of the element block, so the
    // pool and the elements share an origin.
    const size_t poolBase = kHeaderSize;

    auto channel = [&](int32_t index, float constant, size_t frame) {
        if (index <= 0)
        {
            return constant;
        }
        return read<float>(data, poolBase + (static_cast<size_t>(index) + frame) * 4);
    };

    for (uint16_t e = 0; e < elements; ++e)
    {
        const size_t base = kHeaderSize + static_cast<size_t>(e) * kElementSize;

        AnimationTrack track;
        track.bone = read<uint32_t>(data, base);

        int32_t rotationIndex[4];
        float rotationBase[4];
        int32_t translationIndex[3];
        float translationBase[3];
        int32_t scaleIndex[3];
        float scaleBase[3];
        for (int c = 0; c < 4; ++c)
        {
            rotationIndex[c] = read<int32_t>(data, base + 4 + c * 4);
            rotationBase[c] = read<float>(data, base + 20 + c * 4);
        }
        for (int c = 0; c < 3; ++c)
        {
            translationIndex[c] = read<int32_t>(data, base + 36 + c * 4);
            translationBase[c] = read<float>(data, base + 48 + c * 4);
            scaleIndex[c] = read<int32_t>(data, base + 60 + c * 4);
            scaleBase[c] = read<float>(data, base + 72 + c * 4);
        }

        // A negative rotation index marks a bone the animation does not touch.
        // It still occupies an element, and reading it as data produces a
        // plausible-looking limb in the wrong place.
        const bool inert = rotationIndex[0] < 0 || rotationIndex[1] < 0 || rotationIndex[2] < 0 || rotationIndex[3] < 0;

        // Frame zero is not part of the animation - every one of these starts
        // at frame one - so it is dropped here rather than skipped at every
        // point of use.
        for (uint16_t f = 1; f < frames; ++f)
        {
            if (inert)
            {
                track.rotation.insert(track.rotation.end(), {0.0f, 0.0f, 0.0f, 1.0f});
                track.translation.insert(track.translation.end(), {0.0f, 0.0f, 0.0f});
                track.scale.insert(track.scale.end(), {1.0f, 1.0f, 1.0f});
                continue;
            }
            for (int c = 0; c < 4; ++c)
            {
                track.rotation.push_back(channel(rotationIndex[c], rotationBase[c], f));
            }
            for (int c = 0; c < 3; ++c)
            {
                track.translation.push_back(channel(translationIndex[c], translationBase[c], f));
            }
            for (int c = 0; c < 3; ++c)
            {
                track.scale.push_back(channel(scaleIndex[c], scaleBase[c], f));
            }
        }
        animation.tracks.push_back(std::move(track));
    }

    animation.frames = static_cast<uint16_t>(frames - 1);
    return animation;
}
} // namespace ffxi
