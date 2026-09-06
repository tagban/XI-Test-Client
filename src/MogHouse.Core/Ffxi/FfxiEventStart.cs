using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_EVENT (S2C 0x032) - "a cutscene is starting".
///
/// The client is expected to play it and say when it is done. Until it does,
/// the character is InEvent, and a character in an event is never spawned for
/// anyone else - present, addressable, and invisible to every other player.
///
/// The field names are a trap worth documenting, because they cost a long
/// detour. From the server's own builder:
///
///     packet.EventNum  = the zone id
///     packet.EventPara = the event id
///
/// So EventNum is not the event. A login reply that appeared to carry a
/// nonsense event number of 235 was carrying the zone id in exactly the field
/// named for the event, and reading it as an id produced
/// "Event ID mismatch 0 != 152" from the server.
///
/// UniqueNo and ActIndex belong to whatever the event is about - usually an
/// NPC, not the player - and are echoed back unchanged when ending it.
/// </summary>
public sealed record FfxiEventStart(
    uint UniqueNo,
    ushort ActIndex,
    ushort ZoneNo,
    ushort EventId,
    ushort Mode)
{
    public const ushort PacketId = 0x032;

    /// <summary>
    /// The same event start, carrying string parameters - names to slot into
    /// the dialogue. Its header is laid out exactly like <see cref="PacketId"/>,
    /// with the strings following, so it reads with the same offsets.
    /// </summary>
    public const ushort PacketIdStr = 0x033;

    /// <summary>
    /// The event start that carries eight number parameters. The numbers sit
    /// between the id and ActIndex, so this one needs its own offsets.
    /// </summary>
    public const ushort PacketIdNum = 0x034;

    private const int Body = 4; // id/size/sync sub-packet header
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetActIndex = Body + 4;
    private const int OffsetEventNum = Body + 6;  // the zone, despite the name
    private const int OffsetEventPara = Body + 8; // the event
    private const int OffsetMode = Body + 10;

    /// <summary>Reads one, or null if the sub-packet is too short to hold it.</summary>
    // The number-parameter packet (0x034) pushes everything after UniqueNo
    // along by eight int32s, so it reads at its own offsets.
    private const int NumOffsetActIndex = Body + 36;
    private const int NumOffsetEventNum = Body + 38;
    private const int NumOffsetEventPara = Body + 40;
    private const int NumOffsetMode = Body + 42;

    /// <summary>
    /// Reads any of the three event-start packets - 0x032, 0x033 (string
    /// params) and 0x034 (number params) - or null if this is none of them or
    /// is too short.
    ///
    /// All three start an event and all three have to be answered: an event
    /// begun by 0x033 or 0x034 and not ended leaves the character InEvent and
    /// invisible to everyone else just as surely as one begun by 0x032, and
    /// parameter-carrying starts are the common case for anything past a
    /// bare "press enter" cutscene.
    /// </summary>
    public static FfxiEventStart? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < Body + 4)
        {
            return null;
        }

        // Which packet this is, before reading it as one. Without this every
        // sub-packet long enough parses as an event, and the numbers that come
        // out are whatever happened to be at those offsets - 11348, 45321 for a
        // character whose actual event was 305 - which the server answers with
        // "Event ID mismatch" while the real event stays open forever.
        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));

        // 0x032 and 0x033 share a header; 0x034 carries num[8] first.
        (int actIndex, int eventNum, int eventPara, int mode) = id switch
        {
            PacketId or PacketIdStr => (OffsetActIndex, OffsetEventNum, OffsetEventPara, OffsetMode),
            PacketIdNum => (NumOffsetActIndex, NumOffsetEventNum, NumOffsetEventPara, NumOffsetMode),
            _ => (-1, -1, -1, -1),
        };
        if (actIndex < 0 || subPacket.Length < mode + 2)
        {
            return null;
        }

        return new FfxiEventStart(
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(actIndex, 2)),
            ZoneNo: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(eventNum, 2)),
            EventId: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(eventPara, 2)),
            Mode: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(mode, 2)));
    }
}
