#pragma once

// A zone as unique geometry plus per-placement transforms.
//
// The earlier approach baked each placement's transform into its own copy of
// the vertices, which made one zone 658,000 triangles out of about 36,000 of
// actual geometry - the three most-placed models alone account for 3,033 copies.
// Here each mesh is uploaded once and drawn once per placement.

#include "ffxi/mmb.h"
#include "ffxi/mzb.h"
#include "ffxi/texture.h"
#include "linalg.h"
#include "zonemesh.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mh
{
/// The curves a shoreline generator names for its wave, by 0x19 chunk id.
///
/// Unlike the day curves these run over a loop of a few seconds, and they
/// animate the thing rather than deciding whether it is shown. Valkurm Dunes'
/// beach is the case they were read from: see ffxi::EffectPlacement.
struct WaveCurves
{
    std::string scaleZ;  ///< op 0x29, multiplying the placement's own z scale
    std::string opacity; ///< op 0x2d
    std::string u;       ///< op 0x2e, an offset in uv, not a rate
    std::string v;       ///< op 0x2f
    /// Per-wave reach, so a short wave runs as far up the sand as a long one.
    /// Op 0x29 scales each wave's own geometry, so the raised nms (localZ ~4)
    /// reaches a third of what the flat nmia (localZ 11.25) does and its foam
    /// stalls out at sea. Set from the model's own length at build time to
    /// even them out; 1 for the longest.
    float reachScale{1.0f};
    ///
    /// scaleZ (op 0x29) is a factor on the strip's own z scale, not a target
    /// size. nmia is 11.25 deep with its z bounds starting at zero, so it grows
    /// from its seaward edge; the curve peaks at 7.70, which stretches it to 87
    /// units and runs the far edge exactly up to the waterline - measured on
    /// both Valkurm beaches, the strip anchor plus 86.6 landing within two
    /// units of where the terrain meets the sea. An earlier reading took the
    /// curve as a size and divided it by the extent, which shrank the strip to
    /// a fraction of a unit and stranded the foam out at sea.
    bool any() const { return !scaleZ.empty() || !opacity.empty() || !u.empty() || !v.empty(); }
};

/// One mesh, drawn once per placement of the model it belongs to.
struct InstancedDraw
{
    std::string texture;
    bool cutout{};

    /// The mesh header asked to be blended - cloth, banners, glass.
    ///
    /// Read from the blending field rather than guessed at from the texture.
    /// Across Bastok Markets that field holds 0x8000 on 150 meshes, 0x2000 on
    /// 18 (mostly the _-prefixed foliage), and zero on the other thousand.
    /// Drawn opaque, a translucent awning shows its transparent texels as
    /// black blobs on the cloth.
    bool blend{};

    /// One of FFXI's water surfaces, drawn translucent and last.
    ///
    /// Water is ordinary placed geometry with a recognisable model name, not
    /// something derived from the MZB's per-cell height field. Drawn opaque it
    /// comes out a flat white sheet, because the texture carries the water in
    /// its alpha.
    bool water{};
    uint32_t indexOffset{};
    uint32_t indexCount{};
    uint32_t instanceOffset{};
    uint32_t instanceCount{};

    /// A generator-placed mesh whose texture the game scrolls: the fountain's
    /// jets and flames, a waterfall. Drawn by the effect pass, after the
    /// water, with `scroll` in uv per second; skipped by day when nightOnly.
    /// After the counts so the brace initialisers above them still line up.
    bool effect{};
    float scroll[2]{};
    bool nightOnly{};
    std::string curve;
    bool additive{};
    /// Set for a shoreline wave; evaluated every frame against the loop clock.
    WaveCurves wave;
};

struct Scene
{
    std::vector<Vertex> vertices;   ///< model space, each mesh appearing once
    std::vector<uint32_t> indices;
    std::vector<float> instances;   ///< 16 floats per placement, column major
    std::vector<InstancedDraw> draws;

    /// Where each model's placements sit in `instances`: first index and count.
    ///
    /// They are grouped by model as the scene is built, so every copy of one
    /// model is contiguous. Kept because a draw records the texture it uses and
    /// not the model it came from, which leaves no way to find a particular
    /// thing again afterwards - and the monorail in Sel Phiner has to be found
    /// again on every frame it moves.
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> instanceRanges;

    /// Water surfaces, world space, drawn by the water pass.
    ///
    /// From the zone's own water meshes when it has them - see isWaterMesh -
    /// with every placement expanded, since a stream is thirty small models
    /// each placed once. A zone with none gets the collision-derived sheets
    /// loaded in afterwards (viewer.cpp loadWater) as a fallback.
    std::vector<Vertex> waterVertices;
    std::vector<uint32_t> waterIndices;

    /// The ripple sheet the zone's water meshes name most often - "effect
    /// kaw1" for East Ronfaure's stream, "sea     sea01" for a harbour -
    /// so the water pass scrolls the texture the artists put on that water
    /// rather than one picked from a list. Empty when no water mesh named one,
    /// or when most of the water names none - Bastok Markets' canal and
    /// fountain are untextured meshes the client paints at run time, and only
    /// the sea beyond the harbour wall names a sheet. Voted by triangle.
    std::string waterTexture;
    /// Triangles of water whose mesh named no sheet. They are a river or a
    /// canal, never the sea, and the water pass tints them as one.
    size_t waterUntextured{};

    Vec3 boundsMin{};
    Vec3 boundsMax{};

    Vec3 centre() const { return (boundsMin + boundsMax) * 0.5f; }
    float radius() const;

    size_t triangles() const;      ///< unique geometry
    size_t drawnTriangles() const; ///< what the GPU actually processes
    size_t waterTriangles() const { return waterIndices.size() / 3; }
};

/// Whether a mesh is a water surface: a water model by name, or a mesh
/// textured with one of the ripple sheets. See scene.cpp.
bool isWaterMesh(const std::string& modelName, const ffxi::ModelMesh& mesh);

/// What an effect generator says about how to draw a model: see
/// InstancedDraw::effect. Keyed by model name when passed to buildScene.
struct EffectParams
{
    float scrollU{};
    float scrollV{};
    bool nightOnly{};
    /// The intensity curve the generator names (ffxi::IntensityCurve), or
    /// empty. When present it decides whether the thing is drawn at this hour.
    std::string curve;
    /// Added to what is behind it rather than blended over: the flames.
    bool additive{};
    /// An animated shoreline sheet: a wave, not a water surface. Kept out of
    /// the water pass, which bakes its geometry flat in world space and could
    /// neither move nor fade it.
    bool foam{};
    WaveCurves wave;
};

Scene buildScene(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                 const std::unordered_map<std::string, ffxi::Texture>& textures, size_t& placementsResolved,
                 size_t& placementsMissing, const std::unordered_map<std::string, EffectParams>* effects = nullptr);

/// Adds one scene's geometry to another.
///
/// A city zone is more than one file: its buildings' interiors are separate
/// DATs, built the same way and already in the same coordinates, so a zone is
/// assembled by appending them. Indices and instance offsets are rebased as
/// they are copied, and the bounds grow to cover both.
void append(Scene& into, const Scene& extra);
} // namespace mh
