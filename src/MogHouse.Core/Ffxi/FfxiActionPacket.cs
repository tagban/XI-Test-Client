using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_ACTION (C2S 0x01A, 28 bytes) - doing something to something.
///
/// One packet covers every deliberate act against a target: talking, attacking,
/// casting, using an ability. Which one is the ActionID, and the union after it
/// carries whatever that particular act needs - a spell id, a position. Talk
/// needs nothing, so the buffer goes out zeroed.
///
/// Talking is the interesting one here. An NPC does not volunteer anything: the
/// server runs its onTrigger only when asked, and what comes back is either a
/// line of dialogue as a TALKNUM id or an event to play. Without this packet a
/// client can stand in front of a shopkeeper indefinitely and hear nothing,
/// which looks like broken dialogue rather than a conversation never started.
///
/// The union's largest member is 16 bytes (ACTIONBUF_CASTMAGIC), so the body is
/// 24 whatever the action is.
/// </summary>
public static class FfxiActionPacket
{
    public const ushort PacketId = 0x01A;
    public const int PacketSize = 28;

    /// <summary>Interact with an NPC. The rest of the enum is combat.</summary>
    public const ushort ActionTalk = 0x00;

    /// <summary>
    /// Start attacking the target - "engage". The server turns to face the
    /// mob, closes to melee, and swings on its own timer from then on; the
    /// client's part is this one packet and then showing the drawn-weapon
    /// stance. Retail sends it when you press Enter twice on an attackable
    /// target, and /attack is the same thing.
    /// </summary>
    public const ushort ActionEngage = 0x02;

    /// <summary>Stop attacking - "disengage". Sheathes the weapon. /attackoff.</summary>
    public const ushort ActionDisengage = 0x04;

    /// <summary>
    /// Answer the menu a dead character gets. The buffer's StatusId picks
    /// which answer and Accept is zero, so a zeroed buffer is 'yes, send me
    /// to my home point'.
    /// </summary>
    public const ushort ActionHomePointMenu = 0x0B;

    /// <summary>
    /// The other answer: take the raise somebody has offered.
    ///
    /// The same buffer as the home point menu - the server reads this one
    /// through ACTIONBUF_HOMEPOINTMENU too - and Accept is again zero, so a
    /// zeroed buffer says yes. Declining is a 1 there, which this client has
    /// no way to ask for: a raise nobody wants is one you walk away from by
    /// pressing the other button.
    ///
    /// The server drops this unless it has already offered, so it is only
    /// worth sending after a <see cref="FfxiRaiseOffer"/> has arrived.
    /// </summary>
    public const ushort ActionRaiseMenu = 0x0D;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetUniqueNo = 4;
    private const int OffsetActIndex = 8;
    private const int OffsetActionId = 10;

    public static byte[] Build(uint uniqueNo, ushort actIndex, ushort actionId, ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2),
                                                 FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetUniqueNo, 4), uniqueNo);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetActIndex, 2), actIndex);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetActionId, 2), actionId);

        // The union, left zeroed: talking carries no arguments.
        return packet;
    }

    /// <summary>Engage the target - <see cref="ActionEngage"/>.</summary>
    public static byte[] BuildEngage(uint uniqueNo, ushort actIndex, ushort sync) =>
        Build(uniqueNo, actIndex, ActionEngage, sync);

    /// <summary>Disengage - <see cref="ActionDisengage"/>. The target is our own.</summary>
    public static byte[] BuildDisengage(uint uniqueNo, ushort actIndex, ushort sync) =>
        Build(uniqueNo, actIndex, ActionDisengage, sync);
}
