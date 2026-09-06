#include "character.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace mh
{
namespace
{
/// Column-major rotation from a quaternion. No translation: see BonePose.
Mat4 rotationOf(const float q[4])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];

    Mat4 out = Mat4::identity();
    out.m[0] = 1.0f - 2.0f * (y * y + z * z);
    out.m[1] = 2.0f * (x * y + z * w);
    out.m[2] = 2.0f * (x * z - y * w);

    out.m[4] = 2.0f * (x * y - z * w);
    out.m[5] = 1.0f - 2.0f * (x * x + z * z);
    out.m[6] = 2.0f * (y * z + x * w);

    out.m[8] = 2.0f * (x * z + y * w);
    out.m[9] = 2.0f * (y * z - x * w);
    out.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return out;
}

/// Hamilton product, in x y z w order to match the file.
void multiplyQuaternions(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/// Shortest-arc interpolation between two frames.
void slerp(const float a[4], const float b[4], float t, float out[4])
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];

    // A quaternion and its negation are the same rotation, so flipping one
    // when they point apart takes the short way round rather than swinging the
    // limb most of the way through a circle.
    float sign = 1.0f;
    if (dot < 0.0f)
    {
        dot = -dot;
        sign = -1.0f;
    }

    float weightA = 1.0f - t;
    float weightB = t * sign;
    if (dot < 0.9995f)
    {
        const float angle = std::acos(dot);
        const float invSin = 1.0f / std::sin(angle);
        weightA = std::sin((1.0f - t) * angle) * invSin;
        weightB = std::sin(t * angle) * invSin * sign;
    }

    float length = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        out[i] = a[i] * weightA + b[i] * weightB;
        length += out[i] * out[i];
    }
    length = std::sqrt(length);
    if (length > 0.0f)
    {
        for (int i = 0; i < 4; ++i)
        {
            out[i] /= length;
        }
    }
}

Vec3 rotate(const Mat4& m, const Vec3& v)
{
    return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z, m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
            m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z};
}

/// Reflection through one axis, applied in the bone's own space before the
/// bone rotates it. Axis 0 means the vertex sits on the centre line and is
/// shared between the two halves.
Vec3 mirrored(const Vec3& v, uint8_t axis)
{
    switch (axis)
    {
    case 1: return {-v.x, v.y, v.z};
    case 2: return {v.x, -v.y, v.z};
    case 3: return {v.x, v.y, -v.z};
    default: return v;
    }
}

/// The same rule the zone uses: a texture whose transparent blocks are also
/// black is a cutout mask rather than a blend factor.
constexpr float kCutoutSignal = 0.2f;

struct Skinned
{
    Vec3 position;
    Vec3 normal;
};

/// One vertex through the skin.
///
/// The position stored against each influence already carries that
/// influence's weight, so the rotated position is summed as-is and only the
/// bone's translation is scaled.
Skinned skin(const ffxi::SkinVertex& vertex, const std::vector<BonePose>& pose, bool useMirror)
{
    Skinned out{{}, {}};
    for (uint8_t i = 0; i < vertex.influences; ++i)
    {
        const ffxi::SkinInfluence& influence = vertex.influence[i];
        const uint8_t bone = useMirror ? influence.boneMirror : influence.bone;
        const uint8_t axis = useMirror ? influence.mirrorAxis : 0;
        if (bone >= pose.size())
        {
            continue;
        }

        const BonePose& b = pose[bone];
        const Vec3 p{influence.position[0] * b.scale.x, influence.position[1] * b.scale.y,
                     influence.position[2] * b.scale.z};
        const Vec3 n{influence.normal[0], influence.normal[1], influence.normal[2]};

        out.position = out.position + rotate(b.rotation, mirrored(p, axis)) + b.translation * influence.weight;
        out.normal = out.normal + rotate(b.rotation, mirrored(n, axis));
    }
    return out;
}

/// Walks every triangle corner a character draws, in the order it is drawn.
///
/// Building the geometry and re-skinning it each frame have to agree on that
/// order exactly, and the surest way to make them agree is for there to be
/// only one walk.
template <typename Fn> void forEachCorner(const std::vector<ffxi::SkinnedModel>& meshes, Fn&& fn)
{
    for (const ffxi::SkinnedModel& mesh : meshes)
    {
        // A mirrored mesh is half a body: the second pass reflects every
        // vertex onto its opposite bone. Nothing marks which half is stored,
        // because both come from the same triangles.
        const int passes = mesh.mirrored ? 2 : 1;

        for (const ffxi::SkinnedPart& part : mesh.parts)
        {
            if (part.corners.empty())
            {
                continue;
            }
            for (int pass = 0; pass < passes; ++pass)
            {
                for (const ffxi::SkinCorner& corner : part.corners)
                {
                    if (corner.vertex >= mesh.vertices.size())
                    {
                        continue;
                    }
                    fn(mesh.vertices[corner.vertex], corner, part, pass == 1);
                }
            }
        }
    }
}

/// Turns one skinned corner into the vertex the GPU sees.
Vertex toVertex(const Skinned& s, const ffxi::SkinCorner& corner)
{
    Vertex vertex{};
    // The same half turn about X the zone gets - see zonemesh.cpp. Without it
    // a character stands on its head; with only the vertical negated it is
    // mirrored, which on a roughly symmetric body is invisible until it is
    // wearing something that is not.
    vertex.position[0] = s.position.x;
    vertex.position[1] = -s.position.y;
    vertex.position[2] = -s.position.z;

    const Vec3 unit = normalise(Vec3{s.normal.x, -s.normal.y, -s.normal.z});
    vertex.normal[0] = unit.x;
    vertex.normal[1] = unit.y;
    vertex.normal[2] = unit.z;

    vertex.uv[0] = corner.uv[0];
    vertex.uv[1] = corner.uv[1];
    return vertex;
}
} // namespace

const char* slotChunkId(Slot slot)
{
    switch (slot)
    {
    case Slot::Face: return "hh_1";
    case Slot::Head: return "hh_m";
    case Slot::Body: return "hh_b";
    case Slot::Hands: return "hh_h";
    case Slot::Legs: return "hh_l";
    case Slot::Feet: return "hh_f";
    default: return "";
    }
}

const char* slotName(Slot slot)
{
    switch (slot)
    {
    case Slot::Face: return "face";
    case Slot::Head: return "head";
    case Slot::Body: return "body";
    case Slot::Hands: return "hands";
    case Slot::Legs: return "legs";
    case Slot::Feet: return "feet";
    default: return "?";
    }
}

std::vector<BonePose> bindPose(const ffxi::Skeleton& skeleton)
{
    std::vector<BonePose> pose(skeleton.bones.size());

    // Parents always come before their children in the file, so a single pass
    // in order resolves the whole chain. parseSkeleton has already refused
    // anything that is not a tree, so this cannot loop.
    for (size_t i = 0; i < skeleton.bones.size(); ++i)
    {
        const ffxi::Bone& bone = skeleton.bones[i];
        const Mat4 local = rotationOf(bone.rotation);
        const Vec3 offset{bone.translation[0], bone.translation[1], bone.translation[2]};

        if (bone.parent == i)
        {
            pose[i].rotation = local;
            pose[i].translation = offset;
        }
        else
        {
            const BonePose& parent = pose[bone.parent];
            pose[i].rotation = parent.rotation * local;
            pose[i].translation = parent.translation + rotate(parent.rotation, offset);
        }
    }
    return pose;
}

std::vector<BonePose> animatedPose(const ffxi::Skeleton& skeleton, const ffxi::Animation& animation, float frame)
{
    return animatedPose(skeleton, animation, frame, nullptr, 0.0f);
}

std::vector<BonePose> animatedPose(const ffxi::Skeleton& skeleton, const ffxi::Animation& animation,
                                   float frame, const ffxi::Animation* overlay, float overlayFrame)
{
    const size_t boneCount = skeleton.bones.size();

    // Start from the rest pose. Bones the animation says nothing about keep
    // it, which is most of them - an idle touches sixteen of ninety-four.
    std::vector<std::array<float, 4>> localRotation(boneCount);
    std::vector<Vec3> localTranslation(boneCount);
    std::vector<Vec3> localScale(boneCount, Vec3{1.0f, 1.0f, 1.0f});
    for (size_t i = 0; i < boneCount; ++i)
    {
        const ffxi::Bone& bone = skeleton.bones[i];
        localRotation[i] = {bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3]};
        localTranslation[i] = {bone.translation[0], bone.translation[1], bone.translation[2]};
    }

    // FFXI splits a character's motion into two clips. One drives the root,
    // the hips and the legs - sixteen bones of ninety-four - and a second
    // drives the spine, the torso, the arms, and the tail on the races that
    // have one. Walking and running only exist as the first half, so playing
    // one alone leaves the arms hanging dead still while the legs stride. The
    // standing clip supplies everything above the waist.
    //
    // Whichever layer goes down first owns the bones it touches: the overlay
    // is skipped there, so the legs keep walking instead of being dragged
    // back towards standing.
    std::vector<bool> claimed(boneCount, false);

    const auto applyClip = [&](const ffxi::Animation& clip, float at) {
        if (clip.frames == 0)
        {
            return;
        }

        // Wrap into the animation and split into the two frames either side,
        // so playback is smooth rather than stepping at the source frame rate
        // - an idle runs at seven and a half frames a second.
        const float wrapped = at - std::floor(at / clip.frames) * clip.frames;
        const auto first = static_cast<size_t>(wrapped);
        const size_t second = (first + 1) % clip.frames;
        const float blend = wrapped - static_cast<float>(first);

        for (const ffxi::AnimationTrack& track : clip.tracks)
        {
            if (track.bone >= boneCount || track.rotation.size() < (second + 1) * 4)
            {
                continue;
            }
            if (claimed[track.bone])
            {
                continue;   // the layer underneath already owns this bone
            }

            float rotation[4];
            slerp(&track.rotation[first * 4], &track.rotation[second * 4], blend, rotation);

            // Blend one channel between the two straddling frames. Written
            // out per component rather than as (&vec.x)[c]: punning the Vec3
            // as a float array let the optimiser drop the writes to y and z -
            // it did not see them as touching those members - so every
            // animated translation silently kept only its x. Invisible while
            // skeletal motion is almost all rotation; the weapon socket, which
            // relies on a large translation channel to move from the origin to
            // the hip, is the bone that exposed it.
            const auto lerp = [&](const std::vector<float>& v, int c) {
                const float a = v[first * 3 + c];
                const float b = v[second * 3 + c];
                return a + (b - a) * blend;
            };
            const Vec3 translation{lerp(track.translation, 0), lerp(track.translation, 1),
                                   lerp(track.translation, 2)};
            const Vec3 scale{lerp(track.scale, 0), lerp(track.scale, 1), lerp(track.scale, 2)};

            // The animation turns the bone from where it rests rather than
            // replacing it, and moves it from where it sits.
            float composed[4];
            multiplyQuaternions(rotation, localRotation[track.bone].data(), composed);
            localRotation[track.bone] = {composed[0], composed[1], composed[2], composed[3]};
            localTranslation[track.bone] = localTranslation[track.bone] + translation;
            localScale[track.bone] = scale;
        }

        // Marked after the pass, not during it, or a clip would block itself.
        for (const ffxi::AnimationTrack& track : clip.tracks)
        {
            if (track.bone < boneCount)
            {
                claimed[track.bone] = true;
            }
        }
    };

    applyClip(animation, frame);
    if (overlay != nullptr)
    {
        applyClip(*overlay, overlayFrame);
    }


    std::vector<BonePose> pose(boneCount);
    for (size_t i = 0; i < boneCount; ++i)
    {
        const ffxi::Bone& bone = skeleton.bones[i];
        const Mat4 local = rotationOf(localRotation[i].data());

        if (bone.parent == i)
        {
            pose[i].rotation = local;
            pose[i].translation = localTranslation[i];
            pose[i].scale = localScale[i];
        }
        else
        {
            const BonePose& parent = pose[bone.parent];
            pose[i].rotation = parent.rotation * local;
            pose[i].translation = parent.translation + rotate(parent.rotation, localTranslation[i]);
            pose[i].scale = {parent.scale.x * localScale[i].x, parent.scale.y * localScale[i].y,
                             parent.scale.z * localScale[i].z};
        }
    }
    return pose;
}

Character buildCharacter(const std::vector<BonePose>& pose, const std::vector<ffxi::SkinnedModel>& meshes,
                         const std::unordered_map<std::string, ffxi::Texture>& textures)
{
    Character character;
    bool any = false;

    // One batch per texture across every mesh, so a six-piece outfit sharing a
    // texture sheet does not become six draws.
    std::unordered_map<std::string, std::vector<uint32_t>> byTexture;

    auto grow = [&](const Vec3& p) {
        if (!any)
        {
            character.boundsMin = p;
            character.boundsMax = p;
            any = true;
            return;
        }
        character.boundsMin = {std::min(character.boundsMin.x, p.x), std::min(character.boundsMin.y, p.y),
                               std::min(character.boundsMin.z, p.z)};
        character.boundsMax = {std::max(character.boundsMax.x, p.x), std::max(character.boundsMax.y, p.y),
                               std::max(character.boundsMax.z, p.z)};
    };

    forEachCorner(meshes, [&](const ffxi::SkinVertex& source, const ffxi::SkinCorner& corner,
                              const ffxi::SkinnedPart& part, bool mirrorPass) {
        const Vertex vertex = toVertex(skin(source, pose, mirrorPass), corner);
        byTexture[part.texture].push_back(static_cast<uint32_t>(character.vertices.size()));
        character.vertices.push_back(vertex);
        grow({vertex.position[0], vertex.position[1], vertex.position[2]});
    });

    for (auto& [texture, indices] : byTexture)
    {
        Batch batch;
        batch.texture = texture;
        batch.indexOffset = static_cast<uint32_t>(character.indices.size());
        batch.indexCount = static_cast<uint32_t>(indices.size());

        const auto found = textures.find(texture);
        batch.cutout = found != textures.end() && found->second.blackWhereClear > kCutoutSignal;

        character.indices.insert(character.indices.end(), indices.begin(), indices.end());
        character.batches.push_back(batch);
    }

    return character;
}

void reskin(Character& character, const std::vector<BonePose>& pose, const std::vector<ffxi::SkinnedModel>& meshes)
{
    size_t index = 0;
    forEachCorner(meshes, [&](const ffxi::SkinVertex& source, const ffxi::SkinCorner& corner, const ffxi::SkinnedPart&,
                              bool mirrorPass) {
        if (index < character.vertices.size())
        {
            character.vertices[index] = toVertex(skin(source, pose, mirrorPass), corner);
        }
        ++index;
    });
}
} // namespace mh

namespace mh
{
int headBone(const ffxi::Skeleton& skeleton)
{
    const std::vector<BonePose> rest = bindPose(skeleton);
    if (rest.size() != skeleton.bones.size())
    {
        return -1;
    }

    int best = -1;
    float highest = 0.0f;
    for (size_t parent = 0; parent < rest.size(); ++parent)
    {
        // Every child of this bone, so they can be checked against each other
        // for a mirrored pair.
        std::vector<size_t> children;
        for (size_t i = 0; i < skeleton.bones.size(); ++i)
        {
            if (i != parent && skeleton.bones[i].parent == parent)
            {
                children.push_back(i);
            }
        }

        bool mirrored = false;
        for (size_t a = 0; a < children.size() && !mirrored; ++a)
        {
            for (size_t b = a + 1; b < children.size(); ++b)
            {
                const BonePose& one = rest[children[a]];
                const BonePose& other = rest[children[b]];
                // Same height and depth, opposite sides, and far enough apart
                // to be a pair rather than two bones at the same spot.
                if (std::fabs(one.translation.z + other.translation.z) < 0.004f &&
                    std::fabs(one.translation.z) > 0.012f &&
                    std::fabs(one.translation.y - other.translation.y) < 0.004f &&
                    std::fabs(one.translation.x - other.translation.x) < 0.004f)
                {
                    mirrored = true;
                    break;
                }
            }
        }

        // Y runs negative upward in this space, so the head is the smallest.
        if (mirrored && (best < 0 || rest[parent].translation.y < highest))
        {
            best = static_cast<int>(parent);
            highest = rest[parent].translation.y;
        }
    }
    return best;
}

void pitchHead(std::vector<BonePose>& pose, const ffxi::Skeleton& skeleton, int head, float radians)
{
    if (head < 0 || static_cast<size_t>(head) >= pose.size() || pose.size() != skeleton.bones.size())
    {
        return;
    }

    // About z, the ear-to-ear axis - see headBone.
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 turn = Mat4::identity();
    turn.m[0] = c;
    turn.m[1] = s;
    turn.m[4] = -s;
    turn.m[5] = c;

    const Vec3 pivot = pose[static_cast<size_t>(head)].translation;
    const auto turned = [&](const Vec3& point) {
        const float x = point.x - pivot.x;
        const float y = point.y - pivot.y;
        return Vec3{pivot.x + c * x - s * y, pivot.y + s * x + c * y, point.z};
    };

    // The head and everything hanging off it. Bones are stored parents-first,
    // so one pass down the list reaches every descendant.
    std::vector<bool> moving(pose.size(), false);
    moving[static_cast<size_t>(head)] = true;
    for (size_t i = 0; i < pose.size(); ++i)
    {
        const uint8_t parent = skeleton.bones[i].parent;
        if (parent != i && parent < moving.size() && moving[parent])
        {
            moving[i] = true;
        }
        if (!moving[i])
        {
            continue;
        }
        pose[i].translation = turned(pose[i].translation);
        pose[i].rotation = turn * pose[i].rotation;
    }
}
} // namespace mh
