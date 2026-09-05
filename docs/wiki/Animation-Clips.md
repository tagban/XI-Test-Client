# Animation clips: what a body can be made to do

A clip is named, never numbered, and nothing in the files indexes them. So the
list here was taken by asking a body what it has, and the meanings by playing
each one and looking - `tools/poses.sh` renders a thumbnail per clip.

## The name

Four characters: a **three-character action** and a **half**.

    idl0   idle, root and legs          si10   sitting, root and legs
    idl1   idle, everything above       si11   sitting, everything above

The half is the load-bearing part and the reason `/sit` shipped broken once:
asking for `si1` matches nothing, because sitting still is action `si1` and
half `0`. The clips ending `0` drive the root, hips and legs - sixteen bones
of ninety-four - and the clips ending `1` drive the spine, torso, arms, head
and a tail where the race has one. A body loaded from the skeleton file alone
walks with its arms hanging still.

`upperFor` derives the upper clip by turning a trailing `0` into a `1`, so
naming the lower half is enough.

## Where they live

Per race, in the four files after the skeleton - see `motionFileIds`. The
skeleton file itself holds the `0` and `1` halves; `base+3` and `base+4` hold
a third set ending `2`, whose purpose is not established. Past `base+4` the
race block is gear, not motion.

An assembled Hume male carries **137 clips**.

## What is known

Confirmed by playing them:

| action | what it is |
|---|---|
| `idl` | idle |
| `wlk`, `run` | walking, running |
| `mvb`, `mvl`, `mvr` | stepping back, left, right |
| `jmp` | jump |
| `ded` | dead - the body lies down |
| `std` | standing upper body, layered under a stride |
| `si0`, `si1`, `si2` | sit down, stay sat, get up |
| `sk1`, `sk2` | kneeling |
| `eye3`, `mou4` | face - eyes and mouth |

## What is not

The rest are two-letter actions with two or three variants each - `bb`, `bf`,
`cm`, `cor`, `gc`, `gh`, `gu`, `ma`, `mb`, `mi`, `mn`, `ms`, `mw`, `na`, `ri`,
`rx`, `sf`, `sh`, `yu`. The shape of them argues combat rather than emotes: a
variant per weapon or stance is what `gc0/gc1/gc2` and `ma0/ma1/ma2` look
like, and a body has one idle but three of those.

**So the emote set proper may not be in these files at all**, and that is the
open question rather than which name means "cheer". A contact sheet at a
single frame does not settle it either - most of these are an upper-body
gesture whose characteristic moment is near the end of the clip, and at frame
seven a dozen of them are a person standing still.

Two things would settle it: rendering each clip at several frames rather than
one, and checking whether a race with no combat animations - a Moogle, an NPC
model - carries any of these actions.
