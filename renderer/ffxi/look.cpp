#include "look.h"

#include <cstdio>
#include <cstdlib>

namespace ffxi
{
namespace
{
/// The first file id of each race's block. The skeleton sits at the base and
/// everything else is measured from it.
///
/// Tarutaru share one block: the two sexes are the same skeleton with
/// different models, which is why the same number appears twice.
constexpr size_t kRaceBase[] = {
    0,     // no race 0
    7072,  // hume male
    10248, // hume female
    13424, // elvaan male
    16600, // elvaan female
    19776, // tarutaru male
    19776, // tarutaru female
    23176, // mithra
    26352, // galka
};

/// Where the female Tarutaru faces sit, measured from the Tarutaru base.
///
/// One whole race block past it, which is the position a sixth race would have
/// started at had the two sexes not shared. They do share - all the gear, and
/// the skeleton - so all that lives up there is the heads.
constexpr size_t kTarutaruFemaleFaces = 3176;

struct SlotWindow
{
    size_t offset;
    uint16_t count;
};

/// Offset from the race base and how many model ids each window holds, in the
/// order the slots appear in the file table.
constexpr SlotWindow kSlotWindow[] = {
    {8, 32},     // the character's own head: face and hair
    {40, 256},   // headgear
    {296, 256},  // body
    {552, 256},  // hands
    {808, 256},  // legs
    {1064, 256}, // feet

    // The weapons, which sit straight after the feet and run to where the
    // block goes empty at +2600. Two windows, not three: main and sub read the
    // same 1024 because a sword is one model whichever hand holds it, and a
    // one-handed item names both slots with a single model id.
    //
    // Both offsets were swept rather than assumed, scoring every candidate
    // against the weapon type the server's own item table gives each model id.
    // A DAT's texture is named for what it is - hf_swo6_ is a sword, hf_axe1_
    // an axe - so the type is checkable per file. +1320 scores 63% against 50%
    // one either side, and per type: hand-to-hand 93%, axe 91%, scythe 88%,
    // great katana 88%, dagger 86%, polearm 85%. Ranged scores 98% at +2344,
    // against 77% one step away.
    //
    // Ranged needing its own window is what the sweep found rather than what
    // it was looking for: bows and guns score zero at +1320 while every melee
    // type scores high, which is what says they are somewhere else.
    {1320, 1024}, // main
    {1320, 1024}, // sub
    {2344, 256},  // ranged
};
} // namespace

size_t skeletonFileId(Race race)
{
    const auto index = static_cast<size_t>(race);
    return index < std::size(kRaceBase) ? kRaceBase[index] : 0;
}

std::vector<size_t> motionFileIds(Race race)
{
    const size_t base = skeletonFileId(race);
    if (base == 0)
    {
        return {};
    }

    // The four files between the skeleton and the first slot window. The
    // first holds the movement set; the others are stance variants that the
    // weapon in hand selects, which is a later problem.
    return {base + 1, base + 2, base + 3, base + 4};
}

size_t modelFileId(Race race, LookSlot slot, uint16_t modelId)
{
    const size_t base = skeletonFileId(race);
    const auto slotIndex = static_cast<size_t>(slot);
    if (base == 0 || slotIndex >= std::size(kSlotWindow))
    {
        return 0;
    }

    const SlotWindow& window = kSlotWindow[slotIndex];

    // Tarutaru share a block, but not their heads.
    //
    // Every race block is 3176 files apart - hume male to hume female to
    // elvaan male, all the way to galka - except the step from Tarutaru to
    // Mithra, which is 3400. The extra 224 is the female Tarutaru, and what is
    // in it is faces: 3176 past the Tarutaru base sits a window of exactly 32
    // face-sized files, all of them different files from the male's, and at 32
    // the sizes jump from forty kilobytes to four hundred, so the window ends
    // there rather than running on.
    //
    // Everything else really is shared. The equipment windows are one flat
    // list of gear for both sexes: their upper half looked like a second body
    // set, and drawing from it puts a character in an ornate suit of armour.
    // So only the head moves.
    size_t offset = window.offset;
    if (race == Race::TarutaruFemale && slot == LookSlot::Face)
    {
        offset = kTarutaruFemaleFaces;
    }

    if (modelId >= window.count)
    {
        return 0;
    }

    // An empty hand. Zero is a real model everywhere else - face 0 is a
    // face - but no weapon in the item table has model zero, so for a weapon
    // it can only mean nothing is held. Without this the file at the foot of
    // the weapon window is a real mesh, and every unarmed character would
    // carry it.
    if (slot >= LookSlot::Main && modelId == 0)
    {
        return 0;
    }

    return base + offset + modelId;
}

std::vector<std::filesystem::path> lookFiles(const FileTable& table, const Look& look)
{
    std::vector<std::filesystem::path> paths;
    for (size_t i = 0; i < static_cast<size_t>(LookSlot::Count); ++i)
    {
        const size_t fileId = modelFileId(look.race, static_cast<LookSlot>(i), look.model[i]);
        if (fileId == 0)
        {
            continue;
        }
        if (auto path = table.path(fileId))
        {
            paths.push_back(*path);
        }
    }
    return paths;
}

bool parseLook(const std::string& text, Look& look)
{
    // race, the six worn slots, the size, then the three weapons. Only the
    // first seven are required - a look written before weapons existed still
    // parses, and leaves the hands empty.
    unsigned values[11] = {1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    const int given =
        std::sscanf(text.c_str(), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &values[0], &values[1], &values[2], &values[3],
                    &values[4], &values[5], &values[6], &values[7], &values[8], &values[9], &values[10]);
    if (given < 7)
    {
        return false;
    }
    look.size = given >= 8 && values[7] <= 2 ? static_cast<uint8_t>(values[7]) : 1;
    if (values[0] < 1 || values[0] > 8)
    {
        return false;
    }

    look.race = static_cast<Race>(values[0]);

    // The armour runs straight on from the race; the weapons are past the
    // size, so the two halves are copied separately rather than by one offset.
    for (size_t i = 0; i <= static_cast<size_t>(LookSlot::Feet); ++i)
    {
        look.model[i] = static_cast<uint16_t>(values[i + 1]);
    }
    for (size_t i = 0; i < 3; ++i)
    {
        look.model[static_cast<size_t>(LookSlot::Main) + i] = static_cast<uint16_t>(values[8 + i]);
    }
    return true;
}

const char* raceName(Race race)
{
    switch (race)
    {
    case Race::HumeMale: return "hume male";
    case Race::HumeFemale: return "hume female";
    case Race::ElvaanMale: return "elvaan male";
    case Race::ElvaanFemale: return "elvaan female";
    case Race::TarutaruMale: return "tarutaru male";
    case Race::TarutaruFemale: return "tarutaru female";
    case Race::Mithra: return "mithra";
    case Race::Galka: return "galka";
    default: return "?";
    }
}

const char* slotName(LookSlot slot)
{
    switch (slot)
    {
    case LookSlot::Face: return "face";
    case LookSlot::Head: return "head";
    case LookSlot::Body: return "body";
    case LookSlot::Hands: return "hands";
    case LookSlot::Legs: return "legs";
    case LookSlot::Feet: return "feet";
    case LookSlot::Main: return "main";
    case LookSlot::Sub: return "sub";
    case LookSlot::Ranged: return "ranged";
    default: return "?";
    }
}
} // namespace ffxi
