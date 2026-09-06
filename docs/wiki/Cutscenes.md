# Cutscenes: what they are, and what it takes to play one

A cutscene in FFXI is an **event**: the server says "play event N on entity E",
the client runs a script that moves the camera, poses NPCs, shows dialogue and
offers choices, and then tells the server it is done. The character is
`InEvent` the whole time and invisible to everyone else until it ends.

MogHouse today **skips** every cutscene. When a `0x032` event start arrives -
or when the login reply says the character was parked in one - the client sends
`EVENTEND` straight back and prints `[cutscene N was skipped]`. That is correct
and deliberate: skipping is better than freezing in an event the client cannot
run. Making them *play* is the work below.

## The packet flow

| packet | dir | meaning | MogHouse |
|---|---|---|---|
| `0x032` event | s2c | start event N (the id is `EventPara`; `EventNum` is the *zone*) | parsed (`FfxiEventStart`) |
| `0x033` eventstr | s2c | string parameters for the script (names to slot into text) | not read |
| `0x034` eventnum | s2c | number parameters | not read |
| `0x036` talknum | s2c | a single NPC line, no cutscene | — |
| `0x038` schedulor | s2c | a scheduler command tagged by FourCC | not read |
| `0x052` eventucoff | s2c | release the update-lock during an event | not read |
| `0x05b` eventend | c2s | "the event finished" | sent (to skip) |
| `0x05c` eventendxzy | c2s | event end, plus where the player ended up | sent by position |

So the transport is mostly understood and half-built. The gap is not the
packets; it is what happens between the start and the end.

## The missing piece: the event VM

The script itself lives in the zone's **event DAT** (`5820 + zone`), which
[Events](Events.md) documents and `FfxiEventTable` already segments into
per-entity code blocks. Each block is **bytecode** for a little virtual machine
the client runs: a sequence of opcodes that do things like

- point the camera here, move it there over N frames
- turn an NPC to face the player, play an animation on it
- print a message (by id, into the zone's message DAT), with the `0x033`/`0x034`
  parameters substituted
- offer a menu and wait for the player's choice
- wait, fade, end

**These opcodes are not decoded yet.** The earlier note in the summary - that
dialogue is not reachable by a plain `u16` message id - is the symptom: the
message shown is chosen *by the bytecode*, not by a fixed offset, so nothing
plays until the bytecode is read.

This is the one real blocker. Everything else is plumbing.

## What the DLL shows

From the unpacked client (`tools/dllstrings.py`):

- `CXiMovie` - a class for full-motion video, i.e. the pre-rendered FMV
  cutscenes, separate from the scripted in-engine ones.
- Event UI resources: `eventskip`, `eventpara`, `eventinfo`, `eventtim` - the
  menu that lets you skip or inspect an event.
- `EventData` and `EventMessageData` file errors - the two halves the VM reads:
  the bytecode and the text.

The VM opcode dispatcher is the thing to find in Ghidra. The entry point is the
`0x032` handler: follow it to where it loads the event block and starts
stepping opcodes, and the dispatch table there is the opcode set. That is a
decompiler job, not a strings job, so it waits for a Ghidra session rather than
`dllstrings.py`.

## A staged path to playable

1. **Read the parameters.** Parse `0x033` eventstr and `0x034` eventnum and keep
   them. Cheap, and every later step needs them.
2. **Dialogue only.** Many "cutscenes" are one NPC standing still and talking.
   If the opcode that prints a message and the one that ends can be recognised -
   two opcodes, not the whole set - a large fraction of events play as a
   dialogue box with no camera work, which is most of what a quest turn-in is.
3. **The full VM.** Camera paths, NPC animation, menus. This needs the opcode
   set decoded from the DLL, and is the real feature.
4. **FMVs** (`CXiMovie`) are a separate track - decoding a video container - and
   are worth leaving until last.

Stopping at (2) would already make questing legible, and it is the smallest
next step that shows something on screen instead of skipping.

## Do not lose the skip

Whatever plays events must keep the current skip as a fallback: an event whose
bytecode hits an opcode the VM does not know has to end cleanly rather than
trap the character in an `InEvent` state forever. The skip that exists today is
that fallback.
