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

## How a weapon reaches the hand

A weapon has no attachment code of its own, and it needs none. The `wep0`
mesh is a skinned mesh like any armour piece, skinned to a single bone - a
*socket* the skeleton carries for exactly this.

Reading the bronze sword's `wep0` header:

```
flags 0x0080          bit 0x80 set: indices go through the bone table
bone table  [5]       one entry
vertices              110, every one referencing table slot 0 -> bone 5
```

Every vertex of the sword is bound to bone 5. On a hume male, bone 5 is one of
a handful of bones near the root of the skeleton whose bind translation is all
zeros - a socket, not a body bone. The skeleton even names two of them: it
carries `0x07` chunks `@tr0` and `@tl0`, attach-right and attach-left.

The socket is not fixed in space. It is a child of the body's bones, so it
inherits their animation, and the idle clip drives it: `idl1`, the upper-body
half of the idle, has a track for bone 5 with the rotation and translation that
carry the socket from the origin up to the hip. When the body breathes, the
socket at its hip breathes with it, and the sword hangs there.

**So a weapon works with no special case.** The character is built and animated
exactly as it is without one; the weapon is one more mesh on the skeleton, and
the skeleton puts it where the socket goes. The same sword on a Galka lands at
the Galka's hip, on a 2.43-unit body against a hume's 1.9, with nothing
weapon-specific: the socket is the Galka's socket. Validated in the live client
- the sword sheathes at the hip exactly as retail does.

## Sheathed, not drawn

A weapon in the idle hangs at the hip because that is where the socket sits in
that pose - the sheathed position, which is what the game shows a character who
is not fighting. Drawing it into a fighting grip is the combat stances' job:
the socket bone is driven by every `*1` clip, so a combat idle moves it into
the hand the same way the plain idle keeps it at the hip. Same socket, a
different animation - still nothing extra to do.

## The bug that hid all of this

The socket did not work until a renderer bug was fixed, and it is worth knowing
because it had nothing to do with weapons.

Blending an animation's translation between two frames was written by punning a
`Vec3` as a float array. The optimiser did not see the writes as touching the
y and z members, so **every animated translation kept only its x**. Skeletal
motion is almost all rotation - bones turn rather than slide - so the body
looked correct and the bug stayed invisible. The weapon socket is the one bone
that leans on a large *translation* channel (bone 5 has an identity rotation in
the idle and a translation that lifts it from the origin to the hip), so with y
and z dropped the socket never left the origin and the sword hung at the ankle.

It was found by re-deriving the pose maths from scratch and comparing the two
bone by bone - the replica put the socket at the hip, the renderer at the
origin, they agreed on every rotation-driven bone, and the difference narrowed
to the translation blend. See the commit "Stop animation from dropping the Y
and Z of every translation".

## Reading it back

```
MOGHOUSE_SKIN_BONES=1 moghouse-renderer <zone>   # which bones each mesh hangs on
MOGHOUSE_TRACK_BONE=5 moghouse-renderer <zone>   # which clips drive one bone
MOGHOUSE_BONE_WORLD=5 moghouse-renderer <zone>   # where a bone lands, posed
MOGHOUSE_LOOK="1,0,0,8,8,8,8,1,268,0,0"          # race,face,...,feet,size,main,sub,ranged
```

The weapons go after the size rather than beside the armour so that every look
string written before they existed still parses.
