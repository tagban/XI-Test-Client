// The C ABI between the MogHouse renderer and the C# client.
//
// MogHouse ships as one application. The renderer is C++ because the asset
// pipeline is; the game logic is C# because the protocol work is. This is the
// seam, and it is plain C so neither side needs the other's toolchain.
//
// Deliberately narrow. Everything the client has to say to the renderer today
// is "open this zone", "here is what is nearby" and "please close". Anything
// wider would be guessing at what the two halves will want of each other.
#ifndef MOGHOUSE_INTEROP_H
#define MOGHOUSE_INTEROP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MH_API __declspec(dllexport)
#else
#define MH_API __attribute__((visibility("default")))
#endif

typedef struct MhViewer* MhViewerHandle;

/// Kinds match MogHouse.Core's FfxiEntityKind, so the C# side can cast.
enum
{
    MH_ENTITY_PLAYER = 0,
    MH_ENTITY_NPC = 1,
    MH_ENTITY_ENEMY = 2
};

/// One tracked thing, in world coordinates. The radar uses x and z; the rest
/// is what it takes to stand a body there and point it somewhere.
///
/// Y is up, as everywhere past the DAT readers, and heading is radians with 0
/// along +z. The client converts out of FFXI's own frame on the way in.
typedef struct MhRadarEntity
{
    float x;
    float z;
    float y;
    float heading;
    int32_t kind;

    /// Shown over the body, NUL terminated, ASCII. Fixed width so the whole
    /// array stays blittable and crosses as a pointer - a char* per entity
    /// would mean owning lifetimes across a thread boundary for no gain.
    char name[20];

    /// The server's id, 0x1000000 | zone << 12 | targid. Zero means unknown.
    /// Used to find a name in the zone's own name table when the server sends
    /// none, which for NPCs it always does.
    uint32_t id;

    /// Non-zero when the server wants the name kept off screen until this is
    /// targeted. Doors and zone lines are named but not labelled.
    int32_t nameHidden;

    /// What the entity looks like, when the server describes it the way it
    /// describes a player: race, face, then the head, body, hands, legs and
    /// feet model ids with their slot tags already stripped. All zero means
    /// the server did not describe it this way - a fixed model, a door - and
    /// the shared body stands in.
    uint16_t look[7];

    /// GM level, 0 for an ordinary player. Only the name's colour uses it.
    int32_t gmLevel;

    /// A creature's model, when the server describes it as one fixed model
    /// rather than as a race wearing things: a rabbit, a crab, a goblin.
    /// Zero when it has none. See mh::creatureFileId.
    uint32_t modelId;

    /// Health, 0 to 100. A mob at zero is a corpse: still an entity, still
    /// named, and not something to attack. -1 when the server has not said.
    int32_t healthPercent;

    /// Non-zero when the server will accept a trigger on this entity, which is
    /// what makes it worth putting a cursor over.
    int32_t triggerable;

    /// Non-zero to draw this one as a pale, half-transparent shape instead of
    /// itself: the figure standing in for a character that does not exist yet.
    /// It has a race and a build so it reads as a person, and no face, no
    /// colour and no clothes so it reads as nobody in particular.
    int32_t silhouette;

    /// How big the body is: 1 small, 2 medium, 3 large, as the server's
    /// char_look.size plus one. 0 means nobody said, which is drawn medium.
    /// Plus one so that a caller who never sets it gets medium rather than
    /// small, which is what a zeroed struct would otherwise mean.
    int32_t size;

    /// How long ago this one turned up, in seconds, or a negative number when
    /// nothing is known. What a spawn effect keys off - a worm heaving itself
    /// out of the ground has to know it has only just arrived.
    float spawned_seconds_ago;
} MhRadarEntity;

/// What to open. Every string is borrowed for the duration of the create call
/// and copied, so the caller may free them straight afterwards. A null string
/// means "not set", which for several of these is different from empty.
typedef struct MhViewerOptions
{
    const char* zone_path;
    const char* key_table_path;
    const char* key_table2_path;

    /// "race,face,head,body,hands,legs,feet", or null for no character.
    const char* look;

    /// The server's Vana'diel clock in seconds, or 0 to run the renderer's own.
    uint32_t server_clock;

    /// "x,y,z" to stand the character at, or null to pick somewhere.
    const char* character_at;

    /// Compass degrees, or null.
    const char* character_facing;

    /// Shown along the bottom of the radar. The renderer has no zone-name
    /// table; the caller already knows what it asked for.
    const char* zone_name;

    /// The name over our own character's head. The game shows everyone their
    /// own nameplate, and the renderer has no idea who it is playing.
    const char* player_name;

    /// Vana'diel clock as hhmm, or -1 to let the day run.
    int32_t time_of_day;

    /// Writes one frame to this path and closes, or null to stay open. Meant
    /// for checking what the viewer produced without someone watching it.
    const char* screenshot_path;

    /// Frames to wait first. A shot taken before the caller has posted
    /// anything shows an empty radar and proves nothing.
    int32_t screenshot_after_frames;
} MhViewerOptions;

/// Creates a viewer. Does not open a window - see mh_viewer_run.
MH_API MhViewerHandle mh_viewer_create(const MhViewerOptions* options);

/// Opens the window and runs until it closes. Blocking, and it owns the event
/// loop while it runs, so callers give it a thread of its own. Returns a
/// process-style exit code: 0 for a clean close.
MH_API int32_t mh_viewer_run(MhViewerHandle viewer);

/// Replaces what the radar shows. Safe to call from another thread while
/// mh_viewer_run is going, which is the point of the whole file.
MH_API void mh_viewer_set_entities(MhViewerHandle viewer, const MhRadarEntity* entities, int32_t count);

/// One of this zone's exits, as somewhere to draw a marker.
///
/// Y is up, matching everything else that crosses this boundary, and the
/// radius is how close counts as touching the line.
typedef struct MhZoneLine
{
    float x;
    float y;
    float z;
    float radius;
} MhZoneLine;

/// Replaces the zone lines drawn in the world. Wholesale: a line belongs to
/// the zone it came from and means nothing on the other side.
MH_API void mh_viewer_set_zone_lines(MhViewerHandle viewer, const MhZoneLine* lines, int32_t count);

/// Shows a plain message box and waits for it to be dismissed.
///
/// Needed before anything is drawn: on a first run there is no window, no
/// glyph atlas and nothing to render a screen with, so this is the only way to
/// say anything. Also the only way to explain the folder chooser on macOS,
/// where a panel's title is not shown to the person looking at it.
MH_API void mh_show_message(const char* title, const char* body);

/// What the player asked to do with one slot, taken once.
///
/// Returns 0 when nothing is waiting. `kind` is 1 to equip and 2 to drop; the
/// rest are the server's own numbering, so they can go straight into a packet.
MH_API int32_t mh_viewer_take_inventory_action(MhViewerHandle viewer, int32_t* kind,
                                               int32_t* container, int32_t* slot,
                                               int32_t* equip_slot, uint32_t* count);

/// One slot the player holds.
///
/// Container and slot are the server's own CONTAINER_ID and slot number, so
/// the pair identifies the same place here as it does in a packet.
typedef struct MhInventorySlot
{
    uint8_t container;
    uint8_t slot;
    uint16_t item_id;
    uint32_t count;
} MhInventorySlot;

/// Replaces the bags. Wholesale, because the server resends them wholesale.
///
/// `sizes` is how many slots each of the 18 containers has, as the server
/// reported it - not a constant. A character starts with thirty inventory
/// slots on some servers and quests for the rest, so drawing a fixed eighty
/// would offer places to put things that do not exist.
MH_API void mh_viewer_set_inventory(MhViewerHandle viewer,
                                    const MhInventorySlot* slots, int32_t count,
                                    const uint16_t* sizes, int32_t size_count);

/// Whether the character is kneeling - what a logout does while the server
/// counts down, and what /heal looks like.
MH_API void mh_viewer_set_resting(MhViewerHandle viewer, int32_t resting);

/// Whether the character is sitting. Its own state rather than a kind of
/// resting: resting is something the server puts you in, sitting is a pose you
/// choose and leave by walking out of.
MH_API void mh_viewer_set_sitting(MhViewerHandle viewer, int32_t sitting);
MH_API void mh_viewer_set_drawn(MhViewerHandle viewer, int32_t drawn);
MH_API uint32_t mh_viewer_current_target(MhViewerHandle viewer);

/// The character's job, level and stats, for the equipment screen.
///
/// Stats are STR, DEX, VIT, AGI, INT, MND, CHR in that order: the base from
/// the job and level, and the modifier everything worn adds to it.
typedef struct MhCharacterStats
{
    uint8_t main_job;
    uint8_t sub_job;
    uint8_t main_level;
    uint8_t sub_level;
    int32_t max_hp;
    int32_t max_mp;
    uint16_t base_stats[7];
    int16_t stat_modifiers[7];
} MhCharacterStats;

MH_API void mh_viewer_set_character_stats(MhViewerHandle viewer, const MhCharacterStats* stats);

/// Where each of the sixteen equipment slots is wearing something from.
///
/// A container and a slot per equipment slot, because that is all the server
/// sends: what is worn is whatever is in that place. 255 as the slot means
/// nothing is worn there.
MH_API void mh_viewer_set_equipment(MhViewerHandle viewer, const uint8_t* containers,
                                    const uint8_t* slots, int32_t count);

/// What an item is called and what it looks like.
///
/// Sent once per distinct item, not once per slot: a stack of ninety-nine
/// arrows is one icon. The pixels are RGBA, `width * height * 4` bytes, row
/// zero at the top. The renderer copies them into its atlas and does not keep
/// the pointer.
///
/// `slots` is thirty-two bits rather than the sixteen the DAT stores, because
/// a linkshell is worn in slot sixteen and sixteen bits cannot say so. The
/// file has no room for that bit either - it tells a linkshell apart by item
/// type - so the client sets it and this carries it.
MH_API void mh_viewer_push_item(MhViewerHandle viewer, uint16_t item_id,
                                const char* name, const char* description,
                                uint16_t type, uint16_t level, uint32_t slots,
                                const uint8_t* rgba, int32_t width, int32_t height);

/// What a dead player pressed in the box the renderer draws them. Matches
/// mh::DeathChoice.
enum
{
    MH_DEATH_NONE = 0,
    MH_DEATH_HOME_POINT = 1,
    MH_DEATH_ACCEPT_RAISE = 2
};

/// Whether the character is down, and whether a raise has been offered.
///
/// Per session rather than per entity, which is why this is a call of its own
/// rather than another field on MhRadarEntity. The renderer cannot work either
/// out for itself: it knows where the body is and nothing about the state of
/// it. Hit points arrive in one packet and the raise offer in another, so both
/// are the caller's answer - and together they are the whole of what the box
/// draws itself from. It appears on the first and its second button lights on
/// the second.
MH_API void mh_viewer_set_death(MhViewerHandle viewer, int32_t dead, int32_t raise_offered);

/// The player's own HP, MP and TP, with the two percentages the server sends
/// beside them. Drawn as a panel in the world window - without it, being dead
/// is something you deduce from not being able to move.
MH_API void mh_viewer_set_vitals(MhViewerHandle viewer, uint32_t hp, uint32_t mp, uint32_t tp,
                                 uint8_t hp_percent, uint8_t mp_percent);

/// Which link the player clicked in the world window, taken once: 0 none,
/// 1 Discord, 2 the issue tracker. The renderer knows a corner was pressed and
/// nothing about browsers, so opening it is the caller's job.
MH_API int32_t mh_viewer_take_link(MhViewerHandle viewer);

/// The .bgw the zone wants playing, or null for silence. The server sends a
/// track number and the caller turns that into a path; the renderer owns the
/// audio device because that is where SDL already is.
MH_API void mh_viewer_set_music(MhViewerHandle viewer, const char* path);

/// Draws a different zone in the window already open, rather than closing it
/// and opening another. The position is where the character lands.
MH_API void mh_viewer_load_zone(MhViewerHandle viewer, const char* dat_path, const char* zone_name,
                                float x, float y, float z, float heading);

/// Whether a zone is being read right now. While it is, the position this
/// window reports still belongs to the zone being left, and sending that to the
/// server is how a zone change turns into a loop.
MH_API int32_t mh_viewer_is_loading(MhViewerHandle viewer);

/// Preferences, both ways. Set once when the world opens; read back after the
/// keys in the world window change them, so they can be written to disk.
/// mh_viewer_take_settings returns non-zero when something changed.
MH_API void mh_viewer_set_settings(MhViewerHandle viewer, float music_volume, float sound_volume, float ui_scale,
                                   int32_t radar_turns);
MH_API int32_t mh_viewer_take_settings(MhViewerHandle viewer, float* music_volume, float* sound_volume, float* ui_scale,
                                       int32_t* radar_turns);

/// What the player pressed there, as one of MH_DEATH_*, consumed by the read.
/// Both answers are packets only the caller can send, the same way a jump is.
MH_API int32_t mh_viewer_take_death_choice(MhViewerHandle viewer);

/// Takes the entity the player asked to talk to, if any. Returns 0 when
/// nothing is pending; the request is consumed either way.
MH_API uint32_t mh_viewer_take_talk(MhViewerHandle viewer);

/// Adds one line to the chat panel. UTF-8 in, though the panel's font only
/// covers letters, digits and a little punctuation - anything else becomes a
/// space rather than a wrong glyph.
/// `tone` matches FfxiChatMessageType, and decides the line's colour.
MH_API void mh_viewer_push_chat(MhViewerHandle viewer, const char* line, int32_t tone);

/// Where the character has walked to. Returns 0 before the first frame.
///
/// The client owns the connection, so movement only reaches the server if it
/// asks for it. Y is up; FFXI's own vertical is the negation.
MH_API int32_t mh_viewer_get_character(MhViewerHandle viewer, float* x, float* y, float* z, float* heading);

/// Whether the player asked to jump since this was last called, clearing it.
///
/// The renderer animates the jump itself; this is how the client learns one
/// happened so it can tell the server, which is the only way anyone else sees
/// it.
MH_API int32_t mh_viewer_take_jump(MhViewerHandle viewer);

/// The next line the player typed, copied into `buffer`. Returns 0 when there
/// is nothing waiting, so a caller polls this the way it polls the jump.
MH_API int32_t mh_viewer_take_chat(MhViewerHandle viewer, char* buffer, int32_t capacity);

/// Puts the character somewhere, because the server said so. Y is up here, as
/// everywhere past the DAT readers; the caller converts.
MH_API void mh_viewer_place_character(MhViewerHandle viewer, float x, float y, float z, float heading);

/// Asks a running viewer to close. mh_viewer_run returns shortly after.
MH_API void mh_viewer_stop(MhViewerHandle viewer);

/// Frees the viewer. Stop it and let mh_viewer_run return first.
MH_API void mh_viewer_destroy(MhViewerHandle viewer);

/// Who the player is and what they look like, once the client knows.
///
/// The window now opens before the sign-in screen is drawn in it, so at the
/// moment the viewer is made there is no character yet - the name and the look
/// arrive at character select, long after. Either may be null to leave it as it
/// was.
///
/// `look` is race,face,head,body,hands,legs,feet, and is applied at the next
/// zone load: building a character reads a skeleton, its motions and a file per
/// slot, and until a zone is up there is nowhere for a body to stand.
MH_API void mh_viewer_set_player(MhViewerHandle viewer, const char* name, const char* look);

/// The server's clock, in Earth seconds since the Vana'diel epoch, once the
/// client has been told it.
///
/// Set at zone-in rather than when the viewer is made, because the window is
/// now open through the whole sign-in. The renderer counts from the moment this
/// arrives, so the sky is not ahead by the time someone spent typing.
MH_API void mh_viewer_set_clock(MhViewerHandle viewer, uint32_t server_clock);

/// The weather the zone is under, as the server numbers it (xi::Weather,
/// 0..19). Decides which of the zone's four skies is built; below zero means
/// nobody has said, which reads as the clear one.
MH_API void mh_viewer_set_weather(MhViewerHandle viewer, int32_t weather);

/// Writes the next frame to this path as a BMP and carries on drawing - what
/// /bug attaches to a report. Unlike MOGHOUSE_SCREENSHOT, does not exit.
MH_API void mh_viewer_capture(MhViewerHandle viewer, const char* path);

/// Whether the entities set on this viewer are a character-select line-up
/// rather than a zone's population.
///
/// The client knows who is on the account and what they look like; it does not
/// know where the floor is or where the camera points, and standing a row of
/// people up needs both. So it publishes the roster through
/// mh_viewer_set_entities and turns this on, and the arranging is done on the
/// side that has the zone's collision.
///
/// Picking one is the ordinary entity click, so the choice comes back through
/// mh_viewer_take_talk as the id the client gave that entity.
MH_API void mh_viewer_set_lineup(MhViewerHandle viewer, int32_t on);

/// Whether to draw the game's own furniture - the radar, the chat panel, the
/// clock and the zone's name.
///
/// Off while the client is on its own screens. Signing in and choosing a
/// character are not moments when a compass or a chat log mean anything, and
/// they sit over the very thing being looked at.
MH_API void mh_viewer_set_hud(MhViewerHandle viewer, int32_t on);

/// Puts the character aboard the monorail, or takes them off it.
///
/// While aboard the train carries them: walking, gravity and collision are all
/// skipped, because a carriage is scenery that happens to move and would not
/// hold anybody up. Only meaningful in a zone that has a railway.
MH_API void mh_viewer_set_riding(MhViewerHandle viewer, int32_t aboard);

/// One row of a form the client asks the renderer to draw.
///
/// Fixed-width strings for the same reason MhRadarEntity uses them: the array
/// crosses as a pointer and stays blittable, and nobody owns a lifetime on the
/// other side of the boundary. The renderer only draws about forty characters
/// of a row anyway, so the widths here are generous rather than tight.
typedef struct MhFormRow
{
    /// 0 label, 1 field, 2 secret (typed, drawn as dots), 3 button, 4 choice.
    ///
    /// A choice's value is "<selected>;first|second|third" going in and
    /// coming back. Picking an option hands the form back at once with the
    /// choice's own row as the button, so the caller can react to it before
    /// anything else is pressed.
    int32_t kind;

    /// 0 for a button that cannot be pressed or a field that cannot be typed
    /// into. Drawn greyed either way, so it reads as unavailable rather than
    /// missing.
    int32_t enabled;

    /// The caption over a field, a label's text, or a button's name.
    char text[64];

    /// What a field starts with. Ignored for labels and buttons, and replaced
    /// by whatever the player types.
    char value[128];
} MhFormRow;

/// Puts a form up, replacing any that was showing. A count of zero takes it
/// down.
///
/// The renderer knows nothing about what a login is: the client says what the
/// rows are and reads back what was typed, so every decision stays on the
/// client side where it already lives.
MH_API void mh_viewer_set_form(MhViewerHandle viewer, const char* title, const char* message,
                               const MhFormRow* rows, int32_t count);

/// Whether forms put up from now on stand against the left edge with the
/// world left bright beside them, rather than in the middle over a dimmed
/// world. For a screen that describes something standing in the world - a
/// character being made - which has to be seen while it is described.
MH_API void mh_viewer_set_form_aside(MhViewerHandle viewer, int32_t aside);

/// What the player pressed, taken once. Returns 0 while they are still filling
/// it in, so a caller polls this the way it polls the jump.
///
/// `button` receives the index of the row that was pressed - the caller gets
/// back the row it supplied rather than a count it would have to track. Each
/// row's value is written into `values` NUL terminated, one after another, in
/// the order the rows were given.
///
/// Returns how many values were written, which is how the caller knows where
/// the list ends. A trailing marker cannot do that job here: label and button
/// rows always come back empty, and an empty value is a lone NUL, so the first
/// such row would be read as the end of the list.
///
/// `capacity` should be at least 129 bytes per row - the widest value a
/// MhFormRow can carry, plus its terminator. A value that does not fit is
/// dropped along with everything after it, and the count reflects that.
MH_API int32_t mh_viewer_take_form_result(MhViewerHandle viewer, int32_t* button, char* values,
                                          int32_t capacity);

/// Asks the player where the game is, with the platform's own folder chooser.
///
/// Needs no viewer, because it runs before there is one. On a first run there
/// is no install, so no DATs, so no glyph atlas and nothing to draw a screen
/// with - a native chooser is the only thing that works with no game data at
/// all, and SDL gives the same call on all three platforms.
///
/// **Must be called on the main thread.** macOS will not open a window of any
/// kind anywhere else, this one included.
///
/// Blocks until the player chooses or cancels, pumping events while it waits:
/// SDL reports the answer through a callback, and on a first run there is no
/// other event loop running to deliver it. Writes a NUL-terminated path into
/// `out`.
///
/// Returns 1 when a folder was chosen, 0 when cancelled, and -1 if the chooser
/// could not be opened at all.
MH_API int32_t mh_pick_folder(const char* default_location, char* out, int32_t out_size);

#ifdef __cplusplus
}
#endif

#endif // MOGHOUSE_INTEROP_H
