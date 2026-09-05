using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// How much of an emote to perform - GP_CLI_COMMAND_MOTION's Mode field,
/// EmoteMode on the server.
/// </summary>
public enum FfxiEmoteMode : byte
{
    /// <summary>The animation and the line of text, which is what /emote does.</summary>
    All = 0,

    /// <summary>The text only, no animation.</summary>
    Text = 1,

    /// <summary>The animation only - what /emote motion does, and what an animation wants.</summary>
    Motion = 2,
}

/// <summary>
/// GP_CLI_COMMAND_MOTION (C2S 0x05D, 16 bytes) - perform an emote.
///
/// This is the only way a client tells the server to play an animation on its
/// character that other people will see. Everything else another client shows
/// is inferred: walking and running come from the movement in the position
/// updates, not from any animation being named. So an animation the protocol
/// has no emote for cannot be broadcast at all - it can only ever be local.
///
/// The server rebroadcasts as GP_SERV_COMMAND_MOTIONMES (S2C 0x05A) to
/// everyone nearby, who play it themselves.
///
/// Number is validated as a *range* rather than a list - Emote::Point (0) to
/// Emote::Aim (96) - so the server accepts ids inside that span even where it
/// has no name for them, and there are unnamed gaps at 39, 45-64, 69-72 and
/// 75-95: 46 ids of the 97. What a retail client renders for one of those is a
/// question only a retail client can answer.
///
/// <para>
/// Checked against a server rather than assumed. The handler validates the
/// field with a range check between those two names, and then rebroadcasts
/// whatever arrived - the only ids with anything further to satisfy are the
/// bell, which wants a bell equipped or lockstyled, and the job emote, which
/// wants the job unlocked. An unnamed id matches neither and goes out to
/// everyone in range unchanged.
/// </para>
///
/// <para>
/// So a custom emote needs no server change: it is an unused number in a range
/// the server already passes through. That matters, because a client that
/// needs a patched server only works on patched servers.
/// </para>
/// </summary>
public static class FfxiMotionPacket
{
    public const ushort PacketId = 0x05D;
    public const int PacketSize = 16;

    /// <summary>Lowest and highest ids the server will accept - Emote::Point and Emote::Aim.</summary>
    public const byte MinEmote = 0;
    public const byte MaxEmote = 96;

    /// <summary>
    /// Emote::Wave - the one a dead character has any use for.
    ///
    /// A corpse cannot walk, act, or use an ability: the server turns all of
    /// it away. Emotes it does not, so waving is very nearly the only thing
    /// left that other people can be shown. See
    /// <see cref="FfxiGameSession.JumpAsync"/>, which sends this in place of a
    /// jump when the character is down.
    /// </summary>
    public const byte EmoteWave = 8;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetUniqueNo = 4;
    private const int OffsetActIndex = 8;
    private const int OffsetNumber = 10;
    private const int OffsetMode = 11;
    private const int OffsetParam = 12;

    /// <summary>
    /// Builds an emote. <paramref name="uniqueNo"/> and <paramref name="actIndex"/>
    /// are the target's, which for an emote performed at nobody is our own.
    /// </summary>
    public static byte[] Build(uint uniqueNo, ushort actIndex, byte number, ushort sync,
                               FfxiEmoteMode mode = FfxiEmoteMode.Motion, ushort param = 0)
    {
        ArgumentOutOfRangeException.ThrowIfGreaterThan(number, MaxEmote);

        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetUniqueNo, 4), uniqueNo);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetActIndex, 2), actIndex);
        packet[OffsetNumber] = number;
        packet[OffsetMode] = (byte)mode;
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetParam, 2), param);
        return packet;
    }
}
