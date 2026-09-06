#pragma once

// What a player character is wearing, and which files that comes from.
//
// The layout was derived rather than transcribed: see tools/pcmodels.py, which
// scores every candidate race base against an index of every DAT holding a
// skinned mesh and requires the winner to be the file holding that race's
// skeleton. All eight races resolve.

#include "filetable.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ffxi
{
enum class Race : uint8_t
{
    HumeMale = 1,
    HumeFemale,
    ElvaanMale,
    ElvaanFemale,
    TarutaruMale,
    TarutaruFemale,
    Mithra,
    Galka,
};

enum class LookSlot : uint8_t
{
    Face,
    Head,
    Body,
    Hands,
    Legs,
    Feet,
    /// What is in each hand, and what is slung on the back.
    ///
    /// Main and Sub read the same window: a sword is one model whichever hand
    /// holds it, and a one-handed item names both slots with a single model
    /// id. Ranged has a window of its own - see look.cpp.
    Main,
    Sub,
    Ranged,
    Count
};

/// What a character is wearing. Model ids, not item ids: the item table maps
/// one to the other and is a separate problem.
struct Look
{
    Race race{Race::HumeMale};
    std::array<uint16_t, static_cast<size_t>(LookSlot::Count)> model{};
    /// 0 small, 1 medium, 2 large - the server's char_look.size. Not a file;
    /// the body is drawn a little larger or smaller.
    uint8_t size{1};
};

/// The file id holding this race's skeleton and animation set.
size_t skeletonFileId(Race race);

/// The file ids holding this race's motion sets, in block order.
///
/// The skeleton file carries only half of each movement: the clips ending 0,
/// which drive the root, the hips and the legs. The upper body - spine, torso,
/// arms, head, and a tail where the race has one - lives in these siblings as
/// the clips ending 1. wlk1, run1 and idl1 are in the first of them and
/// nowhere else, so a character loaded from the skeleton alone walks with its
/// arms hanging still.
std::vector<size_t> motionFileIds(Race race);

/// The file id for one slot's model, or 0 if the race or slot is unknown.
///
/// Only the first block is resolved - model ids 0 to 255, which is the gear
/// the game shipped with. Later expansions added blocks elsewhere in the file
/// table that this does not yet reach.
size_t modelFileId(Race race, LookSlot slot, uint16_t modelId);

/// Every file a look needs, skipping slots that resolve to nothing.
std::vector<std::filesystem::path> lookFiles(const FileTable& table, const Look& look);

/// Parses "race,face,head,body,hands,legs,feet" - the shape a look arrives in
/// from the server - with an optional eighth number, the size, and an optional
/// ninth, tenth and eleventh: main hand, sub hand and ranged. Returns false if
/// it does not have at least seven.
///
/// The weapons go after the size rather than beside the armour so that every
/// look string written before they existed still parses.
bool parseLook(const std::string& text, Look& look);

const char* raceName(Race race);
const char* slotName(LookSlot slot);
} // namespace ffxi
