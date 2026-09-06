# Where things are

Start here. The other handoff documents are chronological and long; this one is
short and current, and says what is true today rather than how it got that way.

Last updated after the torchlight, the weather and NPC dialogue, 2026-09-03.

## The environment, which is the part you cannot guess

Nothing runs without these. The install is on an external volume, and the key
tables are built rather than shipped.

```sh
export PATH="$HOME/.dotnet:/opt/homebrew/bin:$PATH"
export MOGHOUSE_FFXI_INSTALL="/Volumes/AppStorage/FFXI Game Folder/PlayOnline/SquareEnix/FINAL FANTASY XI"
export MOGHOUSE_FFXI_KEYTABLE="$PWD/keys/mzb_key_table.bin"
export MOGHOUSE_FFXI_KEYTABLE2="$PWD/keys/mmb_key_table2.bin"
export MOGHOUSE_FFXI_RES="$PWD/dist/x86_64/MogHouse XI.app/Contents/Resources/data/res"
```

`dotnet` is not on the default PATH here, and neither is Homebrew's clang, which
is the one that builds the renderer - Apple's has no `std::jthread`.

## Running it

**The client**, screens and all:

```sh
dotnet run --project src/MogHouse.App/MogHouse.App.csproj --no-launch-profile
```

There is no launcher any more: Avalonia was deleted on 2026-09-02 and the
renderer's screens are the only path. `--screens` is accepted and ignored, for
scripts that still pass it. Sign-in fills itself from the saved profile; tab to
the password.

**The renderer on its own**, which needs no server and is how most of the
graphics work was done:

```sh
./build-renderer/moghouse-renderer "$MOGHOUSE_FFXI_INSTALL/ROM/0/28.DAT"
```

`ROM/0/28.DAT` is zone 0, Sel Phiner - the backdrop the client signs in over,
and the one with the monorail. `wasd` flies, `p` prints where you are, `t` gets
on and off the train, `escape` quits.

**The local server** is LandSandBoat at `/Volumes/AppStorage/LandSandBoat`, and
is usually already running. `pkill -f 'xi_map|xi_world|xi_connect'` stops it.
Two accounts, both `mhtestpw123`: `mhtest` (character Testy) and `mhtest2`
(Duo). A character that was logged in less than a minute ago cannot log in
again - the server holds the session for sixty seconds.

## Commands in the world

Typed with a leading `/`; anything not recognised is said out loud.

| command | does |
|---|---|
| `/attack` (`/at`, `/engage`) | engage the current target - draws the weapon and sends the engage action; the server drives the fight |
| `/attackoff` (`/disengage`) | stop attacking and sheathe |
| `/draw` (`/sheathe`) | a local toggle: ready or sheathe the weapon with no server and no target, for looking at the stance |
| `/sit`, `/stand` | sit down / stand up; walking stands you up too |
| `/logout`, `/shutdown` | leave to the character list / end the session |
| `/bug <words>` | file a bug from where you stand - see below |
| `/say`, `/shout`, `/tell`, `/party`, `/linkshell` (and the short forms) | chat channels |

Weapons are drawn by default. `MOGHOUSE_DRAW_CLIP` overrides which clip the
drawn stance uses while the right per-weapon-type battle idle is pinned down.
Enter-twice-on-a-target to engage (the retail way in) is not wired yet; `/attack`
is the way in meanwhile. Shields, dual-wield and ranged sit in the wrong place
still - only the main-hand weapon is placed correctly; see the weapon notes.

## Reporting what is wrong

`/bug the torch is not lit` from inside the world writes the words, the zone,
the spot, the facing, the hour, the weather and a screenshot to
`bug-reports.md` beside the log - each entry with a line that stands the
standalone renderer in the same place - and posts the same thing to a Discord
webhook when one is configured.

Both secrets live beside the settings and never in this repository, because a
client that ships can be taken apart:

| | |
|---|---|
| `bug-webhook.txt` | where `/bug` posts. Or `MOGHOUSE_BUG_WEBHOOK`. |
| `bug-bot-token.txt` | what `tools/bugs.py` reads the channel back with. Or `MOGHOUSE_BUG_BOT_TOKEN`. |

`tools/bugs.py fetch` collects what is new and downloads the screenshots;
`fetch --accepted` narrows that to reports somebody has reacted to, which is
the queue worth working from. `done <id> <commit>` marks one and says so in
the channel.

## Useful switches

| | |
|---|---|
| `MOGHOUSE_TIME=1700` | holds the hour still; the light is otherwise never twice the same |
| `MOGHOUSE_SCENE_ZONE` | the zone behind the sign-in screen; 0 is Sel Phiner, -1 is none |
| `MOGHOUSE_UI_SCALE` | interface size on top of the display's own correction |
| `MOGHOUSE_TRAIN_WATCH` | prints where the monorail has got to |
| `MOGHOUSE_TRAIN_HOLD` | leaves the train parked, which is the only way to walk its interior |
| `MOGHOUSE_TRAIN_LAMPS` | how brightly its cars light up |
| `MOGHOUSE_CLIPS` | lists a character's animation clips |
| `MOGHOUSE_CAMERA`, `MOGHOUSE_CAMERA_LOOK` | frame a shot from a script |

## What works

Sign in, make an account, choose or make a character, and zone in - all of it
drawn by the renderer in one window, over a live Sel Phiner, with the train
running past. Then the world: walking, chat, jumping, death and raise, zoning
between zones, vitals, music. `WorldLoop` in `MogHouse.Core/Screens/` is that
second half and is not user interface.

## What is next, in the order worth doing it

**Avalonia is gone** (2026-09-02, on Windows). `MogHouse.App` is one file:
`Program.cs` starts the log and hands the main thread to `ClientFlow`. What the
old shell set up that the screens path had not - zone lines, the dialogue file
table, the session's status lines in the log - moved into `ClientFlow`.

**Bastok Mines blows out to white around noon and goes black at night.** An
underground zone lit by the outdoor day and night cycle; only the thirteen rooms
that ship their own lighting escape it. Not the same fault as zones shipping no
lighting at all, which is fixed. `MOGHOUSE_TIME=1200` shows it at once.

**NPC menus.** Clicking somebody talks to them now, and a plain line comes
back as words. A conversation with choices does not: the dialogue, the choices
and the branching are all in an event script in the game's own DATs, and
nothing here can read one. `docs/npc-dialogue.md` has the whole exchange, what
works, and where the scripts have already been ruled out.

**The torches light the ground, but nothing measures how far.** The reach is
the light marker's own size times eight, picked by eye against a wall in West
Ronfaure. A retail client standing in the same place would settle it;
`MOGHOUSE_LAMP_REACH` changes it without a rebuild.

**The weather picks a sky, but only when a zone loads.** Turning weather is
read and carried, and takes effect on the next zone change - swapping a sky in
place needs a rebuild path that does not exist. Which of the four skies each of
the twenty weathers calls for is also a reading rather than a measurement; see
`skyForWeather`.

**Which sound is which.** Sound effects now decode and play - `.spw` is read
in `renderer/ffxi/spw.cpp`, several at once through `renderer/sounds.cpp`, and
`ffxi-sounddump` measures whether a decode came out as a waveform rather than
noise. What is missing is the mapping: nothing read so far says which of the
11,862 files is the noise a worm makes coming out of the ground.
`MOGHOUSE_EMERGE_SOUND` plays a chosen file when one does, for trying
candidates. See `docs/wiki/Audio-Formats.md`.

**A seated pose on the train.** Written up with working notes in
`docs/windows-handoff.md`; the sitting clips are `si00` through `si21`.

## Where the rest is written down

| | |
|---|---|
| `docs/macos-handoff.md` | how the Mac build happened, and two wrong theories worth not repeating |
| `docs/windows-handoff.md` | what is parked for the Windows session, and what blocks deleting Avalonia |
| `docs/npc-dialogue.md` | talking to NPCs: what works, and why menus need an event-script reader |
| `docs/wiki/Audio-Formats.md` | `.bgw` and `.spw`: the header, the split sample rate, and how it was checked |
| `tools/bugs.py` | reading the bug channel `/bug` posts to, and marking reports done |
| `docs/local-test-server.md` | building and running LandSandBoat here |
| `docs/networking-handoff.md` | a false lead, kept so nobody pays for it twice |
| the wiki | data formats: object mapping, and audio |

The commit messages are the other half of this. Where something surprising was
found - the split sample rate, the collision that does not move with the train,
the far plane fixed at 1.0 - the reasoning is in the commit rather than here.
