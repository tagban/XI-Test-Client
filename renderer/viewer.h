#pragma once

// The renderer as something you can call, rather than something you run.
//
// MogHouse ships as one application, so the renderer has to be a library the
// C# client loads rather than a second executable it talks to over a socket. A
// socket would have meant designing, versioning and debugging a wire format
// between two halves of the same program, and then throwing it away.
//
// The standalone `moghouse-renderer` still exists and is a thin wrapper over
// this - it is how the renderer gets exercised without the client, which is
// worth keeping.

#include "ffxi/lighting.h"
#include "linalg.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <array>
#include <vector>

namespace mh
{
/// A placed sprite animation: a flame or a glow. Where it sits, how big the
/// generator made it, which animation it cycles and which curve lights it.
struct SpriteInstance
{
    Vec3 centre;
    struct
    {
        float x, y;
    } scale{1.0f, 1.0f};
    std::string animation;
    std::string curve;
    bool nightOnly{};
    float fade[4]{}; ///< op 0x48 distances; all zero means always shown
};

/// One tracked thing. The radar only needs x and z - it is a plan view, and a
/// dot above you is still a dot - but the same list now also puts a body in
/// the world, which needs somewhere to stand and a way to face.
/// A zone line, as somewhere to draw rather than somewhere to stand.
///
/// The client knows where these are - it has to, since only the client can ask
/// to change zone - but the player had no way to see one. Walking to the edge
/// of a zone and guessing where the boundary is only works if you already know
/// the zone, so this is the client telling the player what it can already see.
///
/// Y is up here as everywhere past the DAT readers, and the radius is the
/// larger half-extent of the box the server keeps: being generous costs an
/// early zone rather than a missed one.
/// One building interior's own lighting, and where it applies.
///
/// A room carries its own times of day, and they are not the zone's: Windurst
/// Waters at noon is 1.48/1.49/1.50, near white, while the shop inside it is
/// 1.13/1.23/0.93 - dimmer, and with the blue pulled down so it reads warm.
/// Lit by the zone's set instead, an interior looks like a room with the roof
/// off, which is exactly what ours looked like.
struct InteriorLighting
{
    /// The room's own times of day, or empty for a room that ships none and
    /// is lit as the outdoors.
    ffxi::Lighting lighting;
    Vec3 boundsMin{};
    Vec3 boundsMax{};
    /// Which of the zone's draws are this room's, so it can be left out of a
    /// frame the player is not inside it for.
    uint32_t firstDraw{};
    uint32_t drawCount{};

    bool holdsDraw(size_t draw) const { return draw >= firstDraw && draw < firstDraw + drawCount; }

    bool contains(const Vec3& point, float margin) const
    {
        return point.x >= boundsMin.x - margin && point.x <= boundsMax.x + margin &&
               point.y >= boundsMin.y - margin && point.y <= boundsMax.y + margin &&
               point.z >= boundsMin.z - margin && point.z <= boundsMax.z + margin;
    }

    bool contains(const Vec3& point) const
    {
        return point.x >= boundsMin.x && point.x <= boundsMax.x && point.y >= boundsMin.y &&
               point.y <= boundsMax.y && point.z >= boundsMin.z && point.z <= boundsMax.z;
    }
};

/// What the world window calls itself.
///
/// One place, because it is the name a player sees and a version they will
/// quote in a bug report.
// Bump with the three packaging scripts, which each carry the version
// separately, and nothing fails when this one is left behind. It was missed at
// the v0.2.0 tag, so anything built from that tag says 0.1.2 - but all three
// 0.2.0 downloads were built after it was corrected, and every one of them was
// checked and reads 0.2.0.
inline constexpr const char* kWindowTitle = "MogHouse XI - Alpha 0.2.1";

struct ZoneLineMarker
{
    float x{};
    float y{};
    float z{};
    float radius{};
};

struct RadarEntity
{
    float x{};
    float z{};

    /// World height, Y up. The radar ignores it.
    float y{};

    /// Compass heading in radians, 0 along +z, the same convention the player
    /// character and the radar notch use.
    float heading{};

    /// 0 player, 1 npc, 2 enemy - matching MogHouse.Core's FfxiEntityKind.
    int kind{};

    /// Shown over the body. Empty draws nothing, which is what an entity the
    /// server has not named yet should look like.
    std::string name;

    /// The server's id for this entity, 0x1000000 | zone << 12 | targid.
    ///
    /// Carried so a name can be found for it. The server sends NPCs with no
    /// name at all - the names are in the client's own files, one table per
    /// zone - so without this every NPC in a city is anonymous.
    uint32_t id{};

    /// The server wants the name shown only when this is targeted. Doors and
    /// zone lines are named in the table but not labelled on screen.
    bool nameHidden{};

    /// Race, face, head, body, hands, legs, feet - what to build this one out
    /// of, when the server describes it the way it describes a player. All
    /// zero means it does not, and the shared body stands in.
    uint16_t look[7]{};

    /// GM level, 0 for an ordinary player.
    int gmLevel{};

    /// How long ago the client first saw this one, in seconds; negative when
    /// nothing is known. Drives the emerging-from-the-ground effect.
    float spawnedSecondsAgo{-1.0f};

    /// A creature's own model, when the server describes it as one fixed
    /// model rather than as a race wearing equipment.
    ///
    /// look_t is a union: a size of 1 means face, race and equipment, which
    /// the character loader already builds; a size of 0 means a single model
    /// id, which is every monster and most creature NPCs in the game. They
    /// were drawn as nothing at all until this existed - a name floating over
    /// empty ground where a rabbit should be.
    uint16_t modelId{};

    /// Health, 0 to 100, or -1 when the server has not said. A mob at zero is
    /// a corpse and is named in grey rather than in the colour of something
    /// worth fighting.
    int healthPercent{-1};

    /// How to draw this one: 0 as itself, 1 as a pale half-transparent shape
    /// with no face or clothes, 2 as itself but faded until the cursor is over
    /// it.
    ///
    /// Character select uses both. The figure standing in for a character that
    /// does not exist yet is the first; everyone real is the second, so the one
    /// being pointed at is the one in full colour.
    int silhouette{};
    /// 1 small, 2 medium, 3 large; 0 for nobody said. See MhRadarEntity.size.
    int size{};

    /// Whether the server will accept a trigger on this one. Only these are
    /// worth picking with a cursor: an auction counter is a real entity with a
    /// real position that answers nothing.
    bool triggerable{};

    /// Whether there is a look here worth building. Race zero is not a race.
    bool hasLook() const { return look[0] != 0; }

    /// Whether this is a creature with a model of its own.
    bool hasModel() const { return modelId != 0; }
};

/// The child NPC races, as the server numbers them: 29 Mithra, 30 Elvaan,
/// 31 Hume. Their own model files have not been found in the file table, so
/// each is drawn as the grown race at a child's height - a stopgap that puts
/// a walking, dressed figure where a blank shape stood, at the cost of the
/// face and clothes being an adult's. Deriving their bases the way
/// tools/pcmodels.py derived the adults' is the real answer.
inline bool isChildRace(uint16_t race) { return race >= 29 && race <= 31; }

inline uint16_t adultRaceFor(uint16_t race)
{
    switch (race)
    {
    case 29: return 7;   // Mithra
    case 30: return 4;   // Elvaan, drawn as the woman
    case 31: return 2;   // Hume, drawn as the woman
    default: return race;
    }
}

/// How much of a grown body's height a child is drawn at.
inline constexpr float kChildScale = 0.68f;

/// How much taller or shorter a body of this size is drawn than medium.
///
/// The game offers three sizes and draws them a little apart - enough that a
/// large Galka and a small one are plainly not the same height. These are
/// approximations of that, not measurements; the retail client's own factors
/// have not been read out of it.
inline float bodyScale(int size)
{
    switch (size)
    {
    case 1: return 0.92f;
    case 3: return 1.08f;
    default: return 1.0f;
    }
}


/// Where a creature's model lives, from the id the server sends.
///
/// One file each, holding the skeleton, the mesh and its animations together -
/// no equipment to assemble and no motion set to find alongside. Confirmed by
/// the skeletons' own names: model 269 is `usa` (usagi, a rabbit) and is what
/// a Savanna Rarab is, 356 is `kani` (a crab), 340 is `shee`, 484 is `gob_`
/// and 580 is `yagu`. 1300 is also exactly where the first skeleton-bearing
/// file in the archive sits.
inline constexpr size_t kCreatureModelBase = 1300;

inline size_t creatureFileId(uint16_t modelId) { return kCreatureModelBase + modelId; }

/// How many tracked entities get drawn as bodies. Beyond this they stay dots.
///
/// The instance buffer is allocated once at this size and never grown, so this
/// is a real ceiling rather than a hint. It is not protecting us from the
/// protocol: LandSandBoat allows 511 dynamic entities in a zone and its static
/// range is larger still, so the limit here is only about how many bodies are
/// worth drawing around you. Fifty turned out to be too few - a field of
/// monsters can pass it - so it is seventy-five.
///
/// Whoever misses out should be whoever is furthest away, which is a matter of
/// the order slots are handed out in rather than of this number. That was the
/// intent here for a long time and was not actually true: the list arrived in
/// whatever order a Dictionary enumerated it, so in a zone with more entities
/// than this the *set* of bodies drawn reshuffled whenever an entity went
/// stale and came back, and they blinked in and out. The frame loop now sorts
/// by distance before slots are handed out, so this number is once again only
/// about how many are worth drawing.
///
/// Raised from seventy-five, which a city passes easily. Each body costs a
/// mesh reposed on the CPU and uploaded every frame, so this is the expensive
/// number in a crowd - if a busy zone drags, it is the first thing to try.
inline constexpr int kMaxDrawnBodies = 192;

/// Everything the viewer needs to start. Fields that were environment
/// variables keep their meaning; an unset optional means the variable was
/// absent, which for several of them is different from being empty.
struct ViewerOptions
{
    /// The zone DAT to open. Empty opens a window with nothing in it, which is
    /// how the graphics path gets checked on a machine with no game installed.
    std::string zonePath;

    /// The two 256-byte tables MZB and MMB decryption need.
    std::string keyTablePath;
    std::string keyTable2Path;

    /// A player character as race,face,head,body,hands,legs,feet.
    std::optional<std::string> look;

    /// An NPC that lives in one DAT, as semicolon-separated paths. Ignored
    /// when `look` is set.
    std::optional<std::string> characterPath;

    std::optional<std::string> characterAt;      ///< "x,y,z"
    std::optional<std::string> characterFacing;  ///< compass degrees, 0 is north (+z)

    /// Shown along the bottom of the radar. The renderer has no zone-name
    /// table and does not want one - the client already knows what zone it
    /// asked for, so it says.
    std::optional<std::string> zoneName;

    /// Our own character's name, for the plate over its head. Unset draws none,
    /// which is what the standalone viewer with nobody logged in wants.
    std::optional<std::string> playerName;
    std::optional<std::string> camera;           ///< "x,y,z"
    std::optional<std::string> cameraLook;       ///< "yaw,pitch" in degrees

    /// Pins one animation. Unset lets movement choose idle, walk or run.
    std::optional<std::string> animation;

    /// Pins the animation clock to a frame, so a screenshot is repeatable.
    std::optional<float> frame;

    /// Pins the Vana'diel clock to hhmm. Unset lets the day run.
    std::optional<int> timeOfDay;

    /// The server's Vana'diel clock at zone-in, in Vana'diel seconds.
    ///
    /// Without it the renderer runs its own day at a made-up rate, and two
    /// clients side by side show different light and different weather, which
    /// is exactly when you most want them to agree. Vana'diel runs 25 times
    /// real time - one of its minutes is 2.4 seconds - so this is seeded once
    /// and advanced from there.
    std::optional<uint32_t> serverClock;

    /// Writes a frame and quits. With `screenshotSequence` set, the path is
    /// treated as a printf format and one file is written per source frame.
    std::optional<std::string> screenshotPath;
    int screenshotSequence{};

    /// Entities to show on the radar before anything is connected, as
    /// "x,z,kind;x,z,kind". The radar is most of the way to useless without
    /// something on it, and this is how it gets checked without a server.
    std::vector<RadarEntity> testEntities;

    /// Chat lines for a run with no client attached, so the panel can be
    /// framed and checked without a server session.
    std::vector<std::string> testChat;

    /// The death box, for a run with no client attached: 0 alive, 1 dead,
    /// 2 dead with a raise offered.
    ///
    /// The one thing on screen a player cannot ask for. Without this it can
    /// only be looked at by finding a server, finding something that kills
    /// you, and then finding someone willing to cast Raise.
    int testDeath{};

    /// Puts a stand-in form up so the widget can be looked at without a client.
    /// Same idea as testDeath: the standalone viewer has no ViewerLink, so
    /// without this there is no way to see a screen the client would set.
    int testForm{};

    /// How many frames to let pass before taking a screenshot. The default
    /// is just enough to let the first frames settle; a caller feeding the
    /// viewer from outside wants longer, because a shot taken before anything
    /// has been posted shows an empty radar and proves nothing.
    int settleFrames{5};

    /// Writes the baked top-down map out as a BMP, for looking at directly.
    std::optional<std::string> mapPath;

    /// 0 never cuts out, 1 always, anything else lets each texture decide.
    int cutoutMode{2};

    /// 0 draws colour with no alpha discard, 2 draws alpha as greyscale.
    float shaderMode{};
};

/// What a dead player picked out of the box the renderer draws them.
///
/// The two answers FFXI allows a corpse. Both leave here as a request rather
/// than as an act: the renderer has no socket, so pressing a button is the
/// player saying what they want and the client saying it to the server.
enum class DeathChoice
{
    None = 0,
    HomePoint = 1,
    AcceptRaise = 2,
};

/// What a row of a form is.
///
/// Deliberately few. The screens this replaces - login, character select, the
/// install prompt - are captions, things you type into, and things you press,
/// and a widget set larger than that is a second UI toolkit to maintain
/// alongside the game's own.
enum class FormRowKind
{
    Label = 0,
    Field = 1,
    /// Typed into like a Field, drawn as dots. Nothing else differs.
    Secret = 2,
    Button = 3,
    /// One option from several. `value` is "<selected>;first|second|...".
    /// Picking one hands the form back with this row as the button, so the
    /// client can react at once - the figure being made changes race the
    /// moment the race does.
    Choice = 4,
};

/// One row of a form the renderer draws and the client fills in.
struct FormRow
{
    FormRowKind kind{FormRowKind::Label};
    /// The caption above a field, the text of a label, or a button's name.
    std::string text;
    /// What a Field or Secret currently holds. Ignored for the others.
    std::string value;
    /// A button that cannot be pressed, or a field that cannot be typed into.
    bool enabled{true};
};

/// What a line of chat is. The numbers match FfxiChatMessageType on the client
/// side, so they cross the C boundary unchanged; the two above the server's
/// range are this client's own.
enum class ChatTone : int
{
    Say = 0,
    Shout = 1,
    Tell = 3,
    Party = 4,
    Linkshell = 5,
    System = 6,
    /// Somebody in the world speaking, as opposed to the server narrating.
    Npc = 20,
};

/// Pink for the two aimed at you personally, a tell brighter than a shout;
/// lime for a linkshell, blue for a party, white for everything ordinary.
inline const float* chatColour(ChatTone tone)
{
    static constexpr float kSay[3] = {0.92f, 0.94f, 0.98f};
    static constexpr float kShout[3] = {1.00f, 0.86f, 0.88f};
    static constexpr float kTell[3] = {1.00f, 0.38f, 0.72f};
    static constexpr float kParty[3] = {0.48f, 0.68f, 1.00f};
    static constexpr float kLinkshell[3] = {0.68f, 0.98f, 0.42f};
    static constexpr float kSystem[3] = {0.86f, 0.84f, 0.62f};
    static constexpr float kNpc[3] = {1.00f, 0.94f, 0.72f};
    switch (tone)
    {
        case ChatTone::Shout: return kShout;
        case ChatTone::Tell: return kTell;
        case ChatTone::Party: return kParty;
        case ChatTone::Linkshell: return kLinkshell;
        case ChatTone::System: return kSystem;
        case ChatTone::Npc: return kNpc;
        default: return kSay;
    }
}

/// A form waiting to be filled in, or nothing.
///
/// This is how the client asks for a screen without the renderer knowing what
/// a login is. The client says what the rows are; the renderer draws them,
/// takes the typing and the clicks, and reports back which button was pressed
/// and what the fields held at the time. All the deciding stays on the client
/// side, where it already lives.
struct Form
{
    std::string title;
    /// Shown under the buttons, for whatever went wrong last time.
    std::string message;
    std::vector<FormRow> rows;
    /// Stood to the left of the window rather than in the middle, with the
    /// world behind it left bright, so something standing in the world - a
    /// character being made - can be seen beside it.
    bool aside{false};
};

/// The live half of a running viewer: what a caller on another thread can
/// change while it runs, and how to ask it to stop.
///
/// Deliberately tiny. Everything the client needs to say to the renderer today
/// is "here is what is nearby" and "please close", and a narrow surface is what
/// makes the C ABI over it worth having rather than a chore to maintain.
///
/// The viewer owns the window and the event loop on whatever thread calls
/// runViewer, so every method here is safe to call from another one.
class ViewerLink
{
public:
    /// Replaces the radar contents. Wholesale rather than merged: the tracker
    /// on the other side already answers "what can be seen right now", and
    /// merging here would mean two places deciding when something has gone.
    void setEntities(std::vector<RadarEntity> entities);

    /// A copy, so the render loop is never holding the lock while it draws.
    std::vector<RadarEntity> entities() const;

    /// Where this zone's exits are, so they can be drawn.
    ///
    /// Replaced wholesale on every zone change: a line belongs to the zone it
    /// was read from and means nothing on the other side.
    void setZoneLines(std::vector<ZoneLineMarker> lines);
    std::vector<ZoneLineMarker> zoneLines() const;

    /// Asks the viewer to close its window and return.
    void stop();
    bool stopping() const;

    /// Adds a line to the chat panel, dropping the oldest.
    ///
    /// The renderer never asks what a line means - colour, sender and channel
    /// are the client's business. This is a window onto whether anything is
    /// arriving at all.
    /// A line and what kind of line it is, which is what decides its colour.
    /// Which channel something came from is most of what you need at a glance:
    /// a tell and a linkshell line read as different things long before either
    /// has been read.
    struct ChatLine
    {
        std::string text;
        ChatTone tone{ChatTone::Say};
    };

    void pushChat(const std::string& line, ChatTone tone = ChatTone::Say);
    std::vector<ChatLine> chat() const;

    /// One slot the player holds.
    ///
    /// Container and slot are the server's own numbering, so the same pair
    /// that identifies a slot in a packet identifies it here - which is what
    /// lets the renderer say "equip this one" without inventing an id scheme.
    struct InventorySlot
    {
        uint8_t container{};
        uint8_t slot{};
        uint16_t itemId{};
        uint32_t count{};
    };

    /// What an item is called and what it looks like.
    ///
    /// Pushed once per distinct item rather than once per slot: a stack of
    /// ninety-nine arrows is one icon and one name. The pixels are 32x32 RGBA
    /// straight from the item DAT, which the client reads because it already
    /// has the file table open.
    struct ItemFace
    {
        uint16_t itemId{};
        std::string name;
        std::string description;

        /// The item DAT's own kind - 4 a weapon, 5 armour, 7 usable, and so
        /// on. Carried so the panel can group by it without asking anyone.
        uint16_t type{};

        /// Equipment level, or zero for anything that is not worn.
        uint16_t level{};

        /// Which equipment slots will take this, as a bitmask - bit 0 the main
        /// hand, bit 15 the back, bit 16 the linkshell. Zero for anything that
        /// is not worn at all, which is how the menu knows whether to offer to
        /// equip it.
        ///
        /// Thirty-two bits, not the sixteen the DAT stores. A linkshell is
        /// worn in slot sixteen and the file's own field has no room to say
        /// so - it marks a linkshell by item type instead - so the client sets
        /// that bit on the way through.
        uint32_t slots{};

        int width{};
        int height{};
        std::vector<uint8_t> rgba;
    };

    /// The bags, as the server last described them.
    ///
    /// Sizes are per container and come from the server, not from a constant:
    /// a character starts with thirty slots on some servers and earns the rest,
    /// so a client that drew eighty would be offering places to put things
    /// that do not exist.
    void setInventory(const InventorySlot* slots, int count, const uint16_t* sizes, int sizeCount);

    /// Where each of the sixteen equipment slots is wearing something from.
    ///
    /// A pair per slot - the container and the slot inside it - because that
    /// is all the server sends. What is actually worn is whatever is in that
    /// place, so the bags have to have arrived for this to mean anything. 255
    /// as the slot means nothing is worn there.
    /// The character's job, level and stats. Stats are STR through CHR: the
    /// base from the job, and what everything worn adds to it.
    struct CharacterStats
    {
        uint8_t mainJob{};
        uint8_t subJob{};
        uint8_t mainLevel{};
        uint8_t subLevel{};
        int32_t maxHp{};
        int32_t maxMp{};
        uint16_t base[7]{};
        int16_t modifier[7]{};
        bool known{false};
    };

    void setCharacterStats(CharacterStats stats);
    CharacterStats characterStats() const;

    void setEquipment(const uint8_t* containers, const uint8_t* slots, int count);
    std::array<std::pair<uint8_t, uint8_t>, 16> equipment() const;
    std::vector<InventorySlot> inventory() const;
    std::array<uint16_t, 18> containerSizes() const;

    /// Bumped whenever the bags change, so the panel can rebuild only then.
    uint64_t inventoryRevision() const { return inventoryRevision_.load(); }

    /// Something the player asked to do with one slot.
    ///
    /// The renderer has no socket, so it can only ask. Taken once, like every
    /// other crossing here, and applied by whoever is holding the connection.
    struct InventoryAction
    {
        enum class Kind
        {
            None,
            Equip,
            Drop,

            /// Ask the server to describe us again, which is how a change of
            /// gear becomes a change of appearance.
            Refresh,
        };

        Kind kind{Kind::None};
        uint8_t container{};
        uint8_t slot{};

        /// Which equipment slot to put it in, for Equip. SLOTTYPE numbering.
        uint8_t equipSlot{};

        /// How many there are, for Drop - the server wants the whole quantity.
        uint32_t count{};
    };

    void requestInventoryAction(InventoryAction action);
    bool takeInventoryAction(InventoryAction& action);

    void pushItemFace(ItemFace face);

    /// Item faces that have arrived since this was last called, and drains
    /// them: the renderer copies the pixels into its atlas and never needs
    /// them again, so holding a second copy here would be a megabyte of
    /// nothing.
    std::vector<ItemFace> takeItemFaces();

    /// Where the character has walked to, posted every frame.
    ///
    /// The client needs this because it, not the renderer, talks to the
    /// server: without it a character walks around on screen while standing
    /// still in the world, which is exactly what it looks like - present,
    /// named, and not moving.
    ///
    /// Y is up here, as everywhere past the DAT readers. The caller converts.
    void setCharacter(float x, float y, float z, float heading);
    bool character(float& x, float& y, float& z, float& heading) const;

    /// A jump the player asked for, consumed by whoever reads it.
    ///
    /// The renderer plays the animation locally the moment the key is pressed,
    /// but only the client talks to the server, and a jump nobody else is told
    /// about is a jump only we can see. FFXI has a packet of its own for this
    /// - it is not an emote - so this is the signal that one is owed.
    void requestJump();
    bool takeJump();

    /// Somebody the player asked to talk to, taken once.
    ///
    /// An NPC volunteers nothing: the server runs its onTrigger only when
    /// asked, so a client that never asks hears nothing and looks like it has
    /// broken dialogue rather than one that never started a conversation.
    /// The renderer knows who is nearby and can be pointed at; only the client
    /// can do the asking.
    void requestTalk(uint32_t entityId);
    bool takeTalk(uint32_t& entityId);

    /// The entity the player currently has selected, or 0. Mirrored out of the
    /// renderer so the client can act on it - /attack engages whatever this
    /// is, the way pressing the attack key on a target does.
    void setTarget(uint32_t entityId);
    uint32_t target() const;

    /// The id posted when escape is pressed while the character line-up is up.
    ///
    /// Backing out of character select had no route at all: the line-up is
    /// figures you click, there is no panel to put a button on, and the only
    /// way Select ever returned empty-handed was the window closing. So an
    /// account that was signed in stayed signed in until the client was killed
    /// - which is also the thing that leaves the login server holding a session
    /// nobody is using.
    ///
    /// Sent through the same channel as a click rather than as a new call, so
    /// the screen polls one thing and the interop keeps its shape.
    static constexpr uint32_t kLineupCancelled = 0xFFFFFFFFu;


    /// A line the player typed and pressed return on, taken once.
    ///
    /// The renderer has no socket; the client does. Anything typed here has to
    /// cross over to be said, including the GM commands starting with '!' that
    /// are how a character moves between zones for now.
    void submitChat(const std::string& line);
    std::optional<std::string> takeChat();

    /// Put the character somewhere, on the server's say-so.
    ///
    /// Movement is otherwise the renderer's own: it walks the character and
    /// tells the client where it went. But the server moves people too - a GM
    /// command, a zone line, a failed zone check putting you back where you
    /// started - and a client that only ever reports its own position has no
    /// way to accept that. Taken once, like the other crossings here.
    void placeCharacter(float x, float y, float z, float heading);
    bool takePlacement(float& x, float& y, float& z, float& heading);

    /// Whether the character is down, and whether a raise has been offered.
    ///
    /// The renderer cannot work either out for itself: it knows where the body
    /// is and nothing about the state of it. Hit points arrive in a packet and
    /// so does the raise, so both are the client's answer - and together they
    /// are the whole of what the death box draws itself from.
    void setDeath(bool dead, bool raiseOffered);

    /// The player's own hit points, magic and TP.
    ///
    /// Nothing on screen said whether a character was alive, which made being
    /// dead something you worked out from not being able to move. The numbers
    /// arrive in one packet - GP_SERV_COMMAND_GROUP_ATTR - and are pushed
    /// straight through rather than kept anywhere clever.
    void setVitals(uint32_t hp, uint32_t mp, uint32_t tp, uint8_t hpPercent, uint8_t mpPercent);

    /// A link the player clicked in the world window, taken once.
    ///
    /// The renderer knows a button was pressed and nothing about browsers;
    /// opening a URL portably is the managed side's job, so this hands the
    /// choice over rather than acting on it.
    enum class Link
    {
        None = 0,
        Discord = 1,
        Issues = 2,
    };

    void chooseLink(Link which);

    /// What the player has chosen, so it can be kept between sessions.
    ///
    /// Both directions: set once when the world opens, and read back after
    /// the keys in the world window change them. The window is where they are
    /// changed and the managed side is what has a file.
    struct Settings
    {
        float musicVolume{0.5f};
        /// Everything that is not music, the way retail has a Sound slider
        /// beside its Music one. Ambience plays at a fraction of this.
        float soundVolume{0.5f};
        bool radarTurns{true};

        /// What the player wants the interface scaled by, or zero to let the
        /// window decide. Not the display's own pixels-over-points correction,
        /// which is applied on top of this and is not anyone's preference.
        float uiScale{0.0f};
    };

    void applySettings(Settings settings);
    Settings settings() const;
    bool settingsChanged();
    void noteSettings(Settings settings);

    /// Takes settings handed in from outside, once. False when there were
    /// none waiting, which is every frame after the first.
    bool takeSettings(float& volume, float& soundVolume, bool& radarTurns, float& uiScale);

    /// The .bgw the zone wants playing, or empty for silence. Set from the
    /// session, which is the half that hears the server say so.
    /// Asks the window to draw a different zone without closing.
    ///
    /// Zoning used to mean disposing this window and opening another, which is
    /// why the client appeared to shut down on !zone: for as long as the new
    /// zone took to read there was nothing on screen at all, and if it failed
    /// there was nothing to say so.
    struct ZoneRequest
    {
        std::string datPath;
        std::string zoneName;
        float x{}, y{}, z{};
        float heading{};
    };

    void requestZone(ZoneRequest request);
    bool takeZoneRequest(ZoneRequest& out);

    /// Whether a zone is being read right now, for anything that should not
    /// act on a world that is half-replaced.
    bool loading() const { return loading_; }
    void setLoading(bool loading) { loading_ = loading; }

    void setMusic(std::string path);
    std::string takeMusic(bool& changed);
    Link takeLink();

    struct Vitals
    {
        uint32_t hp{};
        uint32_t mp{};
        uint32_t tp{};
        uint8_t hpPercent{};
        uint8_t mpPercent{};
        bool known{};
    };

    Vitals vitals() const;
    bool dead(bool& raiseOffered) const;

    /// What the player pressed in that box, taken once.
    ///
    /// Both answers are packets only the client can send, the same way a jump
    /// is. The renderer draws the choice and reports it.
    void chooseDeath(DeathChoice choice);
    DeathChoice takeDeathChoice();

    /// Whether forms put up from now on stand aside - see Form::aside.
    void setFormAside(bool on);
    bool formAside() const;

    /// Puts a form up, or takes it down with an empty one. Replacing a form
    /// while one is showing is how a screen moves on to the next.
    void setForm(Form form);

    /// The form to draw this frame, if any.
    Form form() const;

    /// Records that a button was pressed, with the fields as they stood.
    void submitForm(int button, std::vector<std::string> values);

    /// What the player pressed, taken once. Returns false while they are still
    /// filling it in. `button` indexes the form's rows, so the caller gets back
    /// the row it supplied rather than a count of buttons it would have to
    /// track separately.
    bool takeFormResult(int& button, std::vector<std::string>& values);

    /// Who the player is, for their own nameplate.
    ///
    /// Set after the window exists rather than when it is made: the client now
    /// opens the window first and draws its sign-in inside it, so at the moment
    /// the renderer starts there is no character yet and no name to give it.
    /// Whether the character is resting - the pose a logout puts you in
    /// while the server counts down, and the one /heal uses.
    void setResting(bool resting);
    bool resting() const;

    /// Whether the character is sitting down.
    ///
    /// Its own state rather than a kind of resting: resting is something the
    /// server puts you in and will not let you walk out of, and sitting is a
    /// pose you choose and leave by moving. The clips are si0 to sit down, si1
    /// to stay sat and si2 to get up, each with a 0 for the legs and a 1 for
    /// everything above the waist, the same split as idl0/idl1.
    void setSitting(bool sitting);
    bool sitting() const;

    /// Whether the weapon is drawn - the engaged battle stance.
    ///
    /// Its own state, like sitting: the server drives whether you are actually
    /// attacking, but which pose to show is the client's, and it is held
    /// rather than sent. Set by engaging (/attack, or the server's engaged
    /// flag) and by the local /draw, which draws the weapon without a fight to
    /// pick one.
    void setDrawn(bool drawn);
    bool drawn() const;

    void setPlayerName(std::string name);
    std::string playerName() const;

    /// What the player looks like - race,face,head,body,hands,legs,feet, as
    /// ViewerOptions::look takes it.
    ///
    /// Taken rather than read, and applied at the next zone load. Building a
    /// character means reading a skeleton, its motions and a file per slot, so
    /// it is not something to redo mid-frame; a zone load is already rebuilding
    /// the world, and until one happens there is nowhere for a body to stand.
    void setLook(std::string look);
    bool takeLook(std::string& out);

    /// Whether the character is aboard the monorail.
    ///
    /// While they are, the train carries them: walking is ignored, gravity is
    /// ignored, and their position is whatever the car's is. Letting the usual
    /// movement run underneath would have them fall through the floor of a
    /// carriage that is not solid, because none of this is collision - the
    /// train is scenery that happens to move.
    void setRiding(bool aboard);
    bool riding() const;

    /// Whether to draw the game's own furniture: the radar, the chat panel, the
    /// clock and the zone's name.
    ///
    /// On by default, because the standalone viewer is always in a zone and has
    /// nobody to turn it on for it. The client turns it off while it is on its
    /// own screens - a compass and a chat log mean nothing during a sign-in,
    /// and they sit over the very thing being looked at.
    void setHud(bool on);
    bool hud() const;

    /// Whether the entities are a character-select line-up rather than a zone's
    /// population.
    ///
    /// The client knows who is on the account and what they look like; it does
    /// not know where the ground is or where the camera points, and both are
    /// needed to stand a row of people up and look at them. So it publishes the
    /// roster as ordinary entities and turns this on, and the arranging happens
    /// on the side that has the collision and the camera.
    ///
    /// Picking one is the ordinary entity click - the same path that asks an
    /// NPC to talk - so the client reads the choice back through takeTalk.
    void setLineup(bool on);
    bool lineup() const;

    /// The server's clock, in Earth seconds since the Vana'diel epoch.
    ///
    /// Carries the moment it was set alongside it, which ViewerOptions did not
    /// have to: that seed was fixed before the window opened, so time since
    /// startup was time since the seed. Now the window opens first and the
    /// clock only arrives at zone-in, so counting from startup would put the
    /// sky ahead by however long the player spent signing in.
    void setServerClock(uint32_t clock);

    /// The weather the server says this zone is under, as it numbers it -
    /// 0..19, xi::Weather. Which of the zone's four skies that calls for is
    /// decided in skyForWeather(); this only carries the number across.
    void setWeather(int32_t weather);
    int32_t weather() const;

    /// Asks for the next frame to be written to this path as a BMP, without
    /// ending the session the way the MOGHOUSE_SCREENSHOT path does. What
    /// /bug attaches to a report.
    void requestCapture(const std::string& path);

    /// Takes the pending capture path, if there is one, and clears it.
    bool takeCapture(std::string& path);
    bool serverClock(uint32_t& clock, uint64_t& setAtNs) const;

private:
    mutable std::mutex mutex_;
    std::vector<RadarEntity> entities_;
    std::vector<ZoneLineMarker> zoneLines_;
    std::atomic<bool> stop_{false};
    float character_[4]{};
    bool haveCharacter_{false};
    std::atomic<bool> jump_{false};
    std::atomic<uint32_t> talk_{0};
    std::deque<std::string> outgoing_;
    float placement_[4]{};
    bool havePlacement_{false};
    std::deque<ChatLine> chat_;
    std::vector<InventorySlot> inventory_;
    std::array<uint16_t, 18> containerSizes_{};
    std::atomic<uint64_t> inventoryRevision_{0};
    std::vector<ItemFace> itemFaces_;
    std::deque<InventoryAction> inventoryActions_;
    std::array<std::pair<uint8_t, uint8_t>, 16> equipment_{};
    CharacterStats characterStats_{};

    // Two flags rather than one guarded pair: a raise only means anything
    // while the character is down, so the two being read a moment apart says
    // nothing the box would draw differently.
    std::atomic<bool> dead_{false};
    std::atomic<bool> raiseOffered_{false};

    // Under the mutex rather than atomic, unlike the flags above: a form is a
    // title, a message and a list of rows, and a half-swapped one would draw
    // one screen's captions over another's fields.
    Form form_;
    bool formResultReady_{false};
    int formButton_{-1};
    std::vector<std::string> formValues_;
    std::atomic<uint32_t> hp_{0};
    std::atomic<uint32_t> mp_{0};
    std::atomic<uint32_t> tp_{0};
    std::atomic<uint8_t> hpPercent_{0};
    std::atomic<uint8_t> mpPercent_{0};
    std::atomic<bool> vitalsKnown_{false};
    std::atomic<int> link_{0};
    std::atomic<float> musicVolume_{0.5f};
    std::atomic<float> soundVolume_{0.5f};
    std::atomic<bool> radarTurns_{true};
    std::atomic<float> uiScale_{0.0f};
    std::atomic<bool> settingsDirty_{false};
    std::atomic<bool> settingsPending_{false};
    std::atomic<bool> loading_{false};
    std::mutex zoneLock_;
    ZoneRequest zoneRequest_;
    bool zoneRequested_{false};
    mutable std::mutex musicLock_;
    std::string music_;
    bool musicChanged_{false};
    std::atomic<int> deathChoice_{0};

    // Under the form mutex with everything else the screens touch. A name is
    // read every frame the character is drawn, so it is copied out rather than
    // held by reference.
    std::string playerName_;
    std::atomic<bool> resting_{false};
    std::atomic<bool> sitting_{false};
    std::atomic<bool> drawn_{false};
    std::atomic<uint32_t> target_{0};
    std::string look_;
    bool lookChanged_{false};
    std::atomic<bool> lineup_{false};
    std::atomic<bool> formAside_{false};
    std::atomic<bool> hud_{true};
    std::atomic<bool> riding_{false};
    std::atomic<int32_t> weather_{-1};   ///< -1 until the server has said
    mutable std::mutex captureMutex_;    ///< a path is not an atomic
    std::string capturePath_;
    uint32_t serverClock_{0};
    uint64_t serverClockSetAtNs_{0};
    bool serverClockKnown_{false};
};

/// Reads the options the standalone viewer has always taken: the zone from
/// argv[1] and everything else from MOGHOUSE_* in the environment.
ViewerOptions optionsFromEnvironment(int argc, char** argv);

/// Opens a window and runs until it closes. Returns a process exit code.
///
/// Blocking, and it owns the window and the event loop while it runs - the
/// same shape the engine this replaced had. A caller that needs to keep doing
/// its own work runs this on its own thread.
///
/// `link`, if given, is how that caller feeds it afterwards.
int runViewer(const ViewerOptions& options, ViewerLink* link = nullptr);
} // namespace mh
