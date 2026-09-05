// MogHouse's renderer. Opens a window on WebGPU - Metal on macOS, D3D12 on
// Windows, Vulkan on Linux - and draws FFXI zone geometry read from the retail
// DATs.
//
// Given a DAT it draws that zone's collision geometry. Given nothing it only
// clears, so the graphics path can still be checked on a machine with no game
// installed.

#include "ffxi/dat.h"
#include "ffxi/filetable.h"
#include "ffxi/generator.h"
#include "ffxi/d3m.h"
#include "ffxi/sprite.h"
#include "ffxi/look.h"
#include "ffxi/mmb.h"
#include "ffxi/lighting.h"
#include "ffxi/mo2.h"
#include "ffxi/entitynames.h"
#include "ffxi/mzb.h"
#include "ffxi/os2.h"
#include "ffxi/skeleton.h"
#include "ffxi/texture.h"
#include "gputexture.h"
#include "camera.h"
#include "character.h"
#include "collision.h"
#include "viewer.h"

#include <deque>
#include "coverage.h"
#include "linalg.h"
#include "chat_shader.h"
#include "dialog_shader.h"
#include "hud_shader.h"
#include "inventory_shader.h"
#include "nameplate_shader.h"
#include "zoneline_shader.h"
#include "textfont.h"
#include "radar_shader.h"
#include "scene.h"
#include "surface.h"
#include "sky_shader.h"
#include "monorail.h"
#include "music.h"
#include "ffxi/soundrefs.h"
#include "sounds.h"
#include "water_shader.h"
#include "effect_shader.h"
#include "zone_shader.h"
#include "zonemesh.h"

#include <SDL3/SDL.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <string>
#include <fstream>
#include <set>
#include <sstream>

namespace
{
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth24Plus;

/// Where along the line a character-select line-up stands, and how far back
/// from it.
///
/// Well inside the long diagonal, which runs unbroken from 168 to 528 - the
/// only stretch with room for the whole train, which is sixty-three units long,
/// to pass without touching a bend at either end.
///
/// That matters because the cars are laid along the route one at a time and
/// each turns to the piece of track it is on, so through a corner they splay
/// rather than following each other round it. The first attempt at 150 put the
/// row immediately after a forty-seven degree bend, which is the worst place on
/// the line to watch from.
///
/// These two numbers are tied together. The train starts its run a whole
/// approach behind the row - at twenty-two units a second that is
/// 400 - 22*5 - 63 = 227 - and 227 is past 168, so it is on the straight from
/// the first frame rather than rounding the bend in shot. Lengthening the
/// approach without moving the row back is what breaks that.
inline constexpr float kLineupAlongTrack = 400.0f;
inline constexpr float kLineupFromTrack = 26.0f;

/// How long after the line-up appears the train reaches it.
inline constexpr float kLineupTrainSeconds = 5.0f;

/// How far below a carriage's own origin somebody riding in it stands.
///
/// The cars hang from the beam, so their origin is up at the coupling rather
/// than at the floor. Measured rather than guessed: mono_b1's geometry runs
/// from 2.00 below that origin to 3.80 above it, so its floor is the 2.00 and
/// this sits a rider a fraction inside it. Guessing 3.6 put their feet more
/// than a unit under the carriage and their legs hung out of the bottom.
inline constexpr float kRideDrop = 1.9f;

/// How much of a faded character is drawn. Enough to read the shape and the
/// colours, little enough that the one being pointed at is obviously the one in
/// front.
inline constexpr wgpu::Color kFadedBody{0.42, 0.42, 0.42, 1.0};

/// How many torches can light a frame at once. A zone has more - West
/// Ronfaure has forty-two - so the nearest are chosen each frame; the rest are
/// too far to be seen lighting anything.
constexpr int kMaxLamps = 24;

/// Where a flame stands and how far its light reaches.
struct Lamp
{
    float x{};
    float y{};
    float z{};
    float reach{};
};

/// A place in the zone that makes a noise, and which noise it makes.
///
/// Derived rather than listed. A zone DAT declares its sounds as 0x3D chunks
/// and its effects as generators, and where the two share a directory the
/// generators are saying where that sound is heard from - which is how a
/// waterfall gets fifty-six placements of one looping sound in West Ronfaure's
/// `mode/ligh/taki`. See docs/wiki/Audio-Formats.md.
struct SoundEmitter
{
    float x{};
    float y{};
    float z{};
    uint32_t sound{};
};

struct Uniforms
{
    float viewProjection[16];
    float lightDirection[4];
    float ambient[4];
    float sunlight[4];
    float fogColour[4];
    float fogRange[4];
    float eye[4];
    float lampCount[4];
    float lamps[kMaxLamps][4];
};

/// Matches RadarUniforms in radar_shader.h.
/// Index into the 4x6 font's character set, or 0 - a space - for anything it
/// does not have. Lower case folds to upper, because the font has no lower.
///
/// A missing glyph becomes a space rather than a wrong letter: a gap reads as
/// "this font cannot show that", where a substitution reads as a bug in
/// whatever produced the text.
inline int glyphIndex(char raw)
{
    static const std::string kOrder = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-.";
    const char upper = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
    const size_t found = kOrder.find(upper == '_' ? ' ' : upper);
    return found == std::string::npos ? 0 : static_cast<int>(found);
}

/// A character cell in the chat panel, wide over tall. The glyph is 4x6 with
/// a column of space beside it and two rows above and below.
inline constexpr float kGlyphAspect = 5.0f / 8.0f;

/// Name colours, by what the thing is.
///
/// The full list a player would expect - party, linkshell, friend, pet - needs
/// membership this client does not read from any packet yet. Rather than guess
/// at those, only the three that can actually be told apart are coloured.
inline constexpr float kHudBright[3] = {0.97f, 0.97f, 1.00f};
inline constexpr float kHudDim[3] = {0.78f, 0.82f, 0.90f};

inline constexpr float kNameWhite[3] = {0.98f, 0.98f, 1.00f};
inline constexpr float kNameNpc[3] = {0.60f, 0.98f, 0.60f};

/// How far above the top of a character its name sits. Added to the model's
/// own height rather than used as one: a fixed height tuned for a hume left a
/// tarutaru's name floating a whole body length over its head, because a taru
/// is 0.94 tall against a hume's 1.79.
inline constexpr float kPlateClearance = 0.16f;
inline constexpr float kNameMonster[3] = {0.98f, 0.86f, 0.30f};

/// A corpse. Named, targetable, and not worth attacking - the one state where
/// a monster should not be wearing the colour that says "fight me".
inline constexpr float kNameDead[3] = {0.55f, 0.55f, 0.58f};

/// A GM. Darker than the red an aggressive monster gets, which is the
/// distinction the real client draws.
inline constexpr float kNameGm[3] = {0.80f, 0.14f, 0.14f};

/// Matches HudUniforms in hud_shader.h.
struct HudUniforms
{
    float counts[4];
    float atlas[4];
    float boxes[mh::kHudStrings][4];
    float colours[mh::kHudStrings][4];
    float glyphs[mh::kHudStrings * mh::kHudChars][4];
    float bars[mh::kHudBars][4];
    float barColours[mh::kHudBars][4];
};

/// Matches InventoryUniforms in inventory_shader.h.
struct InventoryUniforms
{
    float counts[4];
    float font[4];
    float rects[mh::kInventoryQuads][4];
    float looks[mh::kInventoryQuads][4];
    float tints[mh::kInventoryQuads][4];
};

/// Matches NameplateUniforms in nameplate_shader.h.
struct NameplateUniforms
{
    float viewProjection[16];
    float counts[4];
    float atlas[4];
    float positions[mh::kNameplateMax][4];
    float colours[mh::kNameplateMax][4];
    float glyphs[mh::kNameplateMax * mh::kNameplateChars][4];
};

/// Matches ChatUniforms in chat_shader.h.
struct ChatUniforms
{
    float placement[4];
    float counts[4];
    float glyphs[mh::kChatLines * mh::kChatColumns][4];
};

/// Matches DialogUniforms in dialog_shader.h.
struct DialogUniforms
{
    float counts[4];
    float atlas[4];
    float panel[4];
    float caret[4];
    float rects[mh::kDialogRows][4];
    float fills[mh::kDialogRows][4];
    float boxes[mh::kDialogRows][4];
    float colours[mh::kDialogRows][4];
    float glyphs[mh::kDialogRows * mh::kDialogChars][4];
};

/// The death box. Amber for the heading, because that is the colour the game
/// itself uses to say something has happened to you.
inline constexpr float kDialogTitle[3] = {1.00f, 0.84f, 0.48f};
inline constexpr float kDialogText[3] = {0.86f, 0.89f, 0.96f};

/// A button's own label: bright when it can be pressed, and near enough to
/// the panel to disappear into it when it cannot.
inline constexpr float kDialogLabel[3] = {0.98f, 0.98f, 1.00f};
inline constexpr float kDialogLabelOff[3] = {0.42f, 0.44f, 0.50f};

/// And the button behind it. Greyed is not a lighter blue: a disabled button
/// that still looks like a button is one people press and then wonder about,
/// so it loses the colour entirely and keeps only the outline.
inline constexpr float kDialogButton[3] = {0.15f, 0.24f, 0.42f};
inline constexpr float kDialogButtonHot[3] = {0.27f, 0.42f, 0.68f};
inline constexpr float kDialogButtonOff[3] = {0.11f, 0.12f, 0.15f};

/// How far a corpse hauls itself on one press of the jump key, in world units.
///
/// Half a walking pace. A whole one reads as the body getting up and taking a
/// step, which is the opposite of the point; anything much under this is not
/// visible from across a clearing, which is the whole reason for it - the
/// person who might raise you is not standing over you.
inline constexpr float kCorpseDrag = 0.35f;

/// How much of the death clip to replay on that press, counted back from its
/// end. Its tail is the body's last settle onto the ground; its beginning is
/// the character still standing, which is why the clip is rewound to near the
/// end rather than restarted.
inline constexpr int kCorpseTwitchFrames = 3;

/// Where the box put a button last frame, so a click can be tested against
/// what the player is actually looking at.
///
/// Laid out as it is drawn and remembered rather than computed twice: the
/// text is proportional and the box is sized to fit it, so working out where
/// a button is means most of the work of drawing one.
struct DialogButton
{
    float left{};
    float bottom{};
    float width{};
    float height{};
    bool enabled{};
    mh::DeathChoice choice{mh::DeathChoice::None};

    bool holds(float x, float y) const
    {
        return width > 0.0f && x >= left && x < left + width && y >= bottom && y < bottom + height;
    }
};

// A Choice row's value is "<selected>;first|second|third". Kept as a string
// because that is what crosses the boundary and what the client gets back;
// these take it apart and put it together.
std::vector<std::string> choiceOptions(const std::string& value)
{
    std::vector<std::string> options;
    const size_t split = value.find(';');
    if (split == std::string::npos)
    {
        return options;
    }
    std::string rest = value.substr(split + 1);
    size_t start = 0;
    while (start <= rest.size())
    {
        const size_t bar = rest.find('|', start);
        options.push_back(rest.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos)
        {
            break;
        }
        start = bar + 1;
    }
    return options;
}

int choiceSelected(const std::string& value)
{
    const size_t split = value.find(';');
    if (split == std::string::npos || split == 0)
    {
        return 0;
    }
    return std::atoi(value.substr(0, split).c_str());
}

/// One lighting set part way between two others, for the frames after a
/// doorway is crossed: going from the street's light to a shop's in one frame
/// reads as a flash, and coming back out reads as another.
ffxi::LightingSet blendLighting(const ffxi::LightingSet& from, const ffxi::LightingSet& to, float t)
{
    if (t >= 1.0f)
    {
        return to;
    }
    const auto colour = [t](const ffxi::Colour& a, const ffxi::Colour& b) {
        return ffxi::Colour{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                            a.a + (b.a - a.a) * t};
    };
    const auto number = [t](float a, float b) { return a + (b - a) * t; };

    ffxi::LightingSet set = to;
    set.sunlight = colour(from.sunlight, to.sunlight);
    set.moonlight = colour(from.moonlight, to.moonlight);
    set.ambient = colour(from.ambient, to.ambient);
    set.fog = colour(from.fog, to.fog);
    set.minFog = number(from.minFog, to.minFog);
    set.maxFog = number(from.maxFog, to.maxFog);
    set.brightness = number(from.brightness, to.brightness);
    set.landscapeSunlight = colour(from.landscapeSunlight, to.landscapeSunlight);
    set.landscapeMoonlight = colour(from.landscapeMoonlight, to.landscapeMoonlight);
    set.landscapeAmbient = colour(from.landscapeAmbient, to.landscapeAmbient);
    set.landscapeFog = colour(from.landscapeFog, to.landscapeFog);
    set.landscapeMinFog = number(from.landscapeMinFog, to.landscapeMinFog);
    set.landscapeMaxFog = number(from.landscapeMaxFog, to.landscapeMaxFog);
    set.landscapeBrightness = number(from.landscapeBrightness, to.landscapeBrightness);
    set.fogColour = colour(from.fogColour, to.fogColour);
    set.fogOffset = number(from.fogOffset, to.fogOffset);
    set.maxFarClip = number(from.maxFarClip, to.maxFarClip);
    for (size_t i = 0; i < set.skyColours.size(); ++i)
    {
        set.skyColours[i] = colour(from.skyColours[i], to.skyColours[i]);
        set.skyAltitudes[i] = number(from.skyAltitudes[i], to.skyAltitudes[i]);
    }
    return set;
}

std::string choiceValue(int selected, const std::vector<std::string>& options)
{
    std::string value = std::to_string(selected) + ';';
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (i > 0)
        {
            value += '|';
        }
        value += options[i];
    }
    return value;
}

struct RadarUniforms
{
    float placement[4];
    float mapExtent[4];
    float viewer[4];
    float counts[4];
    float label[mh::kRadarMaxLabel][4];
    float entities[mh::kRadarMaxEntities][4];
};

struct SkyUniforms
{
    float forward[4];
    float right[4];
    float up[4];
    float skyColours[8][4];
    float skyAltitudes[8][4];
    float fogColour[4];
};

const char* backendName(wgpu::BackendType backend)
{
    switch (backend)
    {
    case wgpu::BackendType::D3D12: return "D3D12";
    case wgpu::BackendType::Metal: return "Metal";
    case wgpu::BackendType::Vulkan: return "Vulkan";
    case wgpu::BackendType::OpenGL: return "OpenGL";
    case wgpu::BackendType::OpenGLES: return "OpenGLES";
    default: return "other";
    }
}

// Loads the largest MZB in a DAT. Largest because the ferry DATs hold both a
// world and a vessel, and the world is the interesting one.
/// Reads one character out of a DAT: its skeleton, every mesh hung on it, and
/// the textures those meshes name.
///
/// A DAT like ROM/3/6.DAT holds a whole NPC. Player characters are assembled
/// from several files instead - one per equipment slot - which is the same
/// work with more inputs, so this takes a list.
/// A character kept in a form that can still be posed. The geometry is what
/// gets drawn; the skeleton, meshes and animations are what it is rebuilt from
/// every frame.
/// Matches ZoneLineUniforms in zoneline_shader.h.
struct ZoneLineUniforms
{
    float viewProjection[16]{};
    float counts[4]{};
    float lines[mh::kZoneLineMarkers][4]{};
    float axes[mh::kZoneLineMarkers][4]{};
};

struct LoadedCharacter
{
    ffxi::Skeleton skeleton;
    std::vector<ffxi::SkinnedModel> meshes;
    std::map<std::string, ffxi::Animation> animations;
    mh::Character geometry;
};

std::optional<LoadedCharacter> loadCharacter(const std::vector<std::string>& datPaths,
                                             std::unordered_map<std::string, ffxi::Texture>& textures)
{
    LoadedCharacter loaded;
    ffxi::Skeleton& skeleton = loaded.skeleton;
    bool haveSkeleton = false;
    std::vector<ffxi::SkinnedModel>& meshes = loaded.meshes;

    for (const std::string& datPath : datPaths)
    {
        ffxi::DatFile dat{std::filesystem::path{datPath}};

        // The first skeleton found wins. Everything after it has to be hung on
        // the same bones, so a second one would be a different character.
        if (!haveSkeleton)
        {
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkeleton))
            {
                try
                {
                    skeleton = ffxi::parseSkeleton(chunk);
                    haveSkeleton = true;
                    break;
                }
                catch (const std::exception& e)
                {
                    std::printf("skeleton %.4s: %s\n", chunk.id, e.what());
                }
            }
        }

        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
        {
            try
            {
                ffxi::Texture texture = ffxi::parseTexture(chunk);
                textures.insert_or_assign(texture.name, std::move(texture));
            }
            catch (const std::exception&)
            {
            }
        }

        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkinnedMesh))
        {
            try
            {
                ffxi::SkinnedModel model = ffxi::parseOs2(chunk);
                if (!model.parts.empty())
                {
                    meshes.push_back(std::move(model));
                }
            }
            catch (const std::exception& e)
            {
                std::printf("skinned %.4s: %s\n", chunk.id, e.what());
            }
        }

        // Animations. The race's own file carries most of them; a piece of
        // equipment can bring its own, which is why they are collected from
        // every file rather than only the first.
        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkAnimation))
        {
            try
            {
                ffxi::Animation animation = ffxi::parseMo2(chunk);
                loaded.animations.insert_or_assign(animation.name, std::move(animation));
            }
            catch (const std::exception&)
            {
                // Eye and mouth tracks share the chunk type and are not poses.
            }
        }
    }

    if (!haveSkeleton || meshes.empty())
    {
        std::printf("no character in those files\n");
        return std::nullopt;
    }

    loaded.geometry = mh::buildCharacter(mh::bindPose(skeleton), meshes, textures);
    std::printf("character: %zu bones, %zu meshes, %zu triangles, %.2f tall, %zu animations\n",
                skeleton.bones.size(), meshes.size(), loaded.geometry.triangles(), loaded.geometry.height(),
                loaded.animations.size());
    return loaded;
}

/// The interior DATs that belong with a zone file, from assets/subrooms.txt.
///
/// A city zone's own DAT is only its shell - the insides of its buildings are
/// separate files. They are built exactly the same way and their placements are
/// already in zone coordinates, so they need loading and no alignment at all.
///
/// Keyed by the zone's own path relative to the install root, which is what
/// this has to hand; nothing here needs to know a zone id.
/// The words after "<zone dat>:" in an asset file of that shape - subrooms.txt,
/// hidden-models.txt - for the zone at zonePath. Empty when the file or the
/// zone's line is absent.
/// Opens an asset by name, wherever the renderer happens to have been started
/// from - as a library inside the app, as a standalone binary from the build
/// directory, or from the source tree. Only the first of those has an assets/
/// beside its working directory, which is why this looks in four places.
std::ifstream openAsset(const char* fileName, const char* envOverride)
{
    std::vector<std::filesystem::path> candidates;
    if (const char* fromEnv = envOverride ? std::getenv(envOverride) : nullptr)
    {
        candidates.emplace_back(fromEnv);
    }
    if (const char* nativeDir = std::getenv("MOGHOUSE_NATIVE_DIR"))
    {
        candidates.push_back(std::filesystem::path{nativeDir} / "assets" / fileName);
    }
    if (const char* fontDir = std::getenv("MOGHOUSE_FONT"))
    {
        candidates.push_back(std::filesystem::path{fontDir} / fileName);
    }
    // Beside the executable, which is where CMake puts them and where the
    // working directory is not. Running the standalone renderer from the
    // repository root found no assets at all - the relative path below only
    // works when it happens to be launched from inside build-renderer.
    if (const char* base = SDL_GetBasePath())
    {
        candidates.push_back(std::filesystem::path{base} / "assets" / fileName);
    }
    candidates.push_back(std::filesystem::path{"assets"} / fileName);

    std::ifstream file;
    for (const std::filesystem::path& candidate : candidates)
    {
        file.open(candidate);
        if (file)
        {
            return file;
        }
        file.clear();
    }
    return file;
}

/// The models that come up out of the ground, from assets/burrowers.txt.
///
/// Read once. A worm heaving itself out of the earth is half of what makes one
/// recognisable, and the list is a file rather than a constant because most of
/// the common worms carry model 0 in the server's own tables and had to be
/// found by looking - see the note in that file.
const std::set<uint16_t>& burrowerModels()
{
    static const std::set<uint16_t> models = [] {
        std::set<uint16_t> found;
        std::ifstream file = openAsset("burrowers.txt", "MOGHOUSE_BURROWERS");
        std::string line;
        while (std::getline(file, line))
        {
            const size_t hash = line.find('#');
            if (hash != std::string::npos)
            {
                line.erase(hash);
            }
            std::istringstream words{line};
            int model = 0;
            while (words >> model)
            {
                if (model > 0 && model <= 0xFFFF)
                {
                    found.insert(static_cast<uint16_t>(model));
                }
            }
        }
        if (std::getenv("MOGHOUSE_SPAWN_WATCH") != nullptr)
        {
            std::printf("burrowers: %zu models that come up out of the ground\n", found.size());
        }
        return found;
    }();
    return models;
}

/// How far below the ground to draw something that is still coming up, and
/// zero once it has arrived.
///
/// The client cannot know when a mob spawned, only when it first heard about
/// one - an entity walking into range is new to us and old to the world. That
/// is a small lie for a worm and a smaller one than never emerging at all, so
/// this keys off first sighting.
///
/// Eased rather than linear: a worm shoves itself clear and then settles,
/// which is a curve that starts fast. Linear reads as a lift.
float emergeOffset(const mh::RadarEntity& entity, float scale, float sinceZoneSeconds)
{
    constexpr float kEmergeSeconds = 1.1f;

    // Nothing emerges in the first moments of a zone. Everything the server
    // names then was already there, and the client cannot tell the difference -
    // it has only just heard of all of it.
    constexpr float kSettleSeconds = 3.0f;
    if (sinceZoneSeconds < kSettleSeconds)
    {
        return 0.0f;
    }

    if (entity.spawnedSecondsAgo < 0.0f || entity.spawnedSecondsAgo >= kEmergeSeconds)
    {
        return 0.0f;
    }
    if (!burrowerModels().contains(entity.modelId))
    {
        return 0.0f;
    }

    // Far enough under to be out of sight at the start, in the model's own
    // terms so a big one does not peek over the edge of its hole.
    constexpr float kBuried = 2.4f;
    const float t = entity.spawnedSecondsAgo / kEmergeSeconds;
    const float eased = 1.0f - (1.0f - t) * (1.0f - t);   // fast, then settling
    return -(1.0f - eased) * kBuried * scale;
}

std::vector<std::string> assetWordsFor(const char* fileName, const char* envOverride,
                                       const std::filesystem::path& zonePath)
{
    const std::filesystem::path root = ffxi::defaultInstallRoot();
    std::error_code ignored;
    std::string key = std::filesystem::relative(zonePath, root, ignored).generic_string();
    if (key.empty())
    {
        return {};
    }

    // One search, shared with openAsset - it used to have its own copy, and
    // the copy was the one missing "beside the executable".
    std::ifstream file = openAsset(fileName, envOverride);
    if (!file)
    {
        return {};
    }

    std::vector<std::string> found;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || line.compare(0, colon, key) != 0)
        {
            continue;
        }
        std::istringstream rest{line.substr(colon + 1)};
        std::string one;
        while (rest >> one)
        {
            found.push_back(one);
        }
        break;
    }
    return found;
}

std::vector<std::filesystem::path> subroomsFor(const std::filesystem::path& zonePath)
{
    const std::filesystem::path root = ffxi::defaultInstallRoot();
    std::vector<std::filesystem::path> found;
    for (const std::string& one : assetWordsFor("subrooms.txt", "MOGHOUSE_SUBROOMS", zonePath))
    {
        found.push_back(root / one);
    }
    return found;
}

/// Whether a sprite animation's texture marks a light source rather than
/// something visible. Bastok Markets' `lt` and `ligh` animations carry
/// "effect  light" and "effect  light2", one frame each, and sit beside every
/// lamp and torch in the effe/ligh directory. The user, who knows the game,
/// says these are invisible in retail: they mark where a flame lights the
/// ground, and are not the flare itself.
bool isLightSource(const std::string& texture)
{
    std::string own = texture.size() > 8 ? texture.substr(8) : texture;
    while (!own.empty() && (own.back() == ' ' || own.back() == 0))
    {
        own.pop_back();
    }
    return own == "light" || own == "light2";
}

/// The same thing, asked of the directory rather than the texture.
///
/// The name test above catches Bastok Markets, whose light sheets are actually
/// called "light" and "light2". It catches nothing in West Ronfaure, where the
/// same job is done by sheets named hit3 and hit4 - so forty-two of them drew,
/// as pale pixelated discs two and a half to three units across, sitting over
/// every torch and waterfall and washing out the trees behind them.
///
/// What both have in common is where they live: a `ligh` directory. West
/// Ronfaure's are under `mode/ligh/taki` and `mode/ligh/s_li`, Bastok's under
/// `effe/ligh`. The directory is the reliable signal and the texture name was
/// the incidental one.
///
/// Same reasoning as before: these mark where a lamp throws light on the
/// ground, retail draws nothing for them, and the lighting they should cast is
/// not built yet - so for now they are left out rather than drawn.
bool isLightDirectory(const std::string& directory)
{
    return directory.find("/ligh/") != std::string::npos ||
           (directory.size() >= 5 && directory.compare(directory.size() - 5, 5, "/ligh") == 0);
}

/// Whether a light directory holds markers rather than anything to look at.
///
/// The distinction is `mode` against `effe`, and getting it wrong put out
/// every lamp in San d'Oria. West Ronfaure's markers - the pale discs three
/// units across that hung over each torch - are `mode/ligh/taki` and
/// `mode/ligh/s_li`. Southern San d'Oria's `effe/ligh` is a different thing
/// entirely: a hundred and eighty-four `lig1` flames at scale 0.4, one in each
/// wall sconce, and they are meant to be seen.
///
/// Bastok Markets has markers under `effe/ligh` too, so the directory alone
/// cannot settle it there - those are caught by their sheet being called
/// "light" or "light2", which is what isLightSource() is for. Between the two
/// tests every marker seen so far is excluded and no flame is.
bool isMarkerOnlyDirectory(const std::string& directory)
{
    return directory.find("/mode/ligh") != std::string::npos;
}

/// Which of a zone's four skies the weather calls for.
///
/// A zone's DAT ships exactly four - `suny`, `fine`, `clod` and `mist`, each
/// with its own cloud dome, moon, stars and lens flares - while the server
/// numbers twenty weathers. So several necessarily share a sky, and this is
/// where it is decided.
///
/// **Provisional.** The four names and the twenty numbers are both certain;
/// which maps to which is read from what the words mean and is not yet checked
/// against a retail client standing in the same zone under the same weather.
/// `!weather <n>` on a LandSandBoat server sets it, which makes that a quick
/// thing to settle one row at a time. Kept as one table so correcting a row
/// costs nothing.
/// Volume as eleven steps, because the form has a dropdown and no slider.
/// Coarse on purpose: the difference between 45 and 50 per cent is not worth a
/// row of the menu, and eleven options fit on one screen where a hundred do not.
const std::vector<std::string>& volumeSteps()
{
    static const std::vector<std::string> steps = [] {
        std::vector<std::string> made;
        for (int i = 0; i <= 10; ++i)
        {
            made.push_back(std::to_string(i * 10) + "%");
        }
        return made;
    }();
    return steps;
}

int volumeStep(float volume)
{
    return std::clamp(static_cast<int>(std::lround(volume * 10.0f)), 0, 10);
}

/// The two icons the interface needs that the game never drew.
///
/// FFXI's own menu is text, so there is no bag and no cog anywhere in the menu
/// DAT - the closest it has are a journal, an hourglass and the round chat
/// markers, none of which mean either of these. Borrowing one of those would
/// read as a mistake, so these are drawn.
///
/// Four samples to a pixel: a cog's teeth alias badly at this size, and the
/// difference between a soft edge and a jagged one is most of whether a
/// generated icon looks deliberate.
std::vector<uint8_t> drawToolbarIcon(int which)
{
    constexpr int kSide = 32;
    std::vector<uint8_t> pixels(static_cast<size_t>(kSide) * kSide * 4, 0);

    const auto coverage = [which](float x, float y)
    {
        const float dx = x - 15.5f;
        const float dy = y - 15.5f;
        const float radius = std::sqrt(dx * dx + dy * dy);

        if (which == 2)
        {
            // A heater shield: flat across the top, curving to a point.
            const float down = (y - 5.0f) / 22.0f;
            if (down < 0.0f || down > 1.0f)
            {
                return 0.0f;
            }
            const float halfWidth = 11.0f * (1.0f - std::pow(down, 2.3f));
            return std::fabs(dx) <= halfWidth ? 1.0f : 0.0f;
        }

        if (which == 1)
        {
            // A ring with eight teeth and a hole: inside the body, or inside a
            // tooth, and outside the middle.
            constexpr float kBody = 9.0f;
            constexpr float kTeeth = 13.5f;
            constexpr float kHole = 4.2f;
            const float angle = std::atan2(dy, dx);
            const bool onTooth = std::cos(angle * 8.0f) > 0.45f;
            const bool solid = radius <= kBody || (radius <= kTeeth && onTooth);
            return solid && radius >= kHole ? 1.0f : 0.0f;
        }

        // A pouch: a body that widens towards the bottom, and a handle over it.
        const float fromTop = y - 13.0f;
        if (fromTop >= 0.0f && y <= 27.5f)
        {
            const float spread = 5.5f + fromTop * 0.28f;
            if (std::fabs(dx) <= spread)
            {
                return 1.0f;
            }
        }

        // The handle, an arc standing on the mouth of the bag.
        const float handleY = y - 13.0f;
        const float handle = std::sqrt(dx * dx + handleY * handleY);
        if (handleY <= 0.0f && handle >= 4.0f && handle <= 6.0f)
        {
            return 1.0f;
        }

        return 0.0f;
    };

    for (int y = 0; y < kSide; ++y)
    {
        for (int x = 0; x < kSide; ++x)
        {
            float hit = 0.0f;
            for (int sub = 0; sub < 4; ++sub)
            {
                hit += coverage(static_cast<float>(x) + (sub % 2 == 0 ? 0.25f : 0.75f),
                                static_cast<float>(y) + (sub < 2 ? 0.25f : 0.75f));
            }

            const float alpha = hit * 0.25f;
            const size_t at = (static_cast<size_t>(y) * kSide + x) * 4;
            pixels[at + 0] = 210;
            pixels[at + 1] = 216;
            pixels[at + 2] = 236;
            pixels[at + 3] = static_cast<uint8_t>(alpha * 255.0f);
        }
    }

    return pixels;
}

/// What the interface can be scaled by, with "Auto" for letting the window
/// decide. Zero is what Auto stores, which is why it is not in the values.
std::vector<std::string> uiScaleSteps()
{
    return {"Auto", "75%", "100%", "125%", "150%", "175%", "200%", "250%", "300%"};
}

float uiScaleForStep(int step)
{
    static const float kValues[] = {0.0f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
    const int count = static_cast<int>(sizeof(kValues) / sizeof(kValues[0]));
    return step > 0 && step < count ? kValues[step] : 0.0f;
}

int uiScaleStep(float value)
{
    if (value <= 0.0f)
    {
        return 0;
    }

    // Nearest, so a file hand-edited to 1.3 shows as 125% rather than as Auto.
    int best = 1;
    float closest = 1e9f;
    for (int step = 1; step < static_cast<int>(uiScaleSteps().size()); ++step)
    {
        const float distance = std::fabs(uiScaleForStep(step) - value);
        if (distance < closest)
        {
            closest = distance;
            best = step;
        }
    }
    return best;
}

/// The options menu.
///
/// Built here rather than by the client, unlike every other form: these are the
/// renderer's own levels, and sending a volume across the boundary and waiting
/// for it to come back would put a round trip between the key and the sound.
/// The client keeps what it is told, through the settings it already carries.
mh::Form optionsMenu(float musicVolume, float soundVolume, float uiScaleChoice)
{
    mh::Form form;
    form.title = "OPTIONS";
    form.rows.push_back(mh::FormRow{mh::FormRowKind::Choice, "MUSIC",
                                    choiceValue(volumeStep(musicVolume), volumeSteps()), true});
    form.rows.push_back(mh::FormRow{mh::FormRowKind::Choice, "SOUND",
                                    choiceValue(volumeStep(soundVolume), volumeSteps()), true});
    form.rows.push_back(mh::FormRow{mh::FormRowKind::Choice, "UI SCALE",
                                    choiceValue(uiScaleStep(uiScaleChoice), uiScaleSteps()), true});
    form.rows.push_back(mh::FormRow{mh::FormRowKind::Label,
                                    "Ambience follows Sound. Auto scale follows the window.", "", true});
    form.rows.push_back(mh::FormRow{mh::FormRowKind::Button, "CLOSE", "", true});
    return form;
}

/// The colour of the orb the weather is shown as, or nullptr for none.
///
/// FFXI's twenty weathers come in pairs sharing an element - rain and squall
/// are water, snow and blizzards ice - and retail shows the element beside the
/// clock. The first four carry no element at all: no weather, sunshine, clouds
/// and fog are a sky rather than a strength, so they take the sky's own colour
/// instead of an elemental one.
///
/// The pairing is the game's, and it is the same order the server numbers them
/// in - see FfxiWeather. Only the colours are chosen here.
const float* weatherOrb(int weather)
{
    static const float kSun[4] = {1.00f, 0.86f, 0.42f, 0.95f};
    static const float kCloud[4] = {0.72f, 0.74f, 0.78f, 0.95f};
    static const float kFog[4] = {0.60f, 0.62f, 0.64f, 0.85f};
    static const float kFire[4] = {0.94f, 0.33f, 0.24f, 0.95f};
    static const float kEarth[4] = {0.80f, 0.65f, 0.33f, 0.95f};
    static const float kWater[4] = {0.33f, 0.58f, 0.92f, 0.95f};
    static const float kWind[4] = {0.42f, 0.80f, 0.50f, 0.95f};
    static const float kIce[4] = {0.62f, 0.88f, 0.94f, 0.95f};
    static const float kLightning[4] = {0.72f, 0.48f, 0.90f, 0.95f};
    static const float kLight[4] = {0.96f, 0.95f, 0.88f, 0.95f};
    static const float kDark[4] = {0.42f, 0.32f, 0.52f, 0.95f};

    switch (weather)
    {
    case 1:  return kSun;
    case 2:  return kCloud;
    case 3:  return kFog;
    case 4:
    case 5:  return kFire;
    case 6:
    case 7:  return kWater;
    case 8:
    case 9:  return kEarth;
    case 10:
    case 11: return kWind;
    case 12:
    case 13: return kIce;
    case 14:
    case 15: return kLightning;
    case 16:
    case 17: return kLight;
    case 18:
    case 19: return kDark;
    default: return nullptr;
    }
}

/// What the weather is called, for the label beside the orb.
const char* weatherName(int weather)
{
    static const char* kNames[20] = {"",         "Sunshine", "Clouds",   "Fog",       "Hot Spell",
                                     "Heat Wave", "Rain",    "Squall",   "Dust Storm", "Sand Storm",
                                     "Wind",     "Gales",    "Snow",     "Blizzards", "Thunder",
                                     "Thunderstorms", "Auroras", "Stellar Glare", "Gloom", "Darkness"};
    return weather >= 0 && weather < 20 ? kNames[weather] : "";
}

const char* skyForWeather(int weather)
{
    switch (weather)
    {
        case 1:  // sunshine
        case 4:  // hot spell
        case 5:  // heat wave
            return "suny";

        case 2:  // clouds
        case 10: // wind
        case 11: // gales
        case 14: // thunder
        case 15: // thunderstorms
            return "clod";

        case 3:  // fog
        case 6:  // rain
        case 7:  // squall
        case 8:  // dust storm
        case 9:  // sand storm
        case 12: // snow
        case 13: // blizzards
            return "mist";

        // 0 is "none", which every indoor zone reports and which the server
        // also sends where it simply has no opinion. The odd ones at the top
        // of the range - auroras, stellar glare, gloom, darkness - belong to
        // places built long after these four skies were, and have no sky of
        // their own to pick. Both fall back to the clear one.
        default:
            return "fine";
    }
}

/// MOGHOUSE_WEATHER=<0..19> stands a zone under a weather of your choosing,
/// which is the only way to see three of its four skies without a server that
/// happens to be running that weather. Below zero when unset.
int forcedWeatherFromEnvironment()
{
    const char* set = std::getenv("MOGHOUSE_WEATHER");
    return set != nullptr ? std::atoi(set) : -1;
}

/// Whether a generator belongs to this zone or to the library every zone
/// carries a copy of.
///
/// `fser` and `fses` hold the same 39 and 32 generators in every DAT - checked
/// across Southern San d'Oria, Bastok Markets and East Ronfaure, identical
/// counts in all three. They are the effects the game fires when something
/// happens, a spell landing or a weapon skill, rather than anything standing
/// in the world, and every one of them says it is at the origin because where
/// it goes is settled when it is used.
///
/// The zone's own generators are in `effe` and `mode`, and between them those
/// two have not one generator at the origin in any zone looked at.
bool isTriggeredEffectLibrary(const std::string& directory)
{
    return directory.find("/fser") != std::string::npos ||
           directory.find("/fses") != std::string::npos;
}

/// Models a player is allowed to stand on, from assets/standable-models.txt.
///
/// A flat list rather than a per-zone one: a bench is the same model wherever
/// it is placed, and a list keyed by zone would repeat itself for every plaza
/// in the game.
const std::vector<std::string>& standableModels()
{
    static const std::vector<std::string> named = [] {
        std::vector<std::string> found;
        std::ifstream file = openAsset("standable-models.txt", "MOGHOUSE_STANDABLE_MODELS");
        if (!file)
        {
            return found;
        }

        std::string line;
        while (std::getline(file, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.pop_back();
            }
            if (!line.empty() && line[0] != '#')
            {
                found.push_back(line);
            }
        }
        return found;
    }();

    return named;
}

/// Placed models the retail client does not show, from assets/hidden-models.txt:
/// the older of two versions of a building the placement table lists at the
/// same spot. See the file for the case and what is not known.
std::vector<std::string> hiddenModelsFor(const std::filesystem::path& zonePath)
{
    return assetWordsFor("hidden-models.txt", "MOGHOUSE_HIDDEN_MODELS", zonePath);
}

/// The zone's water surfaces, precomputed by tools/ximesh.py.
///
/// Water is a material on each collision triangle - ShallowWater and DeepWater
/// in the server's own TerrainType - rather than a model with a recognisable
/// name or a height on the MZB's cells. Both of those were tried and both were
/// wrong. The server ships the decoded meshes, so the triangles are lifted out
/// of those and written world-space ahead of time; reading them here would mean
/// linking zlib to parse a file that never changes.
size_t loadWater(const std::string& zoneName, mh::Scene& scene)
{
    // The file is named the way the server names its zone directories -
    // Bastok_Markets, Southern_San_dOria - and the name arriving here is the
    // one shown to the player, which has had its underscores turned into
    // spaces for display. Looking up "Bastok Markets.water" found nothing, so
    // every zone entered by zoning had no material water at all; only the
    // named water models showed, which is why a canal with no `water` model
    // over it was dry.
    std::string stem;
    stem.reserve(zoneName.size());
    for (char c : zoneName)
    {
        if (c == '\'')
        {
            continue;                       // Southern San d'Oria -> Southern_San_dOria
        }
        stem.push_back(c == ' ' ? '_' : c);
    }

    std::filesystem::path path = std::filesystem::path{"assets"} / "water" / (stem + ".water");
    if (const char* nativeDir = std::getenv("MOGHOUSE_NATIVE_DIR"))
    {
        const std::filesystem::path beside =
            std::filesystem::path{nativeDir} / "assets" / "water" / (stem + ".water");
        if (std::filesystem::exists(beside))
        {
            path = beside;
        }
    }
    if (const char* fontDir = std::getenv("MOGHOUSE_FONT"))
    {
        const std::filesystem::path beside = std::filesystem::path{fontDir} / "water" / (stem + ".water");
        if (std::filesystem::exists(beside))
        {
            path = beside;
        }
    }

    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return 0;
    }

    char magic[4] = {};
    uint32_t count = 0;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (std::string(magic, 4) != "MHWA" || count == 0 || count > 4000000)
    {
        return 0;
    }

    std::vector<float> corners(static_cast<size_t>(count) * 9);
    file.read(reinterpret_cast<char*>(corners.data()),
              static_cast<std::streamsize>(corners.size() * sizeof(float)));
    if (!file)
    {
        return 0;
    }

    for (uint32_t triangle = 0; triangle < count; ++triangle)
    {
        // Already flat at its pool's waterline - tools/ximesh.py works that
        // out, because the MZB's per-cell height cannot: Windurst Waters
        // writes the same 0.0010 into every water cell, which is a flag rather
        // than a height, and lifting to it drags surfaces under their own bed.
        for (int corner = 0; corner < 3; ++corner)
        {
            const float* p = corners.data() + (static_cast<size_t>(triangle) * 9 + corner * 3);
            mh::Vertex vertex{};
            vertex.position[0] = p[0];
            vertex.position[1] = p[1];
            vertex.position[2] = p[2];
            vertex.normal[1] = 1.0f;
            // World-space UVs, so the surface is continuous across the seams
            // between one collision block and the next.
            vertex.uv[0] = p[0] * 0.06f;
            vertex.uv[1] = p[2] * 0.06f;
            scene.waterIndices.push_back(static_cast<uint32_t>(scene.waterVertices.size()));
            scene.waterVertices.push_back(vertex);
        }
    }
    return count;
}

/// West Ronfaure's DAT, whose day is lent to zones that have none. Its file id
/// rather than its zone id: 100 is the offset between the two.
inline constexpr size_t kBorrowedLightingFileId = 200;

std::optional<mh::Scene> loadZone(const char* datPath, const char* keyPath, const char* key2Path, std::string& zoneId,
                                     std::unordered_map<std::string, ffxi::Texture>& textures, ffxi::Lighting& lighting,
                                     mh::Collision& collision, std::vector<mh::InteriorLighting>& interiors,
                                     mh::Scene& skyObjects,
                                     std::unordered_map<std::string, ffxi::IntensityCurve>& curves,
                                     std::unordered_map<std::string, ffxi::SpriteAnimation>& sprites,
                                     std::vector<mh::SpriteInstance>& spriteInstances,
                                     int weather, std::vector<Lamp>& lamps, std::vector<SoundEmitter>& emitters)
{
    lamps.clear();
    emitters.clear();

    // How far a torch throws light, as a multiple of the marker's own size.
    // The marker is a disc the artists sized to the lit patch, so its scale is
    // the right thing to key off; the multiple is a guess and is the one number
    // here worth checking against a retail client.
    static const float lampReach = [] {
        const char* set = std::getenv("MOGHOUSE_LAMP_REACH");
        return set != nullptr ? static_cast<float>(std::atof(set)) : 8.0f;
    }();

    // Which of this zone's four skies to build. Chosen once, here, because the
    // sky is baked into the scene with everything else - changing weather
    // without reloading the zone needs a rebuild path that does not exist yet.
    const std::string skyDirectory = std::string("/weat/") + skyForWeather(weather);
    std::printf("weather: %d, sky %s\n", weather, skyForWeather(weather));

    spriteInstances.clear();
    auto keys = ffxi::KeyTable::load(keyPath);
    if (!keys)
    {
        std::printf("could not read a 256-byte key table from %s\n", keyPath);
        return std::nullopt;
    }

    ffxi::DatFile dat{std::filesystem::path{datPath}};

    // The lighting sets sit one per weather under weat/<weather>, and
    // Lighting::add keeps the first chunk it sees for each hour. The first
    // weather directory in every city file is "clod" - cloudy - which is why
    // the night sky was a flat grey against retail's deep blue under a clear
    // sky. Fine weather's sets are taken when the file has them; a file with
    // no weather directories (Sel Phiner) keeps whatever it has. Which
    // weather the server has set (packet 0x057) is not read yet.
    {
        std::vector<const ffxi::Chunk*> fine;
        std::vector<const ffxi::Chunk*> any;
        std::vector<std::string> path;
        for (const ffxi::Chunk& chunk : dat.chunks())
        {
            if (chunk.type == ffxi::kChunkEnd)
            {
                if (!path.empty())
                {
                    path.pop_back();
                }
                continue;
            }
            if (chunk.type == ffxi::kChunkDirectory)
            {
                std::string dir(chunk.id, 4);
                while (!dir.empty() && (dir.back() == ' ' || dir.back() == 0))
                {
                    dir.pop_back();
                }
                path.push_back(dir);
                continue;
            }
            if (chunk.type != ffxi::kChunkLighting)
            {
                continue;
            }
            any.push_back(&chunk);
            if (std::find(path.begin(), path.end(), "fine") != path.end())
            {
                fine.push_back(&chunk);
            }
        }
        for (const ffxi::Chunk* chunk : fine.empty() ? any : fine)
        {
            lighting.add(*chunk);
        }
        if (!fine.empty())
        {
            std::printf("lighting: %zu sets from the fine weather\n", fine.size());
        }
    }

    // A zone with no day of its own borrows one.
    //
    // Sel Phiner - the backdrop the retail client stands its characters in -
    // ships exactly one lighting set, for midnight. Interpolating a day out of
    // a single entry gives that entry at every hour, so the sky was black at
    // noon and the grass was lit by moonlight, which is what "this zone was
    // never finished" looks like from inside.
    //
    // Two sets is the least that can describe a change, so anything less takes
    // a full day from a zone that has one. West Ronfaure by default: open
    // grassland under an open sky, which is what these zones are, and present
    // in every installation.
    if (lighting.sets().size() < 2)
    {
        const char* donor = SDL_getenv("MOGHOUSE_LIGHTING_FROM");
        std::string borrowed = donor ? donor : "";
        if (borrowed.empty())
        {
            try
            {
                const ffxi::FileTable table{ffxi::defaultInstallRoot()};
                if (auto path = table.path(kBorrowedLightingFileId))
                {
                    borrowed = path->string();
                }
            }
            catch (const std::exception&)
            {
                // No file table, no loan. The zone keeps whatever it had.
            }
        }

        if (!borrowed.empty() && borrowed != datPath)
        {
            try
            {
                ffxi::DatFile lender{std::filesystem::path{borrowed}};
                size_t added = 0;
                for (const ffxi::Chunk& chunk : lender.chunksOfType(ffxi::kChunkLighting))
                {
                    lighting.add(chunk);
                    ++added;
                }
                if (added)
                {
                    std::printf("lighting: this zone has none of its own, borrowed %zu sets\n", added);
                }
            }
            catch (const std::exception& e)
            {
                std::printf("could not borrow lighting: %s\n", e.what());
            }
        }
    }

    // Textures are not obfuscated, so they need no keys.
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
    {
        try
        {
            ffxi::Texture texture = ffxi::parseTexture(chunk);
            std::string key = texture.name;
            textures.emplace(std::move(key), std::move(texture));
        }
        catch (const std::exception&)
        {
        }
    }

    // Every model in the DAT, keyed by the name a placement refers to it by.
    std::unordered_map<std::string, ffxi::Model> models;
    // And by the four-character id of the chunk holding it, which is how an
    // effect generator refers to one: "funm" for funmiz, "alls" for allsea.
    //
    // Ids are not unique in a file. Bastok Markets has two chunks called
    // "auc_": auc_lt, the lamp glow in effe/ligh, and auc_stdl, the auction
    // house stand with its stairs under mode. The glow's generator says
    // "auc_", the first match was the stand, and the stairs were placed a
    // second time as a scrolling effect - "flowing like water". So each id
    // keeps every chunk that carries it with the directory it sits in, and a
    // generator takes the one from its own directory.
    struct ChunkModel
    {
        std::string directory;
        std::string name;
    };
    std::unordered_map<std::string, std::vector<ChunkModel>> modelsByChunkId;
    size_t modelsFailed = 0;
    if (key2Path)
    {
        if (auto keys2 = ffxi::KeyTable::load(key2Path))
        {
            std::vector<std::string> path;
            for (const ffxi::Chunk& chunk : dat.chunks())
            {
                if (chunk.type == ffxi::kChunkEnd)
                {
                    if (!path.empty())
                    {
                        path.pop_back();
                    }
                    continue;
                }
                if (chunk.type == ffxi::kChunkDirectory)
                {
                    std::string dir(chunk.id, 4);
                    while (!dir.empty() && (dir.back() == ' ' || dir.back() == 0))
                    {
                        dir.pop_back();
                    }
                    path.push_back(dir);
                    continue;
                }
                if (chunk.type != ffxi::kChunkMmb)
                {
                    continue;
                }
                try
                {
                    ffxi::Model model = ffxi::parseMmb(chunk, *keys, *keys2);
                    std::string key = model.name;
                    std::string chunkId(chunk.id, 4);
                    while (!chunkId.empty() && (chunkId.back() == ' ' || chunkId.back() == 0))
                    {
                        chunkId.pop_back();
                    }
                    std::string directory;
                    for (size_t i = 0; i < path.size(); ++i)
                    {
                        directory += (i ? "/" : "") + path[i];
                    }
                    modelsByChunkId[chunkId].push_back(ChunkModel{std::move(directory), key});
                    models.emplace(std::move(key), std::move(model));
                }
                catch (const std::exception&)
                {
                    ++modelsFailed;
                }
            }
        }
    }
    // The shared effects file, ROM/0/0.DAT: the flames, sparks and glows
    // that every zone's generators reach into, as type 0x1f meshes with
    // their textures. A zone DAT carries only an empty MMB stub under the
    // same id - "hi12", the torch flame - so an id that resolves to a model
    // with no triangles is looked up here. Loaded with the zone; forty-five
    // small meshes and sixty textures.
    std::unordered_map<std::string, std::string> sharedByChunkId;
    // The sprite animations by id, from this zone and the shared file - the
    // zone's own first, so a zone's "lt" glow wins over any shared one.
    sprites.clear();
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSprite))
    {
        if (std::optional<ffxi::SpriteAnimation> sprite = ffxi::parseSprite(chunk))
        {
            sprites.emplace(sprite->name, std::move(*sprite));
        }
    }
    {
        std::filesystem::path sharedPath;
        try
        {
            sharedPath = ffxi::defaultInstallRoot() / "ROM" / "0" / "0.DAT";
        }
        catch (const std::exception&)
        {
        }
        if (!sharedPath.empty() && std::filesystem::exists(sharedPath))
        {
            try
            {
                ffxi::DatFile shared{sharedPath};
                size_t sharedModels = 0;
                for (const ffxi::Chunk& chunk : shared.chunksOfType(ffxi::kChunkD3m))
                {
                    if (std::optional<ffxi::Model> model = ffxi::parseD3m(chunk))
                    {
                        // Under a name no zone model uses, so a zone's own
                        // "hi12" stub does not shadow it.
                        const std::string key = "effect:" + model->name;
                        sharedByChunkId.emplace(model->name, key);
                        models.emplace(key, std::move(*model));
                        ++sharedModels;
                    }
                }
                for (const ffxi::Chunk& chunk : shared.chunksOfType(ffxi::kChunkTexture))
                {
                    try
                    {
                        ffxi::Texture texture = ffxi::parseTexture(chunk);
                        textures.emplace(texture.name, std::move(texture));
                    }
                    catch (const std::exception&)
                    {
                    }
                }
                size_t sharedSprites = 0;
                for (const ffxi::Chunk& chunk : shared.chunksOfType(ffxi::kChunkSprite))
                {
                    if (std::optional<ffxi::SpriteAnimation> sprite = ffxi::parseSprite(chunk))
                    {
                        if (sprites.emplace(sprite->name, std::move(*sprite)).second)
                        {
                            ++sharedSprites;
                        }
                    }
                }
                std::printf("shared effects: %zu meshes, %zu sprite animations from %s\n", sharedModels, sharedSprites,
                            sharedPath.filename().string().c_str());
            }
            catch (const std::exception& e)
            {
                std::printf("shared effects file did not load: %s\n", e.what());
            }
        }
    }

    // The model a generator means: the chunk with its id in the generator's
    // own directory, else the first with that id anywhere that has geometry,
    // else the shared effects file.
    const auto resolveGenerated = [&modelsByChunkId, &models,
                                   &sharedByChunkId](const ffxi::EffectPlacement& effect) -> const std::string* {
        auto found = modelsByChunkId.find(effect.modelId);
        if (found != modelsByChunkId.end())
        {
            for (const ChunkModel& candidate : found->second)
            {
                if (candidate.directory == effect.directory)
                {
                    auto model = models.find(candidate.name);
                    if (model != models.end() && model->second.triangleCount() > 0)
                    {
                        return &candidate.name;
                    }
                }
            }
            for (const ChunkModel& candidate : found->second)
            {
                auto model = models.find(candidate.name);
                if (model != models.end() && model->second.triangleCount() > 0)
                {
                    return &candidate.name;
                }
            }
        }
        auto shared = sharedByChunkId.find(effect.modelId);
        return shared == sharedByChunkId.end() ? nullptr : &shared->second;
    };

    std::optional<mh::Scene> best;

    // Held because the collision is built from all of them at the end: the
    // zone's shell plus the floors and walls inside each building.
    ffxi::Zone outside;
    std::vector<ffxi::Zone> insides;

    // What the effect system places. The MZB's table places the terrain and
    // the buildings; the water is placed by generators, one per surface, and
    // so are the fish, the birds and the torch flames. Only the water is
    // taken - the rest are animated things this renderer has no animation for
    // yet, and a still school of fish in mid-air is worse than none.
    // MOGHOUSE_ALL_GENERATORS places every one, for finding out what they are.
    const std::vector<ffxi::EffectPlacement> generated = ffxi::parseGenerators(dat);

    // Where the zone makes a noise. A sound and a generator sharing a
    // directory is the whole of the link: the sound says what is heard and the
    // generator says where from. Walked separately from the effects below
    // because whether a thing is drawn has no bearing on whether it is heard -
    // the waterfall markers are invisible and still audible.
    {
        std::unordered_map<std::string, std::vector<uint32_t>> soundsByDirectory;
        for (const ffxi::SoundRef& sound : ffxi::soundReferences(std::filesystem::path{datPath}))
        {
            soundsByDirectory[sound.directory].push_back(sound.id);
        }
        for (const ffxi::EffectPlacement& effect : generated)
        {
            // A zone keeps ambience for all four of its weathers and only one
            // of them is up, so the others are passed over the same way their
            // skies are. It changes nothing in West Ronfaure, where all four
            // name the same wind, but a zone whose rain sounds different from
            // its sunshine would otherwise play both at once.
            if (effect.directory.find("/weat/") != std::string::npos &&
                effect.directory.find(skyDirectory) == std::string::npos)
            {
                continue;
            }

            const auto found = soundsByDirectory.find(effect.directory);
            if (found == soundsByDirectory.end())
            {
                continue;
            }
            for (const uint32_t sound : found->second)
            {
                emitters.push_back(SoundEmitter{effect.translate[0], -effect.translate[1],
                                                -effect.translate[2], sound});
            }
        }

        std::set<uint32_t> distinct;
        for (const SoundEmitter& emitter : emitters)
        {
            distinct.insert(emitter.sound);
        }
        std::printf("ambience: %zu emitters, %zu distinct sounds\n", emitters.size(), distinct.size());
    }
    curves = ffxi::parseIntensityCurves(dat);
    static const bool everyGenerator = std::getenv("MOGHOUSE_ALL_GENERATORS") != nullptr;
    std::vector<ffxi::Placement> effectPlacements;
    size_t generatedWaves = 0;
    std::unordered_map<std::string, mh::EffectParams> effectParams;
    size_t generatedWater = 0;
    size_t generatedEffects = 0;

    // The sky. The weather directories place it - "weat/fine" holds the
    // clear-weather cloud dome, sun, moon and star field - at the origin with
    // large scales, meaning "around the camera". Fine weather only, until
    // the weather packet is read; the untextured sun and moon spheres wait
    // for their texture animation to be understood.
    ffxi::Zone skyZone;
    std::unordered_map<std::string, mh::EffectParams> skyParams;
    for (const ffxi::EffectPlacement& effect : generated)
    {
        // Not the zone's furniture: the library every DAT carries, which says
        // it stands at the origin because it is placed when it is fired. Drawn
        // as scenery they stack up at 0,0,0 - in Southern San d'Oria that is
        // seventy-odd torch flames burning on the pavement a few paces from
        // where a player zones in, with nothing under them. Left in under
        // MOGHOUSE_ALL_GENERATORS, which exists to look at exactly this.
        if (!everyGenerator && isTriggeredEffectLibrary(effect.directory))
        {
            continue;
        }

        // A generator whose id has a sprite animation places one, whether or
        // not it also has a mesh: hi12 has both, the flame and its haze; the
        // lamp glow lt has the sprite alone. Sky and hidden ones excepted.
        if (effect.directory.find("/weat") == std::string::npos)
        {
            auto animation = sprites.find(effect.modelId);
            // A sprite on one of the light sheets is a light source, not
            // something to look at: it tells the client where a lamp throws
            // light on the ground, and the retail client draws nothing for
            // it. Drawn, they were the white flares hanging beside every
            // lantern and over the player's head. The lighting they should
            // cast is not built yet, so for now they are simply left out.
            // Anything in a light directory lights something, whether or not
            // it is itself drawn. A marker is invisible and lights the ground;
            // a sconce's flame is visible and lights the ground too, and only
            // the first of those was being collected.
            const bool marker = animation != sprites.end() &&
                                (isLightSource(animation->second.texture) ||
                                 isMarkerOnlyDirectory(effect.directory));
            if (marker || isLightDirectory(effect.directory))
            {
                // The one thing in the zone that says where a flame stands.
                // Until this was kept, a torch at night was a bright sprite
                // lighting nothing at all.
                //
                // How far it carries is keyed off the size, but a marker's size
                // is the patch it lights while a flame's is the flame - a
                // sconce is 0.4 across and lights a good deal more than that -
                // so a floor keeps the small ones from lighting nothing.
                const float size = std::max(effect.scale[0], effect.scale[1]);
                lamps.push_back(Lamp{effect.translate[0], -effect.translate[1], -effect.translate[2],
                                     std::max(size, 1.0f) * lampReach});
            }

            if (marker)
            {
                animation = sprites.end();
            }
            if (animation != sprites.end())
            {
                // MOGHOUSE_SPRITE_WATCH=1 says what each placed sprite is, how
                // big it was told to be and where it went. A lamp halo that is
                // too large and one that is drawn from the wrong sheet look the
                // same from inside the zone.
                static const bool watchSprites = std::getenv("MOGHOUSE_SPRITE_WATCH") != nullptr;
                if (watchSprites)
                {
                    std::printf("sprite %-6s tex %-16s scale %.2f x %.2f  at %8.1f %8.1f %8.1f  fade %.0f %.0f %.0f %.0f  %s\n",
                                effect.modelId.c_str(), animation->second.texture.c_str(),
                                effect.scale[0], effect.scale[1],
                                effect.translate[0], -effect.translate[1], -effect.translate[2],
                                effect.fade[0], effect.fade[1], effect.fade[2], effect.fade[3],
                                effect.directory.c_str());
                }

                mh::SpriteInstance instance;
                // The DAT's frame to the world's: (x, -y, -z), as everywhere.
                instance.centre = {effect.translate[0], -effect.translate[1], -effect.translate[2]};
                instance.scale = {effect.scale[0], effect.scale[1]};
                instance.animation = effect.modelId;
                instance.curve = effect.textureAnimation;
                instance.nightOnly = effect.nightOnly;
                for (int i = 0; i < 4; ++i)
                {
                    instance.fade[i] = effect.fade[i];
                }
                spriteInstances.push_back(std::move(instance));
            }
        }

        // Op 0x27 was read as "not drawn" for a day: allsea, the whole-zone
        // water table, had shown through the auction house floor. The floor
        // was missing because the tent version of the building was drawn
        // (see assets/hidden-models.txt), and hiding allsea took the harbour
        // with it - retail draws that sheet, rippling, beside the bridge.
        const std::string* resolved = resolveGenerated(effect);
        if (!resolved)
        {
            continue;
        }
        const std::string& modelName = *resolved;
        auto model = models.find(modelName);
        if (model == models.end())
        {
            continue;
        }
        bool water = false;
        for (const ffxi::ModelMesh& mesh : model->second.meshes)
        {
            if (mh::isWaterMesh(model->second.name, mesh))
            {
                water = true;
                break;
            }
        }
        // A water sheet whose generator animates it is a wave, not a
        // surface. Valkurm Dunes' beach is three of them - nmia, nmib, nmic -
        // over a sea body that really is a surface. The water pass bakes its
        // geometry flat into one world-space buffer, which is right for a
        // canal and no use for something that has to spread, wash up the sand
        // and fade; so these go to the effect pass, which already scrolls a
        // texture and can now offset and fade one.
        // Motion is what makes a wave, not a fade. Valkurm's nmic is six 6x6
        // sheets of the caustic net "umi1" carrying an opacity curve and
        // nothing else - no spread, no uv, and a scroll rate slow enough to
        // take a minute for one repeat. Counted as waves they came out of the
        // water pass as a wide sheet of foam sitting perfectly still, which is
        // both wrong and the most visible thing on the beach. umi1 is a
        // caustic net like kaw1 and ike1: it belongs to the water, which
        // drifts it in two layers.
        //
        // The spread is the test, and it has to be a test every placement of a
        // model agrees on: the effect parameters are keyed by model, so one
        // placement calling itself a wave takes all of them out of the water
        // pass with it, and a model split between the two passes is drawn
        // twice. Op 0x29 is uniform here - it is on all three copies of nmia
        // and nmib and on none of the six nmic - where a uv or opacity curve
        // is not: one nmic of the six carries `umcv` and the rest do not.
        //
        // Op 0x29's curve is the depth the strip should reach, in the units its
        // own bounds are in - not a factor to multiply the placement by. nmia
        // measures 29.70 x 0 x 11.25 and its curve reaches 7.70: multiplied it
        // gives a strip 87 units deep, covering a bay whose three shoreline
        // features stand five to seven units apart, and on screen that filled
        // 428 rows of 1440. Read as a depth it gives 7.70 units.
        //
        // The model's z bounds run 0..11.25 rather than about its middle, so
        // the strip grows from its own seaward edge: its far edge is a
        // waterline that runs up the sand and draws back. That is the motion,
        // and it is real geometry moving rather than a texture sliding under a
        // fixed edge. What these cannot do is rise and fall - the y extent is
        // exactly zero, and no curve here gives a flat sheet a crest.
        //
        // MOGHOUSE_WAVES=0 turns them off.
        // Off, because where they land is wrong and it is not yet known why.
        //
        // The strips land out to sea, by a constant. MOGHOUSE_BEACH_PROBE
        // walks the collision and reports the waterline; against it,
        //
        //   beach 1, x  299:  strips z 237, 242   waterline z 152   +85, +90
        //   beach 2, x -180:  strips z 197, 202   waterline z 112   +85, +90
        //
        // The same two offsets at two beaches four hundred and eighty yalms
        // apart, so this is one cause and not scattered misplacement. Looking
        // straight down at the strips shows open water under them, so the drawn
        // terrain agrees with the collision.
        //
        // The layers do overlap each other - the shimmer nmic spans z 197..263,
        // nmia 226..237, nmib 231..242 - which is the arrangement that should
        // read as a wave rolling in. They overlap in the wrong place.
        //
        // Ruled out so far, so nobody spends the afternoon again:
        //
        //   the animation      the curves resolve, the loop advances (phase
        //                      0.127 to 0.504 measured live), and the strips
        //                      draw, spread and fade
        //   the direction      growing the other way is worse
        //   the sea panels     removing them empties the bay, so they stay
        //   a parent frame     the generators sit under directory chunks
        //                      f_ki/effe/umi1..3, and every one of those
        //                      chunks is sixteen bytes of zero - there is no
        //                      transform on a directory to inherit
        //   the collision      looking straight down at the strips shows open
        //                      water in the drawn terrain too, so it is not
        //                      that the server's collision disagrees with what
        //                      is on screen
        //
        // What is left is the frame the generator translations are in, or op
        // 0x0f's (6, 0, 1). Worth noting: mirroring a strip in z about its own
        // group's sea panel would land it at the waterline on both beaches -
        // 242 about 202 gives 162 against a shore at 152, and 202 about 159
        // gives 116 against a shore at 112 - which is a suspiciously good fit
        // for two beaches and worth chasing before anything else.
        //
        // MOGHOUSE_WAVES=1 turns them on to keep working on it.
        static const bool wavesEnabled = std::getenv("MOGHOUSE_WAVES") != nullptr;
        const bool wave = wavesEnabled && water && !effect.scaleZCurve.empty();
        if (wave)
        {
            water = false;
        }
        // A model with a texture animation is a visible effect - the
        // fountain's jets and flames, a waterfall's sheet - and is placed
        // and scrolled. Anything else a generator places is a particle
        // emitter, a light or an animated creature, none of which this
        // renderer can do yet.
        // Only from the effects directory. The weather directories place the
        // sky with the same opcodes - Bastok Markets' stars drew as a flock
        // of grey triangles over the city.
        const bool inEffects = effect.directory.find("/effe") != std::string::npos ||
                               effect.directory.rfind("effe", 0) == 0;
        if (!water && effect.directory.find(skyDirectory) != std::string::npos)
        {
            bool textured = false;
            for (const ffxi::ModelMesh& mesh : model->second.meshes)
            {
                textured = textured || !mesh.texture.empty();
            }
            if (textured)
            {
                ffxi::Placement placement;
                placement.model = modelName;
                for (int axis = 0; axis < 3; ++axis)
                {
                    placement.translate[axis] = effect.translate[axis];
                    placement.rotate[axis] = effect.rotate[axis];
                    placement.scale[axis] = effect.scale[axis];
                }
                skyZone.placements.push_back(std::move(placement));
                // Clouds drift; stars keep still and come out at night. Which
                // is which is read off the name until the generators' own
                // timing opcodes are understood.
                const bool stars = modelName.rfind("sta", 0) == 0;
                skyParams.emplace(modelName,
                                  mh::EffectParams{stars ? 0.0f : 0.004f, 0.0f, stars, effect.textureAnimation});
            }
            continue;
        }
        const bool animated = wave || (!water && inEffects && !effect.textureAnimation.empty());
        if (!water && !animated && !everyGenerator && modelName.rfind("effect:", 0) != 0)
        {
            continue;
        }
        // The shared file's 0x1f meshes are parsed but not drawn: placed and
        // drawn, hi12's ribbon stood as a pale streak several units over every
        // lamp, and retail shows nothing there but the flame sprite. Most
        // likely it is the path the sparks travel rather than a surface.
        // MOGHOUSE_EFFECT_MESHES=1 draws them, for looking.
        static const bool drawEffectMeshes = std::getenv("MOGHOUSE_EFFECT_MESHES") != nullptr;
        const bool sharedEffect = modelName.rfind("effect:", 0) == 0;
        if (sharedEffect && !drawEffectMeshes)
        {
            continue;
        }
        if ((animated || sharedEffect) && effectParams.find(modelName) == effectParams.end())
        {
            // The rate is per frame in the file, at the game's thirty a
            // second. A generator with no rate still animates in the game,
            // by its keyframe chunk, which is not read yet; a slow slide is
            // the stand-in. The direction is a guess until checked.
            // Only what the file says. A rate of zero used to become a slow
            // slide, on the reasoning that a generator with no 0x28 still
            // animates in the game by a keyframe chunk this does not read -
            // but only six to nine per cent of texture-animated generators
            // carry a rate, so that stand-in was inventing motion for well
            // over ninety per cent of them. It shows: a wall sconce in San
            // d'Oria had its texture running down it like water.
            //
            // Still rather than wrong. When the keyframe chunk is read this
            // can animate properly.
            const float perSecond = effect.scroll * 30.0f;
            mh::EffectParams params{0.0f, perSecond, effect.nightOnly, effect.textureAnimation};
            if (wave)
            {
                // The day curve is a visibility gate and a wave is not gated;
                // ours run on the loop clock instead.
                params.curve.clear();
                params.foam = true;
                params.wave.scaleZ = effect.scaleZCurve;
                params.wave.opacity = effect.opacityCurve;
                params.wave.u = effect.uCurve;
                params.wave.v = effect.vCurve;
                const float zExtent = model->second.boundsMax[2] - model->second.boundsMin[2];
                params.wave.spreadPerUnit = zExtent > 0.01f ? 1.0f / zExtent : 1.0f;
                if (std::getenv("MOGHOUSE_WAVE_WATCH"))
                {
                    std::printf("wave setup %-4s generator says %8.2f %8.2f %8.2f   model z bounds %.2f..%.2f\n",
                                modelName.c_str(), effect.translate[0], effect.translate[1], effect.translate[2],
                                model->second.boundsMin[2], model->second.boundsMax[2]);
                }
                ++generatedWaves;
            }
            // A flame from the shared file adds to what is behind it; a
            // zone's own jets and waterfalls blend.
            params.additive = sharedEffect;
            effectParams.emplace(modelName, params);
            ++generatedEffects;
        }
        ffxi::Placement placement;
        placement.model = modelName;
        for (int axis = 0; axis < 3; ++axis)
        {
            placement.translate[axis] = effect.translate[axis];
            placement.rotate[axis] = effect.rotate[axis];
            placement.scale[axis] = effect.scale[axis];
        }
        effectPlacements.push_back(std::move(placement));
        generatedWater += water ? 1 : 0;
    }
    if (!generated.empty())
    {
        std::printf("sprites: %zu placed from %zu animations\n", spriteInstances.size(), sprites.size());
        std::printf("generators: %zu, %zu placing water models, %zu waves, %zu animated effect models, "
                    "%zu sky objects%s\n",
                    generated.size(), generatedWater, generatedWaves, generatedEffects, skyZone.placements.size(),
                    everyGenerator ? " (all placed)" : "");
    }
    skyObjects = mh::Scene{};
    if (!skyZone.placements.empty())
    {
        size_t skyResolved = 0, skyMissing = 0;
        skyObjects = mh::buildScene(skyZone, models, textures, skyResolved, skyMissing, &skyParams);
    }
    else if (key2Path)
    {
        // A zone with no sky of its own borrows one, from the same zone that
        // lends its lighting: Sel Phiner, the sign-in backdrop, has neither.
        // The lender's cloud and star models and their textures come along;
        // the textures are added to this zone's map so the sky bind groups
        // find them.
        std::string borrowed;
        try
        {
            const ffxi::FileTable table{ffxi::defaultInstallRoot()};
            if (auto path = table.path(kBorrowedLightingFileId))
            {
                borrowed = path->string();
            }
        }
        catch (const std::exception&)
        {
        }
        if (!borrowed.empty() && borrowed != datPath)
        {
            try
            {
                ffxi::DatFile lender{std::filesystem::path{borrowed}};
                auto keys2 = ffxi::KeyTable::load(key2Path);
                std::unordered_map<std::string, ffxi::Model> lentModels;
                std::unordered_map<std::string, std::string> lentByChunk;
                if (keys2)
                {
                    for (const ffxi::Chunk& chunk : lender.chunksOfType(ffxi::kChunkMmb))
                    {
                        try
                        {
                            ffxi::Model model = ffxi::parseMmb(chunk, *keys, *keys2);
                            std::string chunkId(chunk.id, 4);
                            while (!chunkId.empty() && (chunkId.back() == ' ' || chunkId.back() == 0))
                            {
                                chunkId.pop_back();
                            }
                            lentByChunk.emplace(std::move(chunkId), model.name);
                            lentModels.emplace(model.name, std::move(model));
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                }
                ffxi::Zone lentSky;
                for (const ffxi::EffectPlacement& effect : ffxi::parseGenerators(lender))
                {
                    if (effect.directory.find(skyDirectory) == std::string::npos)
                    {
                        continue;
                    }
                    auto id = lentByChunk.find(effect.modelId);
                    if (id == lentByChunk.end())
                    {
                        continue;
                    }
                    auto model = lentModels.find(id->second);
                    bool textured = false;
                    for (const ffxi::ModelMesh& mesh : model->second.meshes)
                    {
                        textured = textured || !mesh.texture.empty();
                    }
                    if (!textured)
                    {
                        continue;
                    }
                    ffxi::Placement placement;
                    placement.model = id->second;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        placement.translate[axis] = effect.translate[axis];
                        placement.rotate[axis] = effect.rotate[axis];
                        placement.scale[axis] = effect.scale[axis];
                    }
                    lentSky.placements.push_back(std::move(placement));
                    const bool stars = id->second.rfind("sta", 0) == 0;
                    skyParams.emplace(id->second,
                                      mh::EffectParams{stars ? 0.0f : 0.004f, 0.0f, stars, effect.textureAnimation});
                }
                if (!lentSky.placements.empty())
                {
                    for (auto& [name, curve] : ffxi::parseIntensityCurves(lender))
                    {
                        curves.emplace(name, std::move(curve));
                    }
                    for (const ffxi::Chunk& chunk : lender.chunksOfType(ffxi::kChunkTexture))
                    {
                        try
                        {
                            ffxi::Texture texture = ffxi::parseTexture(chunk);
                            if (textures.find(texture.name) == textures.end())
                            {
                                textures.emplace(texture.name, std::move(texture));
                            }
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                    size_t skyResolved = 0, skyMissing = 0;
                    skyObjects = mh::buildScene(lentSky, lentModels, textures, skyResolved, skyMissing, &skyParams);
                    std::printf("sky: this zone has none of its own, borrowed %zu objects\n", lentSky.placements.size());
                }
            }
            catch (const std::exception& e)
            {
                std::printf("could not borrow a sky: %s\n", e.what());
            }
        }
    }

    bool effectsPlaced = false;
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);

        // The generators' placements go with the first MZB - the zone's
        // shell. The same DAT can hold a second one (24.DAT has two zones).
        if (!effectsPlaced)
        {
            zone.placements.insert(zone.placements.end(), effectPlacements.begin(), effectPlacements.end());
            effectsPlaced = true;

            const std::vector<std::string> hidden = hiddenModelsFor(datPath);
            if (!hidden.empty())
            {
                const size_t before = zone.placements.size();
                zone.placements.erase(std::remove_if(zone.placements.begin(), zone.placements.end(),
                                                     [&hidden](const ffxi::Placement& placement) {
                                                         return std::find(hidden.begin(), hidden.end(),
                                                                          placement.model) != hidden.end();
                                                     }),
                                      zone.placements.end());
                std::printf("hidden models: %zu placements struck (assets/hidden-models.txt)\n",
                            before - zone.placements.size());
            }
        }

        // Placed models are the visible world; collision geometry is the
        // fallback when the model key table is not available.
        mh::Scene mesh;
        if (!models.empty())
        {
            size_t resolved = 0;
            size_t missing = 0;
            mesh = mh::buildScene(zone, models, textures, resolved, missing, &effectParams);
            if (!mesh.vertices.empty())
            {
                std::printf("  %zu models (%zu unreadable), %zu placements drawn, %zu with no model\n", models.size(),
                            modelsFailed, resolved, missing);
                std::printf("  %zu unique triangles, %zu drawn - instancing saves %.1fx\n", mesh.triangles(),
                            mesh.drawnTriangles(),
                            mesh.triangles() ? static_cast<double>(mesh.drawnTriangles()) / static_cast<double>(mesh.triangles()) : 0.0);
            }
        }
        // Collision geometry is deliberately not drawn. Its meshes carry a
        // flags field with only two values across all 5,921 of them, so it
        // references no material - it is genuine collision, not terrain, and
        // drawing it just covers the world in untextured white.
        if (!best || mesh.vertices.size() > best->vertices.size())
        {
            best = std::move(mesh);
            zoneId = zone.id;
            // The same chunk that produced the visible world produces the
            // ground to stand on, so they cannot disagree.
            outside = std::move(zone);
        }
    }

    // The buildings' insides. Each is a whole little scene of its own - its own
    // models, its own textures, its own placements - so it is loaded the same
    // way the zone was and appended. Windurst Waters is twenty-two of these on
    // top of one zone file, and without them its rooms are empty shells.
    if (best)
    {
        size_t rooms = 0;
        size_t added = 0;
        for (const std::filesystem::path& roomPath : subroomsFor(datPath))
        {
            try
            {
                ffxi::DatFile room{roomPath};
                for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkTexture))
                {
                    try
                    {
                        ffxi::Texture texture = ffxi::parseTexture(chunk);
                        std::string key = texture.name;
                        textures.emplace(std::move(key), std::move(texture));
                    }
                    catch (const std::exception&)
                    {
                    }
                }

                std::unordered_map<std::string, ffxi::Model> roomModels;
                if (key2Path)
                {
                    if (auto keys2 = ffxi::KeyTable::load(key2Path))
                    {
                        for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkMmb))
                        {
                            try
                            {
                                ffxi::Model model = ffxi::parseMmb(chunk, *keys, *keys2);
                                std::string key = model.name;
                                roomModels.emplace(std::move(key), std::move(model));
                            }
                            catch (const std::exception&)
                            {
                            }
                        }
                    }
                }

                // A room's own times of day, kept apart from the zone's rather
                // than merged into them - they describe the inside of one
                // building, not the world.
                ffxi::Lighting inner;
                for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkLighting))
                {
                    inner.add(chunk);
                }

                for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkMzb))
                {
                    ffxi::Zone inside = ffxi::parseMzb(chunk, *keys);
                    size_t resolved = 0;
                    size_t missing = 0;
                    mh::Scene scene = mh::buildScene(inside, roomModels, textures, resolved, missing);
                    if (!scene.vertices.empty())
                    {
                        // Its floors and walls, which the zone's own collision
                        // does not have: without them a player inside a
                        // building stands on the outdoor ground beneath it and
                        // is stopped by walls that on screen are a doorway.
                        insides.push_back(inside);

                        // Every room, whether or not it brought lighting: the
                        // draw range is what lets a room be left out of a
                        // frame the player is not inside it for. The game
                        // keeps its interiors in the sky above the city and
                        // never draws one from the street; drawing them all
                        // put two shops in the air over Southern San d'Oria.
                        //
                        // Grown a little, so standing in a doorway does not
                        // flicker between the two lighting sets frame to frame.
                        constexpr float kMargin = 2.0f;
                        mh::InteriorLighting room{
                            inner,
                            {scene.boundsMin.x - kMargin, scene.boundsMin.y - kMargin, scene.boundsMin.z - kMargin},
                            {scene.boundsMax.x + kMargin, scene.boundsMax.y + kMargin, scene.boundsMax.z + kMargin}};
                        room.firstDraw = static_cast<uint32_t>(best->draws.size());
                        room.drawCount = static_cast<uint32_t>(scene.draws.size());
                        interiors.push_back(std::move(room));

                        // Which rooms bring geometry whose textures are not
                        // in this zone's files. A room mapped to the wrong
                        // zone looks exactly like this: its meshes name the
                        // textures of the city it belongs to.
                        size_t textureless = 0;
                        for (const mh::InstancedDraw& draw : scene.draws)
                        {
                            if (!draw.water && (draw.texture.empty() || textures.find(draw.texture) == textures.end()))
                            {
                                ++textureless;
                            }
                        }
                        std::printf("  room %s: %zu draws, %zu without texture, %.0f across at y %.0f..%.0f\n",
                                    roomPath.filename().string().c_str(), scene.draws.size(), textureless,
                                    std::max(scene.boundsMax.x - scene.boundsMin.x, scene.boundsMax.z - scene.boundsMin.z),
                                    scene.boundsMin.y, scene.boundsMax.y);

                        mh::append(*best, scene);
                        added += resolved;
                        ++rooms;
                    }
                }
            }
            catch (const std::exception& e)
            {
                std::printf("  interior %s did not load: %s\n", roomPath.filename().string().c_str(), e.what());
            }
        }
        std::vector<const ffxi::Zone*> all{&outside};
        for (const ffxi::Zone& one : insides)
        {
            all.push_back(&one);
        }
        collision = mh::Collision{all};

        if (rooms)
        {
            const size_t lit = static_cast<size_t>(std::count_if(
                interiors.begin(), interiors.end(), [](const mh::InteriorLighting& r) { return !r.lighting.empty(); }));
            std::printf("  %zu building interiors, %zu placements, %zu with their own lighting\n", rooms, added, lit);
            for (size_t i = 0; i < interiors.size(); ++i)
            {
                const mh::InteriorLighting& room = interiors[i];
                const float wide = std::max(room.boundsMax.x - room.boundsMin.x, room.boundsMax.z - room.boundsMin.z);
                if (wide > 120.0f)
                {
                    // A room this size is a district. The frame loop leaves
                    // any wider than kRoomAtMost out of the indoor test.
                    std::printf("  room %zu is %.0f across: %.0f..%.0f %.0f..%.0f %.0f..%.0f\n", i, wide,
                                room.boundsMin.x, room.boundsMax.x, room.boundsMin.y, room.boundsMax.y, room.boundsMin.z, room.boundsMax.z);
                }
            }
        }
    }
    return best;
}

wgpu::Buffer createBuffer(const wgpu::Device& device, const void* data, size_t size, wgpu::BufferUsage usage)
{
    wgpu::BufferDescriptor descriptor{.usage = usage | wgpu::BufferUsage::CopyDst, .size = size};
    wgpu::Buffer buffer = device.CreateBuffer(&descriptor);
    if (!buffer)
    {
        std::printf("failed to create a %zu byte buffer\n", size);
        return buffer;
    }

    // Written in chunks rather than one call: a single multi-megabyte
    // WriteBuffer stages the whole thing at once, and it is cheap to avoid.
    constexpr size_t kChunk = 4u * 1024u * 1024u;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t written = 0; written < size; written += kChunk)
    {
        const size_t amount = std::min(kChunk, size - written);
        device.GetQueue().WriteBuffer(buffer, written, bytes + written, amount);
    }
    return buffer;
}
/// Writes a 24-bit BMP. Uncompressed and about as simple as an image format
/// gets, which matters because the alternative is pulling in a PNG encoder to
/// look at one frame.
bool writeBmp(const char* path, const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t bytesPerRow)
{
    std::FILE* file = std::fopen(path, "wb");
    if (!file)
    {
        return false;
    }

    const uint32_t rowBytes = width * 3;
    const uint32_t padding = (4 - (rowBytes % 4)) % 4;
    const uint32_t imageBytes = (rowBytes + padding) * height;
    const uint32_t fileBytes = 54 + imageBytes;

    uint8_t header[54] = {};
    header[0] = 0x42;
    header[1] = 0x4D;
    std::memcpy(header + 2, &fileBytes, 4);
    const uint32_t pixelOffset = 54;
    std::memcpy(header + 10, &pixelOffset, 4);
    const uint32_t infoSize = 40;
    std::memcpy(header + 14, &infoSize, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const uint16_t planes = 1;
    const uint16_t bits = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &imageBytes, 4);
    std::fwrite(header, 1, sizeof(header), file);

    // BMP rows run bottom to top.
    std::vector<uint8_t> row(rowBytes + padding, 0);
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* source = bgra + static_cast<size_t>(height - 1 - y) * bytesPerRow;
        for (uint32_t x = 0; x < width; ++x)
        {
            row[x * 3 + 0] = source[x * 4 + 0];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}

/// A stand-in form, for MOGHOUSE_FORM=1.
///
/// Login-shaped because that is the first screen the client will set and the
/// one worth looking at while the widget is being built - a caption, three
/// things to type into, a button, and somewhere for a refusal to appear.
/// Nothing here is what the client will actually send; it only has to exercise
/// every row kind.
mh::Form demoForm()
{
    mh::Form form;
    form.title = "MogHouse XI";
    form.message = "";
    form.rows = {
        mh::FormRow{mh::FormRowKind::Field, "Server", "127.0.0.1", true},
        mh::FormRow{mh::FormRowKind::Field, "Username", "", true},
        mh::FormRow{mh::FormRowKind::Secret, "Password", "", true},
        mh::FormRow{mh::FormRowKind::Button, "Log In", "", true},
    };
    return form;
}
} // namespace

void mh::ViewerLink::setEntities(std::vector<RadarEntity> entities)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    entities_ = std::move(entities);
}

void mh::ViewerLink::setZoneLines(std::vector<ZoneLineMarker> lines)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    zoneLines_ = std::move(lines);
}

std::vector<mh::ZoneLineMarker> mh::ViewerLink::zoneLines() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return zoneLines_;
}

std::vector<mh::RadarEntity> mh::ViewerLink::entities() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return entities_;
}

void mh::ViewerLink::pushChat(const std::string& line, ChatTone tone)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    chat_.push_back(ChatLine{line, tone});
    while (chat_.size() > static_cast<size_t>(mh::kChatLines))
    {
        chat_.pop_front();
    }
}

std::vector<mh::ViewerLink::ChatLine> mh::ViewerLink::chat() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return {chat_.begin(), chat_.end()};
}

void mh::ViewerLink::setInventory(const InventorySlot* slots, int count, const uint16_t* sizes, int sizeCount)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    inventory_.assign(slots, slots + std::max(0, count));
    containerSizes_.fill(0);
    for (int i = 0; i < sizeCount && i < static_cast<int>(containerSizes_.size()); ++i)
    {
        containerSizes_[static_cast<size_t>(i)] = sizes[i];
    }

    // Bumped last, so a panel that sees a new number is looking at a whole
    // set of bags rather than half of one.
    inventoryRevision_.fetch_add(1);
}

void mh::ViewerLink::setCharacterStats(CharacterStats stats)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    characterStats_ = stats;
}

mh::ViewerLink::CharacterStats mh::ViewerLink::characterStats() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return characterStats_;
}

void mh::ViewerLink::setEquipment(const uint8_t* containers, const uint8_t* slots, int count)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    for (size_t i = 0; i < equipment_.size(); ++i)
    {
        equipment_[i] = i < static_cast<size_t>(count)
                            ? std::pair<uint8_t, uint8_t>{containers[i], slots[i]}
                            : std::pair<uint8_t, uint8_t>{0, 255};
    }
}

std::array<std::pair<uint8_t, uint8_t>, 16> mh::ViewerLink::equipment() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return equipment_;
}

std::vector<mh::ViewerLink::InventorySlot> mh::ViewerLink::inventory() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return inventory_;
}

std::array<uint16_t, 18> mh::ViewerLink::containerSizes() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return containerSizes_;
}

void mh::ViewerLink::requestInventoryAction(InventoryAction action)
{
    const std::lock_guard<std::mutex> guard{mutex_};

    // Queued rather than replaced: equipping four pieces in four clicks is one
    // click each, and a single slot would drop three of them.
    inventoryActions_.push_back(action);
}

bool mh::ViewerLink::takeInventoryAction(InventoryAction& action)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    if (inventoryActions_.empty())
    {
        return false;
    }

    action = inventoryActions_.front();
    inventoryActions_.pop_front();
    return true;
}

void mh::ViewerLink::pushItemFace(ItemFace face)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    itemFaces_.push_back(std::move(face));
}

std::vector<mh::ViewerLink::ItemFace> mh::ViewerLink::takeItemFaces()
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return std::exchange(itemFaces_, {});
}

void mh::ViewerLink::setCharacter(float x, float y, float z, float heading)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    character_[0] = x;
    character_[1] = y;
    character_[2] = z;
    character_[3] = heading;
    haveCharacter_ = true;
}

bool mh::ViewerLink::character(float& x, float& y, float& z, float& heading) const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    if (!haveCharacter_)
    {
        return false;
    }
    x = character_[0];
    y = character_[1];
    z = character_[2];
    heading = character_[3];
    return true;
}

void mh::ViewerLink::submitChat(const std::string& line)
{
    std::lock_guard<std::mutex> lock{mutex_};
    outgoing_.push_back(line);
}

std::optional<std::string> mh::ViewerLink::takeChat()
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (outgoing_.empty())
    {
        return std::nullopt;
    }

    std::string line = std::move(outgoing_.front());
    outgoing_.pop_front();
    return line;
}

void mh::ViewerLink::placeCharacter(float x, float y, float z, float heading)
{
    std::lock_guard<std::mutex> lock{mutex_};
    placement_[0] = x;
    placement_[1] = y;
    placement_[2] = z;
    placement_[3] = heading;
    havePlacement_ = true;
}

bool mh::ViewerLink::takePlacement(float& x, float& y, float& z, float& heading)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (!havePlacement_)
    {
        return false;
    }

    x = placement_[0];
    y = placement_[1];
    z = placement_[2];
    heading = placement_[3];
    havePlacement_ = false;
    return true;
}

void mh::ViewerLink::requestJump() { jump_ = true; }

/// Exchange rather than a read and a clear, so a jump is delivered exactly
/// once even if the client polls from a different thread than the one that
/// set it.
bool mh::ViewerLink::takeJump() { return jump_.exchange(false); }

void mh::ViewerLink::requestTalk(uint32_t entityId) { talk_ = entityId; }

void mh::ViewerLink::setWeather(int32_t weather) { weather_ = weather; }

void mh::ViewerLink::requestCapture(const std::string& path)
{
    std::lock_guard<std::mutex> held(captureMutex_);
    capturePath_ = path;
}

bool mh::ViewerLink::takeCapture(std::string& path)
{
    std::lock_guard<std::mutex> held(captureMutex_);
    if (capturePath_.empty())
    {
        return false;
    }
    path = capturePath_;
    capturePath_.clear();
    return true;
}

int32_t mh::ViewerLink::weather() const { return weather_.load(); }

/// Exchange rather than a read and a clear, for the same reason takeJump is.
/// Entity 0 is nobody, which is what makes it usable as "nothing pending".
bool mh::ViewerLink::takeTalk(uint32_t& entityId)
{
    entityId = talk_.exchange(0);
    return entityId != 0;
}

void mh::ViewerLink::setDeath(bool dead, bool raiseOffered)
{
    dead_ = dead;
    raiseOffered_ = raiseOffered;
}

bool mh::ViewerLink::dead(bool& raiseOffered) const
{
    raiseOffered = raiseOffered_;
    return dead_;
}

void mh::ViewerLink::setVitals(uint32_t hp, uint32_t mp, uint32_t tp, uint8_t hpPercent, uint8_t mpPercent)
{
    hp_ = hp;
    mp_ = mp;
    tp_ = tp;
    hpPercent_ = hpPercent;
    mpPercent_ = mpPercent;
    vitalsKnown_ = true;
}

void mh::ViewerLink::chooseLink(Link which) { link_ = static_cast<int>(which); }

void mh::ViewerLink::applySettings(Settings settings)
{
    musicVolume_ = settings.musicVolume;
    soundVolume_ = settings.soundVolume;
    radarTurns_ = settings.radarTurns;
    uiScale_ = settings.uiScale;
    settingsPending_ = true;
}

mh::ViewerLink::Settings mh::ViewerLink::settings() const
{
    return Settings{musicVolume_.load(), soundVolume_.load(), radarTurns_.load(), uiScale_.load()};
}

bool mh::ViewerLink::settingsChanged() { return settingsDirty_.exchange(false); }

bool mh::ViewerLink::takeSettings(float& volume, float& soundVolume, bool& radarTurns, float& uiScale)
{
    if (!settingsPending_.exchange(false))
    {
        return false;
    }
    volume = musicVolume_.load();
    soundVolume = soundVolume_.load();
    radarTurns = radarTurns_.load();
    uiScale = uiScale_.load();
    return true;
}

void mh::ViewerLink::noteSettings(Settings settings)
{
    musicVolume_ = settings.musicVolume;
    soundVolume_ = settings.soundVolume;
    radarTurns_ = settings.radarTurns;
    uiScale_ = settings.uiScale;
    settingsDirty_ = true;
}

void mh::ViewerLink::requestZone(ZoneRequest request)
{
    std::lock_guard<std::mutex> held{zoneLock_};
    zoneRequest_ = std::move(request);
    zoneRequested_ = true;
}

bool mh::ViewerLink::takeZoneRequest(ZoneRequest& out)
{
    std::lock_guard<std::mutex> held{zoneLock_};
    if (!zoneRequested_)
    {
        return false;
    }
    out = zoneRequest_;
    zoneRequested_ = false;
    return true;
}

void mh::ViewerLink::setMusic(std::string path)
{
    std::lock_guard<std::mutex> held{musicLock_};
    if (path != music_)
    {
        music_ = std::move(path);
        musicChanged_ = true;
    }
}

std::string mh::ViewerLink::takeMusic(bool& changed)
{
    std::lock_guard<std::mutex> held{musicLock_};
    changed = musicChanged_;
    musicChanged_ = false;
    return music_;
}

mh::ViewerLink::Link mh::ViewerLink::takeLink()
{
    return static_cast<Link>(link_.exchange(static_cast<int>(Link::None)));
}

mh::ViewerLink::Vitals mh::ViewerLink::vitals() const
{
    return Vitals{hp_.load(), mp_.load(), tp_.load(), hpPercent_.load(), mpPercent_.load(), vitalsKnown_.load()};
}

void mh::ViewerLink::chooseDeath(DeathChoice choice) { deathChoice_ = static_cast<int>(choice); }

/// Exchange rather than a read and a clear, for the same reason takeJump is.
/// A press read twice is two home point requests for one button, and the
/// second arrives at a character the server has already moved.
mh::DeathChoice mh::ViewerLink::takeDeathChoice()
{
    return static_cast<DeathChoice>(deathChoice_.exchange(static_cast<int>(DeathChoice::None)));
}

void mh::ViewerLink::setForm(Form form)
{
    std::lock_guard<std::mutex> guard(mutex_);
    form_ = std::move(form);

    // A new form clears whatever the last one produced. Otherwise a result the
    // client had not collected yet would be handed back as this screen's
    // answer, which is how a login refusal becomes a character selection.
    formResultReady_ = false;
    formButton_ = -1;
    formValues_.clear();
}

mh::Form mh::ViewerLink::form() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return form_;
}

void mh::ViewerLink::submitForm(int button, std::vector<std::string> values)
{
    std::lock_guard<std::mutex> guard(mutex_);

    // First press wins. A second one before the client has read the first
    // would replace an answer already given with whatever the fields hold now.
    if (formResultReady_)
    {
        return;
    }

    formResultReady_ = true;
    formButton_ = button;
    formValues_ = std::move(values);
}

bool mh::ViewerLink::takeFormResult(int& button, std::vector<std::string>& values)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!formResultReady_)
    {
        return false;
    }

    button = formButton_;
    values = std::move(formValues_);

    formResultReady_ = false;
    formButton_ = -1;
    formValues_.clear();
    return true;
}

void mh::ViewerLink::setResting(bool resting) { resting_ = resting; }

void mh::ViewerLink::setSitting(bool sitting) { sitting_ = sitting; }

bool mh::ViewerLink::sitting() const { return sitting_; }

bool mh::ViewerLink::resting() const { return resting_; }

void mh::ViewerLink::setPlayerName(std::string name)
{
    std::lock_guard<std::mutex> guard(mutex_);
    playerName_ = std::move(name);
}

std::string mh::ViewerLink::playerName() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return playerName_;
}

void mh::ViewerLink::setLook(std::string look)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (look == look_)
    {
        // Same as it was, so nothing to rebuild. Worth checking here: the
        // client has no reason to track whether it has said this before, and
        // rebuilding a character on every zone load would be a visible stall
        // for no change.
        return;
    }

    look_ = std::move(look);
    lookChanged_ = true;
}

bool mh::ViewerLink::takeLook(std::string& out)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!lookChanged_)
    {
        return false;
    }

    out = look_;
    lookChanged_ = false;
    return true;
}

void mh::ViewerLink::setRiding(bool aboard) { riding_ = aboard; }

bool mh::ViewerLink::riding() const { return riding_; }

void mh::ViewerLink::setHud(bool on) { hud_ = on; }

bool mh::ViewerLink::hud() const { return hud_; }

void mh::ViewerLink::setLineup(bool on) { lineup_ = on; }

void mh::ViewerLink::setFormAside(bool on) { formAside_ = on; }

bool mh::ViewerLink::formAside() const { return formAside_; }

bool mh::ViewerLink::lineup() const { return lineup_; }

void mh::ViewerLink::setServerClock(uint32_t clock)
{
    std::lock_guard<std::mutex> guard(mutex_);
    serverClock_ = clock;
    serverClockSetAtNs_ = SDL_GetTicksNS();
    serverClockKnown_ = true;
}

bool mh::ViewerLink::serverClock(uint32_t& clock, uint64_t& setAtNs) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!serverClockKnown_)
    {
        return false;
    }

    clock = serverClock_;
    setAtNs = serverClockSetAtNs_;
    return true;
}

void mh::ViewerLink::stop() { stop_ = true; }

bool mh::ViewerLink::stopping() const { return stop_; }

int mh::runViewer(const ViewerOptions& options, ViewerLink* link)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string zoneId;

    // How far back the player has asked the camera to sit. Kept apart from
    // camera.distance, which a wall can shorten for a frame - folding the two
    // together means walking through a doorway permanently zooms you in.
    float wantedDistance = 6.0f;

    // Filled the first time entities arrive - see the nameplate loop, which
    // works out the zone from an entity's own id.
    /// The drawn world's opaque surfaces, for asking what can be seen.
    ///
    /// Not the collision, which is a different dataset and knows nothing about
    /// how a thing looks - a fence is a solid box in it and a tree a solid
    /// cylinder, so a name went missing behind a railing you can see over and
    /// behind leaves.
    mh::Collision sight;

    ffxi::EntityNames entityNames;
    bool triedEntityNames = false;
    std::optional<mh::Scene> zone;
    mh::Collision collision;
    std::unordered_map<std::string, ffxi::Texture> textures;
    ffxi::Lighting lighting;

    // Each building interior lights its own inside; see InteriorLighting.
    std::vector<mh::InteriorLighting> interiors;
    int lastActiveRoom = -2;   // which room's lighting was used last frame; -1 outdoors, -2 never decided
    // The set being faded from after a doorway, and when the fade began.
    const ffxi::Lighting* lightingFrom = nullptr;
    const ffxi::Lighting* lightingLast = nullptr;
    uint64_t lightingFadeStartNs = 0;
    constexpr float kLightingFadeSeconds = 0.6f;
    // Rooms the player is not in this frame, as draw ranges to skip.
    std::vector<std::pair<uint32_t, uint32_t>> hiddenDraws;
    // How far outside a room's box the player can be and still see into it -
    // a doorway is approached from the street.
    constexpr float kRoomReach = 6.0f;
    // Wider than this and a sub-file is a district rather than a room, whose
    // lighting is not the inside of anything. The largest real building in the
    // three cities is well under a hundred units.
    constexpr float kRoomAtMost = 150.0f;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(mh::kWindowTitle, kWidth, kHeight,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // Waiting on a future with a non-zero timeout is opt-in. Without this every
    // WaitAny fails with "Timeout waits are either not enabled or not
    // supported", which reads like a driver limitation and is not one.
    static constexpr wgpu::InstanceFeatureName kInstanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor instanceDescriptor{.requiredFeatureCount = std::size(kInstanceFeatures),
                                                .requiredFeatures = kInstanceFeatures};
    wgpu::Instance instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!instance)
    {
        std::printf("could not create a WebGPU instance\n");
        return 1;
    }

    wgpu::Surface surface = mh::CreateSurface(instance, window);
    if (!surface)
    {
        std::printf("could not create a surface for this window\n");
        return 1;
    }

    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions adapterOptions{.compatibleSurface = surface};
    instance.WaitAny(instance.RequestAdapter(&adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message)
                                             {
                                                 if (status != wgpu::RequestAdapterStatus::Success)
                                                 {
                                                     std::printf("no adapter: %.*s\n", static_cast<int>(message.length), message.data);
                                                     return;
                                                 }
                                                 adapter = std::move(result);
                                             }),
                     UINT64_MAX);
    if (!adapter)
    {
        return 1;
    }

    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    std::printf("adapter: %.*s (%s)\n", static_cast<int>(info.device.length), info.device.data, backendName(info.backendType));

    // BC2 is the format FFXI's DXT3 textures already are, but a WebGPU device
    // will not accept it unless the feature is asked for up front.
    const bool supportsBc = adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
    if (!supportsBc)
    {
        std::printf("this adapter has no BC texture support - compressed textures will be skipped\n");
    }
    const wgpu::FeatureName requiredFeatures[] = {wgpu::FeatureName::TextureCompressionBC};

    wgpu::DeviceDescriptor deviceDescriptor{};
    if (supportsBc)
    {
        deviceDescriptor.requiredFeatureCount = 1;
        deviceDescriptor.requiredFeatures = requiredFeatures;
    }
    deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message)
                                                { std::printf("webgpu error: %.*s\n", static_cast<int>(message.length), message.data); });

    wgpu::Device device;
    instance.WaitAny(adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message)
                                           {
                                               if (status != wgpu::RequestDeviceStatus::Success)
                                               {
                                                   std::printf("no device: %.*s\n", static_cast<int>(message.length), message.data);
                                                   return;
                                               }
                                               device = std::move(result);
                                           }),
                     UINT64_MAX);
    if (!device)
    {
        return 1;
    }

    wgpu::Queue queue = device.GetQueue();

    // The sky is drawn before anything else, at the far plane, with no depth
    // writes - it is a backdrop rather than a surface.
    wgpu::ShaderSourceWGSL skyWgsl;
    skyWgsl.code = mh::kSkyShader;
    wgpu::ShaderModuleDescriptor skyModuleDescriptor{.nextInChain = &skyWgsl};
    wgpu::ShaderModule skyModule = device.CreateShaderModule(&skyModuleDescriptor);

    wgpu::BufferDescriptor skyUniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                .size = sizeof(SkyUniforms)};
    wgpu::Buffer skyUniformBuffer = device.CreateBuffer(&skyUniformDescriptor);

    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);
    const wgpu::TextureFormat surfaceFormat = capabilities.formats[0];

    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);

    int pointWidth = 0;
    int pointHeight = 0;
    SDL_GetWindowSize(window, &pointWidth, &pointHeight);
    std::printf("window: %dx%d points, %dx%d pixels\n", pointWidth, pointHeight, pixelWidth, pixelHeight);

    wgpu::ColorTargetState skyTarget{.format = surfaceFormat};
    wgpu::FragmentState skyFragment{
        .module = skyModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &skyTarget};
    // Depth is compared but never written, so geometry drawn afterwards always
    // wins and the sky fills only what is left.
    wgpu::DepthStencilState skyDepth{.format = kDepthFormat,
                                     .depthWriteEnabled = wgpu::OptionalBool::False,
                                     .depthCompare = wgpu::CompareFunction::Always};
    wgpu::RenderPipelineDescriptor skyPipelineDescriptor{
        .vertex = {.module = skyModule, .entryPoint = "vertexMain"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
        .depthStencil = &skyDepth,
        .fragment = &skyFragment};
    wgpu::RenderPipeline skyPipeline = device.CreateRenderPipeline(&skyPipelineDescriptor);

    wgpu::BindGroupEntry skyEntry{.binding = 0, .buffer = skyUniformBuffer, .size = sizeof(SkyUniforms)};
    wgpu::BindGroupDescriptor skyBindGroupDescriptor{
        .layout = skyPipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &skyEntry};
    wgpu::BindGroup skyBindGroup = device.CreateBindGroup(&skyBindGroupDescriptor);

    wgpu::Texture depthTexture;
    auto configure = [&](uint32_t width, uint32_t height)
    {
        wgpu::SurfaceConfiguration configuration{.device = device,
                                                 .format = surfaceFormat,
                                                 // CopySrc so a frame can be read back; see writeBmp.
                                                 .usage = wgpu::TextureUsage::RenderAttachment |
                                                          wgpu::TextureUsage::CopySrc,
                                                 .width = width,
                                                 .height = height,
                                                 .presentMode = wgpu::PresentMode::Fifo};
        surface.Configure(&configuration);

        wgpu::TextureDescriptor depthDescriptor{.usage = wgpu::TextureUsage::RenderAttachment,
                                                .dimension = wgpu::TextureDimension::e2D,
                                                .size = {width, height, 1},
                                                .format = kDepthFormat,
                                                .mipLevelCount = 1,
                                                .sampleCount = 1};
        depthTexture = device.CreateTexture(&depthDescriptor);
    };
    configure(static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight));

    wgpu::Buffer vertexBuffer;
    wgpu::Buffer indexBuffer;
    wgpu::Buffer instanceBuffer;
    wgpu::Buffer waterVertexBuffer;
    wgpu::Buffer waterIndexBuffer;
    wgpu::RenderPipeline waterPipeline;
    // Whether this zone's water is the sea, decided by the ripple sheet it
    // ships - see the chooser. The water shader draws the two differently.
    bool waterIsSea = false;
    wgpu::BindGroup waterBindGroup;
    uint32_t waterIndexCount = 0;
    wgpu::Buffer uniformBuffer;
    wgpu::RenderPipeline pipeline;
    wgpu::RenderPipeline cutoutPipeline;
    wgpu::RenderPipeline translucentPipeline;

    /// The same again, blended against a constant set per draw - how a
    /// character is shown faded without the shader knowing anything about it.
    wgpu::RenderPipeline fadePipeline;

    /// Draws something a second time, adding to what is already there. The
    /// monorail's cars light up through this.
    wgpu::RenderPipeline glowPipeline;

    /// One flat pale surface, for the figure that stands in for a character
    /// nobody has made yet.
    wgpu::Texture ghostTexture;
    wgpu::BindGroup ghostBindGroup;
    wgpu::BindGroupLayout zoneBindGroupLayout;
    // The effect pass: generator-placed meshes with a scrolling texture. One
    // bind group per effect draw, indexed like batchBindGroups, with a small
    // uniform holding its scroll rate; an empty handle for every other draw.
    wgpu::RenderPipeline effectPipeline;
    // The same, adding to what is behind rather than blending over it: the
    // star sheet is white points on black, and alpha-blended it drew the
    // black too, a flock of dark triangles across the night.
    wgpu::RenderPipeline effectAdditivePipeline;
    wgpu::BindGroupLayout effectBindGroupLayout;
    std::vector<wgpu::BindGroup> effectBindGroups;
    /// Parallel to effectBindGroups - effectBuffers is not, since it only
    /// grows for draws that are effects and then takes the sky's and the
    /// sprites' too. A wave rewrites its own uniform every frame and has to
    /// be able to find it by draw index.
    std::vector<wgpu::Buffer> effectWaveBuffers;
    /// The placement matrices as built, before any wave rescaled them.
    std::vector<float> baseInstances;
    std::vector<wgpu::Buffer> effectBuffers;
    // The sky objects - cloud dome, star field - as their own little scene
    // with their own buffers, drawn camera-relative by the effect pipeline
    // between the sky gradient and the zone.
    mh::Scene skyObjects;
    // The zone's intensity curves, by id; a draw that names one is shown
    // only while the curve is above nothing at the current hour.
    std::unordered_map<std::string, ffxi::IntensityCurve> curves;
    // The sprites: flames and glows. Rebuilt every frame as camera-facing
    // quads into one vertex buffer, drawn additively per texture.
    std::unordered_map<std::string, ffxi::SpriteAnimation> sprites;
    std::vector<mh::SpriteInstance> spriteInstances;

    // Short sounds over the top of the music. Separate from it because they
    // overlap each other and it does not - and declared up here rather than
    // beside the music because the body-drawing lambda below plays them, and a
    // lambda cannot capture something declared after it.
    mh::Sounds sounds;

    // Where the zone's torches stand. Rebuilt with the zone, read every frame.
    std::vector<Lamp> lamps;
    /// Where this zone makes noise, and which of those are sounding now -
    /// sound id to voice, where a handle of 0 records a sound that was asked
    /// for once and refused because it does not loop, so it is not asked again.
    std::vector<SoundEmitter> emitters;
    std::map<uint32_t, uint32_t> ambienceVoices;

    // Scratch for choosing the nearest few, kept out of the frame loop so it is
    // not reallocated sixty times a second.
    std::vector<std::pair<float, size_t>> lampOrder;

    // When the zone last finished loading. Everything the server mentions in
    // the burst after that was already standing there, so nothing should be
    // seen arriving for a moment - otherwise walking through a zone line has
    // every worm in the new zone heave itself out of the ground at once.
    uint64_t zoneLoadedAtMs = 0;
    wgpu::Buffer spriteVertexBuffer;
    wgpu::Buffer spriteIndexBuffer;
    wgpu::Buffer spriteInstanceBuffer;       ///< one identity matrix
    std::vector<mh::Vertex> spriteVertices; ///< scratch, kept between frames
    struct SpriteBatch
    {
        std::string texture;
        wgpu::BindGroup bindGroup;
        uint32_t first{};
        uint32_t count{};
    };
    std::vector<SpriteBatch> spriteBatches;
    size_t spriteCapacity = 0;
    wgpu::Buffer skyVertexBuffer;
    wgpu::Buffer skyIndexBuffer;
    wgpu::Buffer skyInstanceBuffer;
    std::vector<wgpu::BindGroup> skyObjectBindGroups;
    wgpu::Sampler sampler;
    wgpu::Texture whiteTexture;
    // Bound to water meshes, which name no texture at all in their mesh
    // header - FFXI supplies theirs some other way, and until that is worked
    // out the white fallback makes a canal look like a sheet of paper.
    //
    // A placeholder, and deliberately a recognisable one: this is a flat
    // colour, not water, and it should look like a stand-in rather than like
    // a rendering someone signed off.
    wgpu::Texture waterFallbackTexture;
    std::vector<wgpu::Texture> batchTextures;
    std::vector<wgpu::BindGroup> batchBindGroups;
    uint32_t indexCount = 0;

    // The zone is read here, after the window and the device exist, rather
    // than before SDL is even started.
    //
    // Reading it first meant there was nothing on screen while it happened -
    // no surface, no pipelines, nothing to draw a loading screen on. Several
    // seconds of a zone opening with no window at all, and on a zone change
    // the client looked like it had closed.
    //
    // Nothing between here and there needs the zone: the device, the swap
    // chain and the shaders are all built from the window.

    // Reading a zone, and everything the GPU needs to draw it.
    //
    // A lambda rather than a straight line because it has to run more than
    // once: zoning used to close this window and open another, which is why
    // the client appeared to vanish on !zone. Everything below is assigned to
    // variables held outside it, and every WebGPU handle releases what it
    // held when it is reassigned, so calling it again simply replaces the
    // zone in the window already on screen.
    // Which zone is being drawn. Its own variables rather than the options,
    // because the options describe how the window was started and this changes
    // every time the player walks through a door.
    std::string currentZonePath = options.zonePath;
    std::optional<std::string> currentZoneName = options.zoneName;

    // The baked overhead map. Declared out here rather than beside the bake
    // because the radar reads it every frame and a new zone replaces it.
    //
    // Replaces its *contents*, that is. The texture itself is made once and
    // baked into again on every zone: the radar's bind group points at it,
    // and a fresh texture per zone left the radar pointing at the first one -
    // so every zone entered from the sign-in screen showed Sel Phiner's grass,
    // which is the green disc the minimap had become.
    wgpu::Texture mapTexture;
    wgpu::Texture maskTexture;
    float mapCentreX = 0.0f;
    float mapCentreZ = 0.0f;
    float mapHalf = 1.0f;

    const auto readZone = [&]() -> int {
        // Everything a zone contributes is added to, not replaced, so a second
        // zone has to start from nothing.
        //
        // Left alone, the times of day of both zones end up in one set and get
        // interpolated together - which is why the second zone came out white -
        // and both zones' textures compete for the same names, so meshes bind
        // whichever was inserted first and the rest render black. The interiors
        // keep lighting boxes for buildings that are no longer anywhere.
        zone.reset();
        textures.clear();

        // These are appended to, and the draw list indexes them by position.
        // Left alone, a second zone's draws pick up the first zone's bind
        // groups - so the world renders as nothing at all while the water and
        // the characters, which do not use them, carry on drawing.
        batchTextures.clear();
        batchBindGroups.clear();
        effectBindGroups.clear();
        effectWaveBuffers.clear();
        effectBuffers.clear();
        skyObjectBindGroups.clear();
        spriteBatches.clear();
        spriteVertexBuffer = nullptr;
        spriteIndexBuffer = nullptr;
        spriteCapacity = 0;
        skyVertexBuffer = nullptr;
        skyIndexBuffer = nullptr;
        skyInstanceBuffer = nullptr;
        indexCount = 0;
        waterIndexCount = 0;
        lighting = ffxi::Lighting{};
        interiors.clear();
        lastActiveRoom = -2;
        lightingFrom = nullptr;
        lightingLast = nullptr;
        hiddenDraws.clear();
        collision = mh::Collision{};

        if (!currentZonePath.empty())
        {
            const char* keyPath = options.keyTablePath.empty() ? nullptr : options.keyTablePath.c_str();
            if (!keyPath)
            {
                std::printf("set MOGHOUSE_FFXI_KEYTABLE to the 256-byte MZB key table to load a zone\n");
                return 2;
            }
            static const int forcedWeather = forcedWeatherFromEnvironment();
            // What the server said, or MOGHOUSE_WEATHER for looking at a sky
            // without one. Below zero means nobody has said, which reads as
            // fine - the standalone renderer has no server to ask.
            const int weatherNow = link && link->weather() >= 0 ? link->weather() : forcedWeather;

            // A zone whose file cannot be read used to take the process with
            // it. DatFile throws when it cannot open the path, nothing here
            // caught it, and the abort landed in the crash log as the renderer
            // dying for no stated reason - which is what a zone the install
            // does not hold looked like from the outside. An installation is
            // allowed to be missing a file; the client is not allowed to die
            // of it, so this reports the zone and carries on without one.
            try
            {
                zone = loadZone(currentZonePath.c_str(), keyPath,
                                options.keyTable2Path.empty() ? nullptr : options.keyTable2Path.c_str(), zoneId,
                                textures, lighting, collision, interiors, skyObjects, curves, sprites, spriteInstances,
                                weatherNow, lamps, emitters);
            }
            catch (const std::exception& e)
            {
                std::printf("could not load zone %s from %s: %s\n", zoneId.c_str(), currentZonePath.c_str(),
                            e.what());
                zone.reset();
            }
            // The old zone's waterfall does not follow you into the next one.
            for (const auto& [sound, handle] : ambienceVoices)
            {
                if (handle != 0)
                {
                    sounds.release(handle);
                }
            }
            ambienceVoices.clear();
            std::printf("lamps: %zu torches to light by\n", lamps.size());
            zoneLoadedAtMs = SDL_GetTicksNS() / 1000000ull;

            // Touched here so the list is read - and reported - at startup
            // rather than the first time something spawns. Whether the file was
            // found at all is the thing worth knowing early.
            if (std::getenv("MOGHOUSE_SPAWN_WATCH") != nullptr)
            {
                (void)burrowerModels();
            }
            if (!zone)
            {
                return 1;
            }
            // The character is loaded after the zone so it can share the texture
            // map: a PC in a town wears textures the zone never mentions, and a
            // zone texture the character happens to name should not be read twice.
            // The zone's own water meshes first - see mh::isWaterMesh - and
            // the sheets derived from the server's collision only for a zone
            // that has none. The derived sheets are flat at a guessed
            // waterline and stop wherever the walkable mesh does, which is
            // how a canal came out with a hole under every bridge and a
            // stream with its surface up the banks; the meshes are what the
            // retail client draws.
            if (zone->waterTriangles() > 0)
            {
                std::printf("water: %zu triangles from the zone's own meshes%s%s\n", zone->waterTriangles(),
                            zone->waterTexture.empty() ? "" : ", sheet ",
                            zone->waterTexture.empty() ? "" : zone->waterTexture.c_str());
            }
            else if (currentZoneName)
            {
                const size_t water = loadWater(*currentZoneName, *zone);
                // Reported either way. A silent zero is how a zone name
                // that did not match a filename went unnoticed.
                std::printf("water: %zu derived triangles for %s\n", water,
                            currentZoneName->c_str());
            }
            std::printf("collision: %zu triangles, %zu walls\n", collision.triangleCount(), collision.wallCount());
            {
                // What a sight test against the drawn world would have to
                // intersect: the opaque draws only. Foliage and glass are
                // excluded because they are exactly what should not block a
                // name - you can see a chocobo through a fence and through
                // leaves, and not through a trunk.
                size_t seeThrough = 0;
                std::vector<mh::Vec3> corners;
                for (const mh::InstancedDraw& draw : zone->draws)
                {
                    const size_t tris = static_cast<size_t>(draw.indexCount / 3) * draw.instanceCount;
                    if (draw.cutout || draw.blend || draw.water || draw.effect)
                    {
                        seeThrough += tris;
                        continue;
                    }

                    for (uint32_t n = 0; n < draw.instanceCount; ++n)
                    {
                        const size_t at = (static_cast<size_t>(draw.instanceOffset) + n) * 16;
                        if (at + 16 > zone->instances.size())
                        {
                            break;
                        }
                        const float* m = zone->instances.data() + at;
                        const auto place = [&](uint32_t index) {
                            const mh::Vertex& v = zone->vertices[index];
                            return mh::Vec3{m[0] * v.position[0] + m[4] * v.position[1] + m[8] * v.position[2] + m[12],
                                            m[1] * v.position[0] + m[5] * v.position[1] + m[9] * v.position[2] + m[13],
                                            m[2] * v.position[0] + m[6] * v.position[1] + m[10] * v.position[2] + m[14]};
                        };
                        for (uint32_t i = 0; i + 2 < draw.indexCount; i += 3)
                        {
                            const uint32_t base = draw.indexOffset + i;
                            if (base + 2 >= zone->indices.size())
                            {
                                break;
                            }
                            corners.push_back(place(zone->indices[base]));
                            corners.push_back(place(zone->indices[base + 1]));
                            corners.push_back(place(zone->indices[base + 2]));
                        }
                    }
                }

                // What a name can be hidden behind. See Collision::fromTriangles
                // for why this is not the collision mesh.
                sight = mh::Collision::fromTriangles(corners);

                // And the things you are allowed to stand on that the server's
                // mesh leaves out - benches, crates. Their drawn geometry goes
                // into the walking collision as ground, so landing on one and
                // stepping off it work the way they do anywhere else rather
                // than needing a case of their own.
                {
                    std::vector<mh::Vec3> perches;
                    size_t perchModels = 0;
                    for (const std::string& model : standableModels())
                    {
                        auto range = zone->instanceRanges.find(model);
                        if (range == zone->instanceRanges.end() || range->second.second == 0)
                        {
                            continue;
                        }

                        const uint32_t first = range->second.first;
                        const uint32_t last = first + range->second.second;
                        ++perchModels;

                        for (const mh::InstancedDraw& draw : zone->draws)
                        {
                            if (draw.instanceOffset < first || draw.instanceOffset >= last)
                            {
                                continue;
                            }
                            for (uint32_t n = 0; n < draw.instanceCount; ++n)
                            {
                                const size_t at = (static_cast<size_t>(draw.instanceOffset) + n) * 16;
                                if (at + 16 > zone->instances.size())
                                {
                                    break;
                                }
                                const float* m = zone->instances.data() + at;
                                const auto place = [&](uint32_t index) {
                                    const mh::Vertex& v = zone->vertices[index];
                                    return mh::Vec3{
                                        m[0] * v.position[0] + m[4] * v.position[1] + m[8] * v.position[2] + m[12],
                                        m[1] * v.position[0] + m[5] * v.position[1] + m[9] * v.position[2] + m[13],
                                        m[2] * v.position[0] + m[6] * v.position[1] + m[10] * v.position[2] + m[14]};
                                };
                                for (uint32_t i = 0; i + 2 < draw.indexCount; i += 3)
                                {
                                    const uint32_t base = draw.indexOffset + i;
                                    if (base + 2 >= zone->indices.size())
                                    {
                                        break;
                                    }
                                    perches.push_back(place(zone->indices[base]));
                                    perches.push_back(place(zone->indices[base + 1]));
                                    perches.push_back(place(zone->indices[base + 2]));
                                }
                            }
                        }
                    }

                    if (!perches.empty())
                    {
                        collision.addTriangles(perches, true);
                        std::printf("standable: %zu triangles from %zu of %zu named models\n",
                                    perches.size() / 3, perchModels, standableModels().size());
                    }
                }
                std::printf("sight: %zu opaque triangles drawn, %zu see-through left out\n",
                            corners.size() / 3, seeThrough);
            }
            std::printf("zone %s: %zu triangles\n", zoneId.c_str(), zone->indices.size() / 3);
            std::printf("  bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n", zone->boundsMin.x, zone->boundsMax.x,
                        zone->boundsMin.y, zone->boundsMax.y, zone->boundsMin.z, zone->boundsMax.z);
        }
        else
        {
            std::printf("no DAT given - clearing only. Pass a zone DAT to draw one.\n");
        }

        if (zone && !zone->indices.empty())
        {
            vertexBuffer = createBuffer(device, zone->vertices.data(), zone->vertices.size() * sizeof(mh::Vertex),
                                        wgpu::BufferUsage::Vertex);
            indexBuffer = createBuffer(device, zone->indices.data(), zone->indices.size() * sizeof(uint32_t),
                                       wgpu::BufferUsage::Index);
            // CopyDst as well as Vertex: the monorail rewrites its cars'
            // transforms in here every frame it moves.
            instanceBuffer = createBuffer(device, zone->instances.data(), zone->instances.size() * sizeof(float),
                                          wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
            // A wave rescales its own matrices every frame, so it needs the
            // ones it started with. Scaling in place would compound.
            baseInstances = zone->instances;
            indexCount = static_cast<uint32_t>(zone->indices.size());
            std::printf("buffers created\n");

            // Location 7, not 3: the instance matrix already holds 3 through 6, and
            // the two buffers share one location space.
            wgpu::VertexAttribute attributes[4] = {
                {.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
                {.format = wgpu::VertexFormat::Float32x3, .offset = 3 * sizeof(float), .shaderLocation = 1},
                {.format = wgpu::VertexFormat::Float32x2, .offset = 6 * sizeof(float), .shaderLocation = 2},
                {.format = wgpu::VertexFormat::Unorm8x4, .offset = 8 * sizeof(float), .shaderLocation = 7}};
            wgpu::VertexAttribute instanceAttributes[4] = {
                {.format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 3},
                {.format = wgpu::VertexFormat::Float32x4, .offset = 4 * sizeof(float), .shaderLocation = 4},
                {.format = wgpu::VertexFormat::Float32x4, .offset = 8 * sizeof(float), .shaderLocation = 5},
                {.format = wgpu::VertexFormat::Float32x4, .offset = 12 * sizeof(float), .shaderLocation = 6}};

            wgpu::VertexBufferLayout vertexLayout{.stepMode = wgpu::VertexStepMode::Vertex,
                                                  .arrayStride = sizeof(mh::Vertex),
                                                  .attributeCount = 4,
                                                  .attributes = attributes};
            // Stepping per instance rather than per vertex is the whole trick: one
            // copy of the geometry, one matrix per placement.
            wgpu::VertexBufferLayout instanceLayout{.stepMode = wgpu::VertexStepMode::Instance,
                                                    .arrayStride = 16 * sizeof(float),
                                                    .attributeCount = 4,
                                                    .attributes = instanceAttributes};
            wgpu::VertexBufferLayout bufferLayouts[2] = {vertexLayout, instanceLayout};

            // Created once for the life of the window rather than once per zone.
            // A bind group is only valid against the exact layout object it was
            // built from, and the character's are built at startup - so minting a
            // fresh layout on every zone change left every skinned model, the
            // player included, holding bind groups the new pipeline rejects. The
            // same goes for the uniform buffer they bind: a new one each zone is a
            // buffer nothing writes to any more. Nothing here depends on the zone.
            if (!pipeline)
            {
                wgpu::BufferDescriptor uniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                         .size = sizeof(Uniforms)};
                uniformBuffer = device.CreateBuffer(&uniformDescriptor);

                wgpu::ShaderSourceWGSL wgsl;
                wgsl.code = mh::kZoneShader;
                wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl};
                wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);


                // An explicit layout shared by both pipelines. Letting each derive its
                // own default layout makes bind groups built for one incompatible with
                // the other, which fails at draw time rather than at creation.
                wgpu::BindGroupLayoutEntry layoutEntries[3] = {};
                layoutEntries[0].binding = 0;
                layoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
                layoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
                layoutEntries[1].binding = 1;
                layoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
                layoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
                layoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
                layoutEntries[2].binding = 2;
                layoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
                layoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

                wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{.entryCount = 3, .entries = layoutEntries};
                zoneBindGroupLayout = device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);

                wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                        .bindGroupLayouts = &zoneBindGroupLayout};
                wgpu::PipelineLayout sharedLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);

                wgpu::ColorTargetState colorTarget{.format = surfaceFormat};
                wgpu::FragmentState fragment{.module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &colorTarget};
                wgpu::DepthStencilState depthStencil{.format = kDepthFormat,
                                                     .depthWriteEnabled = wgpu::OptionalBool::True,
                                                     .depthCompare = wgpu::CompareFunction::Less};

                wgpu::RenderPipelineDescriptor pipelineDescriptor{
                    .layout = sharedLayout,
                    .vertex = {.module = module, .entryPoint = "vertexMain", .bufferCount = 2, .buffers = bufferLayouts},
                    .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
                    .depthStencil = &depthStencil,
                    .fragment = &fragment};
                pipeline = device.CreateRenderPipeline(&pipelineDescriptor);

                // Same pipeline, alpha-cutout fragment shader. Which one a batch uses
                // comes from its mesh header rather than from one global choice.
                wgpu::FragmentState cutoutFragment{
                    .module = module, .entryPoint = "fragmentCutout", .targetCount = 1, .targets = &colorTarget};
                wgpu::RenderPipelineDescriptor cutoutDescriptor = pipelineDescriptor;
                cutoutDescriptor.fragment = &cutoutFragment;
                cutoutPipeline = device.CreateRenderPipeline(&cutoutDescriptor);

                // And once more for water: the same shader, blended, and not writing
                // depth so a surface does not hide the one behind it. Bastok Markets
                // has two water meshes stacked - a darker body with a lighter sheet
                // over it - and with depth writes on, whichever drew first won.
                wgpu::BlendState surfaceBlend{
                    .color = {.operation = wgpu::BlendOperation::Add,
                              .srcFactor = wgpu::BlendFactor::SrcAlpha,
                              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
                    .alpha = {.operation = wgpu::BlendOperation::Add,
                              .srcFactor = wgpu::BlendFactor::One,
                              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
                wgpu::ColorTargetState surfaceTarget{.format = surfaceFormat, .blend = &surfaceBlend};
                wgpu::FragmentState surfaceFragment{
                    .module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &surfaceTarget};
                wgpu::DepthStencilState surfaceDepth{.format = kDepthFormat,
                                                     .depthWriteEnabled = wgpu::OptionalBool::False,
                                                     .depthCompare = wgpu::CompareFunction::Less};
                wgpu::RenderPipelineDescriptor surfaceDescriptor = pipelineDescriptor;
                surfaceDescriptor.fragment = &surfaceFragment;
                surfaceDescriptor.depthStencil = &surfaceDepth;
                translucentPipeline = device.CreateRenderPipeline(&surfaceDescriptor);

                // The effect pipeline: the zone's vertex layout, a shader that
                // scrolls the texture, a fourth binding for the rate. Blended
                // the way the water is and not writing depth.
                {
                    wgpu::ShaderSourceWGSL effectWgsl;
                    effectWgsl.code = mh::kEffectShader;
                    wgpu::ShaderModuleDescriptor effectModuleDescriptor{.nextInChain = &effectWgsl};
                    wgpu::ShaderModule effectModule = device.CreateShaderModule(&effectModuleDescriptor);

                    wgpu::BindGroupLayoutEntry effectEntries[4] = {};
                    effectEntries[0] = layoutEntries[0];
                    effectEntries[1] = layoutEntries[1];
                    effectEntries[2] = layoutEntries[2];
                    effectEntries[3].binding = 3;
                    effectEntries[3].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
                    effectEntries[3].buffer.type = wgpu::BufferBindingType::Uniform;
                    wgpu::BindGroupLayoutDescriptor effectLayoutDescriptor{.entryCount = 4, .entries = effectEntries};
                    effectBindGroupLayout = device.CreateBindGroupLayout(&effectLayoutDescriptor);
                    wgpu::PipelineLayoutDescriptor effectPipelineLayoutDescriptor{
                        .bindGroupLayoutCount = 1, .bindGroupLayouts = &effectBindGroupLayout};
                    wgpu::PipelineLayout effectLayout = device.CreatePipelineLayout(&effectPipelineLayoutDescriptor);

                    wgpu::FragmentState effectFragment{
                        .module = effectModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &surfaceTarget};
                    wgpu::RenderPipelineDescriptor effectDescriptor = pipelineDescriptor;
                    effectDescriptor.layout = effectLayout;
                    effectDescriptor.vertex.module = effectModule;
                    effectDescriptor.fragment = &effectFragment;
                    effectDescriptor.depthStencil = &surfaceDepth;
                    effectPipeline = device.CreateRenderPipeline(&effectDescriptor);

                    // Source alpha in: the glow sheets are a flat colour with
                    // the disc in their alpha, and adding them whole drew
                    // pale squares over the fountain.
                    wgpu::BlendState additiveBlend{
                        .color = {.operation = wgpu::BlendOperation::Add,
                                  .srcFactor = wgpu::BlendFactor::SrcAlpha,
                                  .dstFactor = wgpu::BlendFactor::One},
                        .alpha = {.operation = wgpu::BlendOperation::Add,
                                  .srcFactor = wgpu::BlendFactor::One,
                                  .dstFactor = wgpu::BlendFactor::One}};
                    wgpu::ColorTargetState additiveTarget{.format = surfaceFormat, .blend = &additiveBlend};
                    wgpu::FragmentState additiveFragment{
                        .module = effectModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &additiveTarget};
                    wgpu::RenderPipelineDescriptor additiveDescriptor = effectDescriptor;
                    additiveDescriptor.fragment = &additiveFragment;
                    effectAdditivePipeline = device.CreateRenderPipeline(&additiveDescriptor);
                }

                // And once more again, blended against a constant rather than
                // against the texture's own alpha. That is the whole trick
                // behind the faded characters at character select: skin and
                // cloth are opaque, so blending on what the texture says can
                // only ever draw them solid, while a blend constant is set per
                // draw and needs nothing from the shader at all.
                wgpu::BlendState fadeBlend{
                    .color = {.operation = wgpu::BlendOperation::Add,
                              .srcFactor = wgpu::BlendFactor::Constant,
                              .dstFactor = wgpu::BlendFactor::OneMinusConstant},
                    .alpha = {.operation = wgpu::BlendOperation::Add,
                              .srcFactor = wgpu::BlendFactor::One,
                              .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
                wgpu::ColorTargetState fadeTarget{.format = surfaceFormat, .blend = &fadeBlend};
                wgpu::FragmentState fadeFragment{
                    .module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &fadeTarget};
                wgpu::RenderPipelineDescriptor fadeDescriptor = pipelineDescriptor;
                fadeDescriptor.fragment = &fadeFragment;
                fadeDescriptor.depthStencil = &surfaceDepth;
                fadePipeline = device.CreateRenderPipeline(&fadeDescriptor);

                // And an additive one, for drawing something a second time to
                // light it up. The constant decides how much is added, so one
                // pipeline covers a lamp coming on, holding, and going out.
                wgpu::BlendState glowBlend{
                    .color = {.operation = wgpu::BlendOperation::Add,
                              .srcFactor = wgpu::BlendFactor::Constant,
                              .dstFactor = wgpu::BlendFactor::One},
                    .alpha = {.operation = wgpu::BlendOperation::Add,
                              .srcFactor = wgpu::BlendFactor::Zero,
                              .dstFactor = wgpu::BlendFactor::One}};
                wgpu::ColorTargetState glowTarget{.format = surfaceFormat, .blend = &glowBlend};
                wgpu::FragmentState glowFragment{
                    .module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &glowTarget};
                wgpu::RenderPipelineDescriptor glowDescriptor = pipelineDescriptor;
                glowDescriptor.fragment = &glowFragment;
                glowDescriptor.depthStencil = &surfaceDepth;
                glowPipeline = device.CreateRenderPipeline(&glowDescriptor);

                // WebGPU has no bindless arrays, so each texture needs its own bind
                // group and its own draw. Fine at a zone's few dozen textures; this is
                // the thing that will need atlasing or caching at a larger scale.
                wgpu::SamplerDescriptor samplerDescriptor{};
                samplerDescriptor.addressModeU = wgpu::AddressMode::Repeat;
                samplerDescriptor.addressModeV = wgpu::AddressMode::Repeat;
                samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
                samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
                sampler = device.CreateSampler(&samplerDescriptor);

                whiteTexture = mh::createWhiteTexture(device);
                // Dark. Bastok's water is nearly black at night and a deep slate by
                // day, and a cheerful mid-blue placeholder reads as a mistake in a
                // city built out of grey stone.
                waterFallbackTexture = mh::createSolidTexture(device, 26, 46, 54, 190);
            }
            const wgpu::TextureView whiteView = whiteTexture.CreateView();
            const wgpu::TextureView waterFallbackView = waterFallbackTexture.CreateView();

            if (!zone->waterIndices.empty())
            {
                // How far the ripple sheet has to travel to be seen moving.
                //
                // The sheet drifts about 0.011 of a texture a second. Over a
                // surface whose texture repeats a few times that reads as
                // ripples; over one where it repeats a hundred times it is
                // invisible, and the sea looks like a painted floor. The two
                // kinds of water come from different places - a zone's own
                // meshes carry the DAT's coordinates, a .water file carries
                // ones this project generated - so they need not agree.
                float lowU = 1e9f, highU = -1e9f, lowV = 1e9f, highV = -1e9f;
                for (const mh::Vertex& vertex : zone->waterVertices)
                {
                    lowU = std::min(lowU, vertex.uv[0]);
                    highU = std::max(highU, vertex.uv[0]);
                    lowV = std::min(lowV, vertex.uv[1]);
                    highV = std::max(highV, vertex.uv[1]);
                }
                std::printf("water uv: u %.2f..%.2f  v %.2f..%.2f  (%.0f x %.0f repeats)\n",
                            lowU, highU, lowV, highV, highU - lowU, highV - lowV);

                waterVertexBuffer = createBuffer(device, zone->waterVertices.data(),
                                                 zone->waterVertices.size() * sizeof(mh::Vertex), wgpu::BufferUsage::Vertex);
                waterIndexBuffer = createBuffer(device, zone->waterIndices.data(),
                                                zone->waterIndices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);
                waterIndexCount = static_cast<uint32_t>(zone->waterIndices.size());

                wgpu::ShaderSourceWGSL waterWgsl;
                waterWgsl.code = mh::kWaterShader;
                wgpu::ShaderModuleDescriptor waterModuleDescriptor{.nextInChain = &waterWgsl};
                wgpu::ShaderModule waterModule = device.CreateShaderModule(&waterModuleDescriptor);

                // Blended, and writing no depth: water is a surface you see through,
                // and letting it write depth hides whatever is under it.
                wgpu::BlendState waterBlend{};
                waterBlend.color.operation = wgpu::BlendOperation::Add;
                waterBlend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
                waterBlend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                waterBlend.alpha.operation = wgpu::BlendOperation::Add;
                waterBlend.alpha.srcFactor = wgpu::BlendFactor::One;
                waterBlend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;

                wgpu::ColorTargetState waterTarget{.format = surfaceFormat, .blend = &waterBlend};
                wgpu::FragmentState waterFragment{
                    .module = waterModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &waterTarget};
                wgpu::DepthStencilState waterDepth{.format = kDepthFormat,
                                                   .depthWriteEnabled = wgpu::OptionalBool::False,
                                                   .depthCompare = wgpu::CompareFunction::Less};

                // FFXI's own water texture, scrolled, rather than an invented
                // colour. The models nothing places - kw01 for rivers, ike for
                // ponds, umi1 for sea - all carry one of these.
                // Blue, not white. This looks for a texture by name and falls
                // back when it finds none, and it finds none in Bastok Markets -
                // so every pooled quad painted an opaque white patch over the
                // floor it was sitting on. Read as missing floor, which is fair.
                wgpu::TextureView waterView = waterFallbackTexture.CreateView();

                // Matched on the texture's own name, the second half of the
                // sixteen-byte field, whatever group the first half puts it
                // in. The rivers and ponds keep theirs under "effect"; Port
                // Bastok's harbour sheets sit under "sea" or under no group
                // at all - umi2, sea01, miz1, miz2 - and matching the whole
                // field found none of them, so the harbour drew as a flat
                // tinted sheet with no ripple on it. Earlier names are
                // preferred: the sea sheets read best on open water, the
                // river ones on a channel.
                static const char* const kWaterSheets[] = {"umi2", "umi1", "sea01", "kaw1", "ike1",
                                                          "ike2",  "umna", "nami",  "miz1", "miz2"};
                // The same sheets with the rivers first. A zone whose water
                // is mostly untextured meshes - Bastok Markets' canal and
                // fountain - is a river zone whatever sheet its harbour
                // names, and wants a river's ripple and tint.
                static const char* const kRiverFirst[] = {"kaw1", "ike1", "ike2", "nami", "miz1",
                                                         "miz2", "umna", "umi2", "umi1", "sea01"};
                const bool riverZone = zone->waterTexture.empty() && zone->waterUntextured > 0;
                const ffxi::Texture* sheet = nullptr;
                const char* sheetName = nullptr;
                size_t sheetRank = sizeof(kWaterSheets) / sizeof(kWaterSheets[0]);
                if (riverZone)
                {
                    for (const char* wanted : kRiverFirst)
                    {
                        for (const auto& [key, texture] : textures)
                        {
                            std::string own = key.size() > 8 ? key.substr(8) : key;
                            while (!own.empty() && (own.back() == ' ' || own.back() == 0))
                            {
                                own.pop_back();
                            }
                            if (own == wanted)
                            {
                                sheet = &texture;
                                sheetName = wanted;
                                sheetRank = 3;      // a river, whatever the sheet
                                break;
                            }
                        }
                        if (sheet)
                        {
                            break;
                        }
                    }
                }
                // The sheet the zone's own water meshes are textured with,
                // when they name one, ahead of the list: it is the one the
                // artists put on that water.
                if (!zone->waterTexture.empty())
                {
                    auto named = textures.find(zone->waterTexture);
                    if (named != textures.end())
                    {
                        sheet = &named->second;
                        sheetName = zone->waterTexture.c_str();
                        std::string own = zone->waterTexture.size() > 8 ? zone->waterTexture.substr(8) : zone->waterTexture;
                        while (!own.empty() && (own.back() == ' ' || own.back() == 0))
                        {
                            own.pop_back();
                        }
                        for (size_t rank = 0; rank < sheetRank; ++rank)
                        {
                            if (own == kWaterSheets[rank])
                            {
                                sheetRank = rank;
                                break;
                            }
                        }
                        if (sheetRank == sizeof(kWaterSheets) / sizeof(kWaterSheets[0]))
                        {
                            sheetRank = 3;      // unknown sheet: treated as a river
                        }
                    }
                }
                for (const auto& [key, texture] : textures)
                {
                    if (sheet && (riverZone || sheetName == zone->waterTexture.c_str()))
                    {
                        break;
                    }
                    std::string own = key.size() > 8 ? key.substr(8) : key;
                    while (!own.empty() && (own.back() == ' ' || own.back() == 0))
                    {
                        own.pop_back();
                    }
                    for (size_t rank = 0; rank < sheetRank; ++rank)
                    {
                        if (own == kWaterSheets[rank])
                        {
                            sheet = &texture;
                            sheetName = kWaterSheets[rank];
                            sheetRank = rank;
                            break;
                        }
                    }
                }
                waterIsSea = false;
                if (sheet)
                {
                    if (wgpu::Texture gpu = mh::uploadTexture(device, *sheet))
                    {
                        batchTextures.push_back(gpu);
                        waterView = batchTextures.back().CreateView();
                        // The first three names are the open-water sheets.
                        waterIsSea = sheetRank <= 2;
                        std::printf("water texture: %s%s\n", sheetName, waterIsSea ? " (sea)" : "");
                    }
                }

                wgpu::BindGroupLayoutEntry waterLayoutEntries[3] = {};
                waterLayoutEntries[0].binding = 0;
                waterLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
                waterLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
                waterLayoutEntries[1].binding = 1;
                waterLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
                waterLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
                waterLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
                waterLayoutEntries[2].binding = 2;
                waterLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
                waterLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
                wgpu::BindGroupLayoutDescriptor waterBglDescriptor{.entryCount = 3, .entries = waterLayoutEntries};
                wgpu::BindGroupLayout waterBgl = device.CreateBindGroupLayout(&waterBglDescriptor);
                wgpu::PipelineLayoutDescriptor waterPlDescriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &waterBgl};
                wgpu::PipelineLayout waterPl = device.CreatePipelineLayout(&waterPlDescriptor);

                wgpu::RenderPipelineDescriptor waterPipelineDescriptor{
                    .layout = waterPl,
                    .vertex = {.module = waterModule, .entryPoint = "vertexMain", .bufferCount = 1, .buffers = &vertexLayout},
                    .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
                    .depthStencil = &waterDepth,
                    .fragment = &waterFragment};
                waterPipeline = device.CreateRenderPipeline(&waterPipelineDescriptor);

                wgpu::BindGroupEntry waterEntries[3] = {};
                waterEntries[0].binding = 0;
                waterEntries[0].buffer = uniformBuffer;
                waterEntries[0].size = sizeof(Uniforms);
                waterEntries[1].binding = 1;
                waterEntries[1].textureView = waterView;
                waterEntries[2].binding = 2;
                waterEntries[2].sampler = sampler;
                wgpu::BindGroupDescriptor waterBgDescriptor{.layout = waterBgl, .entryCount = 3, .entries = waterEntries};
                waterBindGroup = device.CreateBindGroup(&waterBgDescriptor);

                std::printf("water: %zu quads\n", zone->waterIndices.size() / 6);
            }

            std::unordered_map<std::string, wgpu::TextureView> uploadedViews;
            size_t uploaded = 0;
            size_t untextured = 0;
            // Counted apart from untextured. A mesh naming a texture the DAT does
            // not hold and a mesh naming none at all both land on the white
            // fallback and look identical on screen, but only the first is a
            // lookup failure - and only the first was being counted. Bastok
            // Markets' water meshes are the second kind, so the line said "0 with
            // no texture" while the water rendered as a sheet of pure white.
            size_t unnamed = 0;
            for (const mh::InstancedDraw& batch : zone->draws)
            {
                if (batch.texture.empty())
                {
                    ++unnamed;
                }
                // Cached by name: instancing produces one draw per mesh, and many
                // meshes share a texture. Uploading per draw meant 397 GPU textures
                // for 46 distinct images.
                wgpu::TextureView view = batch.water ? waterFallbackView : whiteView;
                if (!batch.texture.empty())
                {
                    auto cached = uploadedViews.find(batch.texture);
                    if (cached != uploadedViews.end())
                    {
                        view = cached->second;
                    }
                    else
                    {
                        auto found = textures.find(batch.texture);
                        if (found != textures.end())
                        {
                            wgpu::Texture gpu = mh::uploadTexture(device, found->second);
                            if (gpu)
                            {
                                batchTextures.push_back(gpu);
                                view = batchTextures.back().CreateView();
                                uploadedViews.emplace(batch.texture, view);
                                ++uploaded;
                            }
                        }
                        else
                        {
                            ++untextured;
                        }
                    }
                }

                wgpu::BindGroupEntry entries[3] = {};
                entries[0].binding = 0;
                entries[0].buffer = uniformBuffer;
                entries[0].size = sizeof(Uniforms);
                entries[1].binding = 1;
                entries[1].textureView = view;
                entries[2].binding = 2;
                entries[2].sampler = sampler;

                wgpu::BindGroupDescriptor bindGroupDescriptor{
                    .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
                batchBindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));

                if (batch.effect && effectBindGroupLayout)
                {
                    // scroll u, scroll v, how much lighting applies, unused.
                    // Flames and glows are self-lit; a waterfall's sheet is
                    // lit like the rock behind it. Told apart by the clock
                    // gate for now, which is what the flames carry.
                    const float params[8] = {batch.scroll[0], batch.scroll[1], batch.nightOnly ? 0.0f : 0.7f, 0.0f,
                                             // The wave, rewritten every frame when the
                                             // draw carries curves; inert otherwise.
                                             0.0f, 0.0f, 1.0f, 0.0f};
                    effectBuffers.push_back(createBuffer(device, params, sizeof(params),
                                                         wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst));
                    wgpu::BindGroupEntry effectEntries[4] = {entries[0], entries[1], entries[2], {}};
                    effectEntries[3].binding = 3;
                    effectEntries[3].buffer = effectBuffers.back();
                    effectEntries[3].size = sizeof(params);
                    wgpu::BindGroupDescriptor effectGroupDescriptor{
                        .layout = effectBindGroupLayout, .entryCount = 4, .entries = effectEntries};
                    effectBindGroups.push_back(device.CreateBindGroup(&effectGroupDescriptor));
                    effectWaveBuffers.push_back(effectBuffers.back());
                }
                else
                {
                    effectBindGroups.push_back(nullptr);
                    effectWaveBuffers.push_back(nullptr);
                }
            }
            if (!skyObjects.indices.empty() && effectBindGroupLayout)
            {
                skyVertexBuffer = createBuffer(device, skyObjects.vertices.data(),
                                               skyObjects.vertices.size() * sizeof(mh::Vertex), wgpu::BufferUsage::Vertex);
                skyIndexBuffer = createBuffer(device, skyObjects.indices.data(),
                                              skyObjects.indices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);
                skyInstanceBuffer = createBuffer(device, skyObjects.instances.data(),
                                                 skyObjects.instances.size() * sizeof(float), wgpu::BufferUsage::Vertex);
                for (const mh::InstancedDraw& draw : skyObjects.draws)
                {
                    wgpu::TextureView view;
                    auto found = textures.find(draw.texture);
                    if (found != textures.end())
                    {
                        if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                        {
                            batchTextures.push_back(gpu);
                            view = batchTextures.back().CreateView();
                        }
                    }
                    if (!view)
                    {
                        skyObjectBindGroups.push_back(nullptr);
                        continue;
                    }
                    // scroll u, v; lighting; camera-relative and unfogged.
                    // The clouds take the zone's light so they darken with
                    // the night; the stars are their own light.
                    const float params[8] = {draw.scroll[0], draw.scroll[1], draw.nightOnly ? 0.0f : 1.0f, 1.0f,
                                             0.0f, 0.0f, 1.0f, 0.0f};
                    effectBuffers.push_back(createBuffer(device, params, sizeof(params), wgpu::BufferUsage::Uniform));
                    wgpu::BindGroupEntry skyEntries[4] = {};
                    skyEntries[0].binding = 0;
                    skyEntries[0].buffer = uniformBuffer;
                    skyEntries[0].size = sizeof(Uniforms);
                    skyEntries[1].binding = 1;
                    skyEntries[1].textureView = view;
                    skyEntries[2].binding = 2;
                    skyEntries[2].sampler = sampler;
                    skyEntries[3].binding = 3;
                    skyEntries[3].buffer = effectBuffers.back();
                    skyEntries[3].size = sizeof(params);
                    wgpu::BindGroupDescriptor skyGroupDescriptor{
                        .layout = effectBindGroupLayout, .entryCount = 4, .entries = skyEntries};
                    skyObjectBindGroups.push_back(device.CreateBindGroup(&skyGroupDescriptor));
                }
                std::printf("sky: %zu objects from the weather generators\n", skyObjects.draws.size());
            }
            if (!spriteInstances.empty() && effectBindGroupLayout)
            {
                // One batch per texture; the instances are sorted to match so
                // each batch is one range of the vertex buffer.
                std::sort(spriteInstances.begin(), spriteInstances.end(),
                          [&sprites](const mh::SpriteInstance& a, const mh::SpriteInstance& b) {
                              const auto ta = sprites.find(a.animation);
                              const auto tb = sprites.find(b.animation);
                              const std::string& na = ta == sprites.end() ? a.animation : ta->second.texture;
                              const std::string& nb = tb == sprites.end() ? b.animation : tb->second.texture;
                              return na < nb;
                          });
                spriteCapacity = spriteInstances.size() * 6;
                spriteVertices.assign(spriteCapacity, mh::Vertex{});
                spriteVertexBuffer = createBuffer(device, spriteVertices.data(), spriteVertices.size() * sizeof(mh::Vertex),
                                                  wgpu::BufferUsage::Vertex);
                std::vector<uint32_t> indices(spriteCapacity);
                for (uint32_t i = 0; i < spriteCapacity; ++i)
                {
                    indices[i] = i;
                }
                spriteIndexBuffer = createBuffer(device, indices.data(), indices.size() * sizeof(uint32_t),
                                                 wgpu::BufferUsage::Index);
                if (!spriteInstanceBuffer)
                {
                    const mh::Mat4 identity = mh::Mat4::identity();
                    spriteInstanceBuffer = createBuffer(device, identity.m, sizeof(identity.m), wgpu::BufferUsage::Vertex);
                }
                for (size_t i = 0; i < spriteInstances.size(); ++i)
                {
                    const auto animation = sprites.find(spriteInstances[i].animation);
                    const std::string textureName = animation == sprites.end() ? "" : animation->second.texture;
                    if (!spriteBatches.empty() && spriteBatches.back().texture == textureName)
                    {
                        spriteBatches.back().count += 6;
                        continue;
                    }
                    SpriteBatch batch;
                    batch.texture = textureName;
                    batch.first = static_cast<uint32_t>(i * 6);
                    batch.count = 6;
                    auto found = textures.find(textureName);
                    if (found != textures.end())
                    {
                        if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                        {
                            batchTextures.push_back(gpu);
                            // no scroll, self-lit, world space, and no wave -
                            // opacity 1 or the sprite would not be drawn at all.
                            const float params[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
                            effectBuffers.push_back(createBuffer(device, params, sizeof(params), wgpu::BufferUsage::Uniform));
                            wgpu::BindGroupEntry spriteEntries[4] = {};
                            spriteEntries[0].binding = 0;
                            spriteEntries[0].buffer = uniformBuffer;
                            spriteEntries[0].size = sizeof(Uniforms);
                            spriteEntries[1].binding = 1;
                            spriteEntries[1].textureView = batchTextures.back().CreateView();
                            spriteEntries[2].binding = 2;
                            spriteEntries[2].sampler = sampler;
                            spriteEntries[3].binding = 3;
                            spriteEntries[3].buffer = effectBuffers.back();
                            spriteEntries[3].size = sizeof(params);
                            wgpu::BindGroupDescriptor spriteGroupDescriptor{
                                .layout = effectBindGroupLayout, .entryCount = 4, .entries = spriteEntries};
                            batch.bindGroup = device.CreateBindGroup(&spriteGroupDescriptor);
                        }
                    }
                    if (!batch.bindGroup)
                    {
                        std::printf("  sprite texture [%s] not found\n", textureName.c_str());
                    }
                    spriteBatches.push_back(std::move(batch));
                }
                std::printf("sprites: %zu batches\n", spriteBatches.size());
            }
            if (unnamed)
            {
                std::printf("  %zu draws name no texture at all - they render white\n", unnamed);
            }
            std::printf("%zu draws, %zu textures uploaded, %zu with no texture in this DAT\n", zone->draws.size(),
                        uploaded, untextured);
        }

        // --- the map ------------------------------------------------------------
        // Baked once, straight down, through the pipeline that draws the zone - so
        // the radar shows the world as it actually looks rather than a schematic
        // redrawn from the same data.
        if (zone && pipeline && !zone->draws.empty())
        {
            constexpr uint32_t kMapSize = 2048;

            wgpu::TextureDescriptor mapDescriptor{
                .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                         wgpu::TextureUsage::CopySrc,
                .dimension = wgpu::TextureDimension::e2D,
                .size = {kMapSize, kMapSize, 1},
                .format = surfaceFormat,
                .mipLevelCount = 1,
                .sampleCount = 1};
            if (!mapTexture)
            {
                mapTexture = device.CreateTexture(&mapDescriptor);
            }

            wgpu::TextureDescriptor mapDepthDescriptor{.usage = wgpu::TextureUsage::RenderAttachment,
                                                       .dimension = wgpu::TextureDimension::e2D,
                                                       .size = {kMapSize, kMapSize, 1},
                                                       .format = kDepthFormat,
                                                       .mipLevelCount = 1,
                                                       .sampleCount = 1};
            wgpu::Texture mapDepth = device.CreateTexture(&mapDepthDescriptor);

            // A square covering the zone, so the map keeps the world aspect and a
            // step north is the same number of pixels as a step east.
            const mh::Vec3 lo = zone->boundsMin;
            const mh::Vec3 hi = zone->boundsMax;
            const float half = std::max(hi.x - lo.x, hi.z - lo.z) * 0.5f;
            const mh::Vec3 middle = zone->centre();

            Uniforms mapUniforms{};
            // Straight down, with +z up the screen and +x to the right.
            //
            // The left and right of this projection used to be swapped, to make the
            // bake agree with rasteriseWalkable - 99.8% of walkable area covered
            // against 52.6% without it. That score was the problem: it compared the
            // bake against a mask nothing draws, rasteriseWalkable is referenced
            // from no live code path at all, and two mirrored things agree with
            // each other perfectly. The map came out reversed, so walking left slid
            // it the wrong way, which is a hard thing to name and an easy thing to
            // blame on yourself.
            //
            // mapUv reads u increasing with world x and the dots are found by
            // comparing world positions, so an unswapped bake is what both of them
            // already expect. The dots do not move either way - they never touch
            // the map's sampling.
            //
            // Same lesson as the world frame's own half turn: validate against
            // something you did not produce.
            const mh::Mat4 mapView = mh::lookAt({middle.x, hi.y + 100.0f, middle.z}, middle, {0.0f, 0.0f, 1.0f});
            const mh::Mat4 mapProjection = mh::orthographic(half, -half, -half, half, 1.0f, (hi.y - lo.y) + 400.0f);
            const mh::Mat4 mapViewProjection = mapProjection * mapView;
            std::memcpy(mapUniforms.viewProjection, mapViewProjection.m, sizeof(mapUniforms.viewProjection));

            // Flat and unfogged. A map lit from an angle reads as terrain in
            // shadow rather than as ground, and fog would erase the far half.
            const mh::Vec3 down = mh::normalise(mh::Vec3{0.0f, 1.0f, 0.0f});
            mapUniforms.lightDirection[0] = down.x;
            mapUniforms.lightDirection[1] = down.y;
            mapUniforms.lightDirection[2] = down.z;
            mapUniforms.ambient[0] = mapUniforms.ambient[1] = mapUniforms.ambient[2] = 0.75f;
            mapUniforms.sunlight[0] = mapUniforms.sunlight[1] = mapUniforms.sunlight[2] = 0.35f;
            mapUniforms.fogRange[0] = 1e9f;
            mapUniforms.fogRange[1] = 1e9f;
            queue.WriteBuffer(uniformBuffer, 0, &mapUniforms, sizeof(mapUniforms));

            wgpu::RenderPassColorAttachment mapColour{.view = mapTexture.CreateView(),
                                                      .loadOp = wgpu::LoadOp::Clear,
                                                      .storeOp = wgpu::StoreOp::Store,
                                                      .clearValue = {0.0f, 0.0f, 0.0f, 0.0f}};
            wgpu::RenderPassDepthStencilAttachment mapDepthAttachment{.view = mapDepth.CreateView(),
                                                                      .depthLoadOp = wgpu::LoadOp::Clear,
                                                                      .depthStoreOp = wgpu::StoreOp::Store,
                                                                      .depthClearValue = 1.0f};
            wgpu::RenderPassDescriptor mapPassDescriptor{.colorAttachmentCount = 1,
                                                         .colorAttachments = &mapColour,
                                                         .depthStencilAttachment = &mapDepthAttachment};

            wgpu::CommandEncoder mapEncoder = device.CreateCommandEncoder();
            wgpu::RenderPassEncoder mapPass = mapEncoder.BeginRenderPass(&mapPassDescriptor);
            mapPass.SetVertexBuffer(0, vertexBuffer);
            mapPass.SetVertexBuffer(1, instanceBuffer);
            mapPass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
            {
                // Rooms are left off the map. They hang in the sky above the
                // streets they belong to, and baked from above they painted
                // their roofs over Southern San d'Oria's whole plan.
                bool insideRoom = false;
                for (const mh::InteriorLighting& room : interiors)
                {
                    const float across =
                        std::max(room.boundsMax.x - room.boundsMin.x, room.boundsMax.z - room.boundsMin.z);
                    if (across <= kRoomAtMost && room.holdsDraw(i))
                    {
                        insideRoom = true;
                        break;
                    }
                }
                if (insideRoom)
                {
                    continue;
                }

                const mh::InstancedDraw& draw = zone->draws[i];
                if ((draw.texture.empty() && !draw.water) || draw.effect)
                {
                    continue;   // an occlusion volume, not scenery - see the frame loop
                }
                mapPass.SetPipeline(draw.cutout ? cutoutPipeline : pipeline);
                mapPass.SetBindGroup(0, batchBindGroups[i]);
                mapPass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
            }
            // No water on the map. Baked from above it paints its whole
            // extent one flat colour, and a harbour's plane is the size of
            // the district around it - Port Bastok's minimap came out as one
            // green disc. The bed beneath shows the shape of the water well
            // enough.
            mapPass.End();
            wgpu::CommandBuffer mapCommands = mapEncoder.Finish();
            queue.Submit(1, &mapCommands);

            std::printf("map: baked %ux%u covering %.0f units, centred on %.0f %.0f\n", kMapSize, kMapSize, half * 2.0f,
                        middle.x, middle.z);

            // The walkable mask and the map's extent go with the bake. At
            // startup the radar is not built yet and does this itself; on a
            // zone change it already holds the mask texture, which is written
            // into again here rather than replaced - see mapTexture.
            if (maskTexture && !collision.empty())
            {
                constexpr uint32_t kMaskSize = 1024;
                mapCentreX = middle.x;
                mapCentreZ = middle.z;
                mapHalf = half;
                const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMaskSize, middle, half);
                wgpu::TexelCopyTextureInfo maskDestination{.texture = maskTexture};
                wgpu::TexelCopyBufferLayout maskLayout{.bytesPerRow = kMaskSize, .rowsPerImage = kMaskSize};
                const wgpu::Extent3D maskExtent{kMaskSize, kMaskSize, 1};
                queue.WriteTexture(&maskDestination, mask.data(), mask.size(), &maskLayout, &maskExtent);
            }

            // mapPath writes the bake out so it can be looked at directly.
            if (options.mapPath)
            {
                const uint32_t bytesPerRow = (kMapSize * 4 + 255) / 256 * 256;
                wgpu::BufferDescriptor readDescriptor{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                                      .size = static_cast<uint64_t>(bytesPerRow) * kMapSize};
                wgpu::Buffer readback = device.CreateBuffer(&readDescriptor);

                wgpu::CommandEncoder copyEncoder = device.CreateCommandEncoder();
                wgpu::TexelCopyTextureInfo source{.texture = mapTexture};
                wgpu::TexelCopyBufferInfo destination{
                    .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = kMapSize}, .buffer = readback};
                const wgpu::Extent3D extent{kMapSize, kMapSize, 1};
                copyEncoder.CopyTextureToBuffer(&source, &destination, &extent);
                wgpu::CommandBuffer copyCommands = copyEncoder.Finish();
                queue.Submit(1, &copyCommands);

                // The mask over the same square, so the two can be checked against
                // each other. They are only useful together, and an offset here
                // would put every radar dot the same distance off the terrain it
                // is meant to sit on.
                {
                    const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMapSize, middle, half);
                    const std::string maskPath = *options.mapPath + ".mask.pgm";
                    if (std::FILE* file = std::fopen(maskPath.c_str(), "wb"))
                    {
                        std::fprintf(file, "P5\n%u %u\n255\n", kMapSize, kMapSize);
                        std::fwrite(mask.data(), 1, mask.size(), file);
                        std::fclose(file);
                        std::printf("wrote %s\n", maskPath.c_str());
                    }
                }

                bool mapped = false;
                wgpu::Future future = readback.MapAsync(wgpu::MapMode::Read, 0, wgpu::kWholeMapSize,
                                                        wgpu::CallbackMode::AllowProcessEvents,
                                                        [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                                                        { mapped = status == wgpu::MapAsyncStatus::Success; });
                instance.WaitAny(future, UINT64_MAX);
                if (mapped)
                {
                    const auto* pixels = static_cast<const uint8_t*>(readback.GetConstMappedRange());
                    if (pixels && writeBmp(options.mapPath->c_str(), pixels, kMapSize, kMapSize, bytesPerRow))
                    {
                        std::printf("wrote %s\n", options.mapPath->c_str());
                    }
                    readback.Unmap();
                }
            }
        }
        return 0;
    };

    if (const int failed = readZone())
    {
        return failed;
    }


    // --- the radar -----------------------------------------------------------
    wgpu::Buffer radarUniformBuffer;
    wgpu::RenderPipeline radarPipeline;
    wgpu::BindGroup radarBindGroup;

    // The glyph atlas. Text is worth doing without rather than failing to
    // open a window over, so an absent atlas leaves the nameplates off and
    // everything else running.
    //
    // The atlas is copied beside the renderer executable, but the renderer
    // also runs in-process from the console, where "the executable" is a .NET
    // host somewhere else entirely, and run.ps1 starts from the repo root
    // where there is no assets folder at all. So try the places it can be,
    // and say which were tried if it is in none of them - an absent atlas
    // takes the clock, the compass, the coordinates and the whole chat panel
    // with it, and did so in silence.
    std::vector<std::filesystem::path> fontCandidates;
    if (const char* installedAt = SDL_GetBasePath())
    {
        fontCandidates.emplace_back(std::filesystem::path{installedAt} / "assets");
    }
    if (const char* nativeDir = std::getenv("MOGHOUSE_NATIVE_DIR"))
    {
        fontCandidates.emplace_back(std::filesystem::path{nativeDir} / "assets");
    }
    const std::filesystem::path here = std::filesystem::current_path();
    fontCandidates.push_back(here / "assets");
    fontCandidates.push_back(here / "build-renderer" / "assets");
    fontCandidates.push_back(here / "renderer" / "assets");

    mh::TextFont textFont;
    for (const std::filesystem::path& candidate : fontCandidates)
    {
        textFont = mh::loadTextFont(candidate.string());
        if (!textFont.empty())
        {
            break;
        }
    }
    if (textFont.empty())
    {
        std::printf("no font atlas found - the HUD will not draw. Set MOGHOUSE_FONT. Looked in:\n");
        for (const std::filesystem::path& candidate : fontCandidates)
        {
            std::printf("  %s\n", candidate.string().c_str());
        }
    }

    wgpu::Texture fontTexture;
    if (!textFont.empty())
    {
        wgpu::TextureDescriptor fontDescriptor{};
        fontDescriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        fontDescriptor.dimension = wgpu::TextureDimension::e2D;
        fontDescriptor.size = {textFont.width, textFont.height, 1};
        fontDescriptor.format = wgpu::TextureFormat::RGBA8Unorm;
        fontDescriptor.mipLevelCount = 1;
        fontDescriptor.sampleCount = 1;
        fontTexture = device.CreateTexture(&fontDescriptor);

        wgpu::TexelCopyTextureInfo destination{};
        destination.texture = fontTexture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = textFont.width * 4;
        layout.rowsPerImage = textFont.height;
        const wgpu::Extent3D extent{textFont.width, textFont.height, 1};
        queue.WriteTexture(&destination, textFont.pixels.data(), textFont.pixels.size(), &layout, &extent);
    }

    // Shared by the nameplates and the HUD, which draw the same atlas.
    wgpu::Sampler fontSampler;
    if (fontTexture)
    {
        wgpu::SamplerDescriptor fontSamplerDescriptor{};
        fontSamplerDescriptor.magFilter = wgpu::FilterMode::Linear;
        fontSamplerDescriptor.minFilter = wgpu::FilterMode::Linear;
        fontSamplerDescriptor.addressModeU = wgpu::AddressMode::ClampToEdge;
        fontSamplerDescriptor.addressModeV = wgpu::AddressMode::ClampToEdge;
        fontSampler = device.CreateSampler(&fontSamplerDescriptor);
    }

    wgpu::Buffer hudUniformBuffer;
    wgpu::RenderPipeline hudPipeline;
    wgpu::BindGroup hudBindGroup;
    if (fontTexture)
    {
        wgpu::BufferDescriptor hudBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                   .size = sizeof(HudUniforms)};
        hudUniformBuffer = device.CreateBuffer(&hudBufferDescriptor);

        wgpu::ShaderSourceWGSL hudWgsl;
        hudWgsl.code = mh::kHudShader;
        wgpu::ShaderModuleDescriptor hudModuleDescriptor{.nextInChain = &hudWgsl};
        wgpu::ShaderModule hudModule = device.CreateShaderModule(&hudModuleDescriptor);

        wgpu::BindGroupLayoutEntry hudLayoutEntries[3] = {};
        hudLayoutEntries[0].binding = 0;
        hudLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        hudLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        hudLayoutEntries[1].binding = 1;
        hudLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        hudLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        hudLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        hudLayoutEntries[2].binding = 2;
        hudLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        hudLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor hudLayoutDescriptor{.entryCount = 3, .entries = hudLayoutEntries};
        wgpu::BindGroupLayout hudBindGroupLayout = device.CreateBindGroupLayout(&hudLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor hudPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                   .bindGroupLayouts = &hudBindGroupLayout};
        wgpu::PipelineLayout hudPipelineLayout = device.CreatePipelineLayout(&hudPipelineLayoutDescriptor);

        wgpu::BlendState hudBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState hudTarget{.format = surfaceFormat, .blend = &hudBlend};
        wgpu::FragmentState hudFragment{
            .module = hudModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &hudTarget};
        wgpu::DepthStencilState hudDepth{.format = kDepthFormat,
                                         .depthWriteEnabled = wgpu::OptionalBool::False,
                                         .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor hudPipelineDescriptor{
            .layout = hudPipelineLayout,
            .vertex = {.module = hudModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &hudDepth,
            .fragment = &hudFragment};
        hudPipeline = device.CreateRenderPipeline(&hudPipelineDescriptor);

        wgpu::BindGroupEntry hudEntries[3] = {};
        hudEntries[0].binding = 0;
        hudEntries[0].buffer = hudUniformBuffer;
        hudEntries[0].size = sizeof(HudUniforms);
        hudEntries[1].binding = 1;
        hudEntries[1].textureView = fontTexture.CreateView();
        hudEntries[2].binding = 2;
        hudEntries[2].sampler = fontSampler;
        wgpu::BindGroupDescriptor hudBindGroupDescriptor{
            .layout = hudBindGroupLayout, .entryCount = 3, .entries = hudEntries};
        hudBindGroup = device.CreateBindGroup(&hudBindGroupDescriptor);
    }

    // The bags. Item icons live on an atlas of their own, filled as the client
    // sends them, and the panel draws quads out of it beside letters from the
    // font atlas - hence two textures on one pipeline.
    wgpu::Buffer inventoryUniformBuffer;
    wgpu::RenderPipeline inventoryPipeline;
    wgpu::BindGroup inventoryBindGroup;
    wgpu::Texture iconAtlas;
    if (fontTexture)
    {
        const uint32_t atlasSide = static_cast<uint32_t>(mh::kIconAtlasCells * mh::kIconSize);
        wgpu::TextureDescriptor atlasDescriptor{
            .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {atlasSide, atlasSide, 1},
            .format = wgpu::TextureFormat::RGBA8Unorm,
            .mipLevelCount = 1,
            .sampleCount = 1};
        iconAtlas = device.CreateTexture(&atlasDescriptor);

        // Cleared once, so a slot whose icon has not arrived draws nothing
        // rather than whatever the driver left in the memory.
        std::vector<uint8_t> blank(static_cast<size_t>(atlasSide) * atlasSide * 4, 0);
        wgpu::TexelCopyTextureInfo blankTarget{.texture = iconAtlas};
        wgpu::TexelCopyBufferLayout blankLayout{.bytesPerRow = atlasSide * 4, .rowsPerImage = atlasSide};
        wgpu::Extent3D blankExtent{atlasSide, atlasSide, 1};
        queue.WriteTexture(&blankTarget, blank.data(), blank.size(), &blankLayout, &blankExtent);

        wgpu::BufferDescriptor inventoryBufferDescriptor{
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
            .size = sizeof(InventoryUniforms)};
        inventoryUniformBuffer = device.CreateBuffer(&inventoryBufferDescriptor);

        wgpu::ShaderSourceWGSL inventoryWgsl;
        inventoryWgsl.code = mh::kInventoryShader;
        wgpu::ShaderModuleDescriptor inventoryModuleDescriptor{.nextInChain = &inventoryWgsl};
        wgpu::ShaderModule inventoryModule = device.CreateShaderModule(&inventoryModuleDescriptor);

        wgpu::BindGroupLayoutEntry inventoryLayoutEntries[5] = {};
        inventoryLayoutEntries[0].binding = 0;
        inventoryLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        inventoryLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        inventoryLayoutEntries[1].binding = 1;
        inventoryLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        inventoryLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        inventoryLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        inventoryLayoutEntries[2].binding = 2;
        inventoryLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        inventoryLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
        inventoryLayoutEntries[3].binding = 3;
        inventoryLayoutEntries[3].visibility = wgpu::ShaderStage::Fragment;
        inventoryLayoutEntries[3].texture.sampleType = wgpu::TextureSampleType::Float;
        inventoryLayoutEntries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        inventoryLayoutEntries[4].binding = 4;
        inventoryLayoutEntries[4].visibility = wgpu::ShaderStage::Fragment;
        inventoryLayoutEntries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor inventoryLayoutDescriptor{.entryCount = 5,
                                                                  .entries = inventoryLayoutEntries};
        wgpu::BindGroupLayout inventoryBindGroupLayout = device.CreateBindGroupLayout(&inventoryLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor inventoryPipelineLayoutDescriptor{
            .bindGroupLayoutCount = 1, .bindGroupLayouts = &inventoryBindGroupLayout};
        wgpu::PipelineLayout inventoryPipelineLayout =
            device.CreatePipelineLayout(&inventoryPipelineLayoutDescriptor);

        // Premultiplied: the shader multiplies colour by alpha so a letter's
        // black outline does not darken the panel it is drawn over.
        wgpu::BlendState inventoryBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState inventoryTarget{.format = surfaceFormat, .blend = &inventoryBlend};
        wgpu::FragmentState inventoryFragment{.module = inventoryModule,
                                              .entryPoint = "fragmentMain",
                                              .targetCount = 1,
                                              .targets = &inventoryTarget};
        wgpu::DepthStencilState inventoryDepth{.format = kDepthFormat,
                                               .depthWriteEnabled = wgpu::OptionalBool::False,
                                               .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor inventoryPipelineDescriptor{
            .layout = inventoryPipelineLayout,
            .vertex = {.module = inventoryModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &inventoryDepth,
            .fragment = &inventoryFragment};
        inventoryPipeline = device.CreateRenderPipeline(&inventoryPipelineDescriptor);

        wgpu::BindGroupEntry inventoryEntries[5] = {};
        inventoryEntries[0].binding = 0;
        inventoryEntries[0].buffer = inventoryUniformBuffer;
        inventoryEntries[0].size = sizeof(InventoryUniforms);
        inventoryEntries[1].binding = 1;
        inventoryEntries[1].textureView = fontTexture.CreateView();
        inventoryEntries[2].binding = 2;
        inventoryEntries[2].sampler = fontSampler;
        inventoryEntries[3].binding = 3;
        inventoryEntries[3].textureView = iconAtlas.CreateView();
        inventoryEntries[4].binding = 4;
        inventoryEntries[4].sampler = fontSampler;
        wgpu::BindGroupDescriptor inventoryBindGroupDescriptor{
            .layout = inventoryBindGroupLayout, .entryCount = 5, .entries = inventoryEntries};
        inventoryBindGroup = device.CreateBindGroup(&inventoryBindGroupDescriptor);
    }

    /// Which atlas cell each item's icon went into, and what it is called.
    struct ItemFacing
    {
        int cell{-1};
        std::string name;
        std::string description;
        uint16_t type{};
        uint16_t level{};
        uint16_t slots{};
    };
    std::unordered_map<uint16_t, ItemFacing> itemFacing;

    // The first two cells are the toolbar's, drawn rather than read: the game
    // has no bag and no cog anywhere in its menu DAT. Items start after them.
    constexpr int kBagIconCell = 0;
    constexpr int kCogIconCell = 1;
    constexpr int kShieldIconCell = 2;
    int nextIconCell = 3;

    if (iconAtlas)
    {
        for (int cell : {kBagIconCell, kCogIconCell, kShieldIconCell})
        {
            const std::vector<uint8_t> drawn = drawToolbarIcon(cell);
            wgpu::TexelCopyTextureInfo target{
                .texture = iconAtlas,
                .origin = {static_cast<uint32_t>((cell % mh::kIconAtlasCells) * mh::kIconSize),
                           static_cast<uint32_t>((cell / mh::kIconAtlasCells) * mh::kIconSize), 0}};
            wgpu::TexelCopyBufferLayout layout{.bytesPerRow = mh::kIconSize * 4,
                                               .rowsPerImage = mh::kIconSize};
            wgpu::Extent3D extent{mh::kIconSize, mh::kIconSize, 1};
            queue.WriteTexture(&target, drawn.data(), drawn.size(), &layout, &extent);
        }
    }

    wgpu::Buffer plateUniformBuffer;
    wgpu::RenderPipeline platePipeline;
    wgpu::BindGroup plateBindGroup;
    if (fontTexture)
    {
        wgpu::BufferDescriptor plateBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                     .size = sizeof(NameplateUniforms)};
        plateUniformBuffer = device.CreateBuffer(&plateBufferDescriptor);

        wgpu::ShaderSourceWGSL plateWgsl;
        plateWgsl.code = mh::kNameplateShader;
        wgpu::ShaderModuleDescriptor plateModuleDescriptor{.nextInChain = &plateWgsl};
        wgpu::ShaderModule plateModule = device.CreateShaderModule(&plateModuleDescriptor);

        wgpu::BindGroupLayoutEntry plateLayoutEntries[3] = {};
        plateLayoutEntries[0].binding = 0;
        plateLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        plateLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        plateLayoutEntries[1].binding = 1;
        plateLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        plateLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        plateLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        plateLayoutEntries[2].binding = 2;
        plateLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        plateLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor plateLayoutDescriptor{.entryCount = 3, .entries = plateLayoutEntries};
        wgpu::BindGroupLayout plateBindGroupLayout = device.CreateBindGroupLayout(&plateLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor platePipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                     .bindGroupLayouts = &plateBindGroupLayout};
        wgpu::PipelineLayout platePipelineLayout = device.CreatePipelineLayout(&platePipelineLayoutDescriptor);

        wgpu::BlendState plateBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState plateTarget{.format = surfaceFormat, .blend = &plateBlend};
        wgpu::FragmentState plateFragment{
            .module = plateModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &plateTarget};

        // Always, because the depth test cannot help here: the whole pass is
        // one fullscreen triangle at depth zero and each label is projected in
        // the fragment shader, so there is no per-label depth to compare.
        // Whether a name is behind a wall is decided on the CPU, with the same
        // raycast the camera uses - see where the plates are laid out.
        wgpu::DepthStencilState plateDepth{.format = kDepthFormat,
                                           .depthWriteEnabled = false,
                                           .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor platePipelineDescriptor{
            .layout = platePipelineLayout,
            .vertex = {.module = plateModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &plateDepth,
            .fragment = &plateFragment};
        platePipeline = device.CreateRenderPipeline(&platePipelineDescriptor);

        wgpu::BindGroupEntry plateEntries[3] = {};
        plateEntries[0].binding = 0;
        plateEntries[0].buffer = plateUniformBuffer;
        plateEntries[0].size = sizeof(NameplateUniforms);
        plateEntries[1].binding = 1;
        plateEntries[1].textureView = fontTexture.CreateView();
        plateEntries[2].binding = 2;
        plateEntries[2].sampler = fontSampler;
        wgpu::BindGroupDescriptor plateBindGroupDescriptor{
            .layout = plateBindGroupLayout, .entryCount = 3, .entries = plateEntries};
        plateBindGroup = device.CreateBindGroup(&plateBindGroupDescriptor);
    }

    wgpu::Buffer zoneLineUniformBuffer;
    wgpu::RenderPipeline zoneLinePipeline;
    wgpu::BindGroup zoneLineBindGroup;
    {
        wgpu::BufferDescriptor zoneLineBufferDescriptor{
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(ZoneLineUniforms)};
        zoneLineUniformBuffer = device.CreateBuffer(&zoneLineBufferDescriptor);

        wgpu::ShaderSourceWGSL zoneLineWgsl;
        zoneLineWgsl.code = mh::kZoneLineShader;
        wgpu::ShaderModuleDescriptor zoneLineModuleDescriptor{.nextInChain = &zoneLineWgsl};
        wgpu::ShaderModule zoneLineModule = device.CreateShaderModule(&zoneLineModuleDescriptor);

        wgpu::BindGroupLayoutEntry zoneLineLayoutEntry{};
        zoneLineLayoutEntry.binding = 0;
        zoneLineLayoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        zoneLineLayoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;

        wgpu::BindGroupLayoutDescriptor zoneLineLayoutDescriptor{.entryCount = 1, .entries = &zoneLineLayoutEntry};
        wgpu::BindGroupLayout zoneLineBindGroupLayout = device.CreateBindGroupLayout(&zoneLineLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor zoneLinePipelineLayoutDescriptor{
            .bindGroupLayoutCount = 1, .bindGroupLayouts = &zoneLineBindGroupLayout};
        wgpu::PipelineLayout zoneLinePipelineLayout = device.CreatePipelineLayout(&zoneLinePipelineLayoutDescriptor);

        // Added rather than blended over: a glow brightens what is behind it.
        wgpu::BlendState zoneLineBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::One},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::One}};
        wgpu::ColorTargetState zoneLineTarget{.format = surfaceFormat, .blend = &zoneLineBlend};
        wgpu::FragmentState zoneLineFragment{.module = zoneLineModule,
                                             .entryPoint = "fragmentMain",
                                             .targetCount = 1,
                                             .targets = &zoneLineTarget};

        // Occluded by the world but not writing depth: a line behind a wall
        // stays behind it, and two rings overlapping do not cut each other up.
        wgpu::DepthStencilState zoneLineDepth{.format = kDepthFormat,
                                              .depthWriteEnabled = false,
                                              .depthCompare = wgpu::CompareFunction::LessEqual};

        wgpu::RenderPipelineDescriptor zoneLinePipelineDescriptor{
            .layout = zoneLinePipelineLayout,
            .vertex = {.module = zoneLineModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &zoneLineDepth,
            .fragment = &zoneLineFragment};
        zoneLinePipeline = device.CreateRenderPipeline(&zoneLinePipelineDescriptor);

        wgpu::BindGroupEntry zoneLineEntry{};
        zoneLineEntry.binding = 0;
        zoneLineEntry.buffer = zoneLineUniformBuffer;
        zoneLineEntry.size = sizeof(ZoneLineUniforms);
        wgpu::BindGroupDescriptor zoneLineBindGroupDescriptor{
            .layout = zoneLineBindGroupLayout, .entryCount = 1, .entries = &zoneLineEntry};
        zoneLineBindGroup = device.CreateBindGroup(&zoneLineBindGroupDescriptor);
    }

    wgpu::Buffer chatUniformBuffer;
    wgpu::RenderPipeline chatPipeline;
    wgpu::BindGroup chatBindGroup;
    {
        wgpu::BufferDescriptor chatBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                    .size = sizeof(ChatUniforms)};
        chatUniformBuffer = device.CreateBuffer(&chatBufferDescriptor);

        wgpu::ShaderSourceWGSL chatWgsl;
        chatWgsl.code = mh::kChatShader;
        wgpu::ShaderModuleDescriptor chatModuleDescriptor{.nextInChain = &chatWgsl};
        wgpu::ShaderModule chatModule = device.CreateShaderModule(&chatModuleDescriptor);

        wgpu::BindGroupLayoutEntry chatLayoutEntry{};
        chatLayoutEntry.binding = 0;
        chatLayoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        chatLayoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;

        wgpu::BindGroupLayoutDescriptor chatLayoutDescriptor{.entryCount = 1, .entries = &chatLayoutEntry};
        wgpu::BindGroupLayout chatBindGroupLayout = device.CreateBindGroupLayout(&chatLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor chatPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                    .bindGroupLayouts = &chatBindGroupLayout};
        wgpu::PipelineLayout chatPipelineLayout = device.CreatePipelineLayout(&chatPipelineLayoutDescriptor);

        wgpu::BlendState chatBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState chatTarget{.format = surfaceFormat, .blend = &chatBlend};
        wgpu::FragmentState chatFragment{
            .module = chatModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &chatTarget};

        wgpu::DepthStencilState chatDepth{.format = kDepthFormat,
                                          .depthWriteEnabled = false,
                                          .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor chatPipelineDescriptor{
            .layout = chatPipelineLayout,
            .vertex = {.module = chatModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &chatDepth,
            .fragment = &chatFragment};
        chatPipeline = device.CreateRenderPipeline(&chatPipelineDescriptor);

        wgpu::BindGroupEntry chatEntry{};
        chatEntry.binding = 0;
        chatEntry.buffer = chatUniformBuffer;
        chatEntry.size = sizeof(ChatUniforms);
        wgpu::BindGroupDescriptor chatBindGroupDescriptor{
            .layout = chatBindGroupLayout, .entryCount = 1, .entries = &chatEntry};
        chatBindGroup = device.CreateBindGroup(&chatBindGroupDescriptor);
    }

    // The death box. Same atlas, same one-triangle-and-discard, and the same
    // three bindings the HUD needs - a uniform block, the glyphs and a
    // sampler for them.
    wgpu::Buffer dialogUniformBuffer;
    wgpu::RenderPipeline dialogPipeline;
    wgpu::BindGroup dialogBindGroup;
    if (fontTexture)
    {
        wgpu::BufferDescriptor dialogBufferDescriptor{
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(DialogUniforms)};
        dialogUniformBuffer = device.CreateBuffer(&dialogBufferDescriptor);

        wgpu::ShaderSourceWGSL dialogWgsl;
        dialogWgsl.code = mh::kDialogShader;
        wgpu::ShaderModuleDescriptor dialogModuleDescriptor{.nextInChain = &dialogWgsl};
        wgpu::ShaderModule dialogModule = device.CreateShaderModule(&dialogModuleDescriptor);

        wgpu::BindGroupLayoutEntry dialogLayoutEntries[3] = {};
        dialogLayoutEntries[0].binding = 0;
        dialogLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        dialogLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        dialogLayoutEntries[1].binding = 1;
        dialogLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        dialogLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        dialogLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        dialogLayoutEntries[2].binding = 2;
        dialogLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        dialogLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor dialogLayoutDescriptor{.entryCount = 3, .entries = dialogLayoutEntries};
        wgpu::BindGroupLayout dialogBindGroupLayout = device.CreateBindGroupLayout(&dialogLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor dialogPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                      .bindGroupLayouts = &dialogBindGroupLayout};
        wgpu::PipelineLayout dialogPipelineLayout = device.CreatePipelineLayout(&dialogPipelineLayoutDescriptor);

        wgpu::BlendState dialogBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState dialogTarget{.format = surfaceFormat, .blend = &dialogBlend};
        wgpu::FragmentState dialogFragment{
            .module = dialogModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &dialogTarget};

        // Over everything, including the water and the names. A modal box that
        // a tree can stand in front of is not modal.
        wgpu::DepthStencilState dialogDepth{.format = kDepthFormat,
                                            .depthWriteEnabled = wgpu::OptionalBool::False,
                                            .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor dialogPipelineDescriptor{
            .layout = dialogPipelineLayout,
            .vertex = {.module = dialogModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &dialogDepth,
            .fragment = &dialogFragment};
        dialogPipeline = device.CreateRenderPipeline(&dialogPipelineDescriptor);

        wgpu::BindGroupEntry dialogEntries[3] = {};
        dialogEntries[0].binding = 0;
        dialogEntries[0].buffer = dialogUniformBuffer;
        dialogEntries[0].size = sizeof(DialogUniforms);
        dialogEntries[1].binding = 1;
        dialogEntries[1].textureView = fontTexture.CreateView();
        dialogEntries[2].binding = 2;
        dialogEntries[2].sampler = fontSampler;
        wgpu::BindGroupDescriptor dialogBindGroupDescriptor{
            .layout = dialogBindGroupLayout, .entryCount = 3, .entries = dialogEntries};
        dialogBindGroup = device.CreateBindGroup(&dialogBindGroupDescriptor);
    }

    // Also a lambda, and for the same reason: the radar is built from the map
    // baked out of the zone and from its collision, neither of which exists
    // when the window opens onto a sign-in screen. Run once here for the
    // standalone viewer, and again whenever a zone arrives.
    const auto setUpRadar = [&]()
    {
        if (!mapTexture || collision.empty())
        {
            return;
        }

        constexpr uint32_t kMaskSize = 1024;
        const mh::Vec3 middle = zone->centre();
        const float half = std::max(zone->boundsMax.x - zone->boundsMin.x, zone->boundsMax.z - zone->boundsMin.z) * 0.5f;
        mapCentreX = middle.x;
        mapCentreZ = middle.z;
        mapHalf = half;

        const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMaskSize, middle, half);

        wgpu::TextureDescriptor maskDescriptor{.usage = wgpu::TextureUsage::TextureBinding |
                                                        wgpu::TextureUsage::CopyDst,
                                               .dimension = wgpu::TextureDimension::e2D,
                                               .size = {kMaskSize, kMaskSize, 1},
                                               .format = wgpu::TextureFormat::R8Unorm,
                                               .mipLevelCount = 1,
                                               .sampleCount = 1};
        maskTexture = device.CreateTexture(&maskDescriptor);

        wgpu::TexelCopyTextureInfo maskDestination{.texture = maskTexture};
        wgpu::TexelCopyBufferLayout maskLayout{.bytesPerRow = kMaskSize, .rowsPerImage = kMaskSize};
        const wgpu::Extent3D maskExtent{kMaskSize, kMaskSize, 1};
        queue.WriteTexture(&maskDestination, mask.data(), mask.size(), &maskLayout, &maskExtent);

        wgpu::BufferDescriptor radarBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                     .size = sizeof(RadarUniforms)};
        radarUniformBuffer = device.CreateBuffer(&radarBufferDescriptor);

        wgpu::ShaderSourceWGSL radarWgsl;
        radarWgsl.code = mh::kRadarShader;
        wgpu::ShaderModuleDescriptor radarModuleDescriptor{.nextInChain = &radarWgsl};
        wgpu::ShaderModule radarModule = device.CreateShaderModule(&radarModuleDescriptor);

        wgpu::BindGroupLayoutEntry radarLayoutEntries[4] = {};
        radarLayoutEntries[0].binding = 0;
        radarLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        radarLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        radarLayoutEntries[1].binding = 1;
        radarLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        radarLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        radarLayoutEntries[2].binding = 2;
        radarLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[2].texture.sampleType = wgpu::TextureSampleType::Float;
        radarLayoutEntries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        radarLayoutEntries[3].binding = 3;
        radarLayoutEntries[3].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[3].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor radarLayoutDescriptor{.entryCount = 4, .entries = radarLayoutEntries};
        wgpu::BindGroupLayout radarBindGroupLayout = device.CreateBindGroupLayout(&radarLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor radarPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                     .bindGroupLayouts = &radarBindGroupLayout};
        wgpu::PipelineLayout radarPipelineLayout = device.CreatePipelineLayout(&radarPipelineLayoutDescriptor);

        wgpu::BlendState radarBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState radarTarget{.format = surfaceFormat, .blend = &radarBlend};
        wgpu::FragmentState radarFragment{
            .module = radarModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &radarTarget};

        // Drawn over everything, so the depth test always passes and nothing is
        // written back.
        wgpu::DepthStencilState radarDepth{.format = kDepthFormat,
                                           .depthWriteEnabled = false,
                                           .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor radarPipelineDescriptor{
            .layout = radarPipelineLayout,
            .vertex = {.module = radarModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &radarDepth,
            .fragment = &radarFragment};
        radarPipeline = device.CreateRenderPipeline(&radarPipelineDescriptor);

        wgpu::BindGroupEntry radarEntries[4] = {};
        radarEntries[0].binding = 0;
        radarEntries[0].buffer = radarUniformBuffer;
        radarEntries[0].size = sizeof(RadarUniforms);
        radarEntries[1].binding = 1;
        radarEntries[1].textureView = mapTexture.CreateView();
        radarEntries[2].binding = 2;
        radarEntries[2].textureView = maskTexture.CreateView();
        radarEntries[3].binding = 3;
        radarEntries[3].sampler = sampler;

        wgpu::BindGroupDescriptor radarBindGroupDescriptor{
            .layout = radarBindGroupLayout, .entryCount = 4, .entries = radarEntries};
        radarBindGroup = device.CreateBindGroup(&radarBindGroupDescriptor);

        std::printf("radar: ready, %zu entities to show\n", options.testEntities.size());
    };
    setUpRadar();

    // `characterPath` is a semicolon-separated list of DATs to assemble
    // one character from, and MOGHOUSE_CHARACTER_AT is where to stand it.
    std::optional<LoadedCharacter> character;
    mh::Vec3 characterAt{};

    // `look` is what a player character actually is:
    // race,face,head,body,hands,legs,feet, all model ids. The skeleton comes
    // from the race and each slot from its own file, which is how a change of
    // outfit is one number rather than a different character.
    // A named lambda rather than a block, because this now runs twice: once
    // here for whatever the options carried, and again when the client says who
    // the player turned out to be. The window opens before the sign-in screen
    // does, so at this point the answer is usually a placeholder.
    auto buildFromLook = [&textures](const char* lookText) -> std::optional<LoadedCharacter>
    {
        ffxi::Look look;
        if (!ffxi::parseLook(lookText, look))
        {
            std::printf("MOGHOUSE_LOOK wants race,face,head,body,hands,legs,feet\n");
            return std::nullopt;
        }

        try
        {
            const ffxi::FileTable table{ffxi::defaultInstallRoot()};
            std::vector<std::string> paths;

            // The skeleton file first: it carries the bones every piece is
            // hung on, and nothing but the race decides it.
            if (auto skeleton = table.path(ffxi::skeletonFileId(look.race)))
            {
                paths.push_back(skeleton->string());
            }

            // Then the movement motions. Only the first of the four: the
            // rest repeat the same clip names for other weapon stances,
            // and loading them all would just overwrite these.
            const std::vector<size_t> motions = ffxi::motionFileIds(look.race);
            if (!motions.empty())
            {
                if (auto motion = table.path(motions.front()))
                {
                    paths.push_back(motion->string());
                }
            }
            std::printf("look: %s", ffxi::raceName(look.race));
            for (size_t i = 0; i < static_cast<size_t>(ffxi::LookSlot::Count); ++i)
            {
                const auto slot = static_cast<ffxi::LookSlot>(i);
                const size_t fileId = ffxi::modelFileId(look.race, slot, look.model[i]);
                auto path = fileId ? table.path(fileId) : std::nullopt;
                std::printf(", %s %u%s", ffxi::slotName(slot), look.model[i], path ? "" : " (missing)");
                if (path)
                {
                    paths.push_back(path->string());
                }
            }
            std::printf("\n");

            std::optional<LoadedCharacter> built = loadCharacter(paths, textures);

            // What this body can be made to do. The clips are named rather
            // than numbered - idl0, wlk0, run0 - and there is no index of them
            // anywhere, so the way to find out whether a race can sit down is
            // to ask it.
            if (built && SDL_getenv("MOGHOUSE_CLIPS") != nullptr)
            {
                std::vector<std::string> named;
                named.reserve(built->animations.size());
                for (const auto& [clip, _] : built->animations)
                {
                    named.push_back(clip);
                }
                std::sort(named.begin(), named.end());

                std::printf("clips (%zu):", named.size());
                for (size_t i = 0; i < named.size(); ++i)
                {
                    std::printf("%s%s", i % 12 == 0 ? "\n  " : " ", named[i].c_str());
                }
                std::printf("\n");
            }

            return built;
        }
        catch (const std::exception& e)
        {
            std::printf("could not read the file table: %s\n", e.what());
            return std::nullopt;
        }
    };

    // What the character was built from, kept so it can be built again: a
    // zone change empties the texture cache, and a body that is not rebuilt
    // afterwards stands in the new zone white.
    std::string currentLook;

    // How much the body is scaled, from the look's size. The mesh is the same
    // file whatever the size; only the instance transform differs.
    float characterScale = 1.0f;
    const auto scaleOfLook = [](const std::string& text) {
        ffxi::Look parsed;
        return ffxi::parseLook(text, parsed) ? mh::bodyScale(parsed.size + 1) : 1.0f;
    };

    if (const char* lookEnv = options.look ? options.look->c_str() : nullptr)
    {
        character = buildFromLook(lookEnv);
        currentLook = lookEnv;
        characterScale = scaleOfLook(currentLook);
    }
    else if (const char* charEnv = options.characterPath ? options.characterPath->c_str() : nullptr)
    {
        std::vector<std::string> paths;
        std::string current;
        for (const char* c = charEnv; *c; ++c)
        {
            if (*c == ';')
            {
                if (!current.empty())
                {
                    paths.push_back(current);
                }
                current.clear();
            }
            else
            {
                current.push_back(*c);
            }
        }
        if (!current.empty())
        {
            paths.push_back(current);
        }
        character = loadCharacter(paths, textures);
    }

    // The character reuses the zone's pipelines and bind group layout, so it
    // needs a zone loaded. Its geometry is already in world orientation, so
    // its one instance is a plain translation rather than a placement matrix.
    wgpu::Buffer characterVertexBuffer;
    // The same geometry frozen in the bind pose, for everyone who is not the
    // player. They shared the player's animated buffer at first, so every NPC
    // in the zone walked in step with whoever was driving - which looks less
    // like a bug than like the whole city being puppeted, but is one.
    //
    // Their own animations need per-entity skinning and a buffer each. Standing
    // still is the honest placeholder until then.
    wgpu::Buffer entityVertexBuffer;
    wgpu::Buffer characterIndexBuffer;
    wgpu::Buffer characterInstanceBuffer;
    std::vector<wgpu::Texture> characterTextures;
    std::vector<wgpu::BindGroup> characterBindGroups;
    // A lambda rather than a block, because the client now opens its window
    // before it has either a zone or a character: this has to run again when
    // they arrive. Left as a one-off, a character signed in through the
    // in-window screens had its model built and never uploaded, so it drew as
    // nothing at all while its nameplate hung in the air above it.
    // Slot 0 is the player. The rest are the entities, one instance each.
    //
    // Made whether or not there is a player to fill slot 0, because everyone
    // else's transforms live here too - and at character select there is a row
    // of people to draw and nobody playing yet. Tied to the player's own upload,
    // the roster stood there as a line of floating names with no bodies under
    // them.
    {
        float instance[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        wgpu::BufferDescriptor instanceDescriptor{.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                                                  .size = sizeof(instance) * (mh::kMaxDrawnBodies + 1)};
        characterInstanceBuffer = device.CreateBuffer(&instanceDescriptor);
        queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));
    }

    // The pale surface the blank figure wears. Made once, from nothing the zone
    // provides, so it survives a zone change like the white fallback does.
    const auto makeGhost = [&]()
    {
        if (ghostBindGroup || !zoneBindGroupLayout || !uniformBuffer || !sampler)
        {
            return;
        }

        ghostTexture = mh::createSolidTexture(device, 225, 232, 245, 255);

        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuffer;
        entries[0].size = sizeof(Uniforms);
        entries[1].binding = 1;
        entries[1].textureView = ghostTexture.CreateView();
        entries[2].binding = 2;
        entries[2].sampler = sampler;

        wgpu::BindGroupDescriptor descriptor{
            .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
        ghostBindGroup = device.CreateBindGroup(&descriptor);
    };
    makeGhost();

    const auto uploadCharacter = [&]()
    {
        if (!character || character->geometry.indices.empty() || !pipeline)
        {
            return;
        }

        // Cleared first: these are appended to, and the draw loop indexes them
        // by batch, so a second call would leave it reading the previous
        // character's bind groups.
        characterTextures.clear();
        characterBindGroups.clear();

        characterVertexBuffer = createBuffer(device, character->geometry.vertices.data(),
                                             character->geometry.vertices.size() * sizeof(mh::Vertex),
                                             wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        characterIndexBuffer = createBuffer(device, character->geometry.indices.data(),
                                            character->geometry.indices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);

        // Taken before the animation loop touches the geometry, so this is the
        // rest pose rather than whatever frame happened to be current.
        mh::reskin(character->geometry, mh::bindPose(character->skeleton), character->meshes);

        // Once, and never again. This is the body borrowed by every entity
        // nothing could be built for, and it used to be rebuilt here on every
        // character build - which includes every time the player changes gear.
        // So the pale figure standing in for an NPC was not merely player
        // shaped, it was wearing what the player was wearing and changed as
        // they changed: "a ghost of whatever character I'm on".
        //
        // Frozen at the first build it is still a body rather than a hole, and
        // it stops being a portrait of the person looking at it. The right
        // answer is a neutral figure of its own, which needs a model built
        // from a fixed look and cannot be reached from here.
        if (!entityVertexBuffer)
        {
            entityVertexBuffer = createBuffer(device, character->geometry.vertices.data(),
                                              character->geometry.vertices.size() * sizeof(mh::Vertex),
                                              wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        }

        for (const mh::Batch& batch : character->geometry.batches)
        {
            wgpu::TextureView view = whiteTexture.CreateView();
            auto found = textures.find(batch.texture);
            if (found != textures.end())
            {
                if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                {
                    characterTextures.push_back(gpu);
                    view = characterTextures.back().CreateView();
                }
            }
            else
            {
                std::printf("character texture %s is in none of those files\n", batch.texture.c_str());
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            characterBindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }
    };
    uploadCharacter();
    if (character && !pipeline)
    {
        // Not fatal any more. It used to be: a character could only ever be
        // drawn with the zone that was loaded at startup, so no zone meant no
        // character for the rest of the run. Now a zone can arrive later and
        // uploadCharacter runs again with it.
        std::printf("a character needs a zone loaded: it draws with the zone pipelines\n");
    }

    const mh::Vec3 centre = zone ? zone->centre() : mh::Vec3{};

    // How big the world is, which decides the far plane and how wide the light
    // is projected. Not const, because it belongs to the zone rather than to
    // the session: a client that opens its window before it has a zone starts
    // with no bounds at all, and one that walks from a small zone to a large
    // one would otherwise keep the small one's.
    //
    // Left fixed at the 1.0 fallback, the far plane sits four units from the
    // eye and everything further away is clipped - the world renders as empty
    // fog with the interface still drawn over it, which looks like a zone that
    // failed to load rather than one being thrown away by the projection.
    float radius = zone ? std::max(zone->radius(), 1.0f) : 1.0f;

    // The railway through Sel Phiner, if this zone has one. Nothing else does.
    mh::Monorail monorail;

    /// What this viewer has said to itself. The client keeps its chat on the
    /// link; the standalone viewer has no link and still has things to say.
    std::vector<std::string> viewerChat = options.testChat;

    /// Whether the character is aboard, as this viewer knows it. The client
    /// says so through the link; the standalone viewer has no link and says so
    /// with the T key, so both have to be asked.
    bool ridingHere = false;

    /// Whether a character-select line-up was up last frame, so the train can
    /// be started the moment one appears.
    bool lineupWas = false;

    /// Which of the zone's draws are the train, so they can be drawn again to
    /// light them. Indices into zone->draws.
    std::vector<size_t> monorailDraws;

    const auto findMonorail = [&]()
    {
        monorailDraws.clear();
        if (!zone || !monorail.find(*zone))
        {
            return;
        }

        for (const char* model : {"mono_a1", "mono_b1"})
        {
            auto range = zone->instanceRanges.find(model);
            if (range == zone->instanceRanges.end())
            {
                continue;
            }
            const uint32_t first = range->second.first;
            const uint32_t last = first + range->second.second;
            for (size_t i = 0; i < zone->draws.size(); ++i)
            {
                if (zone->draws[i].instanceOffset >= first && zone->draws[i].instanceOffset < last)
                {
                    monorailDraws.push_back(i);
                }
            }
        }

        // Where it calls, as distances along the line.
        //
        // Nowhere, by default: it runs end to end and a rider steps off where
        // they like. Stops at 127, 262, 468 and 1050 were tried - beside a
        // signpost, a bridge and a walled mill - and they felt forced. Waiting
        // out a halt you did not ask for is worse than the walk you saved, on a
        // line you can leave at any point along it. The ends remain stops
        // because the train has to turn round somewhere.
        std::vector<float> stops;
        if (const char* wanted = SDL_getenv("MOGHOUSE_TRAIN_STOPS"))
        {
            stops.clear();
            for (const char* at = wanted; *at;)
            {
                char* end = nullptr;
                const float value = std::strtof(at, &end);
                if (end == at)
                {
                    break;
                }
                stops.push_back(value);
                at = (*end == ',') ? end + 1 : end;
            }
        }
        monorail.setStops(std::move(stops));
        monorail.departNow();

        std::printf("monorail: %zu cars on %.0f units of track, %zu stops, %zu draws to light\n",
                    monorail.cars().size(), static_cast<double>(monorail.routeLength()),
                    monorail.stops(), monorailDraws.size());
    };
    findMonorail();

    // Where a character-select line-up stands, and which way it faces.
    //
    // Near the middle of the zone, on whatever floor is under it - a spot
    // rather than a frame's worth of searching, because it belongs to the zone
    // and not to the moment. The yaw is arbitrary; what matters is that the row
    // is laid across it and the camera looks along it.
    mh::Vec3 lineupAt = centre;
    float lineupYaw = 0.0f;
    const auto findLineupSpot = [&]()
    {
        // Beside the railway, when the zone has one.
        //
        // The line-up stands between the camera and the track, far enough along
        // that the train reaches it within a few seconds of setting off - so
        // whoever is choosing a character has it run past behind them rather
        // than somewhere off in the distance they never look. Everything is
        // measured off the route rather than written down, because the route is
        // worked out at load and moving one rail beam would otherwise leave the
        // row standing in a field.
        mh::Vec3 on{};
        float along = 0.0f;
        if (monorail.present() && monorail.at(kLineupAlongTrack, on, along))
        {
            // Square to the track, on the side the camera will be.
            const float acrossX = std::cos(along);
            const float acrossZ = -std::sin(along);

            lineupAt = mh::Vec3{on.x + acrossX * kLineupFromTrack, on.y,
                                on.z + acrossZ * kLineupFromTrack};

            // Facing the track, so the camera - which sits behind the row -
            // looks past them at it.
            lineupYaw = std::atan2(-acrossX, -acrossZ);
        }
        else
        {
            lineupAt = centre;
            lineupYaw = 0.0f;
        }

        if (auto ground = collision.nearestGround(lineupAt.x, lineupAt.z, lineupAt.y + 40.0f, radius))
        {
            lineupAt.y = ground->y;
        }
    };
    findLineupSpot();

    // MOGHOUSE_BEACH_PROBE="x,z0,z1" walks the ground along z at a fixed x and
    // says where it crosses a waterline, which is the only way to find out
    // where a zone's shore actually is. The generator-placed sea panels are
    // decorative and need not end where the terrain does - and if they do not,
    // foam laid along the panel's edge sits out in open water.
    if (const char* probe = std::getenv("MOGHOUSE_BEACH_PROBE"))
    {
        float px = 0.0f, z0 = 0.0f, z1 = 0.0f;
        if (std::sscanf(probe, "%f,%f,%f", &px, &z0, &z1) == 3)
        {
            std::printf("beach probe at x %.1f, z %.0f..%.0f\n", px, z0, z1);
            for (float z = z0; z <= z1; z += 2.0f)
            {
                if (auto ground = collision.nearestGround(px, z, 60.0f, 400.0f))
                {
                    std::printf("   z %7.1f  ground y %7.2f\n", z, ground->y);
                }
                else
                {
                    std::printf("   z %7.1f  ground y     ---\n", z);
                }
            }
        }
    }


    mh::Camera camera;
    camera.target = centre;
    camera.distance = radius * 2.4f;
    // Start at the middle of the zone in all three axes. Starting from the
    // bottom of the bounds put the camera 25 units under the terrain in
    // Sarutabaruta, looking at the underside of the world - which reads as the
    // zone being mostly missing rather than as being in the wrong place.
    camera.position = centre;
    camera.pitch = 0.0f;

    // Somewhere visible by default, since nothing yet knows where the ground
    // is. MOGHOUSE_CHARACTER_AT overrides it, and c drops the character at
    // wherever the camera is standing.
    characterAt = centre;
    if (const char* atEnv = options.characterAt ? options.characterAt->c_str() : nullptr)
    {
        std::sscanf(atEnv, "%f,%f,%f", &characterAt.x, &characterAt.y, &characterAt.z);
    }
    // The model faces +x in its own space, so a heading of zero looks east.
    float characterFacing = 0.0f;
    if (const char* facingEnv = options.characterFacing ? options.characterFacing->c_str() : nullptr)
    {
        characterFacing = static_cast<float>(std::atof(facingEnv)) * 3.14159265f / 180.0f;
    }
    // `search` is how far to look for ground when there is none directly
    // below. Dropping a character into a zone wants a wide sweep; a footstep
    // wants none at all. Sharing one value is what let a step off a ledge
    // teleport into the water sixty units away, which is not falling and is
    // not blocking either.
    // Declared up here because writeCharacterInstance below captures them.
    // The bodies are written in the same place as the player's own instance,
    // which is the rule that file already had: nothing else is allowed to
    // produce instance data.
    std::vector<mh::RadarEntity> radarEntities = options.testEntities;
    float radarRange = 120.0f;

    /// How many entity bodies the instance buffer currently holds.
    int drawnBodies = 0;

    /// How far away a body is still worth drawing, in world units.
    ///
    /// The cap on bodies is a ceiling on buffers; this is the knob a player
    /// actually wants. Zero means no limit beyond that ceiling - draw
    /// everyone, which is what a machine with room to spare should do. A
    /// smaller number is how a slower one keeps up.
    ///
    /// Read once: it is a preference, not something that changes mid-zone.
    const float bodyDistance = [] {
        if (const char* set = std::getenv("MOGHOUSE_BODY_DISTANCE"))
        {
            const float given = static_cast<float>(std::atof(set));
            if (given > 0.0f)
            {
                return given;
            }
        }
        return 0.0f;
    }();

    /// How many of the sorted entities are close enough to draw. The list is
    /// nearest first, so those are always a prefix of it and every index the
    /// draw loops use stays meaningful.
    int bodiesInRange = mh::kMaxDrawnBodies;

    /// Where each entity is being drawn, as opposed to where it was last
    /// reported.
    ///
    /// The server sends a position a few times a second and the tracker holds
    /// the last one, so an entity moved in steps: still for four frames, a
    /// jump on the fifth. At sixty frames a second that reads as a tape being
    /// shuttled rather than as somebody walking. Easing towards the reported
    /// position spreads each step over the frames between.
    struct DrawnEntity
    {
        mh::Vec3 at{};        // where it is drawn this frame
        mh::Vec3 from{};      // where it was drawn when the latest target arrived
        mh::Vec3 target{};    // the latest position the server gave
        float targetTime{};   // when that arrived, on the frame clock
        float interval{0.5f}; // how long the previous target took to be replaced
    };
    std::map<uint32_t, DrawnEntity> drawnAt;
    float lastFrameSeconds = 0.0f;

    /// The Vana'diel clock in seconds, when the server has supplied one. Also
    /// gives the weekday, which is the same eight day cycle the game shows.
    uint64_t vanaSeconds = 0;

    /// Where the character has been, sampled about twice a second.
    ///
    /// Collision can still trap someone - a sign post is thin enough that the
    /// step, the slide and both single-axis escapes can all be blocked at
    /// once - and when it does there is nothing to be done from inside the
    /// window. Backing up along a path already known to be walkable is the
    /// cheap way out, and it needs no cleverness about which way is clear.
    std::deque<mh::Vec3> breadcrumbs;
    float breadcrumbTimer = 0.0f;

    /// Collision off: walk through walls and floors, and do not fall.
    ///
    /// The same thing a private server's !wallhack does. Getting somewhere to
    /// look at it should not depend on the collision being right, which is
    /// awkward when the collision is what is being checked.
    bool noclip = false;

    // Typing a line. While this is open the movement keys are letters again,
    // which is the whole reason it is a mode rather than always-on capture.
    bool typing = false;

    /// The keystroke that opened the chat line, which the text-input event for
    /// that same press would otherwise add a second time.
    std::string swallowText;
    std::string typed;

    // Where the character is drawn. Everything that moves them has to call
    // this - the position and the heading only reach the GPU through here, so
    // anything that updates characterAt and forgets leaves the character
    // standing still while the camera follows the place they should be.
    // One built model per distinct look. NPCs repeat heavily - a row of
    // shopkeepers in the same uniform is one model and five instances - so
    // this is keyed by the look rather than by the entity.
    struct DrawableCharacter
    {
        LoadedCharacter loaded;
        wgpu::Buffer vertices;
        wgpu::Buffer indices;
        std::vector<wgpu::Texture> textures;
        std::vector<wgpu::BindGroup> bindGroups;
    };

    std::map<std::array<uint16_t, 7>, std::optional<DrawableCharacter>> npcModels;

    /// Creatures, in a map of their own.
    ///
    /// Not folded into npcModels under a tagged key: a creature is one model
    /// id and a look is seven fields, and a tag telling the two apart inside
    /// one key was the kind of cleverness that collided once already. Two
    /// maps cannot.
    std::map<uint16_t, std::optional<DrawableCharacter>> creatureModels;

    /// One entity's animation, and the vertices it is skinned into.
    ///
    /// The model cache is keyed by look, so a row of identical NPCs shares a
    /// single set of vertices. That is fine while they all stand in the same
    /// pose and useless the moment each needs its own, so every entity keeps
    /// its own copy of the geometry and its own buffer to draw from. Without
    /// it an NPC is a statue being slid around the zone: the server moves it,
    /// nothing bends, and it arrives without ever having taken a step.
    struct AnimatedEntity
    {
        mh::Character geometry;
        wgpu::Buffer vertices;
        const ffxi::Animation* clip = nullptr;
        float clipStart = 0.0f;
        float lastX = 0.0f;
        float lastZ = 0.0f;
        /// The top of the mesh in this frame's pose.
        ///
        /// The model's own bounds are the rest pose and are computed once, so
        /// a clip that lifts the whole body - which is what a bird's flight is
        /// - leaves them describing something on the ground. The nameplate
        /// then sits under a hovering crow instead of over it.
        float posedTop = 0.0f;
        float lastMoveTime = 0.0f;
        float movingUntil = 0.0f;
        float speed = 0.0f;
        bool placed = false;
        bool drawn = false;
        bool clipsReported = false;
    };

    std::map<uint32_t, AnimatedEntity> entityPoses;

    const auto lookKey = [](const uint16_t look[7]) {
        // Race, face and the five equipment slots. The face used to be left
        // out on the theory that it only changed the head's texture; it does
        // not - it picks the head DAT, hair and all, so two faces are two
        // meshes. Leaving it out meant every face in a crowd wore the first
        // one built, and the character being made kept the same head however
        // the face was chosen. An array rather than a packed integer because
        // seven fields no longer fit in sixty-four bits.
        std::array<uint16_t, 7> key{};
        key[0] = look[0];
        key[1] = look[1];
        for (int slot = 2; slot < 7; ++slot)
        {
            key[static_cast<size_t>(slot)] = look[slot] & 0x0FFFu;
        }
        return key;
    };

    // Builds the GPU side of a character. The player's own was built inline
    // above before there was ever more than one; this is the same steps.
    const auto buildDrawable = [&](LoadedCharacter&& loaded) -> std::optional<DrawableCharacter> {
        if (loaded.geometry.indices.empty() || !pipeline)
        {
            return std::nullopt;
        }

        DrawableCharacter drawable;
        drawable.loaded = std::move(loaded);

        // Entities stand in the rest pose. They are not animated individually
        // yet, so there is no per-frame skinning to pay for.
        mh::reskin(drawable.loaded.geometry, mh::bindPose(drawable.loaded.skeleton), drawable.loaded.meshes);

        drawable.vertices = createBuffer(device, drawable.loaded.geometry.vertices.data(),
                                         drawable.loaded.geometry.vertices.size() * sizeof(mh::Vertex),
                                         wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        drawable.indices = createBuffer(device, drawable.loaded.geometry.indices.data(),
                                        drawable.loaded.geometry.indices.size() * sizeof(uint32_t),
                                        wgpu::BufferUsage::Index);

        for (const mh::Batch& batch : drawable.loaded.geometry.batches)
        {
            wgpu::TextureView view = whiteTexture.CreateView();
            auto found = textures.find(batch.texture);
            if (found != textures.end())
            {
                if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                {
                    drawable.textures.push_back(gpu);
                    view = drawable.textures.back().CreateView();
                }
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            drawable.bindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }

        return drawable;
    };

    // The model for one look, building it the first time it is asked for.
    // Returns null while it has none - a look that resolves to nothing is
    // remembered as nothing rather than retried every frame.
    const auto modelFor = [&](const uint16_t look[7]) -> const DrawableCharacter* {
        const std::array<uint16_t, 7> key = lookKey(look);
        auto found = npcModels.find(key);
        if (found != npcModels.end())
        {
            return found->second ? &*found->second : nullptr;
        }

        ffxi::Look wanted;
        if (mh::isChildRace(look[0]))
        {
            // A child's face and gear ids index tables this cannot reach, so
            // they are not used: the grown race, its first face, and the same
            // stand-in clothes the roster puts on the player's own characters.
            wanted.race = static_cast<ffxi::Race>(mh::adultRaceFor(look[0]));
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Face)] = look[1] & 0x0F;
            for (size_t slot = 1; slot < static_cast<size_t>(ffxi::LookSlot::Count); ++slot)
            {
                wanted.model[slot] = 1;
            }
        }
        else
        {
            wanted.race = static_cast<ffxi::Race>(look[0]);
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Face)] = look[1];
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Head)] = look[2] & 0x0FFF;
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Body)] = look[3] & 0x0FFF;
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Hands)] = look[4] & 0x0FFF;
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Legs)] = look[5] & 0x0FFF;
            wanted.model[static_cast<size_t>(ffxi::LookSlot::Feet)] = look[6] & 0x0FFF;
        }

        std::optional<DrawableCharacter> built;
        try
        {
            const ffxi::FileTable table{ffxi::defaultInstallRoot()};
            std::vector<std::string> paths;
            if (auto skeletonPath = table.path(ffxi::skeletonFileId(wanted.race)))
            {
                paths.push_back(skeletonPath->string());
            }
            for (const std::filesystem::path& piece : ffxi::lookFiles(table, wanted))
            {
                paths.push_back(piece.string());
            }

            if (!paths.empty())
            {
                if (auto loaded = loadCharacter(paths, textures))
                {
                    built = buildDrawable(std::move(*loaded));
                }
            }
        }
        catch (const std::exception& e)
        {
            std::printf("could not build NPC model: %s\n", e.what());
        }

        std::printf("NPC model %s: race %u face %u %u/%u/%u/%u/%u\n", built ? "built" : "failed", look[0],
                    look[1], look[2], look[3], look[4], look[5], look[6]);
        auto inserted = npcModels.emplace(key, std::move(built)).first;
        return inserted->second ? &*inserted->second : nullptr;
    };

    /// A creature's model, cached by the id the server sent.
    ///
    /// Nothing to assemble: one file holds the skeleton, the mesh and the
    /// animations, and the clips are named the way a player's are, so the same
    /// idl0/wlk0/run0 the rest of the renderer looks for is a rabbit sitting,
    /// hopping and running without any special case.
    const auto creatureFor = [&](uint16_t modelId) -> const DrawableCharacter* {
        auto found = creatureModels.find(modelId);
        if (found != creatureModels.end())
        {
            return found->second ? &*found->second : nullptr;
        }

        std::optional<DrawableCharacter> built;
        try
        {
            const ffxi::FileTable table{ffxi::defaultInstallRoot()};
            if (auto path = table.path(mh::creatureFileId(modelId)))
            {
                if (auto loaded = loadCharacter({path->string()}, textures))
                {
                    built = buildDrawable(std::move(*loaded));
                }
            }
        }
        catch (const std::exception& e)
        {
            std::printf("could not build creature %u: %s\n", modelId, e.what());
        }

        std::printf("creature model %s: id %u -> file %zu\n", built ? "built" : "failed", modelId,
                    mh::creatureFileId(modelId));
        auto inserted = creatureModels.emplace(modelId, std::move(built)).first;
        return inserted->second ? &*inserted->second : nullptr;
    };

    /// Whichever way this entity is described.
    const auto modelForEntity = [&](const mh::RadarEntity& entity) -> const DrawableCharacter* {
        if (entity.hasModel())
        {
            return creatureFor(entity.modelId);
        }
        return entity.hasLook() ? modelFor(entity.look) : nullptr;
    };

    /// Kept here as well as in the device so the keys have something to step,
    /// and declared this early because the ambience below scales off it - it
    /// used to sit further down, out of reach of the lambda that plays sound,
    /// which is why the volume keys moved the music and left the ambience
    /// where it was.
    ///
    /// Starts low: music you have to turn down is worse than music you have to
    /// turn up, and this one starts the moment you log in.
    float musicVolume = 0.5f;
    if (const char* volume = std::getenv("MOGHOUSE_MUSIC_VOLUME"))
    {
        musicVolume = std::clamp(static_cast<float>(std::atof(volume)), 0.0f, 1.0f);
    }

    /// And a separate one for everything that is not music, the way retail has
    /// a Sound slider beside its Music slider. Effects play at this; ambience
    /// plays at a fraction of it, since it is meant to sit under everything
    /// else rather than beside it.
    float soundVolume = 0.5f;
    if (const char* volume = std::getenv("MOGHOUSE_SOUND_VOLUME"))
    {
        soundVolume = std::clamp(static_cast<float>(std::atof(volume)), 0.0f, 1.0f);
    }

    /// Who was last spoken to, and how far round they have turned to answer.
    ///
    /// An NPC that stares at a wall while talking to you reads as broken even
    /// when every word is right, and the server never says to turn - retail's
    /// client does this itself.
    uint32_t facingMe = 0;
    float facingMeAngle = 0.0f;
    bool facingMeStarted = false;

    /// Whether the options menu is up, and where its button sits so a click can
    /// find it. The rect is written while drawing and read by the events of the
    /// next frame, which is a frame behind and does not matter for a button
    /// that does not move.
    /// MOGHOUSE_OPTIONS=1 starts with it open, which is how it is looked at
    /// without a keyboard - a screenshot run presses nothing.
    bool optionsOpen = std::getenv("MOGHOUSE_OPTIONS") != nullptr;

    // The bags. Closed until asked for - there is nothing to show until the
    // server has sent them, which it does unprompted on zoning in.
    //
    // A tab is a container that has any slots at all, and a page is twenty of
    // that container's slots. Both are indices into what the server said, not
    // into a fixed list: an inventory is thirty slots on one server and eighty
    // on another, and a wardrobe that has not been bought has none.
    bool inventoryOpen = std::getenv("MOGHOUSE_INVENTORY") != nullptr;
    int inventoryTab = 0;
    int inventoryPage = 0;

    /// Something in the panel that can be clicked, in NDC.
    ///
    /// Filled while drawing and read when the mouse comes up, the same way the
    /// menu icon beside the clock works: the panel's geometry is worked out
    /// from the window and the bag being shown, so the draw is the only place
    /// that knows where anything ended up.
    struct InventoryHit
    {
        float left{};
        float bottom{};
        float wide{};
        float high{};

        /// 0 a bag tab, 1 a page arrow, 2 the sort button.
        int kind{};

        /// The tab index, the page step, or the container to sort.
        int value{};

        bool holds(float x, float y) const
        {
            return x >= left && x < left + wide && y >= bottom && y < bottom + high;
        }
    };
    std::vector<InventoryHit> inventoryHits;

    // The right click menu: which slot it belongs to, and whether the drop has
    // been asked about once already. Closed when the slot is -1.
    //
    // Only the slot is kept. What is in it - the name, the count, whether it
    // can be worn - is looked up again as the menu is drawn, so the menu can
    // never describe an item the server has since taken away.
    int contextSlot = -1;
    int contextContainer = 0;
    bool contextConfirmDrop = false;

    // The equipment screen: its own panel, and which of the sixteen slots is
    // being filled. -1 lists everything that can be worn at all.
    bool equipmentOpen = std::getenv("MOGHOUSE_EQUIPMENT") != nullptr;
    int equipmentSlot = -1;



    // What was open last frame, so closing either panel can be noticed.
    bool wasInventoryOpen = false;
    bool wasEquipmentOpen = false;

    /// What order the grid is shown in.
    ///
    /// A view, not a change: the server owns which slot a thing is in, and
    /// reordering for real means a move packet per swap, which is both slow
    /// and exactly the traffic the server's bot detection watches for. Sorting
    /// here touches nothing and costs one comparison, and because every slot
    /// keeps its real number, acting on what you clicked still names the right
    /// place.
    enum class InventoryOrder
    {
        Slot,
        Name,
        Type,
        Amount,
        Level,
    };
    InventoryOrder inventoryOrder = InventoryOrder::Slot;

    // MOGHOUSE_INVENTORY opens the panel; giving it an order name also picks
    // one, which is the only way to see a sort without a mouse - a captured
    // frame cannot click a button.
    if (const char* wanted = std::getenv("MOGHOUSE_INVENTORY"))
    {
        const std::string asked{wanted};
        if (asked == "name") { inventoryOrder = InventoryOrder::Name; }
        else if (asked == "type") { inventoryOrder = InventoryOrder::Type; }
        else if (asked == "amount") { inventoryOrder = InventoryOrder::Amount; }
        else if (asked == "level") { inventoryOrder = InventoryOrder::Level; }
    }
    float optionsButton[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    auto writeCharacterInstance = [&]() {
        if (!characterInstanceBuffer)
        {
            return;
        }
        // characterFacing is a compass heading: 0 is +z, and the direction is
        // (sin, cos) - the same convention camera.yaw and the radar notch use.
        // The model faces +x when unrotated, so the rotation that points it
        // along the heading is a quarter turn less.
        //
        // Less, not more. The matrix below sends local +x to
        // (cos turn, 0, -sin turn); for that to equal (sin f, 0, cos f) the
        // turn has to be f - pi/2. With the quarter turn the other way round
        // the z component comes out negated, which is the same mistake the
        // heading reported to the server had: correct only when facing due
        // east or west, backwards when facing north, and reading as a
        // character that turns the wrong way as it walks.
        const float turn = characterFacing - 1.57079633f;
        const float s = characterScale;
        const float c = std::cos(turn) * s;
        const float sn = std::sin(turn) * s;
        const float instance[16] = {c,  0, -sn, 0, 0, s, 0, 0, sn, 0, c, 0,
                                    characterAt.x, characterAt.y, characterAt.z, 1};
        queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));

        // And one for each tracked entity, in the same buffer behind the
        // player. Written here rather than in the frame loop because this is
        // the only place the instance data is allowed to be produced - the
        // comment above says so, and the last time something updated a
        // position without coming through here the character stopped moving.
        drawnBodies = 0;

        // A test entity's age has to move for a spawn effect to play. With a
        // client attached the tracker does that; standing one up with
        // MOGHOUSE_ENTITIES there is nobody to, so it is counted from when the
        // window opened - and it is done here, immediately before the bodies
        // are written, because that is what reads it. Set further up the frame
        // it was a frame behind, which on a single screenshot means never set
        // at all.
        //
        // MOGHOUSE_TEST_SPAWN_PHASE pins the age instead of running it, so one
        // still frame can be taken at any point in the effect - the only
        // practical way to look at it, since a sequence long enough to cover it
        // is gigabytes of uncompressed frames.
        if (!link)
        {
            static const float pinned = [] {
                const char* set = std::getenv("MOGHOUSE_TEST_SPAWN_PHASE");
                return set != nullptr ? static_cast<float>(std::atof(set)) : -1.0f;
            }();

            const float since = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
            for (mh::RadarEntity& entity : radarEntities)
            {
                if (entity.spawnedSecondsAgo >= 0.0f)
                {
                    entity.spawnedSecondsAgo = pinned >= 0.0f ? pinned : since;
                }
            }
        }

        sounds.tick();

        // The zone's own noises, held while the listener is near enough to
        // hear them and let go when they are not.
        //
        // One voice per distinct sound, at the distance of its nearest
        // placement - not one per emitter. West Ronfaure puts the same
        // waterfall in fifty-six places, and fifty-six copies of it playing
        // over each other would be both wrong and loud.
        {
            static const float ambienceReach = [] {
                const char* set = std::getenv("MOGHOUSE_AMBIENCE_REACH");
                return set != nullptr ? static_cast<float>(std::atof(set)) : 30.0f;
            }();

            // Ambience sits well under the music and moves with it. The first
            // version of this did neither: it played at full scale against the
            // music's 0.35 and buried it, and the volume keys could not touch
            // it. Southern San d'Oria was the case that showed it - four
            // fountains and a wind, all looping, all at 1.0, which together
            // sound like a storm rather than a town.
            //
            // A fraction of the sound level rather than a level of its own, so
            // the Sound control moves it and the balance holds wherever that is
            // left. At the default 0.5 sound this is 0.15 ambience.
            static const float ambienceRatio = [] {
                const char* set = std::getenv("MOGHOUSE_AMBIENCE_VOLUME");
                return set != nullptr ? std::clamp(static_cast<float>(std::atof(set)), 0.0f, 1.0f) : 0.3f;
            }();
            const float ambienceGain = soundVolume * ambienceRatio;

            // And only a few at a time. The files carry their own loudness -
            // these five peak anywhere from 9% to 37% of full scale - so a
            // gain alone does not stop six of them adding up.
            static const size_t kNearestAmbience = 3;
            static const std::filesystem::path soundRoot = [] {
                const std::filesystem::path root = ffxi::defaultInstallRoot();
                return root.empty() ? root : root / "sound" / "win";
            }();

            if (!soundRoot.empty() && !emitters.empty())
            {
                const mh::Vec3 ear = camera.eye();
                std::map<uint32_t, float> nearest;
                for (const SoundEmitter& emitter : emitters)
                {
                    const float dx = emitter.x - ear.x;
                    const float dy = emitter.y - ear.y;
                    const float dz = emitter.z - ear.z;
                    const float distance = dx * dx + dy * dy + dz * dz;
                    const auto [where, added] = nearest.try_emplace(emitter.sound, distance);
                    if (!added && distance < where->second)
                    {
                        where->second = distance;
                    }
                }

                // Ranked before anything is played, so that only the few
                // closest positional sounds are heard at once.
                struct Candidate
                {
                    uint32_t sound;
                    float distance;
                    bool everywhere;
                };
                std::vector<Candidate> candidates;
                candidates.reserve(nearest.size());
                for (const auto& [sound, distance] : nearest)
                {
                    const std::filesystem::path file =
                        soundRoot / ffxi::SoundRef{std::string{}, sound, std::string{}}.file();

                    // Stereo means everywhere. A two-channel sound cannot be
                    // panned to a place, and the ones here are the zone's own
                    // weather - West Ronfaure's wind sits on the sky
                    // generators, which are nowhere in particular, so taking
                    // its distance seriously would have it fade as you walked.
                    // Mono is the positional kind and does fall off.
                    candidates.push_back(Candidate{sound, distance, sounds.channels(file) > 1});
                }
                std::sort(candidates.begin(), candidates.end(),
                          [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

                size_t positional = 0;
                for (const auto& [sound, distance, everywhere] : candidates)
                {
                    const std::filesystem::path file =
                        soundRoot / ffxi::SoundRef{std::string{}, sound, std::string{}}.file();

                    // Squared falloff, as the lamps use: linear is too loud too
                    // far out, and a waterfall audible across half a zone is
                    // worse than one that fades a little early.
                    const float reach = 1.0f - std::sqrt(distance) / ambienceReach;
                    float volume = everywhere ? ambienceGain
                                              : (reach > 0.0f ? reach * reach * ambienceGain : 0.0f);
                    if (!everywhere && volume > 0.0f && ++positional > kNearestAmbience)
                    {
                        volume = 0.0f;
                    }

                    const auto voice = ambienceVoices.find(sound);
                    if (volume <= 0.0f)
                    {
                        if (voice != ambienceVoices.end() && voice->second != 0)
                        {
                            sounds.release(voice->second);
                            ambienceVoices.erase(voice);
                        }
                        continue;
                    }
                    if (voice == ambienceVoices.end())
                    {
                        // hold() refuses anything that does not loop, so a
                        // one-shot effect that happens to sit beside a
                        // generator quietly never becomes ambience. Whether a
                        // sound is ambience is written in the file itself.
                        const uint32_t handle = sounds.hold(file, volume);
                        ambienceVoices[sound] = handle;
                        // Once per sound, and worth having: it says which of a
                        // zone's declared sounds are ambience and which were
                        // refused for not looping - the test that keeps a
                        // one-shot from being restarted forever.
                        std::printf("ambience: se%06u %s\n", sound,
                                    handle == 0    ? "does not loop, not ambience"
                                    : everywhere   ? "held, zone-wide (stereo)"
                                                   : "held, positional (mono)");
                        continue;
                    }
                    if (voice->second != 0)
                    {
                        sounds.setVolume(voice->second, volume);
                    }
                }
            }
        }

        // How long this zone has been up. Nothing is seen arriving in the
        // first few moments of one - see emergeOffset.
        // The settle exists because a client clears its tracker on a zone
        // change and everything then looks new. With no client there is no
        // tracker and nothing to settle, so it would only stop a test entity
        // ever being seen to arrive.
        const float sinceZoneSeconds =
            !link ? 1e6f
                  : zoneLoadedAtMs == 0
                        ? 0.0f
                        : static_cast<float>((SDL_GetTicksNS() / 1000000ull) - zoneLoadedAtMs) / 1000.0f;

        for (const mh::RadarEntity& entity : radarEntities)
        {
            if (drawnBodies >= bodiesInRange)
            {
                break;
            }
            float facing = entity.heading;
            if (entity.id == facingMe && entity.id != 0)
            {
                const float dx = characterAt.x - entity.x;
                const float dz = characterAt.z - entity.z;
                const float away = dx * dx + dz * dz;

                // Let go once they are out of talking distance, so an NPC does
                // not track you across the zone like a portrait.
                if (away > 12.0f * 12.0f)
                {
                    facingMe = 0;
                    facingMeStarted = false;
                }
                else if (away > 0.01f)
                {
                    // Heading is 0 along +z, the convention RadarEntity states
                    // and the player's own movement uses.
                    const float wanted = std::atan2(dx, dz);
                    if (!facingMeStarted)
                    {
                        facingMeAngle = entity.heading;
                        facingMeStarted = true;
                    }

                    // The short way round, so turning from just west of north
                    // to just east of it is a nudge and not a full circle.
                    float delta = wanted - facingMeAngle;
                    while (delta > 3.14159265f)
                    {
                        delta -= 6.28318531f;
                    }
                    while (delta < -3.14159265f)
                    {
                        delta += 6.28318531f;
                    }
                    facingMeAngle += delta * 0.25f;
                    facing = facingMeAngle;
                }
            }
            const float bodyTurn = facing - 1.57079633f;
            const float grow = mh::bodyScale(entity.size) * (mh::isChildRace(entity.look[0]) ? mh::kChildScale : 1.0f);
            const float bc = std::cos(bodyTurn) * grow;
            const float bs = std::sin(bodyTurn) * grow;
            // MOGHOUSE_SPAWN_WATCH=1 names what just turned up and whether it
            // is on the burrower list. Most of the common worms carry model 0
            // in the server's own tables and take their real one from
            // elsewhere, so the list in assets/burrowers.txt could not be built
            // by reading the database - this is how to fill it in: stand near
            // one, watch it spawn, add the number.
            static const bool watchSpawns = std::getenv("MOGHOUSE_SPAWN_WATCH") != nullptr;
            if (watchSpawns && entity.spawnedSecondsAgo >= 0.0f && entity.spawnedSecondsAgo < 1.0f)
            {
                static std::set<uint32_t> announced;
                if (announced.insert(entity.id).second)
                {
                    const bool burrows = burrowerModels().contains(entity.modelId);
                    std::printf("spawned %08X model %u %-20s %s\n", entity.id, entity.modelId,
                                entity.name.c_str(),
                                burrows ? "(burrows)" : "(add its model to assets/burrowers.txt to burrow)");
                }

                // Every frame while it is coming up, so a curve that looks
                // wrong can be read as numbers rather than guessed at from
                // screenshots.
                if (const float offset = emergeOffset(entity, grow, sinceZoneSeconds); offset != 0.0f)
                {
                    std::printf("  emerging %08X age %.2f offset %.2f\n", entity.id,
                                entity.spawnedSecondsAgo, offset);
                }
            }

            // A worm comes up out of the ground rather than appearing on it.
            // Zero for everything else, and for anything that has been here
            // longer than the effect lasts.
            const float rise = emergeOffset(entity, grow, sinceZoneSeconds);

            // And makes a noise doing it, on the first frame it is seen to
            // move. Keyed off the effect itself rather than off a second
            // window of its own: an earlier attempt fired only within 0.2s of
            // the entity appearing, and a zone takes longer than that to
            // finish loading, so the first frame ever drawn was already at
            // 0.22 and the sound could never play at all.
            //
            // se017024, which is a worm coming out of the ground. Not a guess:
            // the creature's own DAT declares it as a 0x3D sound reference,
            // and it was identified by ear against the retail client. Its pair
            // 17025 is going back under, which wants a despawn to hang off and
            // there is not one yet.
            //
            // MOGHOUSE_EMERGE_SOUND overrides it, which is how it was found.
            static const std::string emergeSound = [] {
                if (const char* chosen = std::getenv("MOGHOUSE_EMERGE_SOUND"))
                {
                    return std::string{chosen};
                }
                const std::filesystem::path root = ffxi::defaultInstallRoot();
                return root.empty() ? std::string{}
                                    : (root / "sound" / "win" / "se" / "se017" / "se017024.spw").string();
            }();

            if (!emergeSound.empty() && rise != 0.0f)
            {
                static std::set<uint32_t> sounded;
                if (sounded.insert(entity.id).second)
                {
                    const bool played = sounds.play(emergeSound, soundVolume);
                    if (watchSpawns)
                    {
                        std::printf("  emerge sound: %s (%zu voices)\n",
                                    played ? "playing" : "REFUSED", sounds.voices());
                    }
                }
            }

            const float body[16] = {bc, 0, -bs, 0, 0, grow, 0, 0, bs, 0, bc, 0,
                                    entity.x, entity.y + rise, entity.z, 1};
            queue.WriteBuffer(characterInstanceBuffer, sizeof(body) * (drawnBodies + 1), body, sizeof(body));
            ++drawnBodies;
        }
    };

    auto placeCharacter = [&](const mh::Vec3& where, float search, bool settle = false) {
        // Copied, not referenced. Every caller so far happened to pass
        // characterAt itself, which made the aliasing invisible; the one that
        // does not - dropping the character at the camera - would have written
        // the unsnapped height.
        const mh::Vec3 target = where;
        characterAt = target;
        if (const std::optional<mh::Vec3> ground =
                collision.nearestGround(target.x, target.z, target.y + 1.0f, search))
        {
            // Snapping up is what a caller wants: a spawn point given a
            // slightly-too-low height belongs on the floor above it. Snapping
            // *down* is not - a position well clear of the ground is a request
            // to be in the air, and gravity is a better answer to that than a
            // teleport.
            //
            // Unless the caller says to settle. A zone arrival is the one case
            // where the height is not a request to be in the air: the server
            // sends the zone line's own y, which sits above the floor often
            // enough that arriving meant visibly dropping onto it.
            characterAt = (!settle && target.y > ground->y + 1.0f)
                              ? mh::Vec3{ground->x, target.y, ground->z}
                              : *ground;
        }
        writeCharacterInstance();
    };
    placeCharacter(characterAt, 60.0f);

    /// Which way a zone line lies, and how wide the opening is.
    ///
    /// A zone line is a doorway, and a doorway is a narrow place: walkable
    /// ground runs freely along the way you are going and stops quickly to
    /// either side. So the walkable span through the point is measured in
    /// sixteen directions and the narrowest one wins - that is the opening,
    /// and the band is laid across it.
    ///
    /// The alternative was to take the direction from the `scale` pair in the
    /// zone's yaml, which describes the box at the *other* end of the line and
    /// says nothing about this one.
    ///
    /// Cached per zone: it walks the collision a few hundred times per line,
    /// which is nothing once and too much every frame.
    const auto orientZoneLine = [&](const mh::ZoneLineMarker& line) -> mh::Vec3 {
        constexpr int kDirections = 16;
        constexpr float kProbeStep = 0.5f;
        constexpr float kProbeReach = 12.0f;

        if (collision.empty())
        {
            return {1.0f, 0.0f, line.radius};
        }

        float narrowest = 1e9f;
        float axisX = 1.0f;
        float axisZ = 0.0f;
        for (int d = 0; d < kDirections; ++d)
        {
            const float angle = static_cast<float>(d) * 3.14159265f / static_cast<float>(kDirections);
            const float dx = std::cos(angle);
            const float dz = std::sin(angle);

            float span = 0.0f;
            for (int sign = -1; sign <= 1; sign += 2)
            {
                float height = line.y;
                float reached = 0.0f;
                for (float step = kProbeStep; step <= kProbeReach; step += kProbeStep)
                {
                    const float px = line.x + dx * step * static_cast<float>(sign);
                    const float pz = line.z + dz * step * static_cast<float>(sign);
                    const std::optional<float> ground =
                        collision.groundAt(px, pz, height, 3.0f, mh::Collision::kDefaultStepUp);
                    if (!ground)
                    {
                        break;
                    }
                    height = *ground;
                    reached = step;
                }
                span += reached;
            }

            if (span < narrowest)
            {
                narrowest = span;
                axisX = dx;
                axisZ = dz;
            }
        }

        // Floored so a line in a doorway barely wider than a character is still
        // visible, and capped so one standing in open country does not become a
        // wall across the horizon. Then run past each jamb: the span is
        // measured from the floor, the floor stops before the wall does, and a
        // band that ends with the floor leaves a gap at both ends of what is
        // meant to read as a line drawn across the entrance.
        const float half = std::clamp(narrowest * 0.5f, 1.5f, 10.0f) + mh::kZoneLineOverhang;
        return {axisX, axisZ, half};
    };

    /// The measured axes, and the zone line list they were measured for.
    std::vector<mh::Vec3> zoneLineAxes;
    std::vector<mh::ZoneLineMarker> orientedFor;

    if (character)
    {
        std::printf("character stands at %.1f %.1f %.1f%s\n", characterAt.x, characterAt.y, characterAt.z,
                    collision.empty() ? " (no collision - movement unconstrained)" : "");
    }

    // Framing a shot from a script needs the camera to be settable; dragging
    // it into place by hand cannot be repeated.
    if (const char* cameraEnv = options.camera ? options.camera->c_str() : nullptr)
    {
        std::sscanf(cameraEnv, "%f,%f,%f", &camera.position.x, &camera.position.y, &camera.position.z);
    }
    if (const char* lookEnv = options.cameraLook ? options.cameraLook->c_str() : nullptr)
    {
        // yaw,pitch in degrees, and optionally how far back to orbit from.
        // The default hundred units surveys a whole zone, which is far too far
        // away to check anything about the character itself.
        float yawDegrees = 0.0f;
        float pitchDegrees = 0.0f;
        float orbitDistance = 0.0f;
        const int given = std::sscanf(lookEnv, "%f,%f,%f", &yawDegrees, &pitchDegrees, &orbitDistance);
        camera.yaw = yawDegrees * 3.14159265f / 180.0f;
        camera.pitch = pitchDegrees * 3.14159265f / 180.0f;
        if (given >= 3 && orbitDistance > 0.0f)
        {
            camera.distance = orbitDistance;
        }
    }

    // Two cursors. The pointer says whether there is anything under it
    // worth clicking, which is the feedback a click wants before it happens
    // rather than after.
    SDL_Cursor* arrowCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    SDL_Cursor* handCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    // The zone's music. Opened here rather than at startup so a client
    // that cannot make a sound still draws.
    mh::Music music;

    music.setVolume(musicVolume);

    bool dragging = false;

    /// Whether the pointer moved between press and release. A drag turns the
    /// camera; a click picks a target, and they start out identical.
    bool dragMoved = false;

    std::printf("wasd to walk, mouse drag to look, space to jump, wheel or numpad 9/3 to zoom,\n");
    std::printf("shift to run, r to auto-run, p to print position,\n");
    std::printf("c to place the character,\n");
    std::printf("u to back up the trail if collision traps you, n for no collision,\n");
    std::printf("numpad 8/2 to move and 4/6 to turn, numpad star to walk, shift to invert it,\n");
    std::printf("numpad minus for the options menu, minus/equals for music, shift for sound,\n");
    std::printf("f to swap between driving the character and flying the camera,\n");
    std::printf("tab to pick the nearest thing and step through the rest, v to follow it,\n");
    std::printf("o to orbit the camera,\n");
    std::printf("t to get on and off the monorail where there is one,\n");
    std::printf("escape to quit\n");

    // `screenshotPath` writes one frame to a BMP and quits. Without it
    // there is no way to check what the renderer actually produced except by
    // looking at the window, which rules out checking anything unattended.
    const char* screenshotPath = options.screenshotPath ? options.screenshotPath->c_str() : nullptr;

    // `screenshotSequence` captures a whole animation in one run,
    // one file per source frame, with the path taken as a printf format. The
    // alternative is relaunching for each frame, which for a walk cycle is
    // eighteen launches of a program that spends most of its time loading a
    // zone.
    const int sequenceCount = options.screenshotSequence;
    int shotIndex = -options.settleFrames; // let the first frames settle
    wgpu::Buffer readbackBuffer;

    // `animation` names one of the character's own animations - idl0
    // for a standing idle, wlk0 to walk, run0 to run. Skinning runs on the CPU
    // and the vertices are rewritten each frame: a couple of thousand
    // triangles is nothing next to a zone, and it keeps the pose maths
    // somewhere it can be read.
    const ffxi::Animation* playing = nullptr;
    const ffxi::Animation* idleClip = nullptr;
    const ffxi::Animation* sitClip = nullptr;
    const ffxi::Animation* walkClip = nullptr;
    const ffxi::Animation* runClip = nullptr;
    const ffxi::Animation* jumpClip = nullptr;

    // Kneeling. Every race ships res0 beside idl0 and wlk0; it is what /heal
    // plays, and what a character does while a logout counts down.
    const ffxi::Animation* restClip = nullptr;
    const ffxi::Animation* deadClip = nullptr;
    // When the jump finishes, on the same clock animationOffset is measured
    // against. Idle, walk and run are chosen every frame from what the
    // character is doing; a jump is not, so it needs an end to hold until.
    float jumpUntil = 0.0f;
    std::function<const ffxi::Animation*(const ffxi::Animation*)> upperFor;
    // MOGHOUSE_ANIMATION pins one clip; without it, movement picks.
    const bool pinnedClip = options.animation.has_value();
    float animationOffset = 0.0f;
    bool driving = character.has_value();

    // The named clips of whoever the character is right now. These point
    // into the character's own animation table, so they are looked up when a
    // character is given at startup and again when one arrives later - the
    // sign-in happens inside this window now, and a body built after it
    // otherwise has five null clips: it walks, and its legs never move.
    const auto bindClips = [&]() {
        auto find = [&](const char* name) -> const ffxi::Animation* {
            if (!character)
            {
                return static_cast<const ffxi::Animation*>(nullptr);
            }
            auto found = character->animations.find(name);
            return found == character->animations.end() ? nullptr : &found->second;
        };
        idleClip = find("idl0");
        restClip = find("res0");

        // si10, not si1. A clip name is four characters - a three-character
        // action and a half, 0 for the root and legs and 1 for everything
        // above the waist - so sitting still is action si1, half 0. Asking for
        // "si1" matches nothing at all, which is why /sit said you had sat
        // down and left you standing. upperFor turns the trailing 0 into a 1,
        // so si11 comes along on its own.
        sitClip = find("si10");
        if (character && !character->animations.empty() && !sitClip)
        {
            std::printf("no sitting clip (si10) on this body; /sit will do nothing\n");
        }
        walkClip = find("wlk0");
        runClip = find("run0");
        jumpClip = find("jmp0");
        deadClip = find("ded0");
    };

    // The clips ending 0 drive the root, hips and legs - sixteen bones of
    // ninety-four. Everything above the waist, the spine and torso and
    // arms and a Mithra or Galka tail, lives in the clips ending 1, and
    // movement has no 1 of its own. std1 is the standing upper body, and
    // layering it under a stride is what gets the arms swinging.
    // A clip's upper body is its own name with the trailing 0 turned into
    // a 1: wlk0 walks the legs and wlk1 swings the arms above them. They
    // are stored apart because the upper half depends on what the
    // character is holding, so the two are chosen separately and played
    // together.
    //
    // MOGHOUSE_UPPER pins one for every clip, or "none" to go back to the
    // legs alone.
    //
    // Bound here rather than inside the block below on purpose: it reads
    // through `character` by reference, so it serves a character that arrives
    // after sign-in just as well - and inside the block it served only one
    // given at startup, which is how a late-built body walked with its arms
    // held still.
    const char* upperPin = std::getenv("MOGHOUSE_UPPER");
    upperFor = [&character, upperPin](const ffxi::Animation* lower) -> const ffxi::Animation* {
        if (!character || (upperPin && std::strcmp(upperPin, "none") == 0))
        {
            return nullptr;
        }
        std::string name = upperPin ? upperPin : (lower ? lower->name : std::string{});
        if (!upperPin)
        {
            if (name.empty() || name.back() != '0')
            {
                return nullptr;   // already an upper body, or unpaired
            }
            name.back() = '1';
        }
        auto found = character->animations.find(name);
        return found == character->animations.end() ? nullptr : &found->second;
    };
    if (character && !character->animations.empty())
    {
        bindClips();

        // How much of the skeleton each clip actually drives. A clip that only
        // carries tracks for the legs leaves the arms in the bind pose, which
        // reads as a character running with its arms held still.
        if (std::getenv("MOGHOUSE_ANIMATION_TRACKS"))
        {
            for (const auto& [clipName, clip] : character->animations)
            {
                std::printf("  %-6s %3u frames, %zu of %zu bones driven", clipName.c_str(), clip.frames,
                            clip.tracks.size(), character->skeleton.bones.size());
                if (std::strcmp(std::getenv("MOGHOUSE_ANIMATION_TRACKS"), "bones") == 0)
                {
                    std::printf("   bones:");
                    for (const ffxi::AnimationTrack& track : clip.tracks)
                    {
                        std::printf(" %u", track.bone);
                    }
                }
                std::printf("\n");
            }
        }

        const char* wanted = options.animation ? options.animation->c_str() : nullptr;
        auto found = character->animations.find(wanted ? wanted : "idl0");
        if (found == character->animations.end() && wanted)
        {
            std::printf("no animation called %s; this character has:", wanted);
            for (const auto& [name, unused] : character->animations)
            {
                std::printf(" %s", name.c_str());
            }
            std::printf("\n");
        }
        if (found != character->animations.end())
        {
            playing = &found->second;
            std::printf("playing %s: %u frames at %.1f a second\n", playing->name.c_str(), playing->frames,
                        1.0f / playing->frameSeconds());
        }
    }

    // `frame` pins the animation clock so a screenshot of a moving
    // character lands on the same pose every time.
    // What the radar shows. Fed from options until something is connected,
      // and replaced wholesale rather than merged - the list is already the
      // answer to "what can be seen right now".

    // The zone name as glyph indices, resolved once. Anything outside the font
    // becomes a space rather than a wrong letter, and underscores read as
    // spaces because that is how the server names zones.
    std::vector<int> labelIndices;
    if (options.zoneName)
    {
        static const std::string kOrder = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-.";
        for (char raw : *options.zoneName)
        {
            if (labelIndices.size() >= mh::kRadarMaxLabel)
            {
                break;
            }
            char upper = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
            if (upper == '_')
            {
                upper = ' ';
            }
            const size_t found = kOrder.find(upper);
            labelIndices.push_back(found == std::string::npos ? 0 : static_cast<int>(found));
        }
    }

    /// How fast a fall accelerates, in units per second squared. Not measured
    /// against anything - FFXI's own value is not in the DATs - so it is
    /// tuned to look right at this scale, where a character is 1.8 units tall.
    /// How deep the water may be and still be walked into. Ankle-deep puddles
/// and fountain rims are walkable in FFXI; a canal is not.
constexpr float kWadeDepth = 1.2f;

/// How far a single step may drop before it is refused.
///
/// Generous enough for a stair tread or a kerb, short enough that walking off
/// a quay or into a gap in the geometry stops at the edge.
constexpr float kMaxStepDown = 1.6f;

constexpr float kGravity = 26.0f;

    /// How far below the feet still counts as standing on something. Slopes
    /// and the discrete steps of a walk both leave small gaps, and treating
    /// those as falling makes a character jitter downhill.
    constexpr float kGroundSnap = 0.25f;

    /// How far down to look for the floor while falling.
    constexpr float kFallReach = 500.0f;

/// How long one shoreline wave takes, in seconds.
///
/// A setting, not a reading. The curves say the shape of a wave exactly and
/// say nothing about its length; the only per-generator number left that
/// could carry it is op 0x30, which is unread (see ffxi::EffectPlacement).
/// Eight seconds is what a beach looks like. MOGHOUSE_WAVE_PERIOD overrides
/// it, for standing on the sand and comparing.
const float kWavePeriod = [] {
    if (const char* set = std::getenv("MOGHOUSE_WAVE_PERIOD"))
    {
        const float given = std::strtof(set, nullptr);
        if (given > 0.1f)
        {
            return given;
        }
    }
    return 8.0f;
}();

    /// How fast a jump leaves the ground, in yalms a second.
    ///
    /// At 6.2 this rose about three quarters of a yalm over roughly half a
    /// second, matched to the animation - the clip is what says how high a
    /// jump looks, and the arc should not finish long before or long after it
    /// does. But three quarters of a yalm does not clear a barrier of one, and
    /// what a jump is actually for is getting over things.
    ///
    /// 7.5 rises 1.08 yalms in 0.58 seconds. A tenth of a second longer in the
    /// air than the clip wants, for a third again the height.
    ///
    /// MOGHOUSE_JUMP overrides it, since how high is high enough is a thing
    /// you find by trying it against the barriers that are actually in the way.
    const float kJumpSpeed = [] {
        const char* set = std::getenv("MOGHOUSE_JUMP");
        if (set != nullptr)
        {
            const float given = std::strtof(set, nullptr);
            if (given > 0.1f)
            {
                return given;
            }
        }
        return 7.5f;
    }();

    float fallSpeed = 0.0f;

    const float pinnedFrame = options.frame.value_or(-1.0f);
    const float shaderMode = options.shaderMode;
    const int cutoutMode = options.cutoutMode;

    // `timeOfDay` = 1830 pins the clock; otherwise a Vana'diel day passes in
    // one real minute, which is fast but makes the whole cycle visible.
    const bool timeFixed = options.timeOfDay.has_value();
    const int fixedMinutes = timeFixed ? (*options.timeOfDay / 100) * 60 + (*options.timeOfDay % 100) : 0;
    if (!lighting.empty())
    {
        std::printf("lighting: %zu times of day, clock %s\n", lighting.sets().size(),
                    timeFixed ? "fixed" : "running");
    }

    // Run unless told otherwise, which is what the game does - there is no
    // walking anywhere by accident. Numpad minus toggles it, the way the real
    // client does, and holding shift inverts whichever way the toggle is set,
    // so a moment of walking does not need a mode change and back.
    bool walkByDefault = false;

    /// Keep going forward without holding the key. R toggles it, and anything
    /// that means "stop" clears it: pressing back, or dying. Holding forward
    /// while it is on is harmless - it is the same direction.
    bool autoRun = false;

    /// A zone change waiting for the loading screen to be shown first.
    mh::ViewerLink::ZoneRequest pending;
    bool pendingZone = false;

    /// The zone being read, drawn over everything while it happens.
    std::string loadingZone;

    /// Whether the radar turns with the player or holds north at the top.
    ///
    /// Both are defensible and people are firm about which they want, so it is
    /// a toggle rather than a decision. M switches it; MOGHOUSE_RADAR_NORTH
    /// picks the one you start with.
    bool radarTurns = std::getenv("MOGHOUSE_RADAR_NORTH") == nullptr;

    // The death box, as the last frame left it.
    //
    // Immediate mode: the box is laid out while it is drawn and the rectangles
    // are kept, so a click is tested against the frame the player was looking
    // at when they pressed. A frame's lag on a box that only appears when a
    // character has stopped moving is not something anyone can aim past.
    bool deathBoxShown = false;
    float deathPanel[4]{};
    DialogButton deathButtons[mh::kDialogButtons]{};

    // The form, if one is up, kept the same immediate-mode way.
    //
    // What the player has typed lives here rather than in the Form the client
    // set, because the client's copy is what it asked for and this is what it
    // is being given - they only meet when a button is pressed. Keeping them
    // apart is what lets the client leave a form up unchanged while somebody
    // is halfway through filling it in.
    bool formShown = false;
    float formPanel[4]{};
    std::vector<std::string> formValues;
    std::vector<float> formFieldRects;   // four floats per row, empty rect when not a field
    std::vector<DialogButton> formButtons;
    std::vector<int> formButtonRow;      // which form row each button came from
    int formFocus = -1;                  // the row being typed into, or -1
    int formPressed = -1;                // the button under a held mouse, by index
    std::string formSignature;           // what the last laid-out form was, to notice a new one
    std::string formChoiceSignature;     // the choice rows' values, to notice the client moving one
    int formOpenChoice = -1;             // the Choice row whose options are unfolded, or -1
    std::vector<DialogButton> formChoiceBoxes;   // the folded box of each Choice row
    std::vector<int> formChoiceBoxRow;
    std::vector<DialogButton> formChoiceHits;    // the unfolded options, while one is open
    std::vector<int> formChoiceHitOption;

    /// The two chips in the top left corner, as the last frame drew them.
    ///
    /// Somewhere to send a bug from inside the game, rather than from the
    /// launcher the player cannot see while the world is up.
    struct CornerLink
    {
        float left{}, bottom{}, width{}, height{};
        mh::ViewerLink::Link target{mh::ViewerLink::Link::None};

        bool holds(float x, float y) const
        {
            return width > 0.0f && x >= left && x < left + width && y >= bottom && y < bottom + height;
        }
    };
    CornerLink cornerLinks[2]{};

    // Which button the mouse went down on, so releasing somewhere else is a
    // change of mind rather than a press. -1 is none.
    int deathPressed = -1;

    // A pointer position in the coordinates the box is laid out in.
    //
    // SDL reports window points and the surface is measured in pixels, which
    // are not the same number on a high density display - so this divides by
    // the window rather than by the frame. What the two share is the aspect,
    // and the aspect is all normalised device coordinates need.
    /// The last frame's view projection, for turning a click into a target.
    ///
    /// Picking needs to know where things were on screen, and the only place
    /// that knows is the draw. Keeping the matrix rather than recomputing it
    /// means the cursor is tested against exactly the frame that was shown.
    /// Who was last clicked, and who the cursor is over.
    ///
    /// FFXI draws a ring under the thing you have targeted, which is both the
    /// clearest indication and the cheapest: the zone line pipeline already
    /// draws a glowing ring at a world position, so a target is one more ring
    /// in a different colour rather than a second way of drawing.
    uint32_t targetId = 0;
    uint32_t hoverId = 0;

    /// What the target looked like last frame, so a body that will not hold
    /// still can be caught saying so.
    ///
    /// An NPC was reported changing form while being watched, and nothing in
    /// the tracker's own log agreed: no look changed, no model rebuilt. So the
    /// change is happening between the look arriving and the body being drawn,
    /// and this is the only place left to look from.
    std::array<uint16_t, 7> targetWas{};

    /// Who targetWas describes. Without this, tabbing from one body to another
    /// compared the new one against the old one and reported a change every
    /// time - which is a diagnostic reporting its own footsteps.
    uint32_t targetWasFor = 0;

    /// Whether the character is walking after whatever is targeted.
    ///
    /// Tab steps through what is near, nearest first, and V follows it. The
    /// target is held by id rather than by index: the entity list is rebuilt
    /// whenever the server changes it, and an index into it means somebody
    /// else a moment later.
    bool following = false;

    mh::Mat4 pickProjection{};
    bool havePickProjection = false;

    /// Who is under the cursor, or 0.
    ///
    /// Entities are projected to the screen rather than the cursor being
    /// unprojected into a ray: the bodies are already drawn from a position
    /// and a height, so a point and a radius describes them as well as a
    /// volume would, and it needs no matrix inversion. Nearest to the camera
    /// wins among those the cursor is over, which is what makes clicking a
    /// shopkeeper standing in front of a wall pick the shopkeeper.
    const auto pickAt = [&](float ndcX, float ndcY) -> uint32_t {
        if (!havePickProjection)
        {
            return 0;
        }

        uint32_t best = 0;
        float bestDepth = 0.0f;
        float bestOffBy = 0.0f;
        for (const mh::RadarEntity& entity : radarEntities)
        {
            // Anything with a body, whether or not the server flagged it.
            //
            // 0x28 bit 0x40 looked like the answer and is not, at least on
            // LandSandBoat: setTriggerable(true) is called from one place, on
            // the path that builds an NPC from a Lua table, and only when an
            // onTrigger is passed there. NPCs loaded from a zone's dataset
            // never get it however many scripts they have - Windurst Waters
            // reported 0 of 28 - while mobs do. Filtering on it made monsters
            // clickable and shopkeepers not, which is backwards.
            //
            // The flag is still read and carried; it is just not a gate. What
            // makes something worth clicking is having a body and not being
            // hidden, and the server's own reply settles the rest.
            if (!entity.hasLook() && !entity.hasModel())
            {
                continue;
            }

            const DrawableCharacter* model = modelForEntity(entity);
            const float height = (model ? model->loaded.geometry.height() : 1.8f) * mh::bodyScale(entity.size) *
                                 (mh::isChildRace(entity.look[0]) ? mh::kChildScale : 1.0f);

            const float world[4] = {entity.x, entity.y + height * 0.5f, entity.z, 1.0f};
            const float* m = pickProjection.m;
            const float clipX = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12];
            const float clipY = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13];
            const float clipW = m[3] * world[0] + m[7] * world[1] + m[11] * world[2] + m[15];
            if (clipW <= 0.01f)
            {
                continue;   // behind the camera
            }

            const float screenX = clipX / clipW;
            const float screenY = clipY / clipW;

            // How big the body is on screen, so a distant NPC is a small
            // target and a near one is a generous one - the same way it looks.
            const float onScreen = std::max(height / clipW, 0.02f);
            const float dx = screenX - ndcX;
            const float dy = (screenY - ndcY) * 0.5f;   // NDC y is not square with x
            if (dx * dx + dy * dy > onScreen * onScreen)
            {
                continue;
            }

            // How centrally the click landed on this one, as a fraction of its
            // own radius - so a big near body and a small far one are judged
            // on the same scale rather than the near one always winning.
            //
            // This used to be decided by depth alone: every body whose radius
            // held the cursor was a candidate, and the one nearest the camera
            // took it. That is a fair guess for NPCs scattered about a zone,
            // which is all there was when it was written. It is quite wrong for
            // a row of characters standing shoulder to shoulder - these radii
            // are generous, in a line-up they overlap almost entirely, and the
            // whole row's clicks went to whichever figure happened to stand
            // closest to the camera. Character select always chose that one
            // person however carefully you aimed at somebody else.
            const float offBy = (dx * dx + dy * dy) / (onScreen * onScreen);
            if (best == 0 || offBy < bestOffBy || (offBy == bestOffBy && clipW < bestDepth))
            {
                best = entity.id;
                bestOffBy = offBy;
                bestDepth = clipW;
            }
        }

        return best;
    };

    const auto pointerNdc = [window](float x, float y, float& ndcX, float& ndcY)
    {
        int pointsAcross = 0;
        int pointsDown = 0;
        SDL_GetWindowSize(window, &pointsAcross, &pointsDown);
        if (pointsAcross <= 0 || pointsDown <= 0)
        {
            return false;
        }
        ndcX = x / static_cast<float>(pointsAcross) * 2.0f - 1.0f;
        ndcY = 1.0f - y / static_cast<float>(pointsDown) * 2.0f;
        return true;
    };

    // What the player wants the interface scaled by, on top of the display's
    // own correction below. "The right physical size" and "the size I want to
    // read" are not the same thing, and on a large monitor at a distance they
    // are quite far apart.
    //
    // The default comes from the window's size in *points*, not in pixels.
    // The pixels-over-points ratio is already applied further down, so a
    // retina display has been doubled before this is consulted at all -
    // deciding from pixels would double it again and hand a Mac an interface
    // at four times the size. A 4K monitor with no scaling of its own reports
    // 2160 points and gets the 2; a 1080p one reports 1080 and gets the 1;
    // that same retina Mac reports 720 and correctly asks for nothing.
    const auto scaleForWindow = [window]()
    {
        int pointsAcross = 0;
        int pointsDown = 0;
        SDL_GetWindowSize(window, &pointsAcross, &pointsDown);
        if (pointsDown >= 1800)
        {
            return 2.0f;
        }
        if (pointsDown >= 1300)
        {
            return 1.5f;
        }
        return 1.0f;
    };

    // Zero means "let the window decide", which is what the setting holds
    // until somebody picks a number.
    float uiScaleChoice = 0.0f;
    float uiScaleSetting = scaleForWindow();

    // The environment wins over both, and keeps winning: it is how a
    // screenshot is taken at a known size.
    bool uiScalePinned = false;
    if (const char* wanted = SDL_getenv("MOGHOUSE_UI_SCALE"))
    {
        const float parsed = std::strtof(wanted, nullptr);
        if (parsed > 0.0f)
        {
            uiScaleSetting = std::clamp(parsed, 0.5f, 4.0f);
            uiScalePinned = true;
            std::printf("interface scaled by %.2f\n", uiScaleSetting);
        }
        else
        {
            std::printf("MOGHOUSE_UI_SCALE wants a number, like 1.25\n");
        }
    }

    uint64_t previousTicks = SDL_GetTicksNS();
    bool running = true;
    while (running)
    {
        // Dead, and whether a way up has been offered. Read once at the top of
        // the frame and answered everywhere below: a corpse does not walk and
        // does not jump, it holds the fallen pose rather than the idle one,
        // and it is looking at the box that asks what to do about it.
        //
        // One read rather than one per site, because one per site drifted.
        // Only the box knew about testDeath, so the standalone viewer drew it
        // over a character still standing up - four parts of one state
        // disagreeing about whether anyone had died.
        bool raiseOffered = options.testDeath > 1;
        const bool dead = link ? link->dead(raiseOffered) : options.testDeath > 0;

        // Read here for the same reason, and it matters more: the events below
        // decide what a keypress means by looking at the form, and the drawing
        // further down lays it out. Two reads of a form the client can replace
        // between them would let a click land on a button that is no longer
        // there.
        // The options menu wins over whatever the client is showing. It is the
        // renderer's own, so it does not go through the link at all - see
        // optionsMenu.
        const mh::Form activeForm = optionsOpen ? optionsMenu(musicVolume, soundVolume, uiScaleChoice)
                                    : link      ? link->form()
                                    : options.testForm ? demoForm()
                                                       : mh::Form{};

        // Whether a row takes typing. Fields do; a choice does not, and a
        // keypress let into a choice's value would corrupt the option list
        // that value carries.
        const auto formTypable = [&](int row) {
            if (row < 0 || static_cast<size_t>(row) >= activeForm.rows.size())
            {
                return false;
            }
            const mh::FormRowKind kind = activeForm.rows[static_cast<size_t>(row)].kind;
            return kind == mh::FormRowKind::Field || kind == mh::FormRowKind::Secret;
        };
        const auto formChoiceAt = [&](int row) {
            return row >= 0 && static_cast<size_t>(row) < activeForm.rows.size() &&
                   activeForm.rows[static_cast<size_t>(row)].kind == mh::FormRowKind::Choice &&
                   activeForm.rows[static_cast<size_t>(row)].enabled;
        };
        // Sets a Choice row and hands the form straight back, so the client
        // can react to it at once. `option` is absolute; `step` is relative
        // and wraps, for the arrow keys.
        const auto pickChoice = [&](int row, int option) {
            if (!formChoiceAt(row) || static_cast<size_t>(row) >= formValues.size() || !link)
            {
                return;
            }
            std::string& value = formValues[static_cast<size_t>(row)];
            const std::vector<std::string> options = choiceOptions(value);
            if (options.empty())
            {
                return;
            }
            const int count = static_cast<int>(options.size());
            const int chosen = ((option % count) + count) % count;
            value = choiceValue(chosen, options);

            // The options menu is answered here rather than sent anywhere. Its
            // rows are volumes this loop owns, and the form is rebuilt from
            // them next frame, so setting them is the whole of applying it.
            if (optionsOpen)
            {
                const float level = static_cast<float>(chosen) / 10.0f;
                if (row == 0)
                {
                    musicVolume = level;
                    music.setVolume(musicVolume);
                    if (link)
                    {
                        link->noteSettings({musicVolume, soundVolume, radarTurns, uiScaleChoice});
                    }
                }
                else if (row == 1)
                {
                    soundVolume = level;
                    if (link)
                    {
                        link->noteSettings({musicVolume, soundVolume, radarTurns, uiScaleChoice});
                    }
                }
                else if (row == 2)
                {
                    // Not the volume arithmetic above: this row's options are
                    // scales, and Auto is not a number at all.
                    uiScaleChoice = uiScaleForStep(chosen);
                    if (!uiScalePinned)
                    {
                        uiScaleSetting = uiScaleChoice > 0.0f ? uiScaleChoice : scaleForWindow();
                    }
                    if (link)
                    {
                        link->noteSettings({musicVolume, soundVolume, radarTurns, uiScaleChoice});
                    }
                }
                return;
            }

            if (link)
            {
                link->submitForm(row, formValues);
            }
        };
        const auto stepChoice = [&](int row, int step) {
            if (!formChoiceAt(row) || static_cast<size_t>(row) >= formValues.size())
            {
                return;
            }
            pickChoice(row, choiceSelected(formValues[static_cast<size_t>(row)]) + step);
        };

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                running = false;
            }
            // A form takes the keyboard and the mouse before anything else.
            // These come first in the chain on purpose: while a login screen is
            // up, typing a W belongs in the username, not to the character.
            else if (event.type == SDL_EVENT_TEXT_INPUT && formShown && formFocus >= 0 &&
                     static_cast<size_t>(formFocus) < formValues.size() && formTypable(formFocus))
            {
                formValues[static_cast<size_t>(formFocus)] += event.text.text;
            }
            // Before the form, because the form is what this opens: handled
            // after it, the menu could be opened and never shut.
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_KP_MINUS)
            {
                optionsOpen = !optionsOpen;
            }
            // Same reason as the menu above - handled before the form so it
            // can shut what it opened - but not while a letter is being typed,
            // where an I belongs in the sentence.
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_I && !typing)
            {
                inventoryOpen = !inventoryOpen;
                inventoryPage = 0;
                contextSlot = -1;
            }
            // G rather than E: E already talks to whoever is in front, and
            // this branch sits ahead of that one, so taking E for a panel
            // silently stopped anyone being spoken to.
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_G && !typing)
            {
                equipmentOpen = !equipmentOpen;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !typing && link &&
                     link->lineup())
            {
                // Out of character select. Nothing else on this screen answers
                // escape, and without it the only way back to the sign-in was
                // to kill the client.
                link->requestTalk(mh::ViewerLink::kLineupCancelled);
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && inventoryOpen && !typing &&
                     (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT ||
                      event.key.key == SDLK_PAGEUP || event.key.key == SDLK_PAGEDOWN ||
                      event.key.key == SDLK_ESCAPE))
            {
                if (event.key.key == SDLK_ESCAPE)
                {
                    inventoryOpen = false;
                }
                else if (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT)
                {
                    inventoryTab += event.key.key == SDLK_RIGHT ? 1 : -1;
                    inventoryPage = 0;
                }
                else
                {
                    inventoryPage += event.key.key == SDLK_PAGEDOWN ? 1 : -1;
                }
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && formShown)
            {
                const SDL_Keycode key = event.key.key;
                const bool hasFocus = formFocus >= 0 && static_cast<size_t>(formFocus) < formValues.size();

                if (key == SDLK_BACKSPACE && hasFocus && formTypable(formFocus))
                {
                    std::string& value = formValues[static_cast<size_t>(formFocus)];

                    // The shortcuts every other text box has. There is no
                    // selection here - no dragging across a word, no select
                    // all - so without these the only way to empty a field of
                    // a saved server name is to hold backspace and count. That
                    // is what someone trying to select the text and delete it
                    // discovers, after the click does nothing.
                    const bool wholeField = (event.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
                    const bool wholeWord = (event.key.mod & SDL_KMOD_ALT) != 0;

                    if (wholeField)
                    {
                        value.clear();
                    }
                    else if (wholeWord)
                    {
                        // Trailing spaces first, then the word itself, so a
                        // press at the end of "one two " leaves "one ".
                        while (!value.empty() && value.back() == ' ')
                        {
                            value.pop_back();
                        }
                        while (!value.empty() && value.back() != ' ')
                        {
                            value.pop_back();
                        }
                    }
                    else if (!value.empty())
                    {
                        // Back off a whole UTF-8 character rather than a byte,
                        // or one press through an accented letter leaves half
                        // of it behind and the next draw walks into nonsense.
                        size_t back = value.size() - 1;
                        while (back > 0 && (static_cast<unsigned char>(value[back]) & 0xC0) == 0x80)
                        {
                            --back;
                        }
                        value.resize(back);
                    }
                }
                else if (formChoiceAt(formFocus) && (key == SDLK_LEFT || key == SDLK_UP))
                {
                    stepChoice(formFocus, -1);
                    formOpenChoice = -1;
                }
                else if (formChoiceAt(formFocus) && (key == SDLK_RIGHT || key == SDLK_DOWN))
                {
                    stepChoice(formFocus, 1);
                    formOpenChoice = -1;
                }
                else if (key == SDLK_TAB)
                {
                    // Round the form, so all of it can be worked from the
                    // keyboard. Shift goes back the way people expect.
                    formOpenChoice = -1;
                    //
                    // Buttons are in the round, not just the fields. A screen
                    // with more than one - sign in, make an account, quit -
                    // otherwise had every button but the first reachable only
                    // with the mouse, because Return presses the first.
                    const bool backwards = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                    const int count = static_cast<int>(formValues.size());
                    for (int step = 1; step <= count; ++step)
                    {
                        const int offset = backwards ? -step : step;
                        const int candidate = ((formFocus + offset) % count + count) % count;
                        const mh::FormRow& row = activeForm.rows[static_cast<size_t>(candidate)];
                        if (row.kind != mh::FormRowKind::Label && row.enabled)
                        {
                            formFocus = candidate;
                            break;
                        }
                    }
                }
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)
                {
                    // On a choice, return unfolds it or folds it back up
                    // rather than pressing anything.
                    if (formChoiceAt(formFocus))
                    {
                        formOpenChoice = formOpenChoice == formFocus ? -1 : formFocus;
                        continue;
                    }

                    // The button being looked at, if one is. Otherwise the
                    // first that can be pressed, which is what a sign-in form
                    // is for - typing a password and pressing return should
                    // sign in rather than do nothing.
                    int pressing = -1;
                    if (formFocus >= 0 && formFocus < static_cast<int>(activeForm.rows.size()) &&
                        activeForm.rows[static_cast<size_t>(formFocus)].kind == mh::FormRowKind::Button &&
                        activeForm.rows[static_cast<size_t>(formFocus)].enabled)
                    {
                        pressing = formFocus;
                    }
                    else if (key != SDLK_SPACE)
                    {
                        // Space only presses the button under the focus. In a
                        // text field it is a space, and typing one should not
                        // submit the form.
                        for (size_t i = 0; i < activeForm.rows.size(); ++i)
                        {
                            if (activeForm.rows[i].kind == mh::FormRowKind::Button &&
                                activeForm.rows[i].enabled)
                            {
                                pressing = static_cast<int>(i);
                                break;
                            }
                        }
                    }

                    if (pressing >= 0 && optionsOpen)
                    {
                        optionsOpen = false;
                    }
                    else if (pressing >= 0 && link)
                    {
                        link->submitForm(pressing, formValues);
                    }
                }
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && formShown)
            {
                float ndcX = 0.0f;
                float ndcY = 0.0f;
                formPressed = -1;
                if (pointerNdc(event.button.x, event.button.y, ndcX, ndcY))
                {
                    // An unfolded choice takes the click first: on one of its
                    // options, that option is picked; anywhere else it folds
                    // up, and a click on its own box while open is a fold
                    // rather than a second unfold.
                    bool taken = false;
                    const int wasOpen = formOpenChoice;
                    if (formOpenChoice >= 0)
                    {
                        for (size_t i = 0; i < formChoiceHits.size(); ++i)
                        {
                            if (formChoiceHits[i].holds(ndcX, ndcY))
                            {
                                pickChoice(formOpenChoice, formChoiceHitOption[i]);
                                taken = true;
                                break;
                            }
                        }
                        formOpenChoice = -1;
                    }
                    for (size_t i = 0; !taken && i < formChoiceBoxes.size(); ++i)
                    {
                        if (formChoiceBoxes[i].enabled && formChoiceBoxes[i].holds(ndcX, ndcY))
                        {
                            formFocus = formChoiceBoxRow[i];
                            if (formChoiceBoxRow[i] != wasOpen)
                            {
                                formOpenChoice = formChoiceBoxRow[i];
                            }
                            taken = true;
                        }
                    }
                    if (taken)
                    {
                        dragging = false;
                        continue;
                    }
                    // A click in a field puts the caret there.
                    for (size_t i = 0; i * 4 + 3 < formFieldRects.size(); ++i)
                    {
                        const float* rect = &formFieldRects[i * 4];
                        if (rect[2] > 0.0f && ndcX >= rect[0] && ndcX < rect[0] + rect[2] &&
                            ndcY >= rect[1] && ndcY < rect[1] + rect[3])
                        {
                            formFocus = static_cast<int>(i);
                        }
                    }

                    for (size_t i = 0; i < formButtons.size(); ++i)
                    {
                        if (formButtons[i].enabled && formButtons[i].holds(ndcX, ndcY))
                        {
                            formPressed = static_cast<int>(i);
                        }
                    }
                }

                // The form owns this press either way: a click on a login
                // screen should never also swing the camera behind it.
                dragging = false;
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && formShown)
            {
                float ndcX = 0.0f;
                float ndcY = 0.0f;
                if (formPressed >= 0 && static_cast<size_t>(formPressed) < formButtons.size() &&
                    pointerNdc(event.button.x, event.button.y, ndcX, ndcY) &&
                    formButtons[static_cast<size_t>(formPressed)].holds(ndcX, ndcY))
                {
                    // Pressed and released on the same button, the way a button
                    // is meant to work - a press that slides off is not a click.
                    //
                    // The options menu is answered here, not sent: it is the
                    // renderer's own form. This used to require a link, so its
                    // CLOSE did nothing at all - and nothing at all in the
                    // standalone viewer, which never has one.
                    if (optionsOpen)
                    {
                        optionsOpen = false;
                    }
                    else if (link)
                    {
                        link->submitForm(formButtonRow[static_cast<size_t>(formPressed)], formValues);
                    }
                }
                formPressed = -1;
            }
            else if (event.type == SDL_EVENT_TEXT_INPUT && typing)
            {
                if (!swallowText.empty())
                {
                    const bool sameKey = event.text.text && swallowText == event.text.text;
                    swallowText.clear();
                    if (sameKey)
                    {
                        continue;
                    }
                }
                typed += event.text.text;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && typing)
            {
                // While typing, the keyboard belongs to the line and nothing
                // else - or w a s d walk the character out from under you
                // mid-sentence.
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                {
                    if (!typed.empty() && link)
                    {
                        link->submitChat(typed);
                    }
                    typed.clear();
                    typing = false;
                    SDL_StopTextInput(window);
                }
                else if (event.key.key == SDLK_ESCAPE)
                {
                    typed.clear();
                    typing = false;
                    SDL_StopTextInput(window);
                }
                else if (event.key.key == SDLK_BACKSPACE && !typed.empty())
                {
                    typed.pop_back();
                }
            }
            else if (event.type == SDL_EVENT_KEY_DOWN)
            {
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                {
                    // Return opens the line, the way the game does. Only when
                    // there is a client to say it to.
                    if (link)
                    {
                        typing = true;
                        typed.clear();
                        SDL_StartTextInput(window);
                    }
                }
                else if (link && (event.key.key == SDLK_SLASH || event.key.key == SDLK_EXCLAIM ||
                                  (event.key.key == SDLK_1 && (event.key.mod & SDL_KMOD_SHIFT))))
                {
                    // Shift and 1, as well as SDLK_EXCLAIM. On most layouts
                    // there is no key that produces ! on its own, so the
                    // keycode for it never arrives and only the shifted digit
                    // does - which is why this did nothing at all.
                    // Almost everything typed here starts with one of these -
                    // / for the client's own commands, ! for the server's - so
                    // they open the line and put themselves in it rather than
                    // opening an empty one to type them into.
                    typing = true;
                    typed = event.key.key == SDLK_SLASH ? "/" : "!";
                    SDL_StartTextInput(window);

                    // The character that opened the line arrives again as text
                    // input a moment later, and would be doubled.
                    swallowText = typed;
                }
                else if (event.key.key == SDLK_ESCAPE)
                {
                    // Escape shuts the topmost thing that is open, and nothing
                    // at all when nothing is. It used to quit the game, which
                    // is what every other window on the machine does with it
                    // and what no game does - the reflex after opening a panel
                    // is to press escape, and here that closed the client and
                    // dropped the session with it. Quitting is the window's
                    // close button, or the usual key for it.
                    if (equipmentOpen)
                    {
                        equipmentOpen = false;
                    }
                    else if (inventoryOpen)
                    {
                        inventoryOpen = false;
                        contextSlot = -1;
                    }
                    else if (optionsOpen)
                    {
                        optionsOpen = false;
                    }
                }
                else if (event.key.key == SDLK_TAB)
                {
                    // Nearest first, and round again from the end. Anything
                    // further than thirty yalms is not worth cycling past -
                    // that is beyond where a name is drawn.
                    std::vector<std::pair<float, uint32_t>> near;
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        const float dx = entity.x - characterAt.x;
                        const float dz = entity.z - characterAt.z;
                        const float distance = std::sqrt(dx * dx + dz * dz);
                        if (distance > 0.01f && distance <= 30.0f)
                        {
                            near.emplace_back(distance, entity.id);
                        }
                    }
                    std::sort(near.begin(), near.end());

                    if (near.empty())
                    {
                        targetId = 0;
                        following = false;
                    }
                    else
                    {
                        size_t at = near.size();
                        for (size_t i = 0; i < near.size(); ++i)
                        {
                            if (near[i].second == targetId)
                            {
                                at = i;
                                break;
                            }
                        }

                        // Past the last one is nobody, so tab can let go as
                        // well as take hold.
                        if (at + 1 < near.size())
                        {
                            targetId = near[at + 1].second;
                        }
                        else if (at == near.size())
                        {
                            targetId = near[0].second;
                        }
                        else
                        {
                            targetId = 0;
                            following = false;
                        }
                    }
                }
                else if (event.key.key == SDLK_O)
                {
                    // The camera orbit, which tab used to hold. Tab is worth
                    // more as a target key: it is what the game uses.
                    camera.orbiting = !camera.orbiting;
                }
                else if (event.key.key == SDLK_V)
                {
                    following = targetId != 0 && !following;
                }
                else if (event.key.key == SDLK_MINUS || event.key.key == SDLK_EQUALS ||
                         event.key.key == SDLK_PLUS)
                {
                    // Minus and equals, because equals is the unshifted plus
                    // and nobody holds shift to turn music up. Shift *is* the
                    // sound control, though - two levels, one pair of keys,
                    // until there is an options menu to put them in.
                    const float step = event.key.key == SDLK_MINUS ? -0.05f : 0.05f;
                    if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0)
                    {
                        soundVolume = std::clamp(soundVolume + step, 0.0f, 1.0f);
                        if (link)
                        {
                            link->noteSettings({musicVolume, soundVolume, radarTurns, uiScaleChoice});
                        }
                        std::printf("sound volume %.0f%% (ambience %.0f%%)\n", soundVolume * 100.0f,
                                    soundVolume * 30.0f);
                    }
                    else
                    {
                        musicVolume = std::clamp(musicVolume + step, 0.0f, 1.0f);
                        music.setVolume(musicVolume);
                        if (link)
                        {
                            link->noteSettings({musicVolume, soundVolume, radarTurns, uiScaleChoice});
                        }
                        std::printf("music volume %.0f%%\n", musicVolume * 100.0f);
                    }
                }
                else if (event.key.key == SDLK_M)
                {
                    radarTurns = !radarTurns;
                    if (link)
                    {
                        link->noteSettings({musicVolume, soundVolume, radarTurns, uiScaleChoice});
                    }
                    std::printf(radarTurns ? "radar turns with you\n" : "radar holds north up\n");
                }
                else if (event.key.key == SDLK_R)
                {
                    autoRun = !autoRun;
                    std::printf(autoRun ? "auto-run on\n" : "auto-run off\n");
                }
                // Deliberately not gated on being dead.
                //
                // A corpse cannot walk, act or use an ability - the server
                // turns all of it away - so a dead player has no way to say "I
                // am over here, and I would like a raise". Two things do still
                // get through, and this key sends both.
                //
                // Alive it is a jump. Dead it is a wave and a shove: the client
                // turns the request into an emote (see FfxiGameSession's
                // JumpAsync), and the body drags itself a little way along the
                // ground here, which the client reports like any other movement.
                // LandSandBoat relays both - its jump, motion and position
                // handlers all check only that you are not mid-event, never
                // that you are alive. See 0x11d_jump.cpp, 0x05d_motion.cpp and
                // 0x015_pos.cpp.
                //
                // What it looks like from here is a twitch rather than a hop:
                // the pose below overrules the jump clip on this same frame and
                // restarts the death clip, so the body flinches where it lies.
                // jumpUntil rate-limits it to one per clip, so it cannot be
                // held down into a seizure.
                else if (event.key.key == SDLK_SPACE && driving && jumpClip)
                {
                    // Only while driving: flying the camera, space is still
                    // up. Restarting mid-jump would reset the clip on every
                    // repeat of a held key, so an unfinished one is left to
                    // land first.
                    const float now = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
                    if (jumpUntil <= now)
                    {
                        if (dead)
                        {
                            // Rewound to the last few frames of the death clip,
                            // never restarted and never handed to the jump.
                            //
                            // Either of those puts the clip back at frame zero,
                            // and frame zero of a death is the character on
                            // their feet - so a press stood the corpse up and
                            // dropped it again, which is a resurrection, not a
                            // twitch. The tail is the body's last settle onto
                            // the ground, which is exactly the movement wanted.
                            if (deadClip && deadClip->frames > kCorpseTwitchFrames)
                            {
                                animationOffset =
                                    now - static_cast<float>(deadClip->frames - kCorpseTwitchFrames) *
                                              deadClip->frameSeconds();
                            }

                            // And the drag. Half a walking pace, along the way
                            // the body already faces, through the same
                            // collision check a step takes - a corpse hauling
                            // itself through a wall would be worse than one
                            // that cannot move at all. The height is left to
                            // the fall below, which settles it back down.
                            if (character)
                            {
                                const mh::Vec3 wanted{characterAt.x + std::sin(characterFacing) * kCorpseDrag,
                                                      characterAt.y,
                                                      characterAt.z + std::cos(characterFacing) * kCorpseDrag};
                                characterAt = collision.empty() || noclip
                                                  ? wanted
                                                  : collision.move(characterAt, wanted, 0.5f);
                            }
                        }
                        else
                        {
                            playing = jumpClip;
                            animationOffset = now;

                            // Upward is negative here: fallSpeed is how fast
                            // the ground is being approached, and the fall code
                            // below integrates it either way.
                            fallSpeed = -kJumpSpeed;
                        }

                        jumpUntil = now + static_cast<float>(jumpClip->frames) * jumpClip->frameSeconds();

                        // And tell the client, which is the only half of this
                        // that talks to the server. Without it the jump - or
                        // the wave it becomes when dead - is ours alone and
                        // nobody else ever sees it.
                        if (link)
                        {
                            link->requestJump();
                        }
                    }
                }
                else if (event.key.key == SDLK_E && driving && link)
                {
                    // Talk to whoever is closest in front.
                    //
                    // The real client targets first and acts second; this is
                    // the short version, because a target you cannot see the
                    // name of is not worth the extra step yet. In front rather
                    // than merely near, so standing between two NPCs picks the
                    // one being faced instead of whichever happens to be a few
                    // centimetres closer.
                    const float facing = camera.yaw;
                    const float aheadX = -std::sin(facing);
                    const float aheadZ = -std::cos(facing);

                    uint32_t chosen = 0;
                    float best = 0.0f;
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        const float dx = entity.x - characterAt.x;
                        const float dz = entity.z - characterAt.z;
                        const float distance = std::sqrt(dx * dx + dz * dz);
                        if (distance < 0.01f || distance > 6.0f)
                        {
                            continue;
                        }

                        // How squarely it is in front, as a cosine.
                        const float towards = (dx / distance) * aheadX + (dz / distance) * aheadZ;
                        if (towards < 0.5f)
                        {
                            continue;   // off to the side or behind
                        }

                        const float score = towards / distance;
                        if (score > best)
                        {
                            best = score;
                            chosen = entity.id;
                        }
                    }

                    if (chosen != 0)
                    {
                        link->requestTalk(chosen);
                        facingMe = chosen;
                        facingMeStarted = false;
                    }
                }
                else if (event.key.key == SDLK_KP_MULTIPLY)
                {
                    // Moved off the numpad minus, which retail uses to open its
                    // menu and which now does that here.
                    walkByDefault = !walkByDefault;
                    std::printf("%s\n", walkByDefault ? "walking" : "running");
                }
                else if (event.key.key == SDLK_T && monorail.present())
                {
                    // On and off the train. The client will do this from a
                    // conversation with somebody standing at a stop; this is
                    // how to try it without one, which means it cannot go
                    // through the link - the standalone viewer has none.
                    ridingHere = !ridingHere;
                    if (link)
                    {
                        link->setRiding(ridingHere);
                    }
                    std::printf("%s the train\n", ridingHere ? "boarded" : "left");

                    // Announced the way a conductor would, in the chat log
                    // where announcements go. It names where the line runs when
                    // the zone has a name to give - the standalone viewer is
                    // often handed a DAT and nothing else.
                    std::string said;
                    if (ridingHere)
                    {
                        said = "All aboard! The train";
                        if (currentZoneName && !currentZoneName->empty())
                        {
                            said += " through " + *currentZoneName;
                        }
                        said += ".";
                    }
                    else
                    {
                        said = "Mind the step.";
                    }

                    if (link)
                    {
                        link->pushChat(said);
                    }
                    else
                    {
                        viewerChat.push_back(said);
                        while (viewerChat.size() > 8)
                        {
                            viewerChat.erase(viewerChat.begin());
                        }
                    }
                }
                else if (event.key.key == SDLK_P)
                {
                    const mh::Vec3 at = camera.eye();
                    std::printf("at %.1f %.1f %.1f   zone y runs %.1f to %.1f\n", at.x, at.y, at.z,
                                zone->boundsMin.y, zone->boundsMax.y);
                }
                else if (event.key.key == SDLK_N)
                {
                    noclip = !noclip;
                    fallSpeed = 0.0f;
                    std::printf("collision %s\n", noclip ? "off - walking through walls" : "on");
                }
                else if (event.key.key == SDLK_U && character)
                {
                    // Back up the trail. Three samples is a second and a half
                    // of walking, which clears anything that catches a corner
                    // without throwing away where you were going.
                    for (int back = 0; back < 3 && breadcrumbs.size() > 1; ++back)
                    {
                        breadcrumbs.pop_back();
                    }
                    if (!breadcrumbs.empty())
                    {
                        placeCharacter(breadcrumbs.back(), 20.0f);
                        std::printf("unstuck to %.1f %.1f %.1f\n", characterAt.x, characterAt.y, characterAt.z);
                    }
                }
                else if (event.key.key == SDLK_F && character)
                {
                    driving = !driving;
                    camera.orbiting = driving;
                    if (!driving)
                    {
                        camera.position = camera.eye();
                    }
                    std::printf("%s\n", driving ? "driving the character" : "flying the camera");
                }
                else if (event.key.key == SDLK_C && character)
                {
                    // Stand the character where the camera is. Nothing knows
                    // where the ground is yet, so putting it somewhere useful
                    // is a matter of walking there and pressing a key.
                    const mh::Vec3 at = camera.eye();
                    placeCharacter(at, 60.0f);
                    std::printf("character moved to %.1f %.1f %.1f\n", at.x, at.y, at.z);
                }
            }
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                configure(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                float ndcX = 0.0f;
                float ndcY = 0.0f;
                const bool onBox = deathBoxShown && pointerNdc(event.button.x, event.button.y, ndcX, ndcY) &&
                                   ndcX >= deathPanel[0] && ndcX < deathPanel[0] + deathPanel[2] &&
                                   ndcY >= deathPanel[1] && ndcY < deathPanel[1] + deathPanel[3];

                deathPressed = -1;
                if (onBox)
                {
                    for (int i = 0; i < mh::kDialogButtons; ++i)
                    {
                        if (deathButtons[i].enabled && deathButtons[i].holds(ndcX, ndcY))
                        {
                            deathPressed = i;
                        }
                    }
                }

                // A press on the box belongs to the box. Otherwise the same
                // press also grabs the camera, and answering the one question
                // a dead character is allowed to answer swings the view round
                // while you do it.
                dragging = !onBox;
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            {
                // A press and release without much movement is a click; with
                // movement it was the camera being turned. Without that
                // distinction every look-around ends by talking to whoever the
                // cursor happened to land on.
                float upX = 0.0f;
                float upY = 0.0f;
                // A right click is the menu, and nothing else: it does not
                // reach the world, and it does not act on what it opens.
                if (event.button.button == SDL_BUTTON_RIGHT)
                {
                    contextSlot = -1;
                    contextConfirmDrop = false;
                    if (!dragMoved && inventoryOpen &&
                        pointerNdc(event.button.x, event.button.y, upX, upY))
                    {
                        for (const InventoryHit& hit : inventoryHits)
                        {
                            if (hit.kind == 8 && hit.holds(upX, upY))
                            {
                                contextSlot = hit.value;
                                break;
                            }
                        }
                    }
                    dragging = false;

                    // On to the next event rather than out of the loop: the
                    // queue is drained once a frame, and leaving early would
                    // hold every event behind this one until the next.
                    continue;
                }

                // The bags first, and only what they were drawn over: a
                // click that lands on the panel belongs to the panel, not to
                // whatever is standing behind it in the world.
                bool tookClick = false;
                if (!dragMoved && pointerNdc(event.button.x, event.button.y, upX, upY))
                {
                    for (const InventoryHit& hit : inventoryHits)
                    {
                        if (!hit.holds(upX, upY))
                        {
                            continue;
                        }

                        if (hit.kind == 0)
                        {
                            inventoryTab = hit.value;
                            inventoryPage = 0;
                        }
                        else if (hit.kind == 1)
                        {
                            inventoryPage += hit.value;
                        }
                        else if (hit.kind == 2)
                        {
                            // Cycles rather than opening a list: five headings
                            // along the top would take more room than the tabs.
                            inventoryOrder = static_cast<InventoryOrder>(
                                (static_cast<int>(inventoryOrder) + 1) % 5);
                            inventoryPage = 0;
                        }
                        else if (hit.kind == 3)
                        {
                            inventoryOpen = !inventoryOpen;
                            contextSlot = -1;
                        }
                        else if (hit.kind == 4)
                        {
                            optionsOpen = !optionsOpen;
                        }
                        else if (hit.kind == 5 && link && contextSlot >= 0)
                        {
                            std::printf("equip: container %d slot %d into equipment slot %d\n",
                                        contextContainer, contextSlot, hit.value);
                            link->requestInventoryAction(
                                {mh::ViewerLink::InventoryAction::Kind::Equip,
                                 static_cast<uint8_t>(contextContainer),
                                 static_cast<uint8_t>(contextSlot),
                                 static_cast<uint8_t>(hit.value), 0});
                            contextSlot = -1;
                            contextConfirmDrop = false;
                        }
                        else if (hit.kind == 6 && link && contextSlot >= 0)
                        {
                            // The first press only asks. Nothing is sent until
                            // the second one, because the server destroys the
                            // item the moment the packet lands.
                            if (!contextConfirmDrop)
                            {
                                contextConfirmDrop = true;
                            }
                            else
                            {
                                link->requestInventoryAction(
                                    {mh::ViewerLink::InventoryAction::Kind::Drop,
                                     static_cast<uint8_t>(contextContainer),
                                     static_cast<uint8_t>(contextSlot), 0, 0});
                                contextSlot = -1;
                                contextConfirmDrop = false;
                            }
                        }
                        else if (hit.kind == 7)
                        {
                            contextSlot = -1;
                            contextConfirmDrop = false;
                        }
                        else if (hit.kind == 9)
                        {
                            equipmentOpen = !equipmentOpen;
                        }
                        else if (hit.kind == 10)
                        {
                            equipmentSlot = equipmentSlot == hit.value ? -1 : hit.value;
                        }
                        else if (hit.kind == 11 && link)
                        {
                            std::printf("equip: container %d slot %d into equipment slot %d\n",
                                        (hit.value >> 8) & 0xFF, hit.value & 0xFF, (hit.value >> 16) & 0xFF);
                            link->requestInventoryAction(
                                {mh::ViewerLink::InventoryAction::Kind::Equip,
                                 static_cast<uint8_t>((hit.value >> 8) & 0xFF),
                                 static_cast<uint8_t>(hit.value & 0xFF),
                                 static_cast<uint8_t>((hit.value >> 16) & 0xFF), 0});
                        }
                        else if (hit.kind == 12 && link)
                        {
                            // Taking something off is the same packet with the
                            // item slot set to zero.
                            //
                            // Not 255. That is the server's "nothing is worn
                            // here" when it tells *us* what is equipped, and
                            // the obvious thing to send back - but the handler
                            // reads any slot above zero as an equip, so 255
                            // asked it to wear whatever is in slot 255, found
                            // nothing, and did nothing. The two directions use
                            // different numbers for the same idea.
                            link->requestInventoryAction(
                                {mh::ViewerLink::InventoryAction::Kind::Equip, 0, 0,
                                 static_cast<uint8_t>(hit.value), 0});
                        }
                        else if (hit.kind == 8)
                        {
                            // A left click on a slot closes the menu rather
                            // than acting: the menu is what acts.
                            contextSlot = -1;
                            contextConfirmDrop = false;
                        }

                        tookClick = true;
                        dragging = false;
                        break;
                    }
                }

                if (tookClick)
                {
                    // Handled.
                }
                else if (!dragMoved && pointerNdc(event.button.x, event.button.y, upX, upY) &&
                    optionsButton[2] > 0.0f && upX >= optionsButton[0] &&
                    upX < optionsButton[0] + optionsButton[2] && upY >= optionsButton[1] &&
                    upY < optionsButton[1] + optionsButton[3])
                {
                    optionsOpen = !optionsOpen;
                    dragging = false;
                }
                else if (!dragMoved && link && pointerNdc(event.button.x, event.button.y, upX, upY))
                {
                    for (const CornerLink& chip : cornerLinks)
                    {
                        if (chip.target != mh::ViewerLink::Link::None && chip.holds(upX, upY))
                        {
                            link->chooseLink(chip.target);
                            dragging = false;
                            break;
                        }
                    }
                }

                if (dragging && !dragMoved && link && pointerNdc(event.button.x, event.button.y, upX, upY))
                {
                    if (uint32_t clicked = pickAt(upX, upY))
                    {
                        targetId = clicked;
                        link->requestTalk(clicked);
                        facingMe = clicked;
                        facingMeStarted = false;
                    }
                    else
                    {
                        targetId = 0;   // clicking the ground clears the target
                    }
                }

                dragging = false;
                dragMoved = false;

                // Pressed and released on the same button. Sliding off one
                // before letting go is how a mis-click is taken back, which
                // every other program on the machine already agrees about.
                float ndcX = 0.0f;
                float ndcY = 0.0f;
                if (deathPressed >= 0 && deathBoxShown && link &&
                    pointerNdc(event.button.x, event.button.y, ndcX, ndcY) &&
                    deathButtons[deathPressed].enabled && deathButtons[deathPressed].holds(ndcX, ndcY))
                {
                    link->chooseDeath(deathButtons[deathPressed].choice);
                }
                deathPressed = -1;
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && !dragging)
            {
                // What the cursor is over, so it can say so before a click.
                float overX = 0.0f;
                float overY = 0.0f;
                hoverId = pointerNdc(event.motion.x, event.motion.y, overX, overY) ? pickAt(overX, overY) : 0;
                SDL_SetCursor(hoverId != 0 ? handCursor : arrowCursor);
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && dragging)
            {
                // A few pixels of slack, because a hand on a mouse is never
                // perfectly still and a click that turns the camera a hair
                // should still count as a click.
                if (std::fabs(event.motion.xrel) + std::fabs(event.motion.yrel) > 3.0f)
                {
                    dragMoved = true;
                }

                camera.look(-event.motion.xrel * 0.005f, -event.motion.yrel * 0.005f);
            }
            else if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                // Driving a character, the camera lives in the same 1.5 to 25
                // as the numpad keys. Flying, it is surveying a whole zone and
                // wants the zone's own scale. Sharing one floor meant the
                // wheel stopped a long way short of the character in a city,
                // where a twentieth of the radius is still tens of units.
                if (driving)
                {
                    wantedDistance = std::clamp(wantedDistance * (event.wheel.y > 0 ? 0.9f : 1.1f), 1.5f, 25.0f);
                    camera.distance = wantedDistance;
                }
                else
                {
                    camera.distance = std::clamp(camera.distance * (event.wheel.y > 0 ? 0.9f : 1.1f),
                                                 radius * 0.05f, radius * 12.0f);
                }
            }
        }

        // The server's clock when it gave us one, our own otherwise.
        //
        // Vana'diel runs 25 times real time - a minute of it is 2.4 seconds -
        // so the seed only has to arrive once and then be advanced. Without
        // this the renderer invented a day at its own rate, and two clients
        // side by side showed different hours and different light, which is
        // precisely when you want them to agree.
        int clockMinutes = 0;

        // What the client has said the server's clock is, if it has said
        // anything. Read before the branches because two of them want it.
        uint32_t liveClock = 0;
        uint64_t clockSetAtNs = 0;
        const bool haveLiveClock = link && link->serverClock(liveClock, clockSetAtNs);

        // A pinned hour holds only until the server says what time it is -
        // the sign-in backdrop is held at late afternoon on purpose, and that
        // used to follow the player into the world, where the clock read
        // 17:00 for as long as they played. MOGHOUSE_TIME in the environment
        // is a deliberate pin and keeps holding.
        static const bool pinnedByEnvironment = std::getenv("MOGHOUSE_TIME") != nullptr;
        if (timeFixed && (pinnedByEnvironment || !haveLiveClock))
        {
            clockMinutes = fixedMinutes;
        }
        else if (haveLiveClock)
        {
            // Counted from when the client handed the clock over, not from
            // startup - the window is open through the whole sign-in, and that
            // time is not time the world has passed through.
            const uint64_t elapsed = (SDL_GetTicksNS() - clockSetAtNs) / 1000000000ull;
            vanaSeconds = (static_cast<uint64_t>(liveClock) + elapsed) * 25ull;
            clockMinutes = static_cast<int>((vanaSeconds / 60ull) % 1440ull);
        }
        else if (options.serverClock)
        {
            // The server sends Earth seconds since the Vana'diel epoch, not
            // Vana'diel seconds - earth_time.h says so in as many words - so
            // the seed is multiplied up rather than used as it stands. Without
            // the 25 the clock both starts in the wrong place and then runs at
            // a twenty-fifth of the right speed.
            const uint64_t elapsed = SDL_GetTicksNS() / 1000000000ull;
            vanaSeconds = (static_cast<uint64_t>(*options.serverClock) + elapsed) * 25ull;
            clockMinutes = static_cast<int>((vanaSeconds / 60ull) % 1440ull);
        }
        else
        {
            clockMinutes = static_cast<int>((SDL_GetTicksNS() / 1000000ull / 42ull) % 1440ull);
        }

        // Which lighting the frame is under. Indoors the room's own set wins;
        // outdoors, and in any room that did not ship one, this is the zone's.
        //
        // Judged from where the character stands, not from the camera. The
        // camera hangs back behind the character and in a narrow street ends
        // up inside the building behind them, and then the whole of Southern
        // San d'Oria was lit as the inside of one shop - every wall, the sky
        // included, one shade of teal. Only the standalone viewer with no
        // character flies the camera on its own, and there the eye is all
        // there is.
        const mh::Vec3 lightingAt = character ? characterAt : camera.eye();
        const ffxi::Lighting* active = &lighting;
        int activeRoom = -1;
        for (size_t i = 0; i < interiors.size(); ++i)
        {
            // Only something the size of a room counts as one. Southern San
            // d'Oria's sub-files include four that span the whole zone - a
            // district each, not a building - and using the lighting of one
            // of those as "indoors" lit every street, and the sky, one shade
            // of teal.
            const mh::InteriorLighting& candidate = interiors[i];
            const float across = std::max(candidate.boundsMax.x - candidate.boundsMin.x,
                                          candidate.boundsMax.z - candidate.boundsMin.z);
            if (across > kRoomAtMost || candidate.lighting.empty())
            {
                continue;
            }
            if (candidate.contains(lightingAt))
            {
                active = &interiors[i].lighting;
                activeRoom = static_cast<int>(i);
                break;
            }
        }
        // Which rooms are drawn: the ones the player is in or at the door of.
        // A district-sized "room" is the outdoors and is always drawn.
        hiddenDraws.clear();
        for (const mh::InteriorLighting& room : interiors)
        {
            if (room.drawCount == 0)
            {
                continue;
            }
            const float across = std::max(room.boundsMax.x - room.boundsMin.x, room.boundsMax.z - room.boundsMin.z);
            if (across > kRoomAtMost || room.contains(lightingAt, kRoomReach))
            {
                continue;
            }
            hiddenDraws.emplace_back(room.firstDraw, room.firstDraw + room.drawCount);
        }

        if (activeRoom != lastActiveRoom)
        {
            // Fade from whatever was lighting the last frame, over a moment.
            // From the set itself rather than the room index: leaving one
            // shop straight into another is two changes in one stride.
            lightingFrom = lightingLast ? lightingLast : active;
            lightingFadeStartNs = SDL_GetTicksNS();
            lastActiveRoom = activeRoom;
            if (activeRoom >= 0)
            {
                const mh::InteriorLighting& room = interiors[static_cast<size_t>(activeRoom)];
                std::printf("lighting: inside room %d, %.0f..%.0f %.0f..%.0f %.0f..%.0f\n", activeRoom,
                            room.boundsMin.x, room.boundsMax.x, room.boundsMin.y, room.boundsMax.y, room.boundsMin.z, room.boundsMax.z);
            }
            else
            {
                std::printf("lighting: outdoors\n");
            }
        }
        lightingLast = active;

        // This frame's light: the set in force, or part way there from the
        // last one while the fade runs.
        const float lightingFade =
            lightingFrom && lightingFrom != active
                ? std::clamp(static_cast<float>(SDL_GetTicksNS() - lightingFadeStartNs) / 1e9f / kLightingFadeSeconds,
                             0.0f, 1.0f)
                : 1.0f;
        const ffxi::LightingSet frameLighting =
            lightingFade < 1.0f ? blendLighting(lightingFrom->at(clockMinutes), active->at(clockMinutes), lightingFade)
                                : active->at(clockMinutes);

        const uint64_t nowTicks = SDL_GetTicksNS();
        const float delta = static_cast<float>(nowTicks - previousTicks) / 1e9f;

        // The train, moved on and written straight into the instance buffer the
        // zone is drawn from - it is scenery like everything else in there, and
        // the only difference is that these four rows change.
        // Holding it still leaves the cars exactly where the zone placed them,
        // which is the only place their collision is: that is baked once at
        // load and does not move with them. So this is how to stand inside a
        // carriage and walk about in it - with the train running, the floor you
        // can feel is back at the depot and the one you can see is not there.
        static const bool trainHeld = SDL_getenv("MOGHOUSE_TRAIN_HOLD") != nullptr;

        if (!trainHeld && monorail.advance(delta) && instanceBuffer)
        {
            // Where it has got to, every couple of seconds. A train on a
            // thousand units of track is mostly somewhere you are not looking,
            // so "is it moving" is otherwise a question you answer by waiting.
            if (SDL_getenv("MOGHOUSE_TRAIN_WATCH") != nullptr)
            {
                static float sinceReport = 0.0f;
                sinceReport += delta;
                if (sinceReport >= 2.0f)
                {
                    sinceReport = 0.0f;
                    const mh::Vec3 at = monorail.head();
                    std::printf("train at %.1f %.1f %.1f\n", at.x, at.y, at.z);
                }
            }

            for (const mh::Monorail::Car& car : monorail.cars())
            {
                queue.WriteBuffer(instanceBuffer, static_cast<uint64_t>(car.instance) * sizeof(car.transform),
                                  car.transform, sizeof(car.transform));
            }
        }

        // The shoreline waves, on a loop of their own.
        //
        // Each is a thin strip laid along the waterline that spreads out of
        // nothing, washes its foam sheet up the sand and fades where it lies:
        // op 0x29 scales it along z, op 0x2f slides the texture, op 0x2d fades
        // it. See ffxi::EffectPlacement for how those were read.
        //
        // The scale goes through the placement matrix and the other two through
        // the effect uniform, which is why this cannot ride in the water pass -
        // that bakes its geometry flat into one world-space buffer and has
        // nowhere to put either.
        if (zone && !baseInstances.empty())
        {
            // MOGHOUSE_WAVE_PHASE pins where in the loop every wave is. A
            // screenshot is taken seconds after startup, which is phase zero -
            // and phase zero is the moment a wave is deliberately not there at
            // all, so without this a still of a working beach and a still of a
            // broken one look identical.
            static const float pinnedPhase = [] {
                const char* set = std::getenv("MOGHOUSE_WAVE_PHASE");
                return set ? std::strtof(set, nullptr) : -1.0f;
            }();
            const float phase =
                pinnedPhase >= 0.0f
                    ? pinnedPhase
                    : std::fmod(static_cast<float>(SDL_GetTicksNS() / 1000000ull) * 0.001f / kWavePeriod, 1.0f);
            for (size_t i = 0; i < zone->draws.size() && i < effectWaveBuffers.size(); ++i)
            {
                const mh::InstancedDraw& draw = zone->draws[i];
                if (!draw.wave.any() || !effectWaveBuffers[i])
                {
                    continue;
                }
                const auto at = [&](const std::string& id, float fallback) {
                    auto found = id.empty() ? curves.end() : curves.find(id);
                    return found == curves.end() ? fallback : found->second.at(phase);
                };
                // Opacity is scaled, not literal. FFXI keeps alpha at quarter
                // scale - 0.25 means fully opaque, which is why the shader
                // multiplies vertex alpha by four - and op 0x2d looks the same:
                // across the twenty-nine curves it names in this zone the
                // ceiling is 0.502, and every one of them sits at or under it.
                // Taken literally a wave peaks at a tenth opaque and the foam
                // is not there; at quarter scale it peaks at four tenths.
                //
                // Four is the convention rather than a measurement. If the
                // beach reads too strong or too weak, MOGHOUSE_WAVE_GAIN is
                // the knob and this is the number to correct.
                // MOGHOUSE_WAVE_GAIN multiplies the opacity, for finding out
                // whether a wave that cannot be seen is absent or just faint.
                static const float gain = [] {
                    const char* set = std::getenv("MOGHOUSE_WAVE_GAIN");
                    return set ? std::strtof(set, nullptr) : 1.0f;
                }();
                const float wave[4] = {at(draw.wave.u, 0.0f), at(draw.wave.v, 0.0f),
                                       std::min(at(draw.wave.opacity, 0.25f) * 4.0f * gain, 1.0f),
                                       // Marks this draw a wave for the shader.
                                       1.0f};
                // MOGHOUSE_WAVE_WATCH=1 says, once, what each wave draw is and
                // where its copies stand. A wave that is not on screen and a
                // wave that is not being drawn look the same from the beach.
                static const bool watchWaves = std::getenv("MOGHOUSE_WAVE_WATCH") != nullptr;
                if (watchWaves)
                {
                    // Once per draw for what it is, then once a second for
                    // where it has got to - a wave that is set up correctly and
                    // never advanced looks, from the beach, exactly like a wave
                    // that was never set up at all.
                    // Per draw, not shared: with one timer between them only
                    // the first wave of the frame ever reported, which reads as
                    // the others being dead.
                    static std::unordered_map<size_t, uint64_t> lastTick;
                    const uint64_t nowMs = SDL_GetTicksNS() / 1000000ull;
                    uint64_t& tick = lastTick[i];
                    if (nowMs - tick >= 2000)
                    {
                        tick = nowMs;
                        std::printf("wave %zu %-16s %2u copies  t %6.1fs phase %.3f  reaches %5.2f units  "
                                    "v %+5.2f  opacity %.3f\n",
                                    i, draw.texture.c_str(), draw.instanceCount,
                                    static_cast<double>(nowMs) * 0.001, phase, at(draw.wave.scaleZ, 1.0f), wave[1],
                                    wave[2]);
                    }
                    static std::set<size_t> reported;
                    if (reported.insert(i).second)
                    {
                        std::printf("wave draw %zu: tex %-16s %u copies, curves sz=%s a=%s u=%s v=%s\n", i,
                                    draw.texture.c_str(), draw.instanceCount, draw.wave.scaleZ.c_str(),
                                    draw.wave.opacity.c_str(), draw.wave.u.c_str(), draw.wave.v.c_str());
                        std::printf("   at phase %.2f: spread %.2f  uv %+.2f %+.2f  opacity %.3f\n", phase,
                                    at(draw.wave.scaleZ, 1.0f), wave[0], wave[1], wave[2]);
                        for (uint32_t n = 0; n < draw.instanceCount; ++n)
                        {
                            const size_t base = (static_cast<size_t>(draw.instanceOffset) + n) * 16;
                            if (base + 16 <= baseInstances.size())
                            {
                                std::printf("     copy %u at %8.1f %8.1f %8.1f\n", n, baseInstances[base + 12],
                                            baseInstances[base + 13], baseInstances[base + 14]);
                            }
                        }
                    }
                }
                queue.WriteBuffer(effectWaveBuffers[i], 4 * sizeof(float), wave, sizeof(wave));

                if (draw.wave.scaleZ.empty() || !instanceBuffer)
                {
                    continue;
                }
                // The curve is the depth the strip should reach, in the same
                // units its own bounds are in - not a factor to multiply by.
                const float spread = at(draw.wave.scaleZ, 1.0f) * draw.wave.spreadPerUnit;
                for (uint32_t n = 0; n < draw.instanceCount; ++n)
                {
                    const size_t at16 = (static_cast<size_t>(draw.instanceOffset) + n) * 16;
                    if (at16 + 16 > baseInstances.size())
                    {
                        break;
                    }
                    float matrix[16];
                    std::memcpy(matrix, baseInstances.data() + at16, sizeof(matrix));
                    // Column two is the z axis. Scaling it stretches the strip
                    // across the sand without moving where it sits.
                    // MOGHOUSE_WAVE_DIR=-1 grows the strip the other way. The
                    // model's z runs 0..11.25, so its geometry lies to one side
                    // of its own origin, and the half turn placementTransform
                    // applies decides which side that is in the world. Growing
                    // the wrong way puts the whole band out to sea and moves it
                    // further out as it swells.
                    static const float dir = [] {
                        const char* set = std::getenv("MOGHOUSE_WAVE_DIR");
                        return set ? std::strtof(set, nullptr) : 1.0f;
                    }();
                    matrix[8] *= spread * dir;
                    matrix[9] *= spread * dir;
                    matrix[10] *= spread * dir;
                    // MOGHOUSE_WAVE_LIFT raises the strip. It sits at -4.7
                    // where the sea sheet it washes over is at -3.96, so it is
                    // three quarters of a unit under the water - and where the
                    // seabed climbs to meet the shore, the sand cuts the foam
                    // off before it cuts the water. The foam then survives only
                    // out where the bottom is deep, which is the opposite of a
                    // beach.
                    static const float lift = [] {
                        const char* set = std::getenv("MOGHOUSE_WAVE_LIFT");
                        return set ? std::strtof(set, nullptr) : 0.0f;
                    }();
                    matrix[13] += lift;
                    // MOGHOUSE_WAVE_Z slides the strip along z, for finding out
                    // how far off the shore it is before working out why.
                    static const float shift = [] {
                        const char* set = std::getenv("MOGHOUSE_WAVE_Z");
                        return set ? std::strtof(set, nullptr) : 0.0f;
                    }();
                    matrix[14] += shift;
                    queue.WriteBuffer(instanceBuffer, static_cast<uint64_t>(at16) * sizeof(float), matrix,
                                      sizeof(matrix));
                }
            }
        }


        // A trail to back up along, sampled on a timer. Only recorded when the
        // character has actually moved, so standing still does not fill the
        // buffer with the same spot and turn the escape key into a no-op.
        if (character)
        {
            breadcrumbTimer += delta;
            if (breadcrumbTimer >= 0.5f)
            {
                breadcrumbTimer = 0.0f;
                const bool worthKeeping =
                    breadcrumbs.empty() ||
                    std::fabs(breadcrumbs.back().x - characterAt.x) + std::fabs(breadcrumbs.back().z - characterAt.z) >
                        0.4f;
                if (worthKeeping)
                {
                    breadcrumbs.push_back(characterAt);
                    while (breadcrumbs.size() > 32)
                    {
                        breadcrumbs.pop_front();
                    }
                }
            }
        }
        previousTicks = nowTicks;
        // With a line open the keyboard belongs to it. Held keys are read in
        // several places below, so this is cut off at the source rather than
        // guarded at each one - miss a guard and the character walks out from
        // under you mid-sentence.
        static const bool kNoKeysHeld[SDL_SCANCODE_COUNT] = {};
        // A form takes the keyboard for the same reason and more completely:
        // with a login screen up there may be no character to walk at all, and
        // a W typed into a username should never also be a step forward.
        const bool* held = (typing || formShown) ? kNoKeysHeld : SDL_GetKeyboardState(nullptr);
        // Shift inverts the walk toggle rather than setting it, so holding
        // it walks while running and runs while walking.
        const bool shiftHeld = held[SDL_SCANCODE_LSHIFT] || held[SDL_SCANCODE_RSHIFT];
        const bool walking = walkByDefault != shiftHeld;

        // Units a second, for a model 1.79 tall. Twelve read as a sprint and
        // seven still ran a little hot, so the animation and the ground speed
        // agree at these.
        float speed = (walking ? 3.2f : 6.2f) * delta;
        if (held[SDL_SCANCODE_LSHIFT] || held[SDL_SCANCODE_RSHIFT])
        {
            // Nothing here: shift means walk now, not sprint.
        }
        // Numpad 8 and 2 move, 4 and 6 turn - which is what they do in the
        // real client, where they are the movement keys rather than a second
        // set of strafes.
        //
        // Nothing moves a corpse. Cut off at the keys rather than around the
        // block that reads them, because that block also settles the body onto
        // the ground, writes the pose the GPU draws and keeps the camera behind
        // the character's shoulder - none of which stops mattering when you
        // die. Skipping the lot left the body frozen wherever it last stood
        // while the camera wandered off on its own.
        const bool backward = !dead && (held[SDL_SCANCODE_S] || held[SDL_SCANCODE_KP_2]);

        // Auto-run ends the moment you ask to go the other way, or die.
        if (backward || dead)
        {
            autoRun = false;
        }
        const bool forward = !dead && (held[SDL_SCANCODE_W] || held[SDL_SCANCODE_KP_8] || autoRun);

        // 4 turns left and 6 turns right, from the character's point of view.
        //
        // Yaw runs the other way here - forward is (sin yaw, 0, cos yaw), so a
        // rising yaw swings to the left - and adding the turn directly made
        // both keys do the opposite of what they say.
        const float turn = (held[SDL_SCANCODE_KP_4] ? 1.0f : 0.0f) - (held[SDL_SCANCODE_KP_6] ? 1.0f : 0.0f);
        if (turn != 0.0f)
        {
            camera.yaw += turn * 2.2f * delta;
        }

        const float ahead = (forward ? speed : 0.0f) - (backward ? speed : 0.0f);
        const float side = dead ? 0.0f
                                : (held[SDL_SCANCODE_D] ? speed : 0.0f) - (held[SDL_SCANCODE_A] ? speed : 0.0f);
        const float lift = (held[SDL_SCANCODE_SPACE] ? speed : 0.0f) - (held[SDL_SCANCODE_LCTRL] ? speed : 0.0f);

        // Numpad 9 and 3 pull the camera in and push it out, the way the real
        // client does it. They used to be space and ctrl, which space now
        // wants for the jump - and the mouse wheel does the same job.
        const float zoom = (held[SDL_SCANCODE_KP_9] ? speed : 0.0f) - (held[SDL_SCANCODE_KP_3] ? speed : 0.0f);

        // Two ways to move: fly the camera around to look at the zone, or
        // drive the character and have the camera follow. A character in the
        // world starts in the second.
        float moved = 0.0f;
        // Aboard the train, which carries the character rather than the
        // character walking. Everything below - the walk, the fall, the
        // collision - is skipped, because the carriage is scenery and would not
        // hold anybody up.
        const bool aboard = (ridingHere || (link && link->riding())) && monorail.present() && character;
        if (aboard)
        {
            const std::vector<mh::Monorail::Car>& cars = monorail.cars();
            const mh::Vec3 at = cars[cars.size() / 2].at;

            // Down inside the carriage rather than on the roof. The cars hang
            // from the beam, so their own origin is up at the coupling.
            characterAt = {at.x, at.y - kRideDrop, at.z};
            characterFacing = cars[cars.size() / 2].heading;
            fallSpeed = 0.0f;
            writeCharacterInstance();

            camera.orbiting = true;
            camera.target = {characterAt.x, characterAt.y + 1.2f, characterAt.z};

            // Much further out than walking allows. Riding is sightseeing: the
            // whole point is watching the country go past, and twenty-five
            // units - which is the right leash for a person on foot - keeps the
            // camera inside the carriage with them.
            wantedDistance = std::clamp(wantedDistance - zoom * 2.0f, 1.5f, 220.0f);
            camera.distance = wantedDistance;
        }
        else if (driving && character)
        {
            // Movement is relative to where the camera is looking, which is
            // what makes it read as steering a person rather than nudging a
            // point on a map.
            const mh::Vec3 forward = mh::normalise(mh::Vec3{std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)});
            const mh::Vec3 right = mh::normalise(mh::cross(forward, mh::Vec3{0.0f, 1.0f, 0.0f}));

            mh::Vec3 wanted{characterAt.x + forward.x * ahead + right.x * side, characterAt.y,
                            characterAt.z + forward.z * ahead + right.z * side};

            // Following, which walks after the target rather than after the
            // keys. Straight at them and no cleverer than that: it goes
            // through the same collision as a step, so a wall stops it the way
            // a wall stops anything, and the fall check below still owns the
            // height.
            //
            // Two yalms is close enough. Walking all the way in leaves the two
            // bodies inside each other, and the game stops short too.
            if (following)
            {
                constexpr float kFollowGap = 2.0f;
                const mh::RadarEntity* chased = nullptr;
                for (const mh::RadarEntity& entity : radarEntities)
                {
                    if (entity.id == targetId)
                    {
                        chased = &entity;
                        break;
                    }
                }

                if (chased == nullptr)
                {
                    // Gone - zoned out, died, walked past the edge of what the
                    // server tells us about. Stop rather than keep walking at
                    // where they used to be.
                    following = false;
                }
                else
                {
                    const float dx = chased->x - characterAt.x;
                    const float dz = chased->z - characterAt.z;
                    const float gap = std::sqrt(dx * dx + dz * dz);
                    if (gap > kFollowGap)
                    {
                        wanted = {characterAt.x + (dx / gap) * speed, characterAt.y,
                                  characterAt.z + (dz / gap) * speed};
                        characterFacing = std::atan2(dx, dz);
                    }
                }
            }

            if (wanted.x != characterAt.x || wanted.z != characterAt.z)
            {
                const bool ignoreCollision = collision.empty() || noclip;
                const mh::Vec3 stepped = ignoreCollision ? wanted : collision.move(characterAt, wanted, 0.5f);

                // Horizontal only. Whether there is anything to stand on is
                // settled below, by falling - a step off a ledge is a step, not
                // a refusal.
                //
                // The exception is a step onto nothing at all: no surface
                // anywhere below, which means the edge of the world rather
                // than a drop. Falling out of a zone is not a behaviour worth
                // having.
                // Searched over the character's own width rather than under a
                // single point.
                //
                // A staircase is not a continuous surface: between one tread
                // and the next there is a sliver with no walkable triangle
                // over it at all, half a unit wide on the steps outside the
                // Bastok auction house. Asking about one point lands in that
                // gap, reports the edge of the world, and refuses the step -
                // so a perfectly ordinary staircase cannot be climbed or
                // descended. Someone standing with a foot on solid ground is
                // not falling out of the zone.
                const bool intoTheVoid =
                    !ignoreCollision &&
                    !collision.nearestGround(stepped.x, stepped.z, characterAt.y, 1.0f).has_value();

                // What a step is not allowed to do, beyond hitting a wall.
                //
                // Wading is fine, swimming is not - FFXI has none - so water
                // deeper than the knee stops a step the way a wall does.
                //
                // And a drop stops it too. Blocking on walls alone is only as
                // good as the walls: where a zone has a hole in it there is no
                // wall around the hole, so a step off the edge is a legal step
                // into a fall. FFXI does not let a character walk off an edge
                // at all, and neither should this.
                //
                // Measured against the floor under each end rather than
                // against the character's own height, so it reads the same
                // whether they are standing or already in the air.
                //
                // Refused per axis rather than outright, so walking into an
                // edge at an angle slides along it instead of sticking.
                mh::Vec3 allowed = stepped;
                if (!ignoreCollision && !intoTheVoid)
                {
                    const std::optional<float> here =
                        collision.groundAt(characterAt.x, characterAt.z, characterAt.y, kFallReach);

                    // Water is not a barrier. It was one here for a while, on
                    // the reasoning that FFXI has no swimming - but the game
                    // does not stop you walking in, it stops you *getting*
                    // there, and the floor of a canal you have fallen into is
                    // as walkable as any other. Refusing the step modelled the
                    // intent rather than the behaviour, and left a character
                    // stuck at the edge of water it should have been able to
                    // cross. Collision::waterDepthAt still reports the depth;
                    // nothing acts on it.
                    // Climbing out is allowed more headroom than stepping up.
                    // A character wading a canal stands on its floor, and the
                    // quay they walked off is further above them than any kerb
                    // - so with one step height for both, they get in and
                    // cannot get out. FFXI has no swimming, which makes that a
                    // trap rather than a rule.
                    const float stepUp = collision.waterDepthAt(characterAt.x, characterAt.z, characterAt.y)
                                             ? mh::Collision::kWaterStepUp
                                             : mh::Collision::kDefaultStepUp;

                    const auto refused = [&](float x, float z) {
                        const std::optional<float> there =
                            collision.groundAt(x, z, characterAt.y, kFallReach, stepUp);
                        return here && there && (*here - *there) > kMaxStepDown;
                    };

                    if (refused(allowed.x, allowed.z))
                    {
                        allowed = {stepped.x, stepped.y, characterAt.z};
                        if (refused(allowed.x, allowed.z))
                        {
                            allowed = {characterAt.x, stepped.y, stepped.z};
                            if (refused(allowed.x, allowed.z))
                            {
                                allowed = {characterAt.x, stepped.y, characterAt.z};
                            }
                        }
                    }
                }

                if (!intoTheVoid)
                {
                    const float dx = allowed.x - characterAt.x;
                    const float dz = allowed.z - characterAt.z;
                    moved = std::sqrt(dx * dx + dz * dz);
                    characterAt.x = allowed.x;
                    characterAt.z = allowed.z;
                }

                // Facing follows the camera, not the step. Walking backwards or
                // strafing should not spin the character round, and taking the
                // direction from the movement delta means sliding along a wall
                // turns you to face it.
                characterFacing = camera.yaw;
            }

            // Stand on the floor, or fall towards it. Not while noclipping:
            // the point of it is to go through floors as well as walls.
            //
            // groundAt allows a little above the feet - the step height - so
            // walking up a stair tread reads as standing on it rather than as
            // rising through it.
            if (!collision.empty() && !noclip)
            {
                // The same extra headroom the step used. Allowing the step
                // onto a bank but not the rise onto it leaves a character
                // walking at the water's edge without ever getting out.
                const float groundStepUp =
                    collision.waterDepthAt(characterAt.x, characterAt.z, characterAt.y)
                        ? mh::Collision::kWaterStepUp
                        : mh::Collision::kDefaultStepUp;
                const std::optional<float> ground =
                    collision.groundAt(characterAt.x, characterAt.z, characterAt.y, kFallReach, groundStepUp);

                // Rising is not standing. Without the speed check the snap
                // fires on the first frame of a jump - the character is still
                // on the floor when it leaves it - and puts it straight back
                // down, which is why the jump played its whole animation
                // without ever going anywhere.
                if (ground && fallSpeed >= 0.0f && *ground >= characterAt.y - kGroundSnap)
                {
                    characterAt.y = *ground;
                    fallSpeed = 0.0f;
                }
                else if (ground)
                {
                    fallSpeed += kGravity * delta;
                    const float next = characterAt.y - fallSpeed * delta;

                    // Land rather than pass through: at speed a single frame
                    // can cover more than the remaining distance.
                    characterAt.y = next <= *ground ? *ground : next;
                    if (next <= *ground)
                    {
                        fallSpeed = 0.0f;
                    }
                }
                else
                {
                    // Nothing underneath at all, which means through the floor
                    // rather than falling: groundAt only looks a step down and
                    // a fall's worth below, so a character who has ended up
                    // under the world finds nothing and simply hangs there,
                    // for ever, watching the zone from below. It happens - a
                    // teleport into a spot with no floor, or a step that got
                    // past the walls - and there was no way back short of
                    // pressing C.
                    //
                    // closestGroundAt looks in both directions and however
                    // far, which is the question this actually wants.
                    if (const std::optional<float> rescue =
                            collision.closestGroundAt(characterAt.x, characterAt.z, characterAt.y))
                    {
                        characterAt.y = *rescue;
                        fallSpeed = 0.0f;
                    }
                }
            }
            writeCharacterInstance();

            // The camera sits behind and above the character, at head height.
            camera.orbiting = true;
            camera.target = {characterAt.x, characterAt.y + 1.2f, characterAt.z};
            // What the player asked for, which a wall may not let them have.
            wantedDistance = std::clamp(wantedDistance - zoom * 0.6f, 1.5f, 25.0f);
            camera.distance = wantedDistance;

            // Indoors the eye ends up through the wall behind the character,
            // looking at the outside of the building they are standing in -
            // which is what logging in inside a house looked like. Pull in to
            // whatever the line of sight hits first.
            //
            // The wanted distance is kept separate from the one in use: pulling
            // in must not be remembered, or stepping back into the open would
            // leave the camera wherever the last doorway put it.
            if (!collision.empty() && !noclip)
            {
                const mh::Vec3 wanted = camera.eye();
                if (auto blocked = collision.firstWallAlong(camera.target, wanted))
                {
                    const float reach = camera.distance * *blocked;
                    camera.distance = std::max(reach - 0.25f, 0.6f);
                }
            }
        }
        else
        {
            camera.walk(ahead, side, lift);
        }

        // Idle, walk or run, chosen by what the character is actually doing
        // - unless a jump is still in the air, which plays to the end.
        const float nowSeconds = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
        // Dead, so nothing else applies.
        //
        // The death clip is not chosen from movement the way idle and walk
        // are: the character is not doing anything, and letting the usual
        // choice run would stand them straight back up the moment the position
        // twitched.
        if (dead)
        {
            const ffxi::Animation* fallen = deadClip ? deadClip : idleClip;
            if (fallen && fallen != playing)
            {
                playing = fallen;
                animationOffset = nowSeconds;
            }
        }
        else if (!pinnedClip && jumpUntil <= nowSeconds)
        {
            // The same walk/run decision the speed uses, so the legs and the
            // ground always agree.
            const ffxi::Animation* moving = walking ? walkClip : runClip;
            if (!moving)
            {
                moving = walking ? runClip : walkClip;   // whichever the model has
            }
            // Resting wins over standing still, and over walking: the
            // server does not let a resting character go anywhere, so if both
            // look true the rest is the honest one.
            // Sitting is left by walking out of it, so movement wins. Resting
            // still beats both: the server will not let a resting character go
            // anywhere, so if all three look true the rest is the honest one.
            const bool satDown = link && link->sitting() && sitClip && moved <= 1e-4f;
            const ffxi::Animation* wanted = link && link->resting() && restClip ? restClip
                                            : satDown                          ? sitClip
                                            : (moved > 1e-4f ? moving : idleClip);
            if (wanted && wanted != playing)
            {
                playing = wanted;
                animationOffset = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
            }
        }

        // A capture walks the animation a frame at a time; otherwise the clock
        // is either pinned or real.
        // Whatever the client has posted since the last frame. Copied out
        // under the lock rather than read in place, so a long draw never holds
        // up the thread feeding it.
        if (link)
        {
            radarEntities = link->entities();

            // Character select: the people on the account standing in the
            // world, rather than their names in a list.
            //
            // The client says who they are and what they look like; where they
            // stand is worked out here, because it takes the zone's collision
            // to find the floor and the camera to frame them, and neither of
            // those crosses the boundary.
            // The line-up has just gone up, so start the train's run now.
            // Otherwise it has been going since the window opened and is
            // wherever a minute of signing in happened to leave it.
            const bool lineupNow = link->lineup() && !radarEntities.empty();
            if (lineupNow && !lineupWas)
            {
                monorail.approach(kLineupAlongTrack, kLineupTrainSeconds);
            }
            lineupWas = lineupNow;

            if (lineupNow)
            {
                const float spacing = 1.9f;
                const float half = static_cast<float>(radarEntities.size() - 1) * 0.5f;

                // Across the camera's view rather than along a world axis, so
                // the row faces whichever way the scene happens to look.
                const float acrossX = std::cos(lineupYaw);
                const float acrossZ = -std::sin(lineupYaw);

                float standY = centre.y;
                for (size_t i = 0; i < radarEntities.size(); ++i)
                {
                    mh::RadarEntity& who = radarEntities[i];
                    const float along = (static_cast<float>(i) - half) * spacing;

                    who.x = lineupAt.x + acrossX * along;
                    who.z = lineupAt.z + acrossZ * along;

                    // On the floor, searched from above the zone rather than
                    // from wherever the client left them - they arrive with no
                    // meaningful height at all.
                    who.y = collision.nearestGround(who.x, who.z, lineupAt.y + 8.0f, 40.0f)
                                .value_or(mh::Vec3{who.x, lineupAt.y, who.z})
                                .y;
                    standY = who.y;

                    // Facing the camera, and pickable: only a triggerable
                    // entity answers a click, and these are the one kind that
                    // has to.
                    //
                    // Half a turn from the camera's own yaw. The camera looks
                    // along its forward and stands behind the row, so a figure
                    // sharing that heading would be looking the same way the
                    // camera does - at the backs of everyone's heads.
                    who.heading = lineupYaw + 3.14159265f;
                    who.triggerable = true;
                    who.nameHidden = false;
                }

                // Framed on the row. Orbiting rather than walking, so the
                // distance widens with the number of people in it and nobody
                // falls off the edge of a full account.
                camera.orbiting = true;
                camera.target = {lineupAt.x, standY + 1.1f, lineupAt.z};
                camera.distance = 3.4f + static_cast<float>(radarEntities.size()) * 0.9f;
                camera.yaw = lineupYaw;
                camera.pitch = -0.08f;
            }

            // Nearest first, and deterministically so.
            //
            // The list arrives in the order a Dictionary happened to enumerate
            // it, which is unspecified and reshuffles whenever an entry is
            // removed and re-added - which happens constantly as entities go
            // stale and come back. Instance slots and posed bodies are handed
            // out by position in this list, and only the first
            // kMaxDrawnBodies of them are drawn, so in a city with more
            // entities than that the *set* of bodies drawn changed every time
            // the order churned. They blinked in and out, worst while moving,
            // because that is when entities come and go fastest.
            //
            // Sorting by distance makes the cap mean what it should - the
            // nearest bodies are the ones drawn - and the id breaks ties so
            // two entities at the same distance cannot swap places frame to
            // frame.
            std::sort(radarEntities.begin(), radarEntities.end(),
                      [&](const mh::RadarEntity& a, const mh::RadarEntity& b) {
                          const float ax = a.x - characterAt.x;
                          const float az = a.z - characterAt.z;
                          const float bx = b.x - characterAt.x;
                          const float bz = b.z - characterAt.z;
                          const float near = ax * ax + az * az;
                          const float far = bx * bx + bz * bz;
                          return near != far ? near < far : a.id < b.id;
                      });

            bodiesInRange = static_cast<int>(radarEntities.size());
            if (bodyDistance > 0.0f)
            {
                const float reach = bodyDistance * bodyDistance;
                bodiesInRange = 0;
                for (const mh::RadarEntity& entity : radarEntities)
                {
                    const float dx = entity.x - characterAt.x;
                    const float dz = entity.z - characterAt.z;
                    if (dx * dx + dz * dz > reach)
                    {
                        break;              // sorted, so the rest are further
                    }
                    ++bodiesInRange;
                }
            }
            bodiesInRange = std::min(bodiesInRange, mh::kMaxDrawnBodies);

            // Glided towards where the server says they are.
            //
            // Done here, before anything reads a position, so the instance
            // transforms, the nameplates and the walk/run decision all agree
            // about where an entity is.
            //
            // The server speaks a few times a second and says where somebody
            // is now, not where they are going. Easing to each position as it
            // arrived caught it up in a tenth of a second and then stood
            // still until the next - a walk read as step, pause, step. So
            // each new position is walked to over as long as the previous one
            // took to be replaced: the body arrives about when the next
            // position does, and is always moving while its owner is. It runs
            // one update behind the server, which nobody can see.
            //
            // A large jump is taken whole rather than glided through: that is
            // a teleport, a zone line or a spawn, and sliding a body across a
            // zone to meet it looks far stranger than the jump it replaces.
            for (mh::RadarEntity& entity : radarEntities)
            {
                const mh::Vec3 reported{entity.x, entity.y, entity.z};
                auto found = drawnAt.find(entity.id);
                if (found == drawnAt.end())
                {
                    DrawnEntity fresh;
                    fresh.at = fresh.from = fresh.target = reported;
                    fresh.targetTime = nowSeconds;
                    drawnAt.emplace(entity.id, fresh);
                    continue;
                }

                DrawnEntity& drawn = found->second;
                const float tx = reported.x - drawn.target.x;
                const float ty = reported.y - drawn.target.y;
                const float tz = reported.z - drawn.target.z;
                if (tx * tx + ty * ty + tz * tz > 1e-6f)
                {
                    // Somewhere new. Start from wherever the body is drawn
                    // now, so a target replaced mid-glide does not snap.
                    const float dx = reported.x - drawn.at.x;
                    const float dy = reported.y - drawn.at.y;
                    const float dz = reported.z - drawn.at.z;
                    if (dx * dx + dy * dy + dz * dz > 64.0f)
                    {
                        drawn.at = drawn.from = reported;
                    }
                    else
                    {
                        drawn.from = drawn.at;
                    }
                    drawn.target = reported;
                    // Bounded: a first update after a long quiet is not a
                    // reason to take seconds over one step, and two updates
                    // in one frame are not a reason to jump.
                    drawn.interval = std::clamp(nowSeconds - drawn.targetTime, 0.1f, 1.0f);
                    drawn.targetTime = nowSeconds;
                }

                const float progress = drawn.interval > 0.0f
                                           ? std::clamp((nowSeconds - drawn.targetTime) / drawn.interval, 0.0f, 1.0f)
                                           : 1.0f;
                drawn.at.x = drawn.from.x + (drawn.target.x - drawn.from.x) * progress;
                drawn.at.y = drawn.from.y + (drawn.target.y - drawn.from.y) * progress;
                drawn.at.z = drawn.from.z + (drawn.target.z - drawn.from.z) * progress;
                entity.x = drawn.at.x;
                entity.y = drawn.at.y;
                entity.z = drawn.at.z;
            }

            // Anything that has gone is not worth remembering a position for.
            for (auto it = drawnAt.begin(); it != drawnAt.end();)
            {
                const bool present = std::any_of(radarEntities.begin(), radarEntities.end(),
                                                 [&](const mh::RadarEntity& e) { return e.id == it->first; });
                it = present ? std::next(it) : drawnAt.erase(it);
            }

            lastFrameSeconds = nowSeconds;

            // Now that the list is final - refreshed, sorted and eased - write
            // the instance transforms from it.
            //
            // They were written earlier in the frame, before any of that, so
            // slot i held the transform of whoever was i'th *last* frame while
            // the draw loops looked up radarEntities[i] as it is *now*. When
            // the order changed - which sorting by distance makes happen every
            // time you walk past somebody - a body was drawn with another
            // entity's model at another entity's position. It read as one NPC
            // turning into a Galka and back as you ran past them.
            //
            // The comment on the easing above already claims this ordering is
            // what it is for. It is true now.
            writeCharacterInstance();

            // The list is not sorted again here, and that is the point.
            //
            // There used to be a second sort on this line - the same "nearest
            // first" as the one further up, minus its tie-break - and it ran
            // *after* the instance transforms had just been written from the
            // list. So slot i held whoever was i'th before it and the draw
            // looked up radarEntities[i] after it, and any two entities the
            // sort chose to reorder were drawn wearing each other. Two bodies
            // walking near each other have distances that cross constantly and
            // std::sort is not stable, so they traded models for a frame at a
            // time, every time they passed: a chocobo wore the NPC beside it
            // and the NPC wore the chocobo.
            //
            // The sort above already does this job and breaks ties on the id
            // so equal distances cannot flip. It runs before bodiesInRange is
            // counted and before the transforms are written, which is the
            // order the whole thing depends on.

            // The server moving us, before we report where we think we are -
            // otherwise the position we just posted would win and the move
            // would be undone on the same frame.
            float placedX = 0.0f;
            float placedY = 0.0f;
            float placedZ = 0.0f;
            float placedHeading = 0.0f;
            if (link->takePlacement(placedX, placedY, placedZ, placedHeading))
            {
                characterAt = {placedX, placedY, placedZ};
                characterFacing = placedHeading;

                // The trail is for backing out of somewhere collision has
                // trapped us. Keeping it across a teleport would walk us back
                // to a place that may be in another zone entirely.
                breadcrumbs.clear();
                breadcrumbTimer = 0.0f;

                std::printf("placed at %.1f %.1f %.1f facing %.0f degrees\n", characterAt.x, characterAt.y,
                            characterAt.z, characterFacing * 180.0f / 3.14159265f);
            }

            // Whatever the server last asked for. Checked every frame rather
            // than pushed, because the link is what the session can reach.
            // Somewhere else entirely, without closing this window.
            //
            // The frame drawn just before this one is still on screen while the
            // read happens, so what the player sees during it is whatever the
            // loading pass put there rather than a window that has gone.
            // Taken now, read after this frame has been shown.
            //
            // Reading a zone blocks this loop for as long as it takes, so a
            // loading screen has to already be on screen before it starts -
            // there is no frame to draw one in during. Deferring the read by
            // exactly one frame means what a player looks at while the world
            // is replaced is the loading pass, not the last frame of the zone
            // they have left.
            if (!pendingZone && link->takeZoneRequest(pending))
            {
                pendingZone = true;
                loadingZone = pending.zoneName;
                link->setLoading(true);
            }

            // Preferences the last session left behind, taken once.
            float storedUiScale = 0.0f;
            if (link->takeSettings(musicVolume, soundVolume, radarTurns, storedUiScale))
            {
                music.setVolume(musicVolume);
                if (!uiScalePinned)
                {
                    uiScaleChoice = storedUiScale;
                    uiScaleSetting = storedUiScale > 0.0f ? std::clamp(storedUiScale, 0.5f, 4.0f)
                                                          : scaleForWindow();
                }
            }

            bool musicChanged = false;
            const std::string wantedMusic = link->takeMusic(musicChanged);
            if (musicChanged)
            {
                music.play(wantedMusic);
            }

            link->setCharacter(characterAt.x, characterAt.y, characterAt.z, characterFacing);
            if (link->stopping())
            {
                running = false;
            }
        }

        const float animationSeconds =
            sequenceCount > 0 && playing ? static_cast<float>(std::max(shotIndex, 0)) * playing->frameSeconds()
            : pinnedFrame >= 0.0f && playing
                ? pinnedFrame * playing->frameSeconds()
                : static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f - animationOffset;

        wgpu::SurfaceTexture surfaceTexture;
        surface.GetCurrentTexture(&surfaceTexture);
        if (!surfaceTexture.texture)
        {
            continue;
        }

        const uint32_t width = surfaceTexture.texture.GetWidth();
        const uint32_t height = surfaceTexture.texture.GetHeight();

        // How much bigger the interface has to be drawn to come out the same
        // physical size on any display.
        //
        // It is laid out in pixels, and a Retina or 4K panel has around twice
        // as many of them per inch as a 1080p one - so text and buttons sized
        // in pixels came out at half the size on a Mac that they do on a
        // 1080p Windows machine, which is exactly how it looked. SDL reports
        // the window in points as well as pixels and the ratio between the two
        // is the correction; on a display where they are the same it is 1 and
        // nothing changes.
        //
        // Per frame rather than once, because a window can be dragged from one
        // display to another and the ratio changes with it.
        float uiScale = uiScaleSetting;
        {
            int pointsAcross = 0;
            int pointsDown = 0;
            SDL_GetWindowSize(window, &pointsAcross, &pointsDown);
            if (pointsDown > 0)
            {
                uiScale *= static_cast<float>(height) / static_cast<float>(pointsDown);
            }
        }

        wgpu::RenderPassColorAttachment colour{.view = surfaceTexture.texture.CreateView(),
                                               .loadOp = wgpu::LoadOp::Clear,
                                               .storeOp = wgpu::StoreOp::Store,
                                               .clearValue = {0.05, 0.07, 0.09, 1.0}};
        wgpu::RenderPassDepthStencilAttachment depth{.view = depthTexture.CreateView(),
                                                     .depthLoadOp = wgpu::LoadOp::Clear,
                                                     .depthStoreOp = wgpu::StoreOp::Store,
                                                     .depthClearValue = 1.0f};
        wgpu::RenderPassDescriptor passDescriptor{.colorAttachmentCount = 1,
                                                  .colorAttachments = &colour,
                                                  .depthStencilAttachment = &depth};

        // Everyone else's legs.
        //
        // Done before the pass opens because it uploads: each entity is posed
        // on the CPU into its own copy of the model's vertices, and the buffer
        // it draws from is written here rather than mid-pass.
        //
        // Which clip is chosen from how far the entity moved since the last
        // frame - the server sends positions, not gaits, so movement is the
        // only evidence there is. The distance is smoothed because updates
        // arrive a few times a second and a raw per-frame delta is zero on
        // every frame between them, which reads as a stutter rather than a
        // walk.
        int posedCount = 0;
        for (const mh::RadarEntity& entity : radarEntities)
        {
            // Only the ones that will actually be drawn. Skinning is the
            // expensive half - a mesh reposed on the CPU and uploaded every
            // frame - and doing it for a body past the instance cap is work
            // whose result is never submitted.
            if (posedCount >= bodiesInRange)
            {
                break;
            }

            if (!entity.hasLook() && !entity.hasModel())
            {
                continue;
            }
            ++posedCount;

            // A body that will not hold still, caught saying so.
            //
            // An NPC was reported flickering between other people's
            // appearances, and nothing in the tracker agreed: no look changed,
            // no model rebuilt. So whatever changes happens between the look
            // arriving and the body being drawn, and this is the last place to
            // watch from.
            if (entity.id == targetId && targetId != 0)
            {
                std::array<uint16_t, 7> now{};
                now[0] = entity.hasModel() ? 0xFFFFu : entity.look[0];
                now[1] = entity.hasModel() ? entity.modelId : entity.look[1];
                for (int slot = 2; slot < 7; ++slot)
                {
                    now[static_cast<size_t>(slot)] = entity.hasModel() ? 0 : entity.look[slot];
                }

                if (targetWasFor == entity.id && now != targetWas)
                {
                    std::printf("target %08X changed: %u,%u %u/%u/%u/%u/%u -> %u,%u %u/%u/%u/%u/%u\n",
                                entity.id, targetWas[0], targetWas[1], targetWas[2], targetWas[3],
                                targetWas[4], targetWas[5], targetWas[6], now[0], now[1], now[2],
                                now[3], now[4], now[5], now[6]);
                }
                targetWas = now;
                targetWasFor = entity.id;
            }

            const DrawableCharacter* model = modelForEntity(entity);

            // MOGHOUSE_LOOK_WATCH=1 reports whenever the model an entity
            // resolves to changes. A body that flickers to somebody else's
            // look and back is either the server changing its mind or this
            // resolving differently on alternate frames, and from a chair the
            // two are the same picture.
            static const bool watchLooks = std::getenv("MOGHOUSE_LOOK_WATCH") != nullptr;
            if (watchLooks)
            {
                static std::unordered_map<uint32_t, const DrawableCharacter*> lastModel;
                auto seen = lastModel.find(entity.id);
                if (seen != lastModel.end() && seen->second != model)
                {
                    std::printf("look change %08X: model %p -> %p   modelId %u  look %u,%u %u/%u/%u/%u/%u\n",
                                entity.id, static_cast<const void*>(seen->second),
                                static_cast<const void*>(model), entity.modelId, entity.look[0], entity.look[1],
                                entity.look[2], entity.look[3], entity.look[4], entity.look[5], entity.look[6]);
                }
                lastModel[entity.id] = model;
            }

            if (!model || model->loaded.animations.empty())
            {
                continue;
            }

            AnimatedEntity& state = entityPoses[entity.id];
            if (!state.vertices)
            {
                state.geometry = model->loaded.geometry;
                wgpu::BufferDescriptor descriptor{
                    .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                    .size = state.geometry.vertices.size() * sizeof(mh::Vertex)};
                state.vertices = device.CreateBuffer(&descriptor);
            }

            const float dx = entity.x - state.lastX;
            const float dz = entity.z - state.lastZ;
            const float stepped = state.placed ? std::sqrt(dx * dx + dz * dz) : 0.0f;
            state.lastX = entity.x;
            state.lastZ = entity.z;
            state.placed = true;

            // Speed over the gap between updates, not between frames.
            //
            // Positions arrive a few times a second, so most frames see no
            // movement at all. A per-frame delta is therefore zero on nearly
            // every frame, and anything derived from it crosses the walk and
            // run thresholds several times a second - restarting the clip each
            // time. Dividing the step by the time since the last one gives a
            // speed that does not depend on how often the server speaks, and
            // movingUntil holds the stride across the quiet frames between.
            if (stepped > 1e-4f)
            {
                state.speed = stepped / std::max(nowSeconds - state.lastMoveTime, 1.0f / 60.0f);
                state.lastMoveTime = nowSeconds;
                state.movingUntil = nowSeconds + 0.4f;
            }

            const auto clipNamed = [&model](const char* name) -> const ffxi::Animation* {
                auto found = model->loaded.animations.find(name);
                return found == model->loaded.animations.end() ? nullptr : &found->second;
            };

            const ffxi::Animation* idle = clipNamed("idl0");
            const ffxi::Animation* walk = clipNamed("wlk0");
            const ffxi::Animation* run = clipNamed("run0");

            // A body with clips, but none of those names, is a creature or an
            // NPC built from its own file, and such a body drawn standing
            // still while it moves is the "sliding" NPC. Say what it does
            // have, once per model, so the names can be learned from the log.
            if (!walk && !idle && !run && !state.clipsReported)
            {
                state.clipsReported = true;
                std::printf("entity %u has no idl0/wlk0/run0; its clips:", entity.id);
                for (const auto& [clipName, unused] : model->loaded.animations)
                {
                    std::printf(" %s", clipName.c_str());
                }
                std::printf("\n");
            }

            // Walking in FFXI is a little under three units a second and
            // running a little over five, so the two part company around four.
            const bool moving = nowSeconds < state.movingUntil;
            const ffxi::Animation* wanted = moving ? (state.speed > 4.0f ? run : walk) : idle;
            if (!wanted)
            {
                wanted = idle ? idle : walk;
            }
            if (!wanted)
            {
                continue;
            }

            if (wanted != state.clip)
            {
                state.clip = wanted;
                state.clipStart = nowSeconds;
            }

            // The arms belong to the same stride as the legs: a clip's upper
            // body is its own name with the trailing 0 turned into a 1.
            //
            // No upper half at all when the model has no paired clip, which is
            // what the player does too. Substituting a standing torso for a
            // missing one puts the character in two poses at once - std folds
            // the body forward, so a walking NPC spends the whole stride bowing.
            const ffxi::Animation* upper = nullptr;
            if (state.clip->name.size() == 4 && state.clip->name.back() == '0')
            {
                std::string above = state.clip->name;
                above.back() = '1';
                upper = clipNamed(above.c_str());
            }

            const float elapsed = nowSeconds - state.clipStart;
            const float frame = elapsed / state.clip->frameSeconds();
            const float upperFrame = upper ? elapsed / upper->frameSeconds() : 0.0f;

            std::vector<mh::BonePose> posed =
                mh::animatedPose(model->loaded.skeleton, *state.clip, frame, upper, upperFrame);

            // Whoever is being spoken to also looks up or down. Turning the
            // body alone leaves a tall galka talking over a tarutaru's head.
            if (entity.id == facingMe && entity.id != 0)
            {
                // Found once per skeleton: headBone walks the whole bind pose
                // and this runs every frame.
                static std::map<const ffxi::Skeleton*, int> heads;
                const ffxi::Skeleton* key = &model->loaded.skeleton;
                auto found = heads.find(key);
                if (found == heads.end())
                {
                    found = heads.emplace(key, mh::headBone(model->loaded.skeleton)).first;
                }

                if (found->second >= 0)
                {
                    const float dx = characterAt.x - entity.x;
                    const float dz = characterAt.z - entity.z;
                    const float flat = std::sqrt(dx * dx + dz * dz);

                    // Eye heights, roughly: the top of each model rather than
                    // its feet, since that is what is doing the looking.
                    const float theirs = entity.y + model->loaded.geometry.height() * 0.92f;
                    const float mine =
                        characterAt.y + (character ? character->geometry.height() : 1.8f) * 0.92f;

                    // Clamped, because a neck has limits and an NPC folded
                    // double to look at your boots is worse than one that does
                    // not quite reach.
                    //
                    // Negated, and not arbitrarily: +x runs behind the model in
                    // this space, so a positive turn about z tips the face away
                    // from whoever is being looked at. Checked against a running
                    // client, which had every head politely looking the wrong
                    // way.
                    const float wanted =
                        std::clamp(std::atan2(mine - theirs, std::max(flat, 0.4f)), -0.5f, 0.5f);
                    mh::pitchHead(posed, model->loaded.skeleton, found->second, -wanted);
                }
            }

            mh::reskin(state.geometry, posed, model->loaded.meshes);
            queue.WriteBuffer(state.vertices, 0, state.geometry.vertices.data(),
                              state.geometry.vertices.size() * sizeof(mh::Vertex));

            // Where the mesh ended up this frame. The top is where the name
            // goes; the bottom says whether the feet are on the ground, which
            // the rest pose cannot answer once a clip has moved the body.
            float top = state.geometry.vertices.empty() ? 0.0f : state.geometry.vertices[0].position[1];
            for (const mh::Vertex& vertex : state.geometry.vertices)
            {
                top = std::max(top, vertex.position[1]);
            }
            state.posedTop = top;
            state.drawn = true;
        }

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);

        {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const float tanHalfFov = std::tan(1.05f * 0.5f);
            const mh::Vec3 f = camera.orbiting ? mh::normalise(camera.lookAtPoint() - camera.eye()) : camera.forward();
            const mh::Vec3 r = mh::normalise(mh::cross(f, mh::Vec3{0.0f, 1.0f, 0.0f}));
            const mh::Vec3 u = mh::cross(r, f);

            SkyUniforms skyUniforms{};
            skyUniforms.forward[0] = f.x;
            skyUniforms.forward[1] = f.y;
            skyUniforms.forward[2] = f.z;
            skyUniforms.right[0] = r.x * tanHalfFov * aspect;
            skyUniforms.right[1] = r.y * tanHalfFov * aspect;
            skyUniforms.right[2] = r.z * tanHalfFov * aspect;
            skyUniforms.up[0] = u.x * tanHalfFov;
            skyUniforms.up[1] = u.y * tanHalfFov;
            skyUniforms.up[2] = u.z * tanHalfFov;

            const ffxi::LightingSet& skySet = frameLighting;
            for (size_t i = 0; i < 8; ++i)
            {
                skyUniforms.skyColours[i][0] = skySet.skyColours[i].r;
                skyUniforms.skyColours[i][1] = skySet.skyColours[i].g;
                skyUniforms.skyColours[i][2] = skySet.skyColours[i].b;
                skyUniforms.skyAltitudes[i][0] = skySet.skyAltitudes[i];
            }
            skyUniforms.fogColour[0] = skySet.landscapeFog.r;
            skyUniforms.fogColour[1] = skySet.landscapeFog.g;
            skyUniforms.fogColour[2] = skySet.landscapeFog.b;
            queue.WriteBuffer(skyUniformBuffer, 0, &skyUniforms, sizeof(skyUniforms));

            pass.SetPipeline(skyPipeline);
            pass.SetBindGroup(0, skyBindGroup);
            pass.Draw(3);
        }

        if (indexCount && skyIndexBuffer && effectPipeline)
        {
            // The cloud dome and the stars, around the camera, before the
            // zone so anything solid draws over them. The uniforms they read
            // are written further down for the zone; a queue write lands
            // before the pass that follows it, so they see this frame's.
            const bool night = clockMinutes < 6 * 60 || clockMinutes >= 18 * 60;
            const float dayFraction = static_cast<float>(clockMinutes) / 1440.0f;
            pass.SetVertexBuffer(0, skyVertexBuffer);
            pass.SetVertexBuffer(1, skyInstanceBuffer);
            pass.SetIndexBuffer(skyIndexBuffer, wgpu::IndexFormat::Uint32);
            for (size_t i = 0; i < skyObjects.draws.size() && i < skyObjectBindGroups.size(); ++i)
            {
                const mh::InstancedDraw& draw = skyObjects.draws[i];
                if (!skyObjectBindGroups[i])
                {
                    continue;
                }
                auto curve = draw.curve.empty() ? curves.end() : curves.find(draw.curve);
                const bool shown = curve != curves.end() ? curve->second.at(dayFraction) > 0.05f : !(draw.nightOnly && !night);
                if (!shown)
                {
                    continue;
                }
                pass.SetPipeline(draw.nightOnly ? effectAdditivePipeline : effectPipeline);
                pass.SetBindGroup(0, skyObjectBindGroups[i]);
                pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
            }
        }

        if (indexCount)
        {
            const mh::Mat4 view = mh::lookAt(camera.eye(), camera.lookAtPoint(), mh::Vec3{0, 1, 0});
            // A near plane scaled to the zone puts everything nearby inside it
            // when standing on the ground, so it is fixed rather than relative.
            const mh::Mat4 projection =
                // A far plane 20x the zone radius wastes most of the depth
                // buffer's precision on space nothing occupies, which is what
                // makes coplanar layers fight in the first place.
                mh::perspective(1.05f, static_cast<float>(width) / static_cast<float>(height), 0.25f, radius * 4.0f);

            Uniforms uniforms{};
            const mh::Mat4 viewProjection = projection * view;
            pickProjection = viewProjection;
            havePickProjection = true;
            std::memcpy(uniforms.viewProjection, viewProjection.m, sizeof(uniforms.viewProjection));
            const mh::Vec3 light = mh::normalise(mh::Vec3{0.4f, 0.8f, 0.45f});
            uniforms.lightDirection[0] = light.x;
            uniforms.lightDirection[1] = light.y;
            uniforms.lightDirection[2] = light.z;

            const ffxi::LightingSet& set = frameLighting;
            uniforms.ambient[0] = set.landscapeAmbient.r * 0.5f;
            uniforms.ambient[1] = set.landscapeAmbient.g * 0.5f;
            uniforms.ambient[2] = set.landscapeAmbient.b * 0.5f;
            uniforms.sunlight[0] = set.landscapeSunlight.r * 0.5f;
            uniforms.sunlight[1] = set.landscapeSunlight.g * 0.5f;
            uniforms.sunlight[2] = set.landscapeSunlight.b * 0.5f;
            uniforms.fogColour[0] = set.landscapeFog.r;
            uniforms.fogColour[1] = set.landscapeFog.g;
            uniforms.fogColour[2] = set.landscapeFog.b;
            uniforms.fogRange[0] = set.landscapeMinFog;
            uniforms.fogRange[1] = set.landscapeMaxFog > 0.0f ? set.landscapeMaxFog : 10000.0f;
            // Spare in the zone pass; the water pass reads it as "this is the
            // sea". The two share this buffer.
            uniforms.fogRange[2] = waterIsSea ? 1.0f : 0.0f;
            const mh::Vec3 eyePoint = camera.eye();
            uniforms.eye[0] = eyePoint.x;
            uniforms.eye[1] = eyePoint.y;
            uniforms.eye[2] = eyePoint.z;
            // Seconds since start, for the water surface.
            //
            // Its own clock, not the character's. animationSeconds counts from
            // animationOffset, which is reset every time a clip starts - so
            // the sea's time was tied to what the character happened to be
            // doing, and jumped backwards on every step taken, jump landed or
            // model rebuilt. A pinned frame or a capture sequence still pins
            // this too, because a screenshot has to be reproducible.
            uniforms.eye[3] = (sequenceCount > 0 || pinnedFrame >= 0.0f)
                                  ? animationSeconds
                                  : static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;

            // The torches near enough to matter. There are more in a zone than
            // will fit in the block - West Ronfaure has forty-two - and a lamp
            // beyond its own reach of every visible surface changes nothing, so
            // the nearest to the eye are the ones worth sending.
            {
                lampOrder.clear();
                lampOrder.reserve(lamps.size());
                for (size_t i = 0; i < lamps.size(); ++i)
                {
                    const float dx = lamps[i].x - eyePoint.x;
                    const float dy = lamps[i].y - eyePoint.y;
                    const float dz = lamps[i].z - eyePoint.z;
                    lampOrder.push_back({dx * dx + dy * dy + dz * dz, i});
                }

                const size_t take = std::min<size_t>(lampOrder.size(), kMaxLamps);
                std::partial_sort(lampOrder.begin(), lampOrder.begin() + static_cast<long>(take),
                                  lampOrder.end(),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });

                for (size_t i = 0; i < take; ++i)
                {
                    const Lamp& lamp = lamps[lampOrder[i].second];
                    uniforms.lamps[i][0] = lamp.x;
                    uniforms.lamps[i][1] = lamp.y;
                    uniforms.lamps[i][2] = lamp.z;
                    uniforms.lamps[i][3] = lamp.reach;
                }
                uniforms.lampCount[0] = static_cast<float>(take);
            }

            // With no lighting data, fall back to a plain lit look rather than
            // a black zone.
            if (lighting.empty())
            {
                uniforms.ambient[0] = uniforms.ambient[1] = uniforms.ambient[2] = 0.35f;
                uniforms.sunlight[0] = uniforms.sunlight[1] = uniforms.sunlight[2] = 0.65f;
                uniforms.fogRange[1] = 1e9f;
            }
            // shaderMode: 0 draws colour with no alpha discard,
            // 2 draws alpha as greyscale, unset is normal rendering.
            uniforms.lightDirection[3] = shaderMode;
            queue.WriteBuffer(uniformBuffer, 0, &uniforms, sizeof(uniforms));

            pass.SetVertexBuffer(0, vertexBuffer);
            pass.SetVertexBuffer(1, instanceBuffer);
            pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            // Two passes over the same list: everything solid, then the water
            // over the top of it. Water blends and does not write depth, so it
            // has to come after whatever is meant to show through it, and a
            // list in model-name order does not give that for free.
            for (int layer = 0; layer < 2; ++layer)
            {
                size_t nextHidden = 0;
                for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
                {
                    // Ranges are in draw order, as the rooms were appended, so
                    // one cursor walks them alongside the draws.
                    while (nextHidden < hiddenDraws.size() && i >= hiddenDraws[nextHidden].second)
                    {
                        ++nextHidden;
                    }
                    if (nextHidden < hiddenDraws.size() && i >= hiddenDraws[nextHidden].first)
                    {
                        continue;   // a room the player is not in
                    }

                    const mh::InstancedDraw& draw = zone->draws[i];
                    if (draw.texture.empty() && !draw.water)
                    {
                        // A mesh with no texture named is not a thing to be
                        // seen: the arch over a stair in Southern San d'Oria
                        // drawn as a cream shell is one of the game's own
                        // occlusion volumes, which its client never draws.
                        continue;
                    }
                    if (draw.effect)
                    {
                        continue;   // drawn by the effect pass below
                    }
                    const bool translucent = draw.water || draw.blend;
                    if (translucent != (layer == 1))
                    {
                        continue;
                    }
                    // cutoutMode: 0 never cuts out, 1 always, otherwise the
                    // texture's own measurement decides.
                    const bool cutout = cutoutMode == 0   ? false
                                        : cutoutMode == 1 ? true
                                                          : draw.cutout;
                    pass.SetPipeline(translucent ? translucentPipeline : (cutout ? cutoutPipeline : pipeline));
                    pass.SetBindGroup(0, batchBindGroups[i]);
                    pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
                }
            }

            // The effects: the fountain's jets and flames, the waterfalls -
            // generator-placed meshes whose texture the game scrolls. After
            // the water, blended, no depth writes. The night-only ones - the
            // flames - are left out by day; Vana'diel's night is 18:00 to
            // 6:00 here, a guess at the game's switch until it is read from
            // the file.
            if (effectPipeline)
            {
                const bool night = clockMinutes < 6 * 60 || clockMinutes >= 18 * 60;
                const float dayFraction = static_cast<float>(clockMinutes) / 1440.0f;
                for (size_t i = 0; i < zone->draws.size() && i < effectBindGroups.size(); ++i)
                {
                    const mh::InstancedDraw& draw = zone->draws[i];
                    if (!draw.effect || !effectBindGroups[i])
                    {
                        continue;
                    }
                    // The generator's own curve says whether this is lit at
                    // this hour - the jets by day, the flames by night.
                    //
                    // Southern San d'Oria's fountain is the case that makes
                    // this worth doubting. Its jets - sfn4 to sfn7, and the awa
                    // bubbles - carry real scroll rates (-0.030, -0.027,
                    // +0.021, -0.023), so they are not missing their motion.
                    // They also name `tmfs` here, which is zero until 0.28 of
                    // the day and after 0.80 - dawn and dusk - so between dusk
                    // and dawn the whole fountain is dropped and the basin
                    // stands dry. That is the "fountain audio plays but no
                    // water movement" report, filed at 00:00.
                    //
                    // What argues the reading is wrong: the sound emitters
                    // beside them, se02 and se03, carry no such curve and play
                    // all night. A fountain that is heard and not seen is not
                    // something retail would ship.
                    //
                    // MOGHOUSE_EFFECT_GATE=0 ignores the day curves entirely,
                    // which is the way to see which reading matches retail
                    // without rebuilding.
                    static const bool gateOnCurves = [] {
                        const char* set = std::getenv("MOGHOUSE_EFFECT_GATE");
                        return set == nullptr || std::strtol(set, nullptr, 10) != 0;
                    }();
                    auto curve = draw.curve.empty() ? curves.end() : curves.find(draw.curve);
                    const bool shown =
                        !gateOnCurves ||
                        (curve != curves.end() ? curve->second.at(dayFraction) > 0.05f : !(draw.nightOnly && !night));
                    if (!shown)
                    {
                        continue;
                    }
                    // A shoreline wave waits for the water. It lies a little
                    // under the sea sheet - the strips sit at -4.7 where the
                    // sea is at -3.96 - so drawn here it would be painted over
                    // by water that is nearly opaque, and the foam that is
                    // meant to be washing over the sand would be a faint stain
                    // beneath it.
                    if (draw.wave.any())
                    {
                        continue;
                    }
                    pass.SetPipeline(draw.additive && effectAdditivePipeline ? effectAdditivePipeline : effectPipeline);
                    pass.SetBindGroup(0, effectBindGroups[i]);
                    pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
                }
            }

            // The sprites: flames and glows as camera-facing quads, one frame
            // of their sheet each, rebuilt every frame and added to the scene.
            // Frames run at ten a second, a guess at the game's rate.
            if (spriteVertexBuffer && effectAdditivePipeline && !spriteBatches.empty())
            {
                const bool night = clockMinutes < 6 * 60 || clockMinutes >= 18 * 60;
                const float dayFraction = static_cast<float>(clockMinutes) / 1440.0f;
                const mh::Vec3 f = camera.orbiting ? mh::normalise(camera.lookAtPoint() - camera.eye()) : camera.forward();
                const mh::Vec3 right = mh::normalise(mh::cross(f, mh::Vec3{0.0f, 1.0f, 0.0f}));
                const mh::Vec3 up = mh::cross(right, f);
                const float seconds = static_cast<float>(SDL_GetTicksNS() / 1000000ull) * 0.001f;
                for (size_t i = 0; i < spriteInstances.size(); ++i)
                {
                    const mh::SpriteInstance& instance = spriteInstances[i];
                    auto animation = sprites.find(instance.animation);
                    bool shown = animation != sprites.end();
                    float strength = 1.0f;
                    if (shown)
                    {
                        auto curve = instance.curve.empty() ? curves.end() : curves.find(instance.curve);
                        shown = curve != curves.end() ? curve->second.at(dayFraction) > 0.05f
                                                      : !(instance.nightOnly && !night);
                    }
                    if (shown && instance.fade[3] > 0.0f)
                    {
                        // Op 0x48: in between the first two distances, out
                        // between the last two.
                        const mh::Vec3 toEye = camera.eye() - instance.centre;
                        const float distance = std::sqrt(toEye.x * toEye.x + toEye.y * toEye.y + toEye.z * toEye.z);
                        const auto ramp = [](float d, float a, float b) {
                            return b > a ? std::clamp((d - a) / (b - a), 0.0f, 1.0f) : (d >= b ? 1.0f : 0.0f);
                        };
                        strength = ramp(distance, instance.fade[0], instance.fade[1]) *
                                   (1.0f - ramp(distance, instance.fade[2], instance.fade[3]));
                        shown = strength > 0.01f;
                    }
                    for (size_t v = 0; v < 6; ++v)
                    {
                        mh::Vertex& out = spriteVertices[i * 6 + v];
                        out = mh::Vertex{};
                        if (!shown)
                        {
                            continue;   // a degenerate triangle at the origin
                        }
                        const ffxi::SpriteAnimation& sheet = animation->second;
                        const size_t frame = static_cast<size_t>(seconds * 10.0f + i * 3) % sheet.frames.size();
                        const ffxi::SpriteVertex& source = sheet.frames[frame].vertices[v];
                        // The quad is drawn in the DAT's frame, y down, so up
                        // is minus y. Scaled by the generator, set on the
                        // camera's right and up so it always faces the eye.
                        const float sx = source.position[0] * instance.scale.x;
                        const float sy = -source.position[1] * instance.scale.y;
                        const mh::Vec3 world = instance.centre + right * sx + up * sy;
                        out.position[0] = world.x;
                        out.position[1] = world.y;
                        out.position[2] = world.z;
                        out.normal[1] = 1.0f;
                        out.uv[0] = source.uv[0];
                        out.uv[1] = source.uv[1];
                        // Vertex alpha is quarter scale in the shader, so
                        // 0x40 is fully on; the distance fade scales it down.
                        const uint32_t alpha = static_cast<uint32_t>(64.0f * strength) & 0xFF;
                        out.colour = (source.colour & 0x00FFFFFFu) | (alpha << 24);
                    }
                }
                queue.WriteBuffer(spriteVertexBuffer, 0, spriteVertices.data(), spriteVertices.size() * sizeof(mh::Vertex));
                pass.SetVertexBuffer(0, spriteVertexBuffer);
                pass.SetVertexBuffer(1, spriteInstanceBuffer);
                pass.SetIndexBuffer(spriteIndexBuffer, wgpu::IndexFormat::Uint32);
                pass.SetPipeline(effectAdditivePipeline);
                for (const SpriteBatch& batch : spriteBatches)
                {
                    if (!batch.bindGroup)
                    {
                        continue;
                    }
                    pass.SetBindGroup(0, batch.bindGroup);
                    pass.DrawIndexed(batch.count, 1, batch.first, 0, 0);
                }
                // The zone's own buffers go back for whatever draws next.
                pass.SetVertexBuffer(0, vertexBuffer);
                pass.SetVertexBuffer(1, instanceBuffer);
                pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            }

            // The train's own lights: the cars drawn a second time, adding to
            // themselves rather than replacing anything. Where the texture is
            // already bright - the windows, the trim along the side - it goes
            // brighter, and where it is dark almost nothing happens, which is
            // what lit windows in a dark carriage look like without anything
            // having to know which triangles are windows.
            if (monorail.lamps() > 0.0f && glowPipeline && !monorailDraws.empty())
            {
                // A slow breath on top of the fade, so it reads as something
                // running rather than a decal.
                const float breathe = 0.88f + 0.12f * std::sin(static_cast<float>(SDL_GetTicksNS() / 1000000ull) * 0.0016f);
                // How bright the lamps burn. Worth a knob: it is the one number
                // here that is a matter of taste, and taste is easier to settle
                // by looking at two of them than by arguing about one.
                static const float lampStrength = []
                {
                    const char* set = SDL_getenv("MOGHOUSE_TRAIN_LAMPS");
                    const float parsed = set ? std::strtof(set, nullptr) : 0.80f;
                    return std::clamp(parsed, 0.0f, 2.0f);
                }();

                const float amount = monorail.lamps() * lampStrength * breathe;
                const wgpu::Color lamp{amount, amount * 0.94f, amount * 0.78f, 1.0};
                pass.SetBlendConstant(&lamp);
                pass.SetPipeline(glowPipeline);

                for (size_t index : monorailDraws)
                {
                    if (index >= batchBindGroups.size())
                    {
                        continue;
                    }
                    const mh::InstancedDraw& draw = zone->draws[index];
                    pass.SetBindGroup(0, batchBindGroups[index]);
                    pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
                }
            }

            // Bodies: the player's, and everyone else's. Either alone is
            // enough of a reason to be here - at character select there is a
            // row of people and nobody playing yet, and gating the whole lot on
            // the player's own bind groups left that row as floating names.
            if (!characterBindGroups.empty() || (!radarEntities.empty() && characterInstanceBuffer))
            {
                pass.SetVertexBuffer(1, characterInstanceBuffer);

                if (!characterBindGroups.empty())
                {
                if (playing)
                {
                    float frame = animationSeconds / playing->frameSeconds();

                    // Falling over happens once. Every clip is sampled with a
                    // wrap - see animatedPose - which is right for a walk and
                    // wrong for a death: left to loop, the body collapsed,
                    // snapped upright and collapsed again for as long as you
                    // were dead. Held on the last frame instead, so a corpse
                    // stays a corpse.
                    if (playing == deadClip && deadClip && deadClip->frames > 0)
                    {
                        frame = std::min(frame, static_cast<float>(deadClip->frames) - 1.0f);
                    }
                    // The arms belong to the same stride as the legs, so
                    // the upper body is stepped on its own frame rate but off
                    // the same clock.
                    const ffxi::Animation* upperClip = upperFor ? upperFor(playing) : nullptr;
                    const float upperFrame =
                        upperClip ? animationSeconds / upperClip->frameSeconds() : 0.0f;
                    mh::reskin(character->geometry,
                               mh::animatedPose(character->skeleton, *playing, frame, upperClip, upperFrame),
                               character->meshes);
                    queue.WriteBuffer(characterVertexBuffer, 0, character->geometry.vertices.data(),
                                      character->geometry.vertices.size() * sizeof(mh::Vertex));
                }
                pass.SetIndexBuffer(characterIndexBuffer, wgpu::IndexFormat::Uint32);

                // Instance 0 is the player, animated. The rest are everyone
                // else, from the still buffer, reached with firstInstance so
                // they read the same instance data without sharing a pose.
                for (size_t i = 0; i < character->geometry.batches.size() && i < characterBindGroups.size(); ++i)
                {
                    const mh::Batch& batch = character->geometry.batches[i];
                    pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                    pass.SetBindGroup(0, characterBindGroups[i]);

                    pass.SetVertexBuffer(0, characterVertexBuffer);
                    pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0, 0);

                    // Everyone without a model of their own, from the shared
                    // body. Drawn one at a time rather than as a run because
                    // the entities that do have models are interleaved with
                    // them, and instance slots are assigned by position in the
                    // list rather than by which model is used.
                    for (int body = 0; body < drawnBodies && entityVertexBuffer; ++body)
                    {
                        const size_t index = static_cast<size_t>(body);
                        // Only skipped if something actually built. A
                        // Chocobo's look names a race the player race table has
                        // no entry for, and dropping it would leave nothing
                        // standing there at all.
                        //
                        // This has to ask the same question the model loop
                        // asks, creatures included: testing only the equipment
                        // form drew the shared body underneath every monster,
                        // so a rabbit came with a Tarutaru standing inside it.
                        if (index < radarEntities.size() && modelForEntity(radarEntities[index]))
                        {
                            continue;   // has its own model, drawn below
                        }
                        if (index < radarEntities.size() && !radarEntities[index].hasLook() &&
                            !radarEntities[index].hasModel())
                        {
                            // Nothing was ever said about what this looks
                            // like. That is an event trigger or a marker, not
                            // a body somebody forgot to describe, and a plaza
                            // has a dozen of them standing on its steps.
                            continue;
                        }

                        // MOGHOUSE_GHOST_WATCH=1 names whoever ends up here.
                        // This is the fallback for an entity nothing could be
                        // built for, and from the zone it reads as a pale
                        // figure that follows whatever the player is wearing -
                        // because the shape drawn is the player's batch list.
                        // Which entity it is, and what it asked for, is the
                        // thing you cannot see from a chair.
                        static const bool watchGhosts = std::getenv("MOGHOUSE_GHOST_WATCH") != nullptr;
                        if (watchGhosts && index < radarEntities.size())
                        {
                            static std::set<uint32_t> ghostReported;
                            const mh::RadarEntity& who = radarEntities[index];
                            if (ghostReported.insert(who.id).second)
                            {
                                std::printf("ghost %08X \"%s\": modelId %u  look %u,%u %u/%u/%u/%u/%u\n",
                                            who.id, who.name.c_str(), who.modelId, who.look[0], who.look[1],
                                            who.look[2], who.look[3], who.look[4], who.look[5], who.look[6]);
                            }
                        }

                        // As a pale blank shape, the way the blank figure at
                        // character select is drawn - not as a copy of the
                        // player. A row of guards whose gear the loader cannot
                        // resolve used to stand there as a dozen of you.
                        pass.SetVertexBuffer(0, entityVertexBuffer);
                        if (ghostBindGroup && fadePipeline)
                        {
                            pass.SetBlendConstant(&kFadedBody);
                            pass.SetPipeline(fadePipeline);
                            pass.SetBindGroup(0, ghostBindGroup);
                        }
                        pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0,
                                         static_cast<uint32_t>(body + 1));
                        pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                        pass.SetBindGroup(0, characterBindGroups[i]);
                    }
                }
                }   // the player's own body, and the shared one everyone borrows

                // And everyone the server described well enough to build: their
                // own race, their own clothes. One model per distinct look,
                // shared by everybody wearing it.
                for (int body = 0; body < drawnBodies; ++body)
                {
                    const size_t index = static_cast<size_t>(body);
                    if (index >= radarEntities.size() ||
                        (!radarEntities[index].hasLook() && !radarEntities[index].hasModel()))
                    {
                        continue;
                    }

                    const DrawableCharacter* model = modelForEntity(radarEntities[index]);
                    if (!model)
                    {
                        continue;
                    }

                    // The entity's own posed vertices when it has them, and
                    // the shared model's when it does not - a look with no
                    // animations at all is still worth drawing standing still.
                    auto posed = entityPoses.find(radarEntities[index].id);
                    const bool animated = posed != entityPoses.end() && posed->second.drawn;
                    pass.SetVertexBuffer(0, animated ? posed->second.vertices : model->vertices);
                    pass.SetIndexBuffer(model->indices, wgpu::IndexFormat::Uint32);

                    // How solid this one is. The character being pointed at is
                    // shown as itself; the rest stand back, the way an invisible
                    // player does. The blank figure never comes forward - it is
                    // nobody yet, and pretending otherwise would suggest there
                    // is someone there to play as.
                    const int style = radarEntities[index].silhouette;
                    const bool pointedAt = hoverId != 0 && radarEntities[index].id == hoverId;
                    const bool ghost = style == 1;
                    const bool faded = style == 2 && !pointedAt;

                    if (ghost || faded)
                    {
                        pass.SetBlendConstant(&kFadedBody);
                    }

                    for (size_t b = 0; b < model->loaded.geometry.batches.size() && b < model->bindGroups.size(); ++b)
                    {
                        const mh::Batch& batch = model->loaded.geometry.batches[b];

                        if (ghost && ghostBindGroup && fadePipeline)
                        {
                            // Its own shape, and none of its own surface: one
                            // flat pale colour instead of a face and clothes.
                            pass.SetPipeline(fadePipeline);
                            pass.SetBindGroup(0, ghostBindGroup);
                        }
                        else if (faded && fadePipeline)
                        {
                            pass.SetPipeline(fadePipeline);
                            pass.SetBindGroup(0, model->bindGroups[b]);
                        }
                        else
                        {
                            pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                            pass.SetBindGroup(0, model->bindGroups[b]);
                        }

                        pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0,
                                         static_cast<uint32_t>(body + 1));
                    }
                }

                // The player's index buffer again, for whatever draws next -
                // when there is a player to have one.
                if (characterIndexBuffer)
                {
                    pass.SetIndexBuffer(characterIndexBuffer, wgpu::IndexFormat::Uint32);
                }
            }

            // The zone's exits, standing in the world.
            //
            // Drawn after the world and the characters so the glow lies over
            // them, and before the HUD so it stays behind the panels.
            if (zoneLinePipeline && zoneLineBindGroup)
            {
                const std::vector<mh::ZoneLineMarker> lines = link ? link->zoneLines()
                                                                   : std::vector<mh::ZoneLineMarker>{};
                int drawn = std::min(static_cast<int>(lines.size()), mh::kZoneLineMarkers);

                // The target gets a ring of its own, after the zone lines. FFXI draws
                // one under whatever you have selected, and the pipeline that draws a
                // glowing ring at a world position already exists.
                int targetRing = -1;
                mh::Vec3 targetAt{};
                if (targetId != 0 && drawn < mh::kZoneLineMarkers)
                {
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        if (entity.id == targetId)
                        {
                            targetAt = mh::Vec3{entity.x, entity.y, entity.z};
                            targetRing = drawn++;
                            break;
                        }
                    }
                }
                // Re-measured only when the list itself changes, which is
                // once per zone.
                if (orientedFor.size() != lines.size() ||
                    !std::equal(lines.begin(), lines.end(), orientedFor.begin(),
                                [](const mh::ZoneLineMarker& a, const mh::ZoneLineMarker& b) {
                                    return a.x == b.x && a.y == b.y && a.z == b.z;
                                }))
                {
                    orientedFor = lines;
                    zoneLineAxes.clear();
                    zoneLineAxes.reserve(lines.size());
                    for (const mh::ZoneLineMarker& line : lines)
                    {
                        zoneLineAxes.push_back(orientZoneLine(line));
                    }
                    std::printf("measured %zu zone line openings\n", zoneLineAxes.size());
                }

                if (drawn > 0)
                {
                    ZoneLineUniforms markers{};
                    std::memcpy(markers.viewProjection, viewProjection.m, sizeof(markers.viewProjection));
                    markers.counts[0] = static_cast<float>(drawn);
                    markers.counts[1] = mh::kZoneLineHeight;
                    markers.counts[2] = nowSeconds;
                    markers.counts[3] = static_cast<float>(targetRing);

                    for (int i = 0; i < drawn; ++i)
                    {
                        if (i == targetRing)
                        {
                            // Tight enough to read as standing around one
                            // person rather than marking a place.
                            markers.lines[i][0] = targetAt.x;
                            markers.lines[i][1] = targetAt.y;
                            markers.lines[i][2] = targetAt.z;
                            markers.lines[i][3] = 0.7f;
                            continue;
                        }

                        markers.lines[i][0] = lines[static_cast<size_t>(i)].x;
                        markers.lines[i][1] = lines[static_cast<size_t>(i)].y;
                        markers.lines[i][2] = lines[static_cast<size_t>(i)].z;
                        markers.lines[i][3] = lines[static_cast<size_t>(i)].radius;

                        if (static_cast<size_t>(i) < zoneLineAxes.size())
                        {
                            markers.axes[i][0] = zoneLineAxes[static_cast<size_t>(i)].x;
                            markers.axes[i][1] = zoneLineAxes[static_cast<size_t>(i)].y;
                            markers.axes[i][2] = zoneLineAxes[static_cast<size_t>(i)].z;
                            markers.axes[i][3] = 1.0f;
                        }
                    }

                    queue.WriteBuffer(zoneLineUniformBuffer, 0, &markers, sizeof(markers));
                    pass.SetPipeline(zoneLinePipeline);
                    pass.SetBindGroup(0, zoneLineBindGroup);
                    pass.Draw(mh::kZoneLineSegments * 6, static_cast<uint32_t>(drawn));
                }
            }

            // Water goes here: after the world it sits in, and before
            // everything drawn on top of the world.
            //
            // It was last, which put a translucent sheet over the radar, the
            // clock and the nameplates - the HUD is drawn in the same pass and
            // does not write depth, so whatever comes after it simply wins. A
            // canal at the edge of the screen tinted the compass.
            if (waterIndexCount && waterPipeline)
            {
                pass.SetPipeline(waterPipeline);
                pass.SetBindGroup(0, waterBindGroup);
                pass.SetVertexBuffer(0, waterVertexBuffer);
                pass.SetIndexBuffer(waterIndexBuffer, wgpu::IndexFormat::Uint32);
                pass.DrawIndexed(waterIndexCount);
            }

            // The shoreline waves, on top of the sea they wash over. Held back
            // from the effect pass above for the order alone; everything else
            // about them is an effect draw. Their motion is set up each frame
            // beside the monorail's - see MOGHOUSE_WAVE_WATCH.
            if (effectPipeline && zone)
            {
                pass.SetVertexBuffer(0, vertexBuffer);
                pass.SetVertexBuffer(1, instanceBuffer);
                pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
                for (size_t i = 0; i < zone->draws.size() && i < effectBindGroups.size(); ++i)
                {
                    const mh::InstancedDraw& draw = zone->draws[i];
                    if (!draw.wave.any() || !draw.effect || !effectBindGroups[i])
                    {
                        continue;
                    }
                    pass.SetPipeline(effectPipeline);
                    pass.SetBindGroup(0, effectBindGroups[i]);
                    pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
                }
            }

            // The game's own furniture, hidden while the client is on its own
            // screens. A compass and a chat log say nothing during a sign-in,
            // and the radar sits exactly where a character-select line-up wants
            // the eye to be.
            const bool showHud = link ? link->hud() : true;

            if (radarPipeline && radarBindGroup && showHud)
            {
                RadarUniforms radar{};
                // Top right, a fifth of the shorter side across, and low
                // enough to leave room for the clock over it - at 0.70 the
                // radar's own top edge was already at 0.96 and the clock drew
                // off the top of the window.
                radar.placement[0] = 0.78f;
                radar.placement[1] = 0.50f;
                radar.placement[2] = 0.21f;
                radar.placement[3] = static_cast<float>(width) / static_cast<float>(height);

                radar.mapExtent[0] = mapCentreX;
                radar.mapExtent[1] = mapCentreZ;
                radar.mapExtent[2] = mapHalf;

                // The character if there is one, otherwise wherever the camera
                // is - a radar centred on nothing is worse than no radar.
                const mh::Vec3 eyePosition = camera.eye();
                radar.viewer[0] = character ? characterAt.x : eyePosition.x;
                radar.viewer[1] = character ? characterAt.z : eyePosition.z;
                radar.viewer[2] = character ? characterFacing : camera.yaw;
                radar.viewer[3] = radarRange;

                const size_t shown = std::min(radarEntities.size(), static_cast<size_t>(mh::kRadarMaxEntities));
                radar.counts[0] = static_cast<float>(shown);
                radar.counts[2] = radarTurns ? 1.0f : 0.0f;
                // The radar's own zone label is off. It was drawn from the
                // 4x6 bitmap font across the bottom of the map, and the HUD
                // now sets the same name in a real typeface underneath - two
                // of them, one of them worse, in the same place.
                radar.counts[1] = 0.0f;
                for (size_t i = 0; i < shown; ++i)
                {
                    radar.entities[i][0] = radarEntities[i].x;
                    radar.entities[i][1] = radarEntities[i].z;
                    radar.entities[i][2] = static_cast<float>(radarEntities[i].kind);
                }
                queue.WriteBuffer(radarUniformBuffer, 0, &radar, sizeof(radar));

                pass.SetPipeline(radarPipeline);
                pass.SetBindGroup(0, radarBindGroup);
                pass.Draw(3);
            }

            if (hudPipeline && hudBindGroup)
            {
                HudUniforms hud{};
                const float windowAspect = static_cast<float>(width) / static_cast<float>(height);
                // One atlas texel to one screen pixel.
                //
                // Raising this by hand twice did not fix the text, because the
                // problem was never the size - it was the sampling. The atlas
                // was generated at a 72 pixel cell and drawn at about 19, so
                // every glyph was minified nearly four times through a linear
                // filter with no mipmaps, which thins the strokes and softens
                // the edges into mush. Bigger only made bigger mush.
                //
                // Deriving the size from the atlas and the window instead
                // keeps it at 1:1, where the glyph the generator drew is the
                // glyph that reaches the screen. The atlas is generated near
                // display size for the same reason; a scale far from 1.0 will
                // soften again, so the labels stay close to it.
                hud.counts[1] = static_cast<float>(textFont.cell) * 2.0f * uiScale / static_cast<float>(height);
                hud.counts[2] = windowAspect;
                hud.atlas[0] = static_cast<float>(textFont.columns);
                hud.atlas[1] = static_cast<float>(textFont.cell);
                hud.atlas[2] = static_cast<float>(textFont.width);
                hud.atlas[3] = static_cast<float>(textFont.height);

                int labels = 0;
                int bar = 0;

                // Laid out around the radar rather than at fixed corners, so
                // the two stay together if the radar ever moves.
                const float radarCentreX = 0.78f;
                const float radarCentreY = 0.50f;
                const float radarRadius = 0.21f;

                // Stacked by the height of a line rather than by numbers
                // chosen to look right at one text size. The size is derived
                // from the atlas and the window now, so anything hand-tuned
                // against it comes apart the moment either changes - which is
                // exactly what happened when the text went to 1:1 and every
                // label landed on its neighbour.
                const float line = hud.counts[1];
                const float gap = line * 0.18f;
                const float above = radarCentreY + radarRadius;

                // Every label's size in one place. The compass is set where it
                // is drawn, because the letters also have to be centred on
                // their points.
                constexpr float kZoneScale = 0.4f;
                constexpr float kPositionScale = 0.4f;
                constexpr float kWeekdayScale = 0.4f;
                const float below = radarCentreY - radarRadius;

                // centred = the x given is the middle, otherwise it is the
                // left edge. Chat reads down a column and has to line up.
                // How wide some text will be once placed. The same sum of
                // advances place() walks, so a box sized from this and the text
                // drawn into it cannot disagree.
                const auto measure = [&](const std::string& text, float scale) {
                    float pen = 0.0f;
                    const float cellSize = static_cast<float>(textFont.cell);
                    for (char raw : text)
                    {
                        pen += textFont.advanceOf(raw) / cellSize;
                    }
                    return pen * ((hud.counts[1] * scale) / windowAspect);
                };

                const auto place = [&](const std::string& text, float x, float bottomY, float scale,
                                       const float* tint, float background, bool centred) {
                    if (labels >= mh::kHudStrings || text.empty() || textFont.empty())
                    {
                        return;
                    }
                    const float cellSize = static_cast<float>(textFont.cell);
                    float pen = 0.0f;
                    int written = 0;
                    for (char raw : text)
                    {
                        if (written >= mh::kHudChars)
                        {
                            break;
                        }
                        const float advance = textFont.advanceOf(raw) / cellSize;
                        float* glyph = hud.glyphs[labels * mh::kHudChars + written];
                        glyph[0] = static_cast<float>(textFont.indexOf(raw));
                        glyph[1] = pen;
                        glyph[2] = advance;
                        pen += advance;
                        ++written;
                    }

                    // Centred on the radar's column, which is what makes the
                    // clock and the zone name read as one instrument.
                    const float cellWide = (hud.counts[1] * scale) / windowAspect;
                    hud.boxes[labels][0] = centred ? x - pen * cellWide * 0.5f : x;
                    hud.boxes[labels][1] = bottomY;
                    hud.boxes[labels][2] = pen;
                    hud.boxes[labels][3] = background;
                    hud.colours[labels][0] = tint[0];
                    hud.colours[labels][1] = tint[1];
                    hud.colours[labels][2] = tint[2];
                    hud.colours[labels][3] = scale;
                    ++labels;
                };

                // Wrapped so the existing calls keep reading as they did.
                const auto label = [&](const std::string& text, float centreX, float bottomY, float scale,
                                       const float* tint, float background) {
                    place(text, centreX, bottomY, scale, tint, background, true);
                };

                // The clock, above the radar. Vana'diel's week is eight days
                // and its hour is 2.4 real seconds; both come from the
                // server's own clock when it gave us one.
                static const char* kWeekdays[8] = {"Firesday",   "Earthsday",    "Watersday", "Windsday",
                                                   "Iceday",     "Lightningday", "Lightsday", "Darksday"};
                char clock[32] = {};
                std::snprintf(clock, sizeof(clock), "%02d:%02d", clockMinutes / 60, clockMinutes % 60);
                const float clockBottom = above + gap * 2.0f + line * 1.5f;
                label(clock, radarCentreX, clockBottom, 0.85f, kHudBright, 0.55f);

                // The weather, as an orb left of the time - which is where
                // retail keeps it. FFXI's twenty weathers pair off by element
                // and the orb takes that element's colour; see weatherOrb.
                //
                // A coloured orb rather than the game's own icon. The icons are
                // in the menu DAT and have not been found yet, and the element
                // colours are at least the right information in the right
                // place. Hovering is not a thing the HUD does, so the name goes
                // under the orb rather than into a tooltip.
                // The same answer the zone load used: what the server said, or
                // MOGHOUSE_WEATHER when there is no server to ask.
                static const int forcedWeather = forcedWeatherFromEnvironment();
                const int shownWeather = link && link->weather() >= 0 ? link->weather() : forcedWeather;
                if (const float* orb = weatherOrb(shownWeather))
                {
                    const float clockWide = measure(clock, 0.85f);
                    const float high = line * 0.5f;
                    const float wide = high / windowAspect;
                    const float left = radarCentreX - clockWide * 0.5f - gap * 2.0f - wide;
                    const float bottom = clockBottom + line * 0.16f;
                    for (int bar = 0; bar < mh::kHudBars; ++bar)
                    {
                        if (hud.bars[bar][2] <= 0.0f)
                        {
                            hud.bars[bar][0] = left;
                            hud.bars[bar][1] = bottom;
                            hud.bars[bar][2] = wide;
                            hud.bars[bar][3] = high;
                            for (int c = 0; c < 4; ++c)
                            {
                                hud.barColours[bar][c] = orb[c];
                            }
                            break;
                        }
                    }
                }

                // The way into the options, right of the clock. Three bars
                // rather than a word: a hamburger is a picture, and the text
                // atlas is a typeface with no glyph for one - but the HUD can
                // already draw rectangles, which is all a hamburger is.
                {
                    const float clockWide = measure(clock, 0.85f);
                    const float high = line * 0.62f;
                    const float wide = high / windowAspect;
                    const float left = radarCentreX + clockWide * 0.5f + gap * 3.4f;
                    const float bottom = clockBottom + line * 0.1f;

                    // The menu used to be a hamburger drawn here out of three
                    // bars. It is a cog on the toolbar above the vitals now,
                    // beside the bag - one place for the buttons rather than
                    // one in each corner. Zeroed rather than left behind, so
                    // the click test cannot match a rectangle nothing draws.
                    (void)high;
                    (void)wide;
                    (void)left;
                    (void)bottom;
                    optionsButton[2] = 0.0f;
                }

                if (vanaSeconds > 0)
                {
                    label(kWeekdays[(vanaSeconds / 86400ull) % 8ull], radarCentreX,
                          clockBottom - line * kWeekdayScale - gap * 0.5f, kWeekdayScale, kHudDim, 0.55f);
                }

                // What the orb means, under the weekday. The HUD has no
                // hovering, so the name is either shown or it is nowhere.
                if (const char* named = weatherName(shownWeather); named != nullptr && named[0] != '\0')
                {
                    label(named, radarCentreX,
                          clockBottom - line * kWeekdayScale * 2.0f - gap * 1.6f, kWeekdayScale, kHudDim, 0.55f);
                }

                // Compass letters around the ring.
                //
                // The map itself is north up - the radar maps its offsets
                // straight to world coordinates without rotating them - so
                // these belong at fixed points. The part that moves is the
                // heading notch the radar already draws; a ring that turned as
                // well would disagree with the map under it.
                float southY = 0.0f;
                {
                    const float ringX = radarRadius * 1.16f / windowAspect;
                    const float ringY = radarRadius * 1.16f;
                    const float compass = 0.5f;
                    const float half = hud.counts[1] * compass * 0.5f;

                    // The letters ride the ring rather than sitting at fixed
                    // corners, because with the map turning they have to. A
                    // compass that says north is up while the map has turned
                    // under it is worse than no compass.
                    //
                    // Screen direction for a world bearing is the inverse of
                    // the turn the map made, so north goes to (-sin, cos).
                    const float turn = radarTurns ? characterFacing : 0.0f;
                    const float turnCos = std::cos(turn);
                    const float turnSin = std::sin(turn);
                    const auto onRing = [&](const char* letter, float worldX, float worldY, const float* tint) {
                        const float screenX = worldX * turnCos - worldY * turnSin;
                        const float screenY = worldX * turnSin + worldY * turnCos;
                        label(letter, radarCentreX + screenX * ringX, radarCentreY + screenY * ringY - half,
                              compass, tint, 0.0f);
                    };

                    // North in red, the way the game does it. With the map
                    // turning, north is somewhere different every time you look
                    // at it, and one letter that stands out is faster to find
                    // than four that read the same.
                    static constexpr float kNorthRed[3] = {1.00f, 0.32f, 0.28f};
                    onRing("N", 0.0f, 1.0f, kNorthRed);
                    onRing("S", 0.0f, -1.0f, kHudDim);
                    onRing("E", 1.0f, 0.0f, kHudDim);
                    onRing("W", -1.0f, 0.0f, kHudDim);

                    // The zone name still hangs below the circle, so it needs
                    // somewhere fixed regardless of where south ended up.
                    southY = radarCentreY - ringY - half;
                }

                // Nothing is drawn while a zone loads.
                //
                // There was a name and the word "Loading" here, and a three
                // second floor under it so it could be seen. Both are gone:
                // swapping the zone inside the live window turned out to take
                // about a frame, so the screen was covering nothing and the
                // wait was the only slow part of zoning. `loadingZone` is still
                // tracked, because position reporting has to stay quiet while
                // the zone underneath it is being replaced.
                //
                // Worth putting back when there is something worth showing -
                // the destination zone's own baked map, which would give every
                // zone its own screen without any art being drawn.

                // Somewhere to send a bug from, top left, without leaving
                // the world.
                //
                // The launcher has the same two links, but the launcher hides
                // itself while the world is up - which is exactly when a
                // player finds something worth reporting.
                //
                // Measured as they are drawn: the text is proportional and the
                // chip is sized to fit it, so working out where one is means
                // doing most of the work of drawing it. The same reason the
                // death box does this.
                {
                    constexpr float kCornerScale = 0.7f;
                    const float chipHeight = line * kCornerScale + gap * 0.6f;
                    float cornerX = -0.97f;
                    const float cornerY = 0.93f;

                    const char* names[2] = {"Discord", "Report a bug"};
                    const mh::ViewerLink::Link targets[2] = {mh::ViewerLink::Link::Discord,
                                                             mh::ViewerLink::Link::Issues};
                    for (int i = 0; i < 2; ++i)
                    {
                        const float width = measure(names[i], kCornerScale);
                        place(names[i], cornerX, cornerY, kCornerScale, kHudDim, 0.55f, false);

                        cornerLinks[i].left = cornerX;
                        cornerLinks[i].bottom = cornerY;
                        cornerLinks[i].width = width;
                        cornerLinks[i].height = chipHeight;
                        cornerLinks[i].target = targets[i];

                        cornerX += width + gap * 2.0f;
                    }
                }


                // HP, MP and TP as three bars in the bottom right corner,
                // with the numbers written on them.
                //
                // The one thing the window never said was whether the player
                // was alive. Being dead read as being unable to move, which is
                // indistinguishable from a stuck client, and that is exactly
                // how it was reported. The numbers come straight off
                // GP_SERV_COMMAND_GROUP_ATTR; the percentages are the server's
                // own, so a full bar means what the server thinks it means.
                //
                // Bottom right rather than bottom left, where they began as
                // three lines of text: the chat log lives bottom left and grew
                // over them, so the TP line was the first thing a busy zone
                // hid. The radar has the top right, the corner links the top
                // left, and this corner was empty.
                if (link)
                {
                    const mh::ViewerLink::Vitals vitals = link->vitals();
                    if (vitals.known)
                    {
                        constexpr float kVitalScale = 0.62f;
                        const float textHigh = line * kVitalScale;
                        const float barHigh = textHigh * 1.45f;
                        const float barGap = textHigh * 0.22f;
                        const float barWide = 0.34f;
                        const float barLeft = 0.97f - barWide;
                        const float inset = textHigh * 0.35f / windowAspect;

                        // Green while it is fine, amber when it is low enough
                        // to matter, red when it is empty - a corpse should not
                        // look like a character on one hit point.
                        const float kHpFull[3] = {0.36f, 0.80f, 0.42f};
                        const float kHpLow[3] = {0.95f, 0.75f, 0.30f};
                        const float kHpGone[3] = {0.90f, 0.30f, 0.30f};
                        const float kMpFill[3] = {0.36f, 0.56f, 0.95f};
                        // TP turns gold at a thousand, which is the number
                        // that lets a weapon skill go.
                        const float kTpFill[3] = {0.85f, 0.60f, 0.28f};
                        const float kTpReady[3] = {1.00f, 0.86f, 0.36f};
                        const float kTrack[3] = {0.05f, 0.05f, 0.07f};

                        const bool dead = vitals.hp == 0;
                        const float* hpFill = dead ? kHpGone : (vitals.hpPercent <= 25 ? kHpLow : kHpFull);

                        const auto meter = [&](const std::string& text, float bottom, float fraction,
                                               const float* fill) {
                            if (bar + 2 > mh::kHudBars)
                            {
                                return;
                            }
                            // The track, then the fill over it. Two rectangles
                            // rather than one with a threshold, so the fill's
                            // colour and the track's stay independent.
                            hud.bars[bar][0] = barLeft;
                            hud.bars[bar][1] = bottom;
                            hud.bars[bar][2] = barWide;
                            hud.bars[bar][3] = barHigh;
                            hud.barColours[bar][0] = kTrack[0];
                            hud.barColours[bar][1] = kTrack[1];
                            hud.barColours[bar][2] = kTrack[2];
                            hud.barColours[bar][3] = 0.62f;
                            ++bar;

                            const float portion = std::clamp(fraction, 0.0f, 1.0f);
                            hud.bars[bar][0] = barLeft;
                            hud.bars[bar][1] = bottom;
                            hud.bars[bar][2] = barWide * portion;
                            hud.bars[bar][3] = barHigh;
                            hud.barColours[bar][0] = fill[0];
                            hud.barColours[bar][1] = fill[1];
                            hud.barColours[bar][2] = fill[2];
                            hud.barColours[bar][3] = portion > 0.0f ? 0.80f : 0.0f;
                            ++bar;

                            // The words on the bar, with no box of their own:
                            // the bar is the box.
                            place(text, barLeft + inset, bottom + (barHigh - textHigh) * 0.5f,
                                  kVitalScale, kHudBright, 0.0f, false);
                        };

                        char row[48] = {};
                        float bottom = -0.95f;

                        std::snprintf(row, sizeof(row), "TP %u", vitals.tp);
                        meter(row, bottom, static_cast<float>(vitals.tp) / 3000.0f,
                              vitals.tp >= 1000 ? kTpReady : kTpFill);
                        bottom += barHigh + barGap;

                        std::snprintf(row, sizeof(row), "MP %u  (%u%%)", vitals.mp, vitals.mpPercent);
                        meter(row, bottom, static_cast<float>(vitals.mpPercent) / 100.0f, kMpFill);
                        bottom += barHigh + barGap;

                        if (dead)
                        {
                            std::snprintf(row, sizeof(row), "HP 0  DEAD");
                        }
                        else
                        {
                            std::snprintf(row, sizeof(row), "HP %u  (%u%%)", vitals.hp, vitals.hpPercent);
                        }
                        // A dead character's bar is drawn full and red rather
                        // than empty: an empty track reads as "no data", and
                        // this is the one number that most needs to be seen.
                        meter(row, bottom, dead ? 1.0f : static_cast<float>(vitals.hpPercent) / 100.0f,
                              hpFill);
                    }
                }

                // The zone name, as a ribbon under the radar, with the position
                // directly beneath it - the two are one block, and splitting
                // them either side of the compass read as two unrelated
                // things.
                const float zoneNameBottom = southY + line * 1.15f;
                if (currentZoneName)
                {
                    // Underscores are how the zone tables spell a space, and
                    // nobody wants to read Bastok_Markets.
                    //
                    // The current zone rather than the one this window was
                    // opened with: after zoning in place the label kept naming
                    // where the player used to be.
                    std::string zone = *currentZoneName;
                    for (char& letter : zone)
                    {
                        if (letter == '_')
                        {
                            letter = ' ';
                        }
                    }
                    label(zone, radarCentreX, zoneNameBottom, kZoneScale, kHudBright, 0.55f);
                }

                // And where we are. FFXI shows a lettered grid here; that
                // comes from map bounds in the DATs which nothing reads yet,
                // so these are the coordinates the server itself uses - which
                // at least match what !pos prints.
                if (character)
                {
                    char position[32] = {};
                    std::snprintf(position, sizeof(position), "%.0f  %.0f", characterAt.x, -characterAt.z);
                    label(position, radarCentreX, zoneNameBottom - line * kPositionScale - gap * 0.5f,
                          kPositionScale, kHudDim, 0.55f);
                }

                // Aboard the train, and how to stop being aboard it.
                //
                // Held on screen the whole ride rather than said once when
                // boarding: the line takes a minute end to end, a rider who
                // looks away has no way back to a message that has scrolled
                // off, and there is nothing else on screen to say that walking
                // will not work because the train has them.
                if (aboard)
                {
                    label("PRESS T TO STEP OFF", 0.0f, -0.82f, 0.5f, kHudBright, 0.55f);
                }

                // Chat, bottom left, oldest at the top. Same atlas as
                // everything else: this was the last thing still drawing from
                // the 4x6 bitmap font, which had no lower case at all.
                if (showHud)
                {
                    std::vector<mh::ViewerLink::ChatLine> said;
                    if (link)
                    {
                        said = link->chat();
                    }
                    else
                    {
                        for (const std::string& line : viewerChat)
                        {
                            // MOGHOUSE_CHAT lines may say what they are, as
                            // "tell|text", so the colours can be looked at
                            // without a server to send them.
                            const size_t bar = line.find('|');
                            mh::ChatTone tone = mh::ChatTone::Say;
                            std::string text = line;
                            if (bar != std::string::npos && bar <= 9)
                            {
                                const std::string kind = line.substr(0, bar);
                                text = line.substr(bar + 1);
                                if (kind == "shout") tone = mh::ChatTone::Shout;
                                else if (kind == "tell") tone = mh::ChatTone::Tell;
                                else if (kind == "party") tone = mh::ChatTone::Party;
                                else if (kind == "ls") tone = mh::ChatTone::Linkshell;
                                else if (kind == "system") tone = mh::ChatTone::System;
                                else if (kind == "npc") tone = mh::ChatTone::Npc;
                                else text = line;
                            }
                            said.push_back({text, tone});
                        }
                    }
                    if (said.empty())
                    {
                        said.push_back({"Chat - waiting for the server", mh::ChatTone::System});
                    }

                    // Wrapped, not cut. A HUD string holds kHudChars glyphs and
                    // everything past that was dropped without a mark, so a
                    // sentence any longer than the panel simply lost its end -
                    // which is how "the train through Remnants of Sel Phiner"
                    // turned into "the train through Remnants of Sel Ph".
                    //
                    // Broken at a space where there is one in reach, and mid
                    // word only when a single word is wider than the panel.
                    // Wrapping keeps the tone: a tell that runs to two lines
                    // is pink on both of them.
                    std::vector<mh::ViewerLink::ChatLine> lines;
                    for (const mh::ViewerLink::ChatLine& entry : said)
                    {
                        const std::string& whole = entry.text;
                        if (whole.empty())
                        {
                            lines.push_back(entry);
                            continue;
                        }

                        for (size_t at = 0; at < whole.size();)
                        {
                            size_t take = std::min<size_t>(mh::kHudChars, whole.size() - at);
                            if (at + take < whole.size())
                            {
                                const size_t space = whole.find_last_of(' ', at + take);
                                if (space != std::string::npos && space > at)
                                {
                                    take = space - at;
                                }
                            }

                            lines.push_back({whole.substr(at, take), entry.tone});
                            at += take;
                            while (at < whole.size() && whole[at] == ' ')
                            {
                                ++at;
                            }
                        }
                    }

                    // Wrapping makes more lines than were said, and the panel
                    // holds what it holds. The newest survive.
                    while (lines.size() > static_cast<size_t>(mh::kChatLines))
                    {
                        lines.erase(lines.begin());
                    }

                    const float line = hud.counts[1] * 0.4f;

                    // One box, the height of the panel and the width of a full
                    // line, rather than a chip behind each line sized to its
                    // text. The chips read as a staircase of unrelated labels;
                    // a box reads as the chat window it is, and is what the
                    // retail client draws. The bars array draws it, after the
                    // vitals have taken their slots.
                    {
                        const float rowStep = line * 1.15f;
                        // Always room for the line you type into, whether or
                        // not it is open. A box that grows a row when you press
                        // return moves everything above it as you start typing,
                        // and a chat window that jumps is worse than one that
                        // reserves the space.
                        const float rows = static_cast<float>(mh::kChatLines) + 1.0f;
                        const float padY = line * 0.35f;
                        const float padX = line * 0.5f / windowAspect;
                        const float boxLeft = -0.98f - padX;
                        const float boxBottom = -0.97f - padY;
                        const float boxWide = measure(std::string(mh::kHudChars, 'M'), 0.4f) * 0.62f + padX * 2.0f;
                        const float boxHigh = rowStep * rows + padY * 2.0f;
                        for (int bar = 0; bar < mh::kHudBars; ++bar)
                        {
                            if (hud.bars[bar][2] <= 0.0f)
                            {
                                hud.bars[bar][0] = boxLeft;
                                hud.bars[bar][1] = boxBottom;
                                hud.bars[bar][2] = boxWide;
                                hud.bars[bar][3] = boxHigh;
                                hud.barColours[bar][0] = 0.03f;
                                hud.barColours[bar][1] = 0.035f;
                                hud.barColours[bar][2] = 0.05f;
                                hud.barColours[bar][3] = 0.6f;
                                break;
                            }
                        }
                    }

                    // The panel stacks upwards from the bottom, so the line
                    // being typed takes the bottom row and the history moves up
                    // to make room rather than being written over.
                    const float base = -0.97f + line * 1.15f;
                    for (size_t i = 0; i < lines.size(); ++i)
                    {
                        const float bottom = base + line * 1.15f * static_cast<float>(lines.size() - 1 - i);
                        place(lines[i].text, -0.98f, bottom, 0.4f, mh::chatColour(lines[i].tone), 0.0f, false);
                    }

                    {
                        // The tail, not the head. A HUD string is forty-eight
                        // characters and the line was drawn from its start, so
                        // past that the screen simply stopped changing while
                        // the keys still went in - which reads as typing
                        // having broken, and stops people mid-sentence. It
                        // did: the first real bug report came in at
                        // sixty-eight characters, cut mid-word, with nothing
                        // wrong with the buffer at all.
                        //
                        // Every text field in the world scrolls to keep the
                        // caret in view. This one now does too.
                        constexpr size_t kRoom = static_cast<size_t>(mh::kHudChars) - 3;
                        const std::string shown =
                            typed.size() > kRoom ? typed.substr(typed.size() - kRoom) : typed;

                        // The box says what it is when it is empty, rather than
                        // sitting there blank and looking broken. No "> " in
                        // front of what is typed - the box is the prompt.
                        static const float kHint[3] = {0.42f, 0.46f, 0.56f};
                        if (typing)
                        {
                            place(shown + "_", -0.98f, -0.97f, 0.4f, kHudBright, 0.0f, false);
                        }
                        else
                        {
                            place("Press Return to chat", -0.98f, -0.97f, 0.4f, kHint, 0.0f, false);
                        }
                    }
                }

                if (labels > 0)
                {
                    hud.counts[0] = static_cast<float>(labels);
                    queue.WriteBuffer(hudUniformBuffer, 0, &hud, sizeof(hud));
                    pass.SetPipeline(hudPipeline);
                    pass.SetBindGroup(0, hudBindGroup);
                    pass.Draw(3);
                }
            }

            // The bags.
            //
            // Icons are drained every frame whether the panel is open or not:
            // the client sends them as the server fills the bags, which is on
            // zoning in, and holding them back until the panel opens would
            // only mean opening it twice to see anything.
            if (inventoryPipeline && link)
            {
                for (mh::ViewerLink::ItemFace& face : link->takeItemFaces())
                {
                    ItemFacing& facing = itemFacing[face.itemId];
                    facing.name = face.name;
                    facing.description = face.description;
                    facing.type = face.type;
                    facing.level = face.level;
                    facing.slots = face.slots;

                    const size_t expected =
                        static_cast<size_t>(mh::kIconSize) * mh::kIconSize * 4;
                    const int cells = mh::kIconAtlasCells * mh::kIconAtlasCells;
                    if (facing.cell >= 0 || nextIconCell >= cells ||
                        face.width != mh::kIconSize || face.height != mh::kIconSize ||
                        face.rgba.size() != expected)
                    {
                        continue;
                    }

                    facing.cell = nextIconCell++;
                    wgpu::TexelCopyTextureInfo target{
                        .texture = iconAtlas,
                        .origin = {static_cast<uint32_t>((facing.cell % mh::kIconAtlasCells) * mh::kIconSize),
                                   static_cast<uint32_t>((facing.cell / mh::kIconAtlasCells) * mh::kIconSize),
                                   0}};
                    wgpu::TexelCopyBufferLayout layout{.bytesPerRow = mh::kIconSize * 4,
                                                       .rowsPerImage = mh::kIconSize};
                    wgpu::Extent3D extent{mh::kIconSize, mh::kIconSize, 1};
                    queue.WriteTexture(&target, face.rgba.data(), face.rgba.size(), &layout, &extent);
                }
            }


            // Not gated on there being entities. Our own nameplate is laid out
            // in here before the entity loop, and it does not come from the
            // entity list at all - so requiring one made the player anonymous
            // whenever nobody else was nearby, which is exactly when a name
            // over your own head is the only one on screen. The inner
            // `named > 0` check already skips the draw when there is nothing
            // to say.
            if (platePipeline && plateBindGroup)
            {
                NameplateUniforms plate{};
                std::memcpy(plate.viewProjection, viewProjection.m, sizeof(plate.viewProjection));

                const float windowAspect = static_cast<float>(width) / static_cast<float>(height);
                // One atlas cell, in NDC y. A cell is taller than the glyph
                // inside it - the outline needs somewhere to go - so the text
                // reads a good deal smaller than this number suggests.
                // 1:1 with the atlas, as the HUD is - see there. Names sit
                // at a distance and shrink with it, so this is the size they
                // reach at their crispest rather than a size they always are.
                // Smaller than 1:1 with the atlas. Names sit out in the world
                // rather than pinned to a panel, and a dozen of them at full
                // size cover more of the zone than they label.
                plate.counts[1] = static_cast<float>(textFont.cell) * 2.0f * uiScale * 0.6f / static_cast<float>(height);
                plate.counts[2] = windowAspect;
                plate.atlas[0] = static_cast<float>(textFont.columns);
                plate.atlas[1] = static_cast<float>(textFont.cell);
                plate.atlas[2] = static_cast<float>(textFont.width);
                plate.atlas[3] = static_cast<float>(textFont.height);

                // The zone's own name table, loaded the first time an entity
                // arrives. The server sends NPCs with no name, and the numeric
                // zone is not otherwise known here - but every entity id
                // carries the zone it belongs to, so the first one to arrive
                // says which table to read.
                if (!triedEntityNames && !radarEntities.empty())
                {
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        if (entity.id > 0x1000000u)
                        {
                            const auto zone = static_cast<uint16_t>((entity.id - 0x1000000u) >> 12);
                            try
                            {
                                const ffxi::FileTable table{ffxi::defaultInstallRoot()};
                                entityNames = ffxi::EntityNames::load(table, zone);
                            }
                            catch (const std::exception& e)
                            {
                                std::printf("no entity names: %s\n", e.what());
                            }
                            triedEntityNames = true;
                            break;
                        }
                    }
                }

                // Laid out here rather than in the shader: the font is
                // proportional, so where a glyph starts depends on every glyph
                // before it, and a fragment shader cannot accumulate that per
                // pixel without redoing the whole string. Measured in cells, so
                // the shader scales one number to whatever size it draws at.
                const auto layOutPlate = [&textFont](NameplateUniforms& into, int slot, const std::string& text) {
                    const float cell = static_cast<float>(textFont.cell);
                    float pen = 0.0f;
                    int written = 0;
                    for (char raw : text)
                    {
                        if (written >= mh::kNameplateChars)
                        {
                            break;
                        }
                        const float advance = textFont.advanceOf(raw) / cell;
                        float* glyph = into.glyphs[slot * mh::kNameplateChars + written];
                        glyph[0] = static_cast<float>(textFont.indexOf(raw));
                        glyph[1] = pen;
                        glyph[2] = advance;
                        pen += advance;
                        ++written;
                    }
                    for (int spare = written; spare < mh::kNameplateChars; ++spare)
                    {
                        into.glyphs[slot * mh::kNameplateChars + spare][2] = 0.0f;
                    }
                    into.positions[slot][3] = pen;   // total width, in cells
                    return slot + 1;
                };

                int named = 0;

                // Our own name, over our own head. The game shows everyone
                // their own nameplate; leaving it off made our character the
                // only anonymous one on screen.
                // The link first: the client learns the name at character
                // select, which is long after the options were fixed. The
                // options are the fallback, for the standalone viewer that has
                // no client to tell it anything.
                std::string shownPlayerName = link ? link->playerName() : std::string{};
                if (shownPlayerName.empty() && options.playerName)
                {
                    shownPlayerName = *options.playerName;
                }

                if (!shownPlayerName.empty() && character)
                {
                    plate.positions[named][0] = characterAt.x;
                    plate.positions[named][1] = characterAt.y + character->geometry.height() + kPlateClearance;
                    plate.positions[named][2] = characterAt.z;
                    plate.colours[named][0] = kNameWhite[0];
                    plate.colours[named][1] = kNameWhite[1];
                    plate.colours[named][2] = kNameWhite[2];
                    named = layOutPlate(plate, named, shownPlayerName);
                }

                for (const mh::RadarEntity& entity : radarEntities)
                {
                    if (named >= mh::kNameplateMax)
                    {
                        break;
                    }

                    // The server's name where it gave one - players - and the
                    // zone's table otherwise, which is where every NPC's name
                    // actually lives.
                    // Doors, zone lines and scenery are named in the zone's
                    // table, and the game shows the name only on target.
                    // Drawing them all labels a city with things nobody asked
                    // about.
                    // MOGHOUSE_PLATE_WATCH=1 says, once per entity, why a
                    // nameplate was not drawn. Three separate rules drop one -
                    // the server's hide flag, having no model to stand it over,
                    // and having no name from either source - and from inside
                    // the zone all three look identical.
                    static const bool watchPlates = std::getenv("MOGHOUSE_PLATE_WATCH") != nullptr;
                    static std::set<uint32_t> plateReported;
                    const auto sayWhy = [&](const char* why) {
                        if (watchPlates && plateReported.insert(entity.id).second)
                        {
                            std::printf("no plate for %08X: %s (server name %s)\n", entity.id, why,
                                        entity.name.empty() ? "empty" : entity.name.c_str());
                        }
                    };

                    if (entity.nameHidden)
                    {
                        sayWhy("the server says hide the name");
                        continue;
                    }

                    // Nothing to label. warp07 in Windurst Waters is model 50,
                    // and that file holds one black triangle and nothing else -
                    // a trigger the game never draws - so a plate over it names
                    // bare ground. The zone table has a name for it either way,
                    // because these are the client's own internal names.
                    if (!modelForEntity(entity))
                    {
                        sayWhy("no model to stand it over");
                        continue;
                    }

                    const std::string& shown =
                        entity.name.empty() ? entityNames.lookup(entity.id) : entity.name;
                    if (shown.empty())
                    {
                        sayWhy(entityNames.empty() ? "the zone's name table did not load"
                                                   : "not in the zone's name table either");
                        continue;
                    }

                    // Over the head rather than at the feet. The model is about
                    // 1.8 tall and the name wants a little air above that.
                    // Each entity's own model decides how high its name sits.
                    // A galka and a tarutaru standing together want quite
                    // different numbers, and the shared body is the fallback
                    // for anyone we could not build.
                    // The pose this frame if there is one, because a clip can
                    // carry the body well away from where the rest pose put it.
                    const DrawableCharacter* plateModel = modelForEntity(entity);
                    auto posedPlate = entityPoses.find(entity.id);
                    const float bodyHeight =
                        posedPlate != entityPoses.end() && posedPlate->second.drawn && posedPlate->second.posedTop > 0.0f
                            ? posedPlate->second.posedTop
                            : (plateModel ? plateModel->loaded.geometry.height()
                                          : (character ? character->geometry.height() : 1.8f));

                    const float headY = entity.y +
                                        bodyHeight * mh::bodyScale(entity.size) *
                                            (mh::isChildRace(entity.look[0]) ? mh::kChildScale : 1.0f) +
                                        kPlateClearance;

                    // Behind a wall, so not shown.
                    //
                    // The depth test cannot do this: the plates are drawn as one
                    // fullscreen triangle at depth zero with the projection done
                    // per fragment, so every label passes any depth comparison
                    // there is. The same wall raycast the camera uses to avoid
                    // orbiting through a house answers it directly instead, and
                    // a handful of entities is a handful of rays.
                    //
                    // MOGHOUSE_PLATE_OCCLUSION picks how strict that is:
                    //
                    //   2  the drawn world's opaque surfaces (the default)
                    //   1  the collision mesh, which is stricter
                    //   0  off, names always shown
                    //
                    // A trunk should hide a name and leaves should not, and the
                    // collision mesh cannot tell you which is which: it is a
                    // different dataset from the drawn one, where a fence is a
                    // solid box and a tree a solid cylinder. So the ray was
                    // stopped by railings you can see over, and names only
                    // appeared once the camera rose above them.
                    //
                    // The drawn geometry does know. A mesh is cutout, blended
                    // or opaque, and only the last should stop a name - which
                    // in West Ronfaure leaves out 589,890 triangles of foliage
                    // and keeps 353,080.
                    static const int occlusion = [] {
                        const char* set = std::getenv("MOGHOUSE_PLATE_OCCLUSION");
                        return set ? static_cast<int>(std::strtol(set, nullptr, 10)) : 2;
                    }();
                    const mh::Vec3 plateAt{entity.x, headY, entity.z};
                    const bool blocked =
                        occlusion == 2   ? sight.firstSolidAlong(camera.eye(), plateAt).has_value()
                        : occlusion == 1 ? collision.firstSolidAlong(camera.eye(), plateAt).has_value()
                                         : false;
                    if (blocked)
                    {
                        sayWhy("behind something solid");
                        continue;
                    }

                    plate.positions[named][0] = entity.x;
                    plate.positions[named][1] = headY;
                    plate.positions[named][2] = entity.z;

                    // Colour by what the entity is. The rest of the palette -
                    // party blue, linkshell green, friend orange, pet grey -
                    // needs membership the client does not parse yet, so those
                    // stay white rather than being guessed at.
                    const float* tint = kNameWhite;
                    if (entity.gmLevel > 0)
                    {
                        // Ahead of everything else: a GM is a GM whatever else
                        // they are, and the real client says so first too.
                        tint = kNameGm;
                    }
                    else if (entity.kind == 2)
                    {
                        // A mob the server says has no health left. The bit
                        // that marks a mob is literally `hp > 0`, so a corpse
                        // stops announcing itself as one - the tracker keeps
                        // it an enemy anyway, and this is what says it is over.
                        tint = entity.healthPercent == 0 ? kNameDead : kNameMonster;
                    }
                    else if (entity.kind == 1)
                    {
                        tint = kNameNpc;
                    }
                    plate.colours[named][0] = tint[0];
                    plate.colours[named][1] = tint[1];
                    plate.colours[named][2] = tint[2];

                    named = layOutPlate(plate, named, shown);
                }

                if (named > 0)
                {
                    plate.counts[0] = static_cast<float>(named);
                    queue.WriteBuffer(plateUniformBuffer, 0, &plate, sizeof(plate));
                    pass.SetPipeline(platePipeline);
                    pass.SetBindGroup(0, plateBindGroup);
                    pass.Draw(3);
                }
            }

        }

            // The bags, drawn after the nameplates rather than before.
            //
            // Every one of these passes writes no depth and compares
            // Always, so what is on top is decided purely by the order
            // they are issued in. Drawn first, the panel had names
            // punching through it - a nameplate belongs to the world and
            // the world is what a panel is covering.

            // Recomputed rather than carried: the flag above belongs to the
            // block that draws the world, and this now sits outside it.
            const bool panelHud = link ? link->hud() : true;
            if (inventoryPipeline && link && panelHud)
            {
                InventoryUniforms inv{};
                int quads = 0;

                // Its own, rather than the HUD's: that one is scoped to the
                // block above and this panel is a sibling of it, not a part.
                const float windowAspect = static_cast<float>(width) / static_cast<float>(height);
                static const float kHint[3] = {0.42f, 0.46f, 0.56f};

                const auto quad = [&](float left, float bottom, float wide, float high, int mode,
                                      int cell, const float rgb[3], float alpha)
                {
                    if (quads >= mh::kInventoryQuads)
                    {
                        return;
                    }
                    inv.rects[quads][0] = left;
                    inv.rects[quads][1] = bottom;
                    inv.rects[quads][2] = wide;
                    inv.rects[quads][3] = high;
                    inv.looks[quads][0] = static_cast<float>(mode);
                    inv.looks[quads][1] = static_cast<float>(cell);
                    inv.tints[quads][0] = rgb[0];
                    inv.tints[quads][1] = rgb[1];
                    inv.tints[quads][2] = rgb[2];
                    inv.tints[quads][3] = alpha;
                    ++quads;
                };

                // Letters are quads too, one per glyph, walked with the same
                // advances the HUD uses so a count sits where it would there.
                const float cellSize = static_cast<float>(textFont.cell);
                const auto write = [&](const std::string& line, float left, float bottom,
                                       float high, const float rgb[3], float alpha)
                {
                    const float wide = high / windowAspect;
                    float pen = left;
                    for (char raw : line)
                    {
                        quad(pen, bottom, wide, high, 2, static_cast<int>(textFont.indexOf(raw)), rgb, alpha);
                        pen += (textFont.advanceOf(raw) / cellSize) * wide;
                    }
                    return pen - left;
                };

                const auto widthOf = [&](const std::string& line, float high)
                {
                    const float wide = high / windowAspect;
                    float pen = 0.0f;
                    for (char raw : line)
                    {
                        pen += (textFont.advanceOf(raw) / cellSize) * wide;
                    }
                    return pen;
                };

                // Only containers the server gave slots to. A wardrobe nobody
                // has bought has none, and offering an empty tab for it would
                // be inventing storage.
                static const char* kContainerNames[18] = {
                    "Inventory", "Mog Safe", "Storage",   "Temporary", "Mog Locker", "Satchel",
                    "Sack",      "Case",     "Wardrobe",  "Mog Safe 2", "Wardrobe 2", "Wardrobe 3",
                    "Wardrobe 4", "Wardrobe 5", "Wardrobe 6", "Wardrobe 7", "Wardrobe 8", "Recycle"};

                const std::array<uint16_t, 18> sizes = link->containerSizes();
                std::vector<int> tabs;
                for (int i = 0; i < 18; ++i)
                {
                    if (sizes[static_cast<size_t>(i)] > 0)
                    {
                        tabs.push_back(i);
                    }
                }

                // Shutting a panel is when someone has finished changing
                // their gear, so it is when to ask the server what we now look
                // like. It sends a look of its own accord, but not dependably
                // in the same breath as the equip - and standing there in the
                // old armour is exactly the moment the change is being looked
                // for.
                if (link && ((wasInventoryOpen && !inventoryOpen) ||
                             (wasEquipmentOpen && !equipmentOpen)))
                {
                    link->requestInventoryAction(
                        {mh::ViewerLink::InventoryAction::Kind::Refresh, 0, 0, 0, 0});
                }
                wasInventoryOpen = inventoryOpen;
                wasEquipmentOpen = equipmentOpen;

                inventoryHits.clear();

                float pointerX = 0.0f;
                float pointerY = 0.0f;
                float mouseX = 0.0f;
                float mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                const bool havePointer = pointerNdc(mouseX, mouseY, pointerX, pointerY);

                // A label with a box behind it that can be clicked. Returns
                // where it ended so a row of them can be laid out.
                static const float kPanel[3] = {0.05f, 0.06f, 0.12f};
                static const float kFrame[3] = {0.20f, 0.22f, 0.34f};
                static const float kFrameLit[3] = {0.36f, 0.40f, 0.58f};
                static const float kTabOn[3] = {0.30f, 0.34f, 0.50f};

                // Worn, and told apart from merely hovered: a brighter frame
                // is what says the equip landed.
                static const float kWorn[3] = {0.30f, 0.52f, 0.36f};
                static const float kButton[3] = {0.24f, 0.27f, 0.40f};

                const auto button = [&](const std::string& label, float left, float bottom, float high,
                                        bool lit, int kind, int value)
                {
                    const float wide = widthOf(label, high);
                    const float padding = high * 0.45f / windowAspect;
                    const float boxLeft = left - padding;
                    const float boxWide = wide + padding * 2.0f;
                    const float boxHigh = high * 1.5f;
                    const float boxBottom = bottom - high * 0.22f;
                    const bool under = havePointer && pointerX >= boxLeft && pointerX < boxLeft + boxWide &&
                                       pointerY >= boxBottom && pointerY < boxBottom + boxHigh;

                    quad(boxLeft, boxBottom, boxWide, boxHigh, 0, 0, lit ? kTabOn : kButton,
                         lit ? 0.95f : (under ? 0.85f : 0.55f));
                    write(label, left, bottom, high, lit || under ? kHudBright : kHudDim, 1.0f);
                    inventoryHits.push_back(InventoryHit{boxLeft, boxBottom, boxWide, boxHigh, kind, value});
                    return boxLeft + boxWide;
                };


                // The toolbar, above the vitals and always on.
                //
                // Placed from the same numbers the vitals are: three bars from
                // -0.95 upward, so the row sits clear of them whatever the
                // interface is scaled to rather than at a constant somebody
                // has to keep in step.
                const float vitalLine =
                    static_cast<float>(textFont.cell) * 2.0f * uiScale / static_cast<float>(height);
                const float vitalText = vitalLine * 0.62f;
                const float vitalBar = vitalText * 1.45f;
                const float vitalGap = vitalText * 0.22f;
                const float vitalsTop = -0.95f + 3.0f * (vitalBar + vitalGap);

                const float toolHigh = vitalBar * 1.10f;
                const float toolWide = toolHigh / windowAspect;
                const float toolGap = toolWide * 0.30f;
                const float toolBottom = vitalsTop + vitalGap;

                const auto tool = [&](int cell, float left, bool lit, int kind)
                {
                    const bool under = havePointer && pointerX >= left && pointerX < left + toolWide &&
                                       pointerY >= toolBottom && pointerY < toolBottom + toolHigh;
                    quad(left, toolBottom, toolWide, toolHigh, 0, 0, lit ? kTabOn : kButton,
                         lit ? 0.90f : (under ? 0.75f : 0.45f));

                    const float inset = toolHigh * 0.16f;
                    quad(left + inset / windowAspect, toolBottom + inset,
                         toolWide - inset * 2.0f / windowAspect, toolHigh - inset * 2.0f, 1, cell,
                         kHudBright, lit || under ? 1.0f : 0.75f);
                    inventoryHits.push_back(
                        InventoryHit{left, toolBottom, toolWide, toolHigh, kind, 0});
                };

                // Rightmost first, so the row grows leftward from the vitals'
                // own right edge and the two line up.
                float toolPen = 0.97f - toolWide;
                tool(kCogIconCell, toolPen, optionsOpen, 4);
                toolPen -= toolWide + toolGap;
                tool(kBagIconCell, toolPen, inventoryOpen, 3);
                toolPen -= toolWide + toolGap;
                tool(kShieldIconCell, toolPen, equipmentOpen, 9);

                if (inventoryOpen)
                {
                    const auto wrap = [](int value, int count)
                    { return count <= 0 ? 0 : ((value % count) + count) % count; };

                    // The bag is resolved before any geometry, because how many
                    // slots it has decides how tall the grid is.
                    int container = -1;
                    int slots = 0;
                    if (!tabs.empty())
                    {
                        inventoryTab = wrap(inventoryTab, static_cast<int>(tabs.size()));
                        container = tabs[static_cast<size_t>(inventoryTab)];
                        slots = sizes[static_cast<size_t>(container)];
                    }

                    // Eight across, and as many rows down as the bag needs.
                    //
                    // The tab is already the bag, so paging inside one is a second
                    // axis that neither this game nor the one the layout is
                    // borrowed from has. The slot shrinks to fit instead, and only
                    // a container too tall for the window still pages - which for
                    // an eighty-slot inventory it never does.
                    const int columns = 8;
                    const int rowsNeeded = std::max(1, (slots + columns - 1) / columns);
                    const int rowsShown = std::min(rowsNeeded, 12);
                    const int perPage = columns * rowsShown;
                    const int pages = std::max(1, (rowsNeeded + rowsShown - 1) / rowsShown);
                    inventoryPage = wrap(inventoryPage, pages);

                    // How big a slot can be and still leave the grid inside the
                    // window, across and down.
                    //
                    // Not scaled by uiScale, which is not a "make it bigger" knob:
                    // it is the setting multiplied by pixels over points, so it is
                    // 2 on a retina display and 1 on an ordinary one. Multiplying
                    // a measurement that is already in normalised device
                    // coordinates by it made the panel take half the screen here
                    // and a quarter of it on a machine without the doubling - the
                    // same code, two different panels.
                    const float gap = 0.14f;
                    const float widest = 0.80f;
                    const float tallest = 1.05f;

                    // Three limits, and the smallest wins: what fits across,
                    // what fits down, and how big a slot is worth being.
                    // Without the last one a bag of thirty gets enormous slots
                    // purely because there is room for them.
                    const float roomiest = 0.098f;
                    const float slotHigh =
                        std::min({roomiest,
                                  tallest / (static_cast<float>(rowsShown) * (1.0f + gap)),
                                  widest / (static_cast<float>(columns) / windowAspect +
                                            gap * static_cast<float>(columns - 1))});
                    const float slotWide = slotHigh / windowAspect;
                    const float slotGap = slotHigh * gap;
                    const float textHigh = std::clamp(slotHigh * 0.34f, 0.030f, 0.048f);
                    const float gridWide = columns * slotWide + (columns - 1) * slotGap;
                    const float gridHigh = rowsShown * slotHigh + (rowsShown - 1) * slotGap;
                    const float padX = std::max(slotWide * 0.55f, textHigh);
                    const float padTop = textHigh * 4.2f;
                    const float padBottom = textHigh * 8.2f;
                    const float panelWide = gridWide + padX * 2.0f;
                    const float panelHigh = gridHigh + padTop + padBottom;
                    const float panelLeft = -panelWide * 0.5f;
                    const float panelBottom = -panelHigh * 0.5f;

                    quad(panelLeft, panelBottom, panelWide, panelHigh, 0, 0, kPanel, 0.90f);

                    if (tabs.empty())
                    {
                        write("Waiting for the server to send the bags", panelLeft + padX,
                              panelBottom + panelHigh * 0.5f, textHigh, kHudDim, 1.0f);
                    }
                    else
                    {
                        // What is actually in this bag, by slot number.
                        //
                        // Except the gil, which is not a thing in a bag even
                        // though the server keeps it in one: item 65535 in
                        // inventory slot zero, which is exactly where getGil
                        // reads it from. No item DAT holds it - they stop at
                        // 0x59FF - so it can never have an icon, and drawing it
                        // as a slot gives a count with an empty square under
                        // it. Retail puts it in a box of its own; so does this.
                        constexpr uint16_t kGilItem = 65535;
                        std::map<int, mh::ViewerLink::InventorySlot> held;
                        uint32_t gil = 0;
                        for (const mh::ViewerLink::InventorySlot& entry : link->inventory())
                        {
                            if (entry.itemId == kGilItem)
                            {
                                gil = entry.count;
                                continue;
                            }

                            if (entry.container == container)
                            {
                                held[entry.slot] = entry;
                            }
                        }

                        contextContainer = container;

                        // Which of these slots is being worn. The equipment
                        // list names a place, so the same place in the bag is
                        // the item that is on - no extra data needed, and it
                        // is the only thing on this screen that says an equip
                        // actually happened.
                        std::set<int> equippedHere;
                        for (const auto& place : link->equipment())
                        {
                            if (place.second != 255 && place.first == container)
                            {
                                equippedHere.insert(place.second);
                            }
                        }

                        const int used = static_cast<int>(held.size());
                        const float titleY = panelBottom + panelHigh - textHigh * 1.6f;
                        write(std::string{kContainerNames[container]} + "   " + std::to_string(used) + " / " +
                                  std::to_string(slots),
                              panelLeft + padX, titleY, textHigh, kHudBright, 1.0f);

                        // Page arrows, on the right of the title, and only when
                        // there is a page to go to. A bag that fits shows none.
                        if (pages > 1)
                        {
                            const std::string counter =
                                std::to_string(inventoryPage + 1) + " / " + std::to_string(pages);
                            const float counterWide = widthOf(counter, textHigh);
                            const float arrowWide = widthOf(">", textHigh) + textHigh * 0.9f / windowAspect;
                            const float rightEdge = panelLeft + panelWide - padX;
                            const float leftEdge =
                                rightEdge - (arrowWide * 2.0f + counterWide + textHigh * 1.2f / windowAspect);

                            const float afterPrev = button("<", leftEdge, titleY, textHigh, false, 1, -1);
                            write(counter, afterPrev + textHigh * 0.4f / windowAspect, titleY, textHigh,
                                  kHudDim, 1.0f);
                            button(">", afterPrev + textHigh * 0.4f / windowAspect + counterWide +
                                            textHigh * 0.5f / windowAspect,
                                   titleY, textHigh, false, 1, 1);
                        }

                        // The bag tabs, each one clickable.
                        const float tabY = titleY - textHigh * 2.0f;
                        float tabPen = panelLeft + padX;
                        for (size_t i = 0; i < tabs.size(); ++i)
                        {
                            const std::string label{kContainerNames[tabs[i]]};
                            const float wide = widthOf(label, textHigh);
                            if (tabPen + wide > panelLeft + panelWide - padX)
                            {
                                break;
                            }

                            tabPen = button(label, tabPen, tabY, textHigh, static_cast<int>(i) == inventoryTab,
                                            0, static_cast<int>(i)) +
                                     textHigh * 0.5f / windowAspect;
                        }

                        // The order the grid is shown in. One button that cycles,
                        // because five of them along the top would take more room
                        // than the tabs.
                        static const char* kOrderNames[5] = {"Slot", "Name", "Type", "Amount", "Level"};
                        const int order = static_cast<int>(inventoryOrder);
                        const std::string orderLabel = std::string{"Sort: "} + kOrderNames[order];
                        button(orderLabel,
                               panelLeft + panelWide - padX - widthOf(orderLabel, textHigh), tabY, textHigh,
                               order != 0, 2, 0);

                        // In slot order the grid is the bag as the server laid it
                        // out, gaps and all. In any other the items are packed to
                        // the front and the leftover slots trail behind - sorting
                        // around holes would show an order nobody asked for.
                        std::vector<mh::ViewerLink::InventorySlot> shown;
                        if (inventoryOrder != InventoryOrder::Slot)
                        {
                            for (const auto& entry : held)
                            {
                                shown.push_back(entry.second);
                            }

                            const auto facingOf = [&](uint16_t itemId) -> const ItemFacing*
                            {
                                const auto found = itemFacing.find(itemId);
                                return found == itemFacing.end() ? nullptr : &found->second;
                            };

                            std::stable_sort(
                                shown.begin(), shown.end(),
                                [&](const mh::ViewerLink::InventorySlot& a,
                                    const mh::ViewerLink::InventorySlot& b)
                                {
                                    const ItemFacing* left = facingOf(a.itemId);
                                    const ItemFacing* right = facingOf(b.itemId);

                                    // Anything the client has not described yet
                                    // sinks, rather than sorting as a blank name
                                    // and taking the front.
                                    if ((left != nullptr) != (right != nullptr))
                                    {
                                        return left != nullptr;
                                    }
                                    if (!left)
                                    {
                                        return a.slot < b.slot;
                                    }

                                    switch (inventoryOrder)
                                    {
                                    case InventoryOrder::Type:
                                        if (left->type != right->type)
                                        {
                                            return left->type < right->type;
                                        }
                                        break;
                                    case InventoryOrder::Amount:
                                        if (a.count != b.count)
                                        {
                                            return a.count > b.count;
                                        }
                                        break;
                                    case InventoryOrder::Level:
                                        if (left->level != right->level)
                                        {
                                            return left->level > right->level;
                                        }
                                        break;
                                    default:
                                        break;
                                    }

                                    return left->name < right->name;
                                });
                        }

                        const float gridLeft = panelLeft + padX;
                        const float gridTop = panelBottom + panelHigh - padTop;

                        // Where the right click menu goes, and what it is
                        // about. Filled by the grid below; the menu is drawn
                        // after it so it lands on top of the slots.
                        float contextLeft = 0.0f;
                        float contextBottom = 0.0f;
                        uint16_t contextItem = 0;

                        std::string hovered;
                        std::vector<std::string> hoveredDetail;
                        int hoveredLevel = 0;
                        int hoveredSlots = 0;
                        int hoveredAt = -1;

                        for (int cellIndex = 0; cellIndex < perPage; ++cellIndex)
                        {
                            const int position = inventoryPage * perPage + cellIndex;
                            if (position >= slots)
                            {
                                break;
                            }

                            const int col = cellIndex % columns;
                            const int row = cellIndex / columns;
                            const float left = gridLeft + static_cast<float>(col) * (slotWide + slotGap);
                            const float bottom = gridTop - slotHigh - static_cast<float>(row) * (slotHigh + slotGap);

                            const bool under = havePointer && pointerX >= left && pointerX < left + slotWide &&
                                               pointerY >= bottom && pointerY < bottom + slotHigh;
                            const bool onYou = equippedHere.count(position) != 0;
                            quad(left, bottom, slotWide, slotHigh, 0, 0,
                                 onYou ? kWorn : (under ? kFrameLit : kFrame), 0.85f);

                            const mh::ViewerLink::InventorySlot* entry = nullptr;
                            if (inventoryOrder == InventoryOrder::Slot)
                            {
                                const auto found = held.find(position);
                                entry = found == held.end() ? nullptr : &found->second;
                            }
                            else if (position < static_cast<int>(shown.size()))
                            {
                                entry = &shown[static_cast<size_t>(position)];
                            }

                            if (!entry)
                            {
                                continue;
                            }

                            inventoryHits.push_back(
                                InventoryHit{left, bottom, slotWide, slotHigh, 8, entry->slot});
                            if (entry->slot == contextSlot)
                            {
                                contextLeft = left;
                                contextBottom = bottom;
                                contextItem = entry->itemId;
                            }

                            const auto facing = itemFacing.find(entry->itemId);
                            if (facing != itemFacing.end() && facing->second.cell >= 0)
                            {
                                const float inset = slotHigh * 0.08f;
                                quad(left + inset / windowAspect, bottom + inset,
                                     slotWide - inset * 2.0f / windowAspect, slotHigh - inset * 2.0f, 1,
                                     facing->second.cell, kHudBright, 1.0f);
                            }

                            // The count, in the top right corner of the icon, the
                            // way every game that stacks anything writes it. A one
                            // is left off: a single item saying "1" is noise.
                            if (entry->count > 1)
                            {
                                const std::string count = std::to_string(entry->count);
                                const float countHigh = slotHigh * 0.30f;
                                const float countWide = widthOf(count, countHigh);
                                write(count, left + slotWide - countWide - slotHigh * 0.06f / windowAspect,
                                      bottom + slotHigh - countHigh - slotHigh * 0.04f, countHigh, kHudBright,
                                      1.0f);
                            }

                            if (under && facing != itemFacing.end())
                            {
                                hovered = facing->second.name;
                                hoveredLevel = facing->second.level;
                                hoveredSlots = facing->second.slots;
                                hoveredAt = entry->slot;

                                hoveredDetail.clear();
                                const std::string& whole = facing->second.description;
                                size_t from = 0;
                                while (from <= whole.size())
                                {
                                    const size_t stop = whole.find('\n', from);
                                    hoveredDetail.push_back(whole.substr(
                                        from, stop == std::string::npos ? std::string::npos : stop - from));
                                    if (stop == std::string::npos)
                                    {
                                        break;
                                    }
                                    from = stop + 1;
                                }
                            }
                        }

                        // The right click menu, over the slots rather than
                        // under them.
                        //
                        // What it offers is decided from the item every frame,
                        // not from what was true when it opened: an item the
                        // server has since taken away leaves nothing to look
                        // up, and the menu closes rather than offering to equip
                        // something that is gone.
                        if (contextSlot >= 0 && contextItem != 0)
                        {
                            const auto facing = itemFacing.find(contextItem);
                            const bool wearable = facing != itemFacing.end() && facing->second.slots != 0;

                            float menuY = contextBottom - textHigh * 1.5f;
                            const float menuX = contextLeft + slotWide * 0.35f;

                            if (wearable)
                            {
                                // The lowest slot the item will go in. A ring
                                // names two and either will do; the server
                                // decides whether the one asked for is free.
                                int equipSlot = 0;
                                while (equipSlot < 16 &&
                                       (facing->second.slots & (1 << equipSlot)) == 0)
                                {
                                    ++equipSlot;
                                }

                                button("Equip", menuX, menuY, textHigh, false, 5, equipSlot);
                                menuY -= textHigh * 1.8f;
                            }

                            // Asked twice, because there is no undo and no
                            // confirmation anywhere on the wire: the server
                            // destroys the item the moment the packet lands.
                            button(contextConfirmDrop ? "Really drop?" : "Drop", menuX, menuY, textHigh,
                                   contextConfirmDrop, 6, 0);
                            menuY -= textHigh * 1.8f;
                            button("Cancel", menuX, menuY, textHigh, false, 7, 0);
                        }
                        else if (contextSlot >= 0)
                        {
                            contextSlot = -1;
                            contextConfirmDrop = false;
                        }

                        // The whole description, not its first line. The DAT
                        // writes its own breaks and the retail tooltip keeps
                        // them, so the lines are already the length the text
                        // was written for.
                        if (!hovered.empty())
                        {
                            float lineY = panelBottom + padBottom - textHigh * 1.5f;
                            write(hovered, panelLeft + padX, lineY, textHigh, kHudBright, 1.0f);
                            lineY -= textHigh * 1.35f;

                            if (hoveredLevel > 0)
                            {
                                write("Lv" + std::to_string(hoveredLevel), panelLeft + padX, lineY,
                                      textHigh * 0.85f, kHudDim, 1.0f);
                                lineY -= textHigh * 1.15f;
                            }

                            for (const std::string& detail : hoveredDetail)
                            {
                                if (lineY < panelBottom + textHigh * 0.2f)
                                {
                                    break;
                                }
                                write(detail, panelLeft + padX, lineY, textHigh * 0.85f, kHudDim, 1.0f);
                                lineY -= textHigh * 1.15f;
                            }
                        }
                        else
                        {
                            write(gil > 0 ? std::to_string(gil) + " gil" : std::string{"No gil"},
                                  panelLeft + padX, panelBottom + padBottom - textHigh * 1.5f, textHigh,
                                  kHudBright, 1.0f);
                            write("right click an item to equip or drop it, I to close",
                                  panelLeft + padX, panelBottom + padBottom - textHigh * 2.9f,
                                  textHigh * 0.85f, kHint, 1.0f);
                        }
                    }

                }


                // The equipment screen. Its own panel, not a wing of the bags:
                // what it is for is deciding what to wear, and a grid of
                // everything you own is the wrong thing to be looking at while
                // doing it.
                if (equipmentOpen)
                {
                    static const char* kSlotNames[16] = {"Main", "Sub",  "Range", "Ammo", "Head", "Body",
                                                         "Hands", "Legs", "Feet",  "Neck", "Waist", "Ear",
                                                         "Ear",  "Ring", "Ring",  "Back"};

                    // 1 to 15, so index 0 is nobody.
                    static const char* kJobNames[16] = {"---", "WAR", "MNK", "WHM", "BLM", "RDM",
                                                        "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                                        "SAM", "NIN", "DRG", "SMN"};
                    static const char* kStatNames[7] = {"STR", "DEX", "VIT", "AGI", "INT", "MND", "CHR"};

                    // Bigger than the bags' slots, not smaller: this screen
                    // is mostly words - a job, seven stats and a list of names
                    // - and it was laid out as though it were mostly pictures.
                    const float eqSlot = 0.125f;
                    const float eqSlotWide = eqSlot / windowAspect;
                    const float eqGap = eqSlot * 0.16f;
                    const float eqText = std::clamp(eqSlot * 0.34f, 0.032f, 0.048f);
                    const float cellHigh = eqSlot + eqText * 1.15f;
                    const float gridWide = 4.0f * eqSlotWide + 3.0f * eqGap;
                    const float gridHigh = 4.0f * cellHigh + 3.0f * eqGap;

                    // Widths, so divided by the aspect: eqText is a height,
                    // and a unit of NDC x is not a unit of NDC y. A column
                    // built out of undivided text heights is right on one
                    // monitor and wrong on the next.
                    const float statsWide = eqText * 8.0f / windowAspect;
                    const float listWide = eqText * 11.0f / windowAspect;
                    const float padX = eqText * 0.8f / windowAspect;
                    const float panelWide = padX * 4.0f + statsWide + gridWide + listWide;
                    const float panelHigh = gridHigh + eqText * 5.2f;
                    const float panelLeft = -panelWide * 0.5f;
                    const float panelBottom = -panelHigh * 0.5f;

                    static const float kPanel[3] = {0.05f, 0.06f, 0.12f};
                    static const float kFrame[3] = {0.20f, 0.22f, 0.34f};
                    static const float kFrameLit[3] = {0.36f, 0.40f, 0.58f};
                    static const float kChosen[3] = {0.42f, 0.48f, 0.70f};

                    quad(panelLeft, panelBottom, panelWide, panelHigh, 0, 0, kPanel, 0.92f);

                    const float topY = panelBottom + panelHigh - eqText * 1.7f;

                    // What is worn is a place, not a thing, so every slot is
                    // resolved through the bags. Built once rather than
                    // searched sixteen times.
                    std::map<std::pair<int, int>, mh::ViewerLink::InventorySlot> byPlace;
                    for (const mh::ViewerLink::InventorySlot& entry : link->inventory())
                    {
                        byPlace[{entry.container, entry.slot}] = entry;
                    }
                    const std::array<std::pair<uint8_t, uint8_t>, 16> worn = link->equipment();

                    // --- the stats column -------------------------------
                    const mh::ViewerLink::CharacterStats stats = link->characterStats();
                    const mh::ViewerLink::Vitals vitals = link->vitals();

                    float statY = topY;
                    const float statLeft = panelLeft + padX;
                    write(link->playerName().empty() ? std::string{"Equipment"} : link->playerName(),
                          statLeft, statY, eqText, kHudBright, 1.0f);
                    statY -= eqText * 1.6f;

                    if (stats.known)
                    {
                        write("Lv" + std::to_string(stats.mainLevel) + " " +
                                  kJobNames[stats.mainJob < 16 ? stats.mainJob : 0],
                              statLeft, statY, eqText, kHudBright, 1.0f);
                        statY -= eqText * 1.3f;

                        if (stats.subJob > 0 && stats.subJob < 16)
                        {
                            write("Lv" + std::to_string(stats.subLevel) + " " + kJobNames[stats.subJob],
                                  statLeft, statY, eqText * 0.9f, kHudDim, 1.0f);
                        }
                        statY -= eqText * 1.5f;
                    }

                    if (vitals.known)
                    {
                        write("HP " + std::to_string(vitals.hp) + " / " + std::to_string(stats.maxHp),
                              statLeft, statY, eqText * 0.9f, kHudDim, 1.0f);
                        statY -= eqText * 1.2f;
                        write("MP " + std::to_string(vitals.mp) + " / " + std::to_string(stats.maxMp),
                              statLeft, statY, eqText * 0.9f, kHudDim, 1.0f);
                        statY -= eqText * 1.2f;
                        write("TP " + std::to_string(vitals.tp), statLeft, statY, eqText * 0.9f, kHudDim,
                              1.0f);
                        statY -= eqText * 1.6f;
                    }

                    // Base and what the gear adds, kept apart the way the game
                    // shows them: the second number is the whole reason to be
                    // looking at this screen.
                    if (stats.known)
                    {
                        for (int i = 0; i < 7; ++i)
                        {
                            const std::string line =
                                std::string{kStatNames[i]} + " " + std::to_string(stats.base[i]);
                            write(line, statLeft, statY, eqText * 0.9f, kHudDim, 1.0f);
                            if (stats.modifier[i] != 0)
                            {
                                const std::string bonus =
                                    (stats.modifier[i] > 0 ? "+" : "") + std::to_string(stats.modifier[i]);
                                write(bonus, statLeft + eqText * 4.4f / windowAspect, statY, eqText * 0.9f,
                                      stats.modifier[i] > 0 ? kHudBright : kHudDim, 1.0f);
                            }
                            statY -= eqText * 1.2f;
                        }
                    }

                    // --- the sixteen slots ------------------------------
                    const float gridLeft = panelLeft + padX * 2.0f + statsWide;
                    const float gridTop = panelBottom + panelHigh - eqText * 3.0f;

                    for (int slot = 0; slot < 16; ++slot)
                    {
                        const int col = slot % 4;
                        const int row = slot / 4;
                        const float left = gridLeft + static_cast<float>(col) * (eqSlotWide + eqGap);
                        const float bottom =
                            gridTop - eqSlot - static_cast<float>(row) * (cellHigh + eqGap);

                        const bool under = havePointer && pointerX >= left && pointerX < left + eqSlotWide &&
                                           pointerY >= bottom && pointerY < bottom + eqSlot;
                        const bool chosen = slot == equipmentSlot;
                        quad(left, bottom, eqSlotWide, eqSlot, 0, 0,
                             chosen ? kChosen : (under ? kFrameLit : kFrame), 0.88f);
                        inventoryHits.push_back(InventoryHit{left, bottom, eqSlotWide, eqSlot, 10, slot});

                        const auto place = worn[static_cast<size_t>(slot)];
                        if (place.second != 255)
                        {
                            const auto found = byPlace.find({place.first, place.second});
                            if (found != byPlace.end())
                            {
                                const auto facing = itemFacing.find(found->second.itemId);
                                if (facing != itemFacing.end() && facing->second.cell >= 0)
                                {
                                    const float inset = eqSlot * 0.08f;
                                    quad(left + inset / windowAspect, bottom + inset,
                                         eqSlotWide - inset * 2.0f / windowAspect, eqSlot - inset * 2.0f,
                                         1, facing->second.cell, kHudBright, 1.0f);
                                }
                            }
                        }

                        write(kSlotNames[slot], left, bottom - eqText * 1.0f, eqText * 0.75f,
                              chosen ? kHudBright : kHudDim, 1.0f);
                    }

                    // --- what will fit the chosen slot ------------------
                    const float listLeft = gridLeft + gridWide + padX;
                    float listY = topY;

                    {
                        // With no slot picked this lists everything wearable
                        // and each row goes where its own item belongs; with
                        // one picked it narrows to that slot. Picking first was
                        // a step nobody wanted for the common case, which is
                        // "put this on".
                        write(equipmentSlot < 0 ? std::string{"Gear"}
                                                : std::string{kSlotNames[equipmentSlot]},
                              listLeft, listY, eqText, kHudBright, 1.0f);

                        // Taking it off is offered where there is something on.
                        if (equipmentSlot >= 0 && worn[static_cast<size_t>(equipmentSlot)].second != 255)
                        {
                            button("Remove", listLeft + eqText * 4.0f / windowAspect, listY, eqText * 0.85f, false, 12,
                                   equipmentSlot);
                        }
                        listY -= eqText * 1.7f;

                        // Only what can be worn, and only out of the bags the
                        // server will equip from: it refuses a satchel unless
                        // it has been configured otherwise, and offering one
                        // would be offering a failure.
                        static const int kEquippableFrom[] = {0, 8, 10, 11, 12, 13, 14, 15, 16};

                        int listed = 0;
                        const int room = static_cast<int>(gridHigh / (eqText * 1.5f));
                        for (const mh::ViewerLink::InventorySlot& entry : link->inventory())
                        {
                            if (listed >= room)
                            {
                                write("...", listLeft, listY, eqText * 0.8f, kHudDim, 1.0f);
                                break;
                            }

                            if (std::find(std::begin(kEquippableFrom), std::end(kEquippableFrom),
                                          static_cast<int>(entry.container)) == std::end(kEquippableFrom))
                            {
                                continue;
                            }

                            const auto facing = itemFacing.find(entry.itemId);
                            if (facing == itemFacing.end() || facing->second.slots == 0)
                            {
                                continue;
                            }

                            // Where this row would go. The chosen slot when
                            // there is one, otherwise the lowest the item
                            // names - a ring names two and either will do,
                            // and the server decides whether one is free.
                            int goesTo = equipmentSlot;
                            if (goesTo < 0)
                            {
                                goesTo = 0;
                                while (goesTo < 17 && (facing->second.slots & (1u << goesTo)) == 0)
                                {
                                    ++goesTo;
                                }
                            }
                            else if ((facing->second.slots & (1 << equipmentSlot)) == 0)
                            {
                                continue;
                            }

                            const float rowHigh = eqText * 1.4f;
                            const float rowBottom = listY - eqText * 0.2f;
                            const bool under = havePointer && pointerX >= listLeft &&
                                               pointerX < listLeft + listWide && pointerY >= rowBottom &&
                                               pointerY < rowBottom + rowHigh;
                            if (under)
                            {
                                quad(listLeft - eqText * 0.2f / windowAspect, rowBottom, listWide, rowHigh,
                                     0, 0, kFrameLit, 0.55f);
                            }

                            if (facing->second.cell >= 0)
                            {
                                quad(listLeft, listY - eqText * 0.1f, eqText * 1.1f / windowAspect,
                                     eqText * 1.1f, 1, facing->second.cell, kHudBright, 1.0f);
                            }

                            write(facing->second.name, listLeft + eqText * 1.4f / windowAspect, listY,
                                  eqText * 0.85f, kHudBright, 1.0f);
                            if (facing->second.level > 0)
                            {
                                write("Lv" + std::to_string(facing->second.level),
                                      listLeft + listWide - eqText * 2.6f / windowAspect, listY, eqText * 0.8f, kHudDim,
                                      1.0f);
                            }

                            // Both halves of where it is, in the one number a
                            // hit carries.
                            // Three numbers in the one a hit carries: where
                            // it goes, and both halves of where it is now.
                            inventoryHits.push_back(
                                InventoryHit{listLeft - eqText * 0.2f / windowAspect, rowBottom, listWide,
                                             rowHigh, 11,
                                             (goesTo << 16) | (static_cast<int>(entry.container) << 8) |
                                                 entry.slot});
                            listY -= rowHigh;
                            ++listed;
                        }

                        if (listed == 0)
                        {
                            write(equipmentSlot < 0 ? "Nothing you own can be worn."
                                                    : "Nothing you own fits here.",
                                  listLeft, listY, eqText * 0.8f, kHudDim, 1.0f);
                        }
                    }

                    write("G to close", panelLeft + padX, panelBottom + eqText * 0.6f, eqText * 0.8f,
                          kHint, 1.0f);
                }

                if (quads > 0)
                {
                    inv.counts[0] = static_cast<float>(quads);
                    inv.counts[1] = windowAspect;
                    inv.counts[2] = static_cast<float>(mh::kIconAtlasCells);
                    inv.counts[3] = static_cast<float>(mh::kIconAtlasCells);
                    inv.font[0] = static_cast<float>(textFont.columns);
                    inv.font[1] = static_cast<float>(textFont.cell);
                    inv.font[2] = static_cast<float>(textFont.width);
                    inv.font[3] = static_cast<float>(textFont.height);
                    queue.WriteBuffer(inventoryUniformBuffer, 0, &inv, sizeof(inv));
                    pass.SetPipeline(inventoryPipeline);
                    pass.SetBindGroup(0, inventoryBindGroup);
                    pass.Draw(6, static_cast<uint32_t>(quads));
                }
            }

        // The box a dead character gets, drawn over everything else.
        //
        // Outside the geometry check above on purpose: whether the zone drew
        // has nothing to do with whether the player is lying on the floor of
        // it, and a box that appeared only in zones we could read would go
        // missing exactly where the client is already least useful.
        //
        // `dead` and `raiseOffered` were read at the top of the frame, with
        // the movement gate and the pose, so what the box says and what the
        // body does cannot disagree.
        deathBoxShown = false;
        for (DialogButton& button : deathButtons)
        {
            button = DialogButton{};
        }

        if (dead && dialogPipeline && dialogBindGroup && !textFont.empty())
        {
            DialogUniforms box{};
            const float windowAspect = static_cast<float>(width) / static_cast<float>(height);

            // 1:1 with the atlas, as the HUD is - see there for why the size
            // is derived from the window rather than chosen.
            box.counts[1] = static_cast<float>(textFont.cell) * 2.0f * uiScale / static_cast<float>(height);
            box.counts[2] = windowAspect;
            box.counts[3] = 0.42f;
            box.atlas[0] = static_cast<float>(textFont.columns);
            box.atlas[1] = static_cast<float>(textFont.cell);
            box.atlas[2] = static_cast<float>(textFont.width);
            box.atlas[3] = static_cast<float>(textFont.height);

            const float line = box.counts[1];

            // One row's glyphs, and how wide they came to in cells. The same
            // shape as layOutPlate above and there for the same reason: the
            // font is proportional, so only the CPU knows where a letter
            // starts.
            const auto layOutRow = [&textFont](DialogUniforms& into, int row, const std::string& text)
            {
                const float cell = static_cast<float>(textFont.cell);
                float pen = 0.0f;
                int written = 0;
                for (char raw : text)
                {
                    if (written >= mh::kDialogChars)
                    {
                        break;
                    }
                    const float advance = textFont.advanceOf(raw) / cell;
                    float* glyph = into.glyphs[row * mh::kDialogChars + written];
                    glyph[0] = static_cast<float>(textFont.indexOf(raw));
                    glyph[1] = pen;
                    glyph[2] = advance;
                    pen += advance;
                    ++written;
                }
                into.boxes[row][2] = pen;   // total width, in cells
                return pen;
            };

            // What it says. The middle line changes the moment a raise is
            // offered, because that is the whole of why anyone waits.
            const char* said[3] = {
                "You have fallen.",
                raiseOffered ? "Someone has offered you a raise." : "Wait here for a raise, or return",
                raiseOffered ? "" : "to your home point.",
            };

            constexpr float kTitleScale = 0.55f;
            constexpr float kBodyScale = 0.40f;
            constexpr float kLabelScale = 0.44f;

            int rows = 0;
            float widest = 0.0f;   // the widest row, in NDC x

            for (int i = 0; i < 3; ++i)
            {
                if (said[i][0] == '\0')
                {
                    continue;
                }

                const float scale = i == 0 ? kTitleScale : kBodyScale;
                const float cells = layOutRow(box, rows, said[i]);
                const float* tint = i == 0 ? kDialogTitle : kDialogText;
                box.colours[rows][0] = tint[0];
                box.colours[rows][1] = tint[1];
                box.colours[rows][2] = tint[2];
                box.colours[rows][3] = scale;
                widest = std::max(widest, cells * line * scale / windowAspect);
                ++rows;
            }

            // The two answers. "Accept Raise" is drawn whether or not one has
            // been offered: a button that appears out of nowhere is a button
            // nobody was watching for, and a dead player wants to know that
            // waiting is a thing that can end.
            const int firstButton = rows;
            const char* labels[mh::kDialogButtons] = {"Return to Home Point", "Accept Raise"};
            const mh::DeathChoice choices[mh::kDialogButtons] = {mh::DeathChoice::HomePoint,
                                                                 mh::DeathChoice::AcceptRaise};
            const bool live[mh::kDialogButtons] = {true, raiseOffered};

            float labelWidest = 0.0f;
            for (int i = 0; i < mh::kDialogButtons && rows < mh::kDialogRows; ++i)
            {
                const float cells = layOutRow(box, rows, labels[i]);
                const float* tint = live[i] ? kDialogLabel : kDialogLabelOff;
                box.colours[rows][0] = tint[0];
                box.colours[rows][1] = tint[1];
                box.colours[rows][2] = tint[2];
                box.colours[rows][3] = kLabelScale;
                labelWidest = std::max(labelWidest, cells * line * kLabelScale / windowAspect);
                ++rows;
            }

            // Sized to what it has to say rather than to a number picked once.
            // The lines change with the state and the font is proportional, so
            // a fixed box would either clip a sentence or stand half empty.
            const float padX = line * 0.85f / windowAspect;
            const float padY = line * 0.45f;
            const float titleGap = line * 0.42f;
            const float bodyGap = line * 0.16f;
            const float buttonsGap = line * 0.60f;

            const float buttonHigh = line * kLabelScale * 2.0f;
            const float buttonWide = labelWidest + padX * 1.3f;
            const float buttonGap = line * 0.45f / windowAspect;
            const float buttonsWide =
                buttonWide * mh::kDialogButtons + buttonGap * (mh::kDialogButtons - 1);

            // How much room to leave under each text row. The last one before
            // the buttons gets the wider gap, which is what separates what the
            // box says from what it is asking.
            const auto gapAfter = [&](int row)
            { return row == firstButton - 1 ? buttonsGap : (row == 0 ? titleGap : bodyGap); };

            float contentHigh = buttonHigh;
            for (int row = 0; row < firstButton; ++row)
            {
                contentHigh += line * box.colours[row][3] + gapAfter(row);
            }

            const float panelWide = std::max(widest, buttonsWide) + padX * 2.0f;
            const float panelHigh = contentHigh + padY * 2.0f;
            const float panelLeft = -panelWide * 0.5f;
            const float panelBottom = -panelHigh * 0.5f;

            box.panel[0] = panelLeft;
            box.panel[1] = panelBottom;
            box.panel[2] = panelWide;
            box.panel[3] = panelHigh;

            // Where the pointer is, so whatever it is over can light up. Read
            // here rather than tracked through motion events: it is one call,
            // and it cannot fall out of step with the box it is tested against.
            float pointerX = -2.0f;
            float pointerY = -2.0f;
            {
                float mouseX = 0.0f;
                float mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                if (!pointerNdc(mouseX, mouseY, pointerX, pointerY))
                {
                    pointerX = -2.0f;   // off the window, so nothing is under it
                    pointerY = -2.0f;
                }
            }

            float cursorY = panelBottom + panelHigh - padY;
            for (int row = 0; row < firstButton; ++row)
            {
                const float high = line * box.colours[row][3];
                cursorY -= high;
                const float wide = box.boxes[row][2] * high / windowAspect;
                box.boxes[row][0] = -wide * 0.5f;   // centred, as the panel is
                box.boxes[row][1] = cursorY;
                cursorY -= gapAfter(row);
            }

            cursorY -= buttonHigh;
            for (int i = 0; i < mh::kDialogButtons; ++i)
            {
                const int row = firstButton + i;
                if (row >= mh::kDialogRows)
                {
                    break;
                }

                const float left = -buttonsWide * 0.5f + (buttonWide + buttonGap) * static_cast<float>(i);
                const bool under = live[i] && pointerX >= left && pointerX < left + buttonWide &&
                                   pointerY >= cursorY && pointerY < cursorY + buttonHigh;
                const float* fill = !live[i]                       ? kDialogButtonOff
                                    : (under || deathPressed == i) ? kDialogButtonHot
                                                                   : kDialogButton;

                box.rects[row][0] = left;
                box.rects[row][1] = cursorY;
                box.rects[row][2] = buttonWide;
                box.rects[row][3] = buttonHigh;
                box.fills[row][0] = fill[0];
                box.fills[row][1] = fill[1];
                box.fills[row][2] = fill[2];
                box.fills[row][3] = 1.0f;

                // The label, centred in its button both ways.
                const float high = line * box.colours[row][3];
                const float wide = box.boxes[row][2] * high / windowAspect;
                box.boxes[row][0] = left + (buttonWide - wide) * 0.5f;
                box.boxes[row][1] = cursorY + (buttonHigh - high) * 0.5f;

                deathButtons[i] = DialogButton{left, cursorY, buttonWide, buttonHigh, live[i], choices[i]};
            }

            deathPanel[0] = panelLeft;
            deathPanel[1] = panelBottom;
            deathPanel[2] = panelWide;
            deathPanel[3] = panelHigh;
            deathBoxShown = true;

            box.counts[0] = static_cast<float>(rows);
            queue.WriteBuffer(dialogUniformBuffer, 0, &box, sizeof(box));
            pass.SetPipeline(dialogPipeline);
            pass.SetBindGroup(0, dialogBindGroup);
            pass.Draw(3);
        }

        // A form the client put up: login, character select, and whatever else
        // used to be a window of its own.
        //
        // Drawn with the same one-triangle pass as the death box, and laid out
        // the same immediate-mode way - the rectangles this frame produces are
        // what the next click is tested against. Deliberately not sharing that
        // block: a form has rows of four different kinds and sizes itself round
        // them, and folding both into one routine made neither readable. The
        // glyph walk below is the one piece genuinely duplicated, and is worth
        // lifting out if a third caller ever wants it.
        formShown = false;
        if (!activeForm.rows.empty() && dialogPipeline && dialogBindGroup && !textFont.empty())
        {
            // What the player has typed survives across frames, but only for
            // the form that is actually up. A different one starts empty rather
            // than inheriting a password somebody typed into the last screen.
            std::string signature = activeForm.title;
            std::string choices;
            for (const mh::FormRow& row : activeForm.rows)
            {
                signature += '\x1f';
                signature += std::to_string(static_cast<int>(row.kind));
                signature += row.text;
                if (row.kind == mh::FormRowKind::Choice)
                {
                    choices += row.value;
                    choices += '\x1e';
                }
            }

            if (signature != formSignature)
            {
                formSignature = signature;
                formChoiceSignature = choices;
                formOpenChoice = -1;
                formValues.assign(activeForm.rows.size(), std::string{});
                for (size_t i = 0; i < activeForm.rows.size(); ++i)
                {
                    formValues[i] = activeForm.rows[i].value;
                }

                // Focus starts on the first thing that can be typed into, so a
                // form can be filled in without reaching for the mouse first.
                formFocus = -1;
                for (size_t i = 0; i < activeForm.rows.size(); ++i)
                {
                    const mh::FormRowKind kind = activeForm.rows[i].kind;
                    if ((kind == mh::FormRowKind::Field || kind == mh::FormRowKind::Secret) &&
                        activeForm.rows[i].enabled)
                    {
                        formFocus = static_cast<int>(i);
                        break;
                    }
                }
                SDL_StartTextInput(window);
            }
            else if (choices != formChoiceSignature)
            {
                // The same screen, but the client moved a choice - a race that
                // changed reset the face, say. Only those rows are refreshed,
                // so what was typed elsewhere stays and the focus stays put.
                formChoiceSignature = choices;
                for (size_t i = 0; i < activeForm.rows.size() && i < formValues.size(); ++i)
                {
                    if (activeForm.rows[i].kind == mh::FormRowKind::Choice)
                    {
                        formValues[i] = activeForm.rows[i].value;
                    }
                }
            }

            DialogUniforms form{};
            const float windowAspect = static_cast<float>(width) / static_cast<float>(height);
            form.counts[1] = static_cast<float>(textFont.cell) * 2.0f * uiScale / static_cast<float>(height);
            form.counts[2] = windowAspect;
            // Dimmer than the death box: there may be no world behind it. Unless
            // the form stands aside for something in the world, which then has
            // to be seen.
            form.counts[3] = activeForm.aside ? 0.10f : 0.62f;
            form.atlas[0] = static_cast<float>(textFont.columns);
            form.atlas[1] = static_cast<float>(textFont.cell);
            form.atlas[2] = static_cast<float>(textFont.width);
            form.atlas[3] = static_cast<float>(textFont.height);

            const float line = form.counts[1];

            const auto layOutRow = [&textFont](DialogUniforms& into, int row, const std::string& text)
            {
                const float cell = static_cast<float>(textFont.cell);
                float pen = 0.0f;
                int written = 0;
                for (char raw : text)
                {
                    if (written >= mh::kDialogChars)
                    {
                        break;
                    }
                    const float advance = textFont.advanceOf(raw) / cell;
                    float* glyph = into.glyphs[row * mh::kDialogChars + written];
                    glyph[0] = static_cast<float>(textFont.indexOf(raw));
                    glyph[1] = pen;
                    glyph[2] = advance;
                    pen += advance;
                    ++written;
                }
                for (int spare = written; spare < mh::kDialogChars; ++spare)
                {
                    into.glyphs[row * mh::kDialogChars + spare][2] = 0.0f;
                }
                into.boxes[row][2] = pen;
                return pen;
            };

            constexpr float kTitleScale = 0.55f;
            constexpr float kCaptionScale = 0.38f;
            constexpr float kValueScale = 0.42f;
            constexpr float kLabelScale = 0.44f;
            constexpr float kMessageScale = 0.36f;

            const float padX = line * 0.85f / windowAspect;
            const float padY = line * 0.45f;

            // What each row of the form becomes on screen, in order. A field is
            // its caption and then the box under it, so one form row can be two
            // drawn rows - which is why this is built rather than indexed.
            struct Placed
            {
                int formRow;          // -1 for the title and the message
                int drawRow;
                mh::FormRowKind kind;
                bool isValue;         // the box under a caption
                float scale;
            };
            std::vector<Placed> placed;
            int drawn = 0;
            float widest = 0.0f;

            const auto addText = [&](int formRow, const std::string& text, float scale,
                                     const float* tint, mh::FormRowKind kind, bool isValue)
            {
                if (drawn >= mh::kDialogRows)
                {
                    return;
                }
                const float cells = layOutRow(form, drawn, text);
                form.colours[drawn][0] = tint[0];
                form.colours[drawn][1] = tint[1];
                form.colours[drawn][2] = tint[2];
                form.colours[drawn][3] = scale;
                widest = std::max(widest, cells * line * scale / windowAspect);
                placed.push_back(Placed{formRow, drawn, kind, isValue, scale});
                ++drawn;
            };

            if (!activeForm.title.empty())
            {
                addText(-1, activeForm.title, kTitleScale, kDialogTitle, mh::FormRowKind::Label, false);
            }

            int buttonCount = 0;
            for (size_t i = 0; i < activeForm.rows.size(); ++i)
            {
                const mh::FormRow& row = activeForm.rows[i];
                const int formRow = static_cast<int>(i);

                if (row.kind == mh::FormRowKind::Button)
                {
                    ++buttonCount;
                    continue;   // buttons go in a row of their own, below
                }

                if (row.kind == mh::FormRowKind::Label)
                {
                    addText(formRow, row.text, kCaptionScale, kDialogText, row.kind, false);
                    continue;
                }

                if (row.kind == mh::FormRowKind::Choice)
                {
                    // One box reading "CAPTION: OPTION v", unfolding into the
                    // options when pressed. No caption row of its own: a
                    // screen with seven of these would be twice the height.
                    const std::vector<std::string> options = choiceOptions(formValues[i]);
                    const int chosen = choiceSelected(formValues[i]);
                    std::string shown = row.text;
                    if (!shown.empty())
                    {
                        shown += ": ";
                    }
                    if (chosen >= 0 && static_cast<size_t>(chosen) < options.size())
                    {
                        shown += options[static_cast<size_t>(chosen)];
                    }
                    shown += "  v";
                    addText(formRow, shown, kValueScale,
                            row.enabled ? kDialogLabel : kDialogLabelOff, row.kind, true);
                    continue;
                }

                // A field: its caption, then what has been typed into it.
                if (!row.text.empty())
                {
                    addText(formRow, row.text, kCaptionScale, kDialogText, mh::FormRowKind::Label, false);
                }

                const std::string& value = formValues[i];
                const std::string shown = row.kind == mh::FormRowKind::Secret
                                              ? std::string(value.size(), '*')
                                              : value;
                addText(formRow, shown, kValueScale,
                        row.enabled ? kDialogLabel : kDialogLabelOff, row.kind, true);
            }

            // The buttons, across the bottom.
            float labelWidest = 0.0f;
            const int firstButtonDraw = drawn;
            for (size_t i = 0; i < activeForm.rows.size() && drawn < mh::kDialogRows; ++i)
            {
                if (activeForm.rows[i].kind != mh::FormRowKind::Button)
                {
                    continue;
                }
                const float cells = layOutRow(form, drawn, activeForm.rows[i].text);
                const float* tint = activeForm.rows[i].enabled ? kDialogLabel : kDialogLabelOff;
                form.colours[drawn][0] = tint[0];
                form.colours[drawn][1] = tint[1];
                form.colours[drawn][2] = tint[2];
                form.colours[drawn][3] = kLabelScale;
                labelWidest = std::max(labelWidest, cells * line * kLabelScale / windowAspect);
                placed.push_back(Placed{static_cast<int>(i), drawn, mh::FormRowKind::Button, false, kLabelScale});
                ++drawn;
            }

            // Wrapped, not cut. A row holds forty characters and the message
            // was drawn as one, so every explanation longer than that lost its
            // end - and the messages worth showing here are the ones that
            // explain something, which are exactly the long ones. "ANOTHER
            // CHARACTER ON THIS ACCOUNT IS STILL LOGGED IN..." stopped at
            // "ACCOUNT".
            //
            // Broken at spaces where there is one, and mid-word only for a
            // word longer than a line, which is not a thing English does but
            // is a thing a server's error code does.
            if (!activeForm.message.empty())
            {
                const size_t width = static_cast<size_t>(mh::kDialogChars);
                size_t at = 0;
                while (at < activeForm.message.size() && drawn < mh::kDialogRows)
                {
                    size_t take = std::min(width, activeForm.message.size() - at);
                    if (at + take < activeForm.message.size())
                    {
                        const size_t space = activeForm.message.rfind(' ', at + take);
                        if (space != std::string::npos && space > at)
                        {
                            take = space - at;
                        }
                    }

                    addText(-1, activeForm.message.substr(at, take), kMessageScale, kDialogTitle,
                            mh::FormRowKind::Label, false);

                    at += take;
                    while (at < activeForm.message.size() && activeForm.message[at] == ' ')
                    {
                        ++at;
                    }
                }
            }

            // A form too tall for the uniform simply stops being drawn part way
            // through, and the rows that fall off the end are the last ones -
            // which on a sign-in screen are the buttons. Adding a field to that
            // screen silently cost it "remember server" and "quit", and nothing
            // anywhere said so. Say so.
            if (drawn >= mh::kDialogRows)
            {
                static bool complained = false;
                if (!complained)
                {
                    complained = true;
                    std::printf("form needs more than %d rows; the rest is not drawn - "
                                "raise kDialogRows in dialog_shader.h\n",
                                mh::kDialogRows);
                }
            }

            const float fieldHigh = line * kValueScale * 2.0f;
            const float buttonHigh = line * kLabelScale * 2.0f;
            const float buttonGap = line * 0.45f / windowAspect;
            const float buttonWide = labelWidest + padX * 1.3f;
            const float buttonsWide = buttonCount > 0
                                          ? buttonWide * buttonCount + buttonGap * (buttonCount - 1)
                                          : 0.0f;

            // Fields are drawn wider than their text so an empty one still looks
            // like somewhere to type.
            const float fieldWide = std::max(widest, buttonsWide) + padX * 0.6f;

            const float rowGap = line * 0.18f;
            const float groupGap = line * 0.34f;

            float contentHigh = 0.0f;
            for (const Placed& item : placed)
            {
                if (item.kind == mh::FormRowKind::Button)
                {
                    continue;
                }
                contentHigh += (item.isValue ? fieldHigh : line * item.scale) + rowGap;
            }
            if (buttonCount > 0)
            {
                contentHigh += groupGap + buttonHigh;
            }

            const float panelWide = std::max(fieldWide, buttonsWide) + padX * 2.0f;
            const float panelHigh = contentHigh + padY * 2.0f;

            // Centred, or stood against the left edge with the world showing
            // beside it. Everything below is laid out about x = 0 and shifted
            // by this, so the one decision is made once.
            const float shiftX = activeForm.aside ? (-0.96f + panelWide * 0.5f) : 0.0f;
            form.panel[0] = -panelWide * 0.5f + shiftX;
            form.panel[1] = -panelHigh * 0.5f;
            form.panel[2] = panelWide;
            form.panel[3] = panelHigh;
            std::memcpy(formPanel, form.panel, sizeof(formPanel));

            float pointerX = -2.0f;
            float pointerY = -2.0f;
            {
                float mouseX = 0.0f;
                float mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                if (!pointerNdc(mouseX, mouseY, pointerX, pointerY))
                {
                    pointerX = -2.0f;
                    pointerY = -2.0f;
                }
            }

            formFieldRects.assign(activeForm.rows.size() * 4, 0.0f);
            formButtons.clear();
            formButtonRow.clear();
            formChoiceBoxes.clear();
            formChoiceBoxRow.clear();
            formChoiceHits.clear();
            formChoiceHitOption.clear();

            // Where the unfolded choice's box ended up, for hanging its
            // options off it once everything else is placed.
            bool openFound = false;
            float openLeft = 0.0f;
            float openBottom = 0.0f;

            float cursorY = form.panel[1] + panelHigh - padY;
            for (const Placed& item : placed)
            {
                if (item.kind == mh::FormRowKind::Button)
                {
                    continue;
                }

                if (item.isValue)
                {
                    cursorY -= fieldHigh;

                    const float left = -fieldWide * 0.5f + shiftX;
                    form.rects[item.drawRow][0] = left;
                    form.rects[item.drawRow][1] = cursorY;
                    form.rects[item.drawRow][2] = fieldWide;
                    form.rects[item.drawRow][3] = fieldHigh;

                    const bool focused = item.formRow == formFocus;
                    const bool isChoice = item.kind == mh::FormRowKind::Choice;
                    const bool unfolded = isChoice && item.formRow == formOpenChoice;
                    // A choice is drawn as a button - something to press -
                    // rather than as a box to type into.
                    const float* fill = isChoice ? ((focused || unfolded) ? kDialogButtonHot : kDialogButton)
                                                 : (focused ? kDialogButtonHot : kDialogButtonOff);
                    form.fills[item.drawRow][0] = fill[0];
                    form.fills[item.drawRow][1] = fill[1];
                    form.fills[item.drawRow][2] = fill[2];
                    form.fills[item.drawRow][3] = 1.0f;

                    // Text sits inside the box rather than centred on it, the
                    // way a field reads: it fills from the left as you type.
                    const float high = line * item.scale;
                    const float inset = padX * 0.35f;
                    form.boxes[item.drawRow][0] = left + inset;
                    form.boxes[item.drawRow][1] = cursorY + (fieldHigh - high) * 0.5f;

                    if (item.formRow >= 0)
                    {
                        float* rect = &formFieldRects[static_cast<size_t>(item.formRow) * 4];
                        rect[0] = left;
                        rect[1] = cursorY;
                        rect[2] = fieldWide;
                        rect[3] = fieldHigh;
                    }

                    if (isChoice)
                    {
                        DialogButton hit{};
                        hit.left = left;
                        hit.bottom = cursorY;
                        hit.width = fieldWide;
                        hit.height = fieldHigh;
                        hit.enabled = item.formRow >= 0 &&
                                      activeForm.rows[static_cast<size_t>(item.formRow)].enabled;
                        formChoiceBoxes.push_back(hit);
                        formChoiceBoxRow.push_back(item.formRow);
                        if (unfolded)
                        {
                            openFound = true;
                            openLeft = left;
                            openBottom = cursorY;
                        }
                    }

                    // The caret, just past the last glyph of the focused field.
                    if (focused && !isChoice)
                    {
                        const float textWide = form.boxes[item.drawRow][2] * high / windowAspect;
                        form.caret[0] = form.boxes[item.drawRow][0] + textWide;
                        form.caret[1] = cursorY + fieldHigh * 0.18f;
                        form.caret[2] = line * 0.06f / windowAspect;
                        form.caret[3] = fieldHigh * 0.64f;
                    }

                    cursorY -= rowGap;
                    continue;
                }

                const float high = line * item.scale;
                cursorY -= high;
                const float wide = form.boxes[item.drawRow][2] * high / windowAspect;
                form.boxes[item.drawRow][0] = -wide * 0.5f + shiftX;
                form.boxes[item.drawRow][1] = cursorY;
                cursorY -= rowGap;
            }

            if (buttonCount > 0)
            {
                cursorY -= groupGap;
                cursorY -= buttonHigh;

                float buttonLeft = -buttonsWide * 0.5f + shiftX;
                for (const Placed& item : placed)
                {
                    if (item.kind != mh::FormRowKind::Button)
                    {
                        continue;
                    }

                    const bool enabled = activeForm.rows[item.formRow].enabled;
                    const bool over = enabled && pointerX >= buttonLeft &&
                                      pointerX < buttonLeft + buttonWide && pointerY >= cursorY &&
                                      pointerY < cursorY + buttonHigh;
                    const bool held = formPressed == static_cast<int>(formButtons.size());

                    // Tab reaches buttons, so one can be the thing being looked
                    // at with no pointer anywhere near it. Lit the same as
                    // hovered: what return will press has to be visible, or
                    // tabbing past sign-in to quit is done blind.
                    const bool focused = enabled && item.formRow == formFocus;

                    form.rects[item.drawRow][0] = buttonLeft;
                    form.rects[item.drawRow][1] = cursorY;
                    form.rects[item.drawRow][2] = buttonWide;
                    form.rects[item.drawRow][3] = buttonHigh;

                    const float* fill = !enabled  ? kDialogButtonOff
                                        : (over || held || focused) ? kDialogButtonHot
                                                                    : kDialogButton;
                    form.fills[item.drawRow][0] = fill[0];
                    form.fills[item.drawRow][1] = fill[1];
                    form.fills[item.drawRow][2] = fill[2];
                    form.fills[item.drawRow][3] = 1.0f;

                    const float high = line * item.scale;
                    const float wide = form.boxes[item.drawRow][2] * high / windowAspect;
                    form.boxes[item.drawRow][0] = buttonLeft + (buttonWide - wide) * 0.5f;
                    form.boxes[item.drawRow][1] = cursorY + (buttonHigh - high) * 0.5f;

                    DialogButton hit{};
                    hit.left = buttonLeft;
                    hit.bottom = cursorY;
                    hit.width = buttonWide;
                    hit.height = buttonHigh;
                    hit.enabled = enabled;
                    formButtons.push_back(hit);
                    formButtonRow.push_back(item.formRow);

                    buttonLeft += buttonWide + buttonGap;
                }
            }

            // The unfolded choice's options, a column of buttons hanging from
            // its box. Laid out last so they sit over whatever is beneath
            // them, and downward unless there is no room, in which case they
            // climb.
            if (openFound && formOpenChoice >= 0 && static_cast<size_t>(formOpenChoice) < formValues.size())
            {
                const std::string& value = formValues[static_cast<size_t>(formOpenChoice)];
                const std::vector<std::string> options = choiceOptions(value);
                const int chosen = choiceSelected(value);
                const float optionHigh = line * kValueScale * 1.7f;
                const float needed = optionHigh * static_cast<float>(options.size());
                const bool upward = openBottom - needed < -0.98f;
                float optionBottom = upward ? openBottom + fieldHigh : openBottom - optionHigh;

                for (size_t i = 0; i < options.size() && drawn < mh::kDialogRows; ++i)
                {
                    const bool over = pointerX >= openLeft && pointerX < openLeft + fieldWide &&
                                      pointerY >= optionBottom && pointerY < optionBottom + optionHigh;
                    const bool current = static_cast<int>(i) == chosen;

                    layOutRow(form, drawn, options[i]);
                    form.colours[drawn][0] = kDialogLabel[0];
                    form.colours[drawn][1] = kDialogLabel[1];
                    form.colours[drawn][2] = kDialogLabel[2];
                    form.colours[drawn][3] = kValueScale;

                    form.rects[drawn][0] = openLeft;
                    form.rects[drawn][1] = optionBottom;
                    form.rects[drawn][2] = fieldWide;
                    form.rects[drawn][3] = optionHigh;
                    const float* fill = over ? kDialogButtonHot : (current ? kDialogButton : kDialogButtonOff);
                    form.fills[drawn][0] = fill[0];
                    form.fills[drawn][1] = fill[1];
                    form.fills[drawn][2] = fill[2];
                    form.fills[drawn][3] = 1.0f;

                    const float high = line * kValueScale;
                    form.boxes[drawn][0] = openLeft + padX * 0.35f;
                    form.boxes[drawn][1] = optionBottom + (optionHigh - high) * 0.5f;

                    DialogButton hit{};
                    hit.left = openLeft;
                    hit.bottom = optionBottom;
                    hit.width = fieldWide;
                    hit.height = optionHigh;
                    hit.enabled = true;
                    formChoiceHits.push_back(hit);
                    formChoiceHitOption.push_back(static_cast<int>(i));

                    ++drawn;
                    optionBottom += upward ? optionHigh : -optionHigh;
                }
            }

            formShown = true;
            form.counts[0] = static_cast<float>(drawn);
            queue.WriteBuffer(dialogUniformBuffer, 0, &form, sizeof(form));
            pass.SetPipeline(dialogPipeline);
            pass.SetBindGroup(0, dialogBindGroup);
            pass.Draw(3);
        }
        else if (formSignature.length() > 0 && activeForm.rows.empty())
        {
            // The form came down. Stop taking text, or the next key press goes
            // into a field nobody can see.
            formSignature.clear();
            formChoiceSignature.clear();
            formValues.clear();
            formButtons.clear();
            formChoiceBoxes.clear();
            formChoiceHits.clear();
            formOpenChoice = -1;
            formFocus = -1;
            SDL_StopTextInput(window);
        }

        pass.End();

        // A texture copy has to start on a 256-byte row, so the readback is
        // padded and the padding is skipped when the rows are written out.
        const uint32_t bytesPerRow = (width * 4 + 255) / 256 * 256;
        // Somebody typing /bug wants this frame written and the game to carry
        // on; MOGHOUSE_SCREENSHOT wants one frame and an exit. Same readback,
        // and only the second one breaks out of the loop below.
        std::string onDemandPath;
        const bool onDemand = link && link->takeCapture(onDemandPath);

        const bool takingShot = onDemand ||
                                (screenshotPath && ++shotIndex >= 0 &&
                                 (sequenceCount == 0 || shotIndex < sequenceCount));
        char shotPath[1024] = {};
        if (onDemand)
        {
            std::snprintf(shotPath, sizeof(shotPath), "%s", onDemandPath.c_str());
        }
        else if (takingShot)
        {
            if (sequenceCount > 0)
            {
                std::snprintf(shotPath, sizeof(shotPath), screenshotPath, shotIndex);
            }
            else
            {
                std::snprintf(shotPath, sizeof(shotPath), "%s", screenshotPath);
            }
        }
        if (takingShot)
        {
            wgpu::BufferDescriptor readbackDescriptor{.usage = wgpu::BufferUsage::CopyDst |
                                                               wgpu::BufferUsage::MapRead,
                                                      .size = static_cast<uint64_t>(bytesPerRow) * height};
            readbackBuffer = device.CreateBuffer(&readbackDescriptor);

            wgpu::TexelCopyTextureInfo source{.texture = surfaceTexture.texture};
            wgpu::TexelCopyBufferInfo destination{
                .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = height}, .buffer = readbackBuffer};
            const wgpu::Extent3D extent{width, height, 1};
            encoder.CopyTextureToBuffer(&source, &destination, &extent);
        }

        wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        surface.Present();

        // The loading screen is on screen now, so the world can be replaced
        // underneath it. Everything belonging to the zone being left goes
        // first: names are per-zone, and so is every entity in the pose table.
        if (pendingZone)
        {
            pendingZone = false;
            currentZonePath = pending.datPath;
            currentZoneName = pending.zoneName;
            entityNames = ffxi::EntityNames{};
            // And let them be looked up again. The table is loaded once, the
            // first time an entity arrives, behind this latch - clearing the
            // names without clearing the latch meant the new zone's table was
            // never read, so every NPC after the first zone was nameless.
            triedEntityNames = false;
            entityPoses.clear();
            radarEntities.clear();

            // Built models are cached by look, and the cache had no end to it:
            // every NPC and every creature ever drawn kept its geometry and its
            // uploaded textures for the rest of the session. Nothing shared
            // them across zones anyway - a zone's cast is its own - so holding
            // them was paying memory for a hit rate of zero.
            npcModels.clear();
            creatureModels.clear();

            if (readZone() == 0)
            {
                // Who the player turned out to be. The sign-in happens in this
                // window now, so the character is only known well after the
                // renderer started, and this is the first moment there is a
                // world for the body to stand in.
                //
                // After readZone, never before it: reading a zone clears the
                // texture cache, so a character built first loses its textures
                // to the zone that follows and renders as white nothing. This
                // is the order the startup path uses for the same reason.
                std::string wanted;
                if (link && link->takeLook(wanted) && !wanted.empty())
                {
                    currentLook = wanted;
                    characterScale = scaleOfLook(currentLook);
                }
                else if (character && !currentLook.empty())
                {
                    // Nobody new, but the body has to be built again all the
                    // same: readZone emptied the texture cache and the
                    // character's textures went with it. Left as it was, it
                    // uploads with none and stands in the new zone white.
                    wanted = currentLook;
                }

                if (!wanted.empty())
                {
                    if (auto rebuilt = buildFromLook(wanted.c_str()))
                    {
                        character = std::move(rebuilt);

                        // "A character in the world starts in the second" - and
                        // this is the first moment there is one. `driving` is
                        // decided once before the loop, from a character that
                        // does not exist yet when the client signs in inside
                        // this window, so without this the camera stays in
                        // free-fly for the rest of the session: it never
                        // follows anyone, and the world is viewed from
                        // wherever the empty screen happened to leave it.
                        driving = true;

                        // And the clips it animates with, which were bound
                        // before the loop from a character that did not exist
                        // then. Nothing is playing yet: the next frame picks
                        // idle, walk or run from what the body is doing, as it
                        // does for a character present from the start.
                        bindClips();
                        playing = nullptr;
                        jumpUntil = 0.0f;
                    }
                }

                // The character needs uploading through the pipelines the zone
                // brought with it. This was one-off setup back when a zone was
                // always present at startup.
                //
                // setUpRadar is deliberately NOT called here. Re-running it
                // took the world and the whole interface with it: the radar
                // builds resources that the pipelines set up after it are bound
                // to, and replacing those leaves every one of those bind groups
                // pointing at something that no longer exists. The radar is
                // built once, from whatever zone the window opened with.
                makeGhost();
                uploadCharacter();

                // The new zone's size, for the far plane and the light.
                radius = zone ? std::max(zone->radius(), 1.0f) : 1.0f;
                findLineupSpot();
                findMonorail();

                characterAt = {pending.x, pending.y, pending.z};
                characterFacing = pending.heading;
                placeCharacter(characterAt, 60.0f, true);
                breadcrumbs.clear();
                std::printf("zoned into %s\n", pending.zoneName.c_str());
            }
            else
            {
                std::printf("could not read %s\n", pending.datPath.c_str());
            }

            link->setLoading(false);
            loadingZone.clear();
        }

        // A change of gear, with no zone change to carry it.
        //
        // The look was read only while a zone was loading, so putting armour
        // on showed up the next time you zoned and never before: the client
        // asked the server what it looked like, the server answered, the
        // answer was correct, and it sat in the link until something else
        // happened to want it.
        //
        // Rebuilt in place, which is what the retail client appears to do -
        // the character is taken away and put back rather than altered. The
        // clips have to be rebound because they point into the old skeleton,
        // and nothing is left playing: the next frame picks idle, walk or run
        // from what the body is actually doing.
        if (!pendingZone && link && character)
        {
            std::string wanted;
            if (link->takeLook(wanted) && !wanted.empty() && wanted != currentLook)
            {
                if (auto rebuilt = buildFromLook(wanted.c_str()))
                {
                    currentLook = wanted;
                    characterScale = scaleOfLook(currentLook);
                    character = std::move(rebuilt);
                    makeGhost();
                    uploadCharacter();
                    bindClips();
                    playing = nullptr;
                    std::printf("rebuilt the character as %s\n", currentLook.c_str());
                }
            }
        }
        instance.ProcessEvents();

        if (takingShot)
        {
            bool mapped = false;
            wgpu::Future future = readbackBuffer.MapAsync(
                wgpu::MapMode::Read, 0, wgpu::kWholeMapSize, wgpu::CallbackMode::AllowProcessEvents,
                [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                { mapped = status == wgpu::MapAsyncStatus::Success; });
            instance.WaitAny(future, UINT64_MAX);

            if (mapped)
            {
                const auto* pixels = static_cast<const uint8_t*>(readbackBuffer.GetConstMappedRange());
                if (pixels && writeBmp(shotPath, pixels, width, height, bytesPerRow))
                {
                    // The character height goes with it: a fall is far easier to
                    // read as a column of numbers than as a strip of pictures.
                    std::printf("wrote %s (%ux%u) characterY=%.2f\n", shotPath, width, height,
                                characterAt.y);
                }
                else
                {
                    std::printf("could not write %s\n", shotPath);
                }
                readbackBuffer.Unmap();
            }
            else
            {
                std::printf("could not read the frame back\n");
            }

            if (!onDemand && (sequenceCount == 0 || shotIndex + 1 >= sequenceCount))
            {
                break;
            }
        }
    }

    // `music` is a local declared far above, so its destructor runs at the end
    // of this function - after SDL_Quit below. Destroying an SDL audio stream
    // once the audio subsystem is gone locks a freed mutex, so shut it down
    // here while SDL is still up. shutdown() is idempotent; the destructor
    // still runs and finds nothing left to do.
    music.shutdown();

    // Before SDL_Quit for the same reason the music is: destroying an audio
    // stream after the subsystem has gone locks a mutex that has been freed.
    sounds.shutdown();

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
