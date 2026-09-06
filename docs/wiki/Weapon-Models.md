# Weapon models: where a sword comes from

A player's look carries three more model ids than the six worn slots: what is
in each hand and what is slung on the back. Nothing drew them, and the files
they name were not known. This is where they are.

## Two windows, in the race's own block

A race block is 3176 files. The six worn slots take the first 1320 of it, and
the weapons take the next 1280:

| what | offset from race base | width |
|---|---|---|
| face | 8 | 32 |
| head | 40 | 256 |
| body | 296 | 256 |
| hands | 552 | 256 |
| legs | 808 | 256 |
| feet | 1064 | 256 |
| **main / sub** | **1320** | **1024** |
| **ranged** | **2344** | **256** |

The block goes empty at +2600, which is exactly where the ranged window ends.

**Main and sub read the same window.** A sword is one model whichever hand
holds it, and a one-handed item names both slots with a single model id.

**Weapons are per race.** Every race block has its own copy of the same
weapon at the same index - the seven bases all carry a weapon region at the
same offset. That is what makes a weapon the right size for its owner: a
Galka's axe is a Galka's copy of it, not a scaled Hume one.

## How the offsets were found

Swept, not assumed, and scored against the server's own item table. A weapon
DAT's texture is named for what the weapon is - `hf_swo6_` is a sword,
`hf_axe1_` an axe - so for any candidate offset you can ask whether a model id
the item table calls a sword lands on a file whose texture says sword.

At **+1320** that agreement is 63%, against 50% one either side; the peak is
sharp because a one-file error decorrelates everything. Per weapon type:

| type | agreement |
|---|---|
| hand-to-hand | 93% |
| axe | 91% |
| scythe | 88% |
| great katana | 88% |
| dagger | 86% |
| polearm | 85% |

The type words are not always the obvious ones - a katana's texture says `shi`,
a club's says `sti`, a staff's says `wand` - so the mapping was read off the
data rather than guessed.

Ranged wanted its own window, and the sweep is what said so: bows and guns
score **zero** at +1320 while every melee type scores high. Swept on their own
they land at **+2344 with 98% agreement**, against 77% one step away - and
+2344 is exactly where the block's own mesh-chunk naming changes to `wep2`.
Two independent methods, same number.

## The tags are different from armour's

Armour ids arrive tagged with the slot in the high nibble - 0x1000 head, 0x2000
body, up to 0x5000 feet. **Weapons carry tag 0.** Strip the low twelve bits the
same way regardless; a handful of items in the table do carry a tag and would
land outside the window unmasked.

**Model 0 is an empty hand.** Zero is a real model everywhere else - face 0 is
a face - but no weapon in the item table has model 0, so for a weapon it can
only mean nothing is held. The 354 items that do have it are fishing rods,
animators and soultrappers, which the server's table simply has no model for.
Without this rule the file at the foot of the window is a real mesh and every
unarmed character carries it.

## What does not work yet

The file is right and the mesh loads. A bronze sword - model 268, so file
`race base + 1320 + 268` - adds its 92 triangles to the character, and the
count goes from five meshes to six.

**It draws at the character's feet rather than in the hand.**

A weapon is skinned to exactly one bone: 5, on a hume male. That is one of the
handful of bones near the root whose bind transform is all zeros, which is what
an attachment point looks like rather than a body bone - the body's own meshes
sit on bones in the 25-86 range.

The bone is animated. Sixty-nine clips drive bone 5, and they are exactly the
ones whose name ends in `1` - the upper-body half of each pair. So the weapon
still not moving says the weapon file's bone numbering is not the skeleton's,
and something has to map one onto the other.

That is the same shape of problem as the head, and
[Skeletons](Skeletons.md) is the precedent: bones have no names, so the answer
is to find the bone by what it is rather than by its index.

`MOGHOUSE_WEAPONS=1` turns weapons on to keep working on it. They are off by
default, because a sword lying on the floor beside its owner is worse than no
sword.

## Reading it back

```
MOGHOUSE_SKIN_BONES=1 moghouse-renderer <zone>   # which bones each piece hangs on
MOGHOUSE_TRACK_BONE=5 moghouse-renderer <zone>   # which clips drive one bone
MOGHOUSE_LOOK="1,0,0,8,8,8,8,1,268,0,0"          # race,face,...,feet,size,main,sub,ranged
```

The weapons go after the size rather than beside the armour so that every look
string written before they existed still parses.
