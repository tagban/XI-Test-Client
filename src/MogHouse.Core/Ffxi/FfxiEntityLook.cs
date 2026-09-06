namespace MogHouse.Core.Ffxi;

/// <summary>
/// How an entity's appearance is described - look_t's `size` field, which is
/// a kind rather than a length.
///
/// Two sources describe this byte and they do not agree, so both are recorded.
/// The names here are LandSandBoat's, because that is the server we talk to
/// and its values are what actually arrive. The second column is what the
/// retail client makes of the same byte:
///
///   0  Standard   non-visible static object
///   1  Equipped   player type
///   2  Door       doors
///   3  Elevator   elevator or other moving platform
///   4  Ship       movable object
///   5  Unknown5   a "binary name" in LSB - really an index code, and LSB's
///                 packet handling of it is wrong
///   6  Automaton  used by besieged and campaign monsters in an "npc" state
///   7  Chocobo    used in cutscenes; these objects are named like "NPC[FE]"
///                 in the DATs
///
/// Where they differ, LSB's writer decides what we receive: toLookFields in
/// src/map/data/shared_types/look.h puts 7 in the first field for a chocobo
/// and nine equipment-shaped fields after it, so 7 is read that way here.
/// </summary>
public enum FfxiLookKind : ushort
{
    /// <summary>A fixed model id. Most scenery and creature NPCs.</summary>
    Standard = 0,

    /// <summary>Race, face and equipment, exactly as a player character is.</summary>
    Equipped = 1,

    /// <summary>A door. Carries a door id or a name, not a model.</summary>
    Door = 2,

    /// <summary>Named transport rather than a model.</summary>
    Elevator = 3,

    /// <summary>Named transport rather than a model.</summary>
    Ship = 4,

    Unknown5 = 5,
    Automaton = 6,

    /// <summary>Race and equipment, like <see cref="Equipped"/>.</summary>
    Chocobo = 7,
}

/// <summary>
/// What an entity looks like, as the server describes it - the `look_t` at
/// offset 0x30 of an entity update.
///
/// FFXI has two quite different ways of saying what something looks like, and
/// which one applies is the first field. A shopkeeper is described the same way
/// a player is - a race and a set of equipment model ids - and can be built
/// from the same files. A crab, a door, a bookshelf is a single model id
/// pointing somewhere else entirely.
///
/// This is also where invisibility lives in practice: an auction counter is a
/// real entity with a real position that the game never draws.
/// </summary>
public sealed record FfxiEntityLook(FfxiLookKind Kind, ushort ModelId, byte Race, byte Face,
                                    ushort Head, ushort Body, ushort Hands, ushort Legs, ushort Feet,
                                    ushort Main, ushort Sub, ushort Ranged)
{
    /// <summary>
    /// Whether this is the race-and-equipment form, which the character loader
    /// already knows how to build.
    /// </summary>
    public bool IsEquipment => Kind is FfxiLookKind.Equipped or FfxiLookKind.Chocobo;

    /// <summary>
    /// Scenery rather than a character: a door, a ship, an elevator. Named in
    /// the zone's table and never labelled on screen - the game shows the name
    /// in the target box when you click one, not floating over it.
    /// </summary>
    public bool IsScenery => Kind is FfxiLookKind.Door or FfxiLookKind.Elevator or FfxiLookKind.Ship;

    /// <summary>
    /// Whether this names a single model rather than a set of pieces. The id
    /// alone is not enough to find the file - that mapping is its own problem.
    /// </summary>
    public bool IsFixedModel => Kind is FfxiLookKind.Standard or FfxiLookKind.Unknown5 or FfxiLookKind.Automaton;

    /// <summary>
    /// An equipment id with its slot tag removed.
    ///
    /// The server tags each id with the slot it belongs to in the high nibble -
    /// 0x1000 head, 0x2000 body, 0x3000 hands, 0x4000 legs, 0x5000 feet - and
    /// the model id is the low twelve bits. A real look reads
    /// 4096,8194,12288,16407,20503, which is the same as 0,2,0,23,23 once the
    /// tags come off, and only the second form means anything to the file
    /// table.
    /// </summary>
    public static ushort ModelOf(ushort tagged) => (ushort)(tagged & 0x0FFF);

    /// <summary>
    /// The numbers the character loader takes, in its order: race, the six
    /// worn slots, the size, then what is in each hand and slung on the back.
    ///
    /// The size is always 1 here. A player's look carries no size field - the
    /// server sends that separately as GraphSize - and the position has to be
    /// filled because the weapons come after it. They come after it so that a
    /// look string written before weapons existed still parses.
    ///
    /// A weapon model of zero is an empty hand. No weapon in the item table
    /// has model zero, so nothing is lost by reading it that way, and the file
    /// at the foot of the weapon window is a real mesh that would otherwise be
    /// put in the hand of everyone standing around unarmed.
    /// </summary>
    public string ToLookString() =>
        $"{Race},{Face},{ModelOf(Head)},{ModelOf(Body)},{ModelOf(Hands)},{ModelOf(Legs)},{ModelOf(Feet)}," +
        $"1,{ModelOf(Main)},{ModelOf(Sub)},{ModelOf(Ranged)}";
}
