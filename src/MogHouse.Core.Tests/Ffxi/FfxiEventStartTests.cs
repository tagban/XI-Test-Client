using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// An event starts by one of three packets. All three have to be recognised:
/// an event begun by any of them and not ended leaves the character InEvent
/// and invisible to everyone else, and the parameter-carrying starts (0x033,
/// 0x034) are the common case for anything past a bare cutscene.
/// </summary>
public class FfxiEventStartTests
{
    private static void Header(Span<byte> p, ushort id) =>
        BinaryPrimitives.WriteUInt16LittleEndian(p, (ushort)(id | ((p.Length / 4) << 9)));

    [Fact]
    public void ReadsThePlainStart()
    {
        var p = new byte[16];
        Header(p, FfxiEventStart.PacketId);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4), 0x0100_0042);  // UniqueNo
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(8), 0x0007);       // ActIndex
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(10), 230);         // zone
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(12), 305);         // event

        FfxiEventStart? e = FfxiEventStart.TryParse(p);
        Assert.NotNull(e);
        Assert.Equal(0x0100_0042u, e!.UniqueNo);
        Assert.Equal(230, e.ZoneNo);
        Assert.Equal(305, e.EventId);
    }

    [Fact]
    public void ReadsTheStringParamStartLikeThePlainOne()
    {
        // 0x033 shares the header layout, so the same offsets read it.
        var p = new byte[112];
        Header(p, FfxiEventStart.PacketIdStr);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(10), 42);   // zone
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(12), 900);  // event

        FfxiEventStart? e = FfxiEventStart.TryParse(p);
        Assert.NotNull(e);
        Assert.Equal(42, e!.ZoneNo);
        Assert.Equal(900, e.EventId);
    }

    [Fact]
    public void ReadsTheNumberParamStartAtItsShiftedOffsets()
    {
        // 0x034 carries num[8] before ActIndex, so zone/event sit 32 bytes on.
        var p = new byte[64];
        Header(p, FfxiEventStart.PacketIdNum);
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4), 0x0EE);   // UniqueNo
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(4 + 36), 5);  // ActIndex
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(4 + 38), 130); // zone
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(4 + 40), 12);  // event

        FfxiEventStart? e = FfxiEventStart.TryParse(p);
        Assert.NotNull(e);
        Assert.Equal(0x0EEu, e!.UniqueNo);
        Assert.Equal(5, e.ActIndex);
        Assert.Equal(130, e.ZoneNo);
        Assert.Equal(12, e.EventId);
    }

    [Fact]
    public void APacketOfAnotherKindIsNotAnEvent()
    {
        var p = new byte[16];
        Header(p, 0x017);
        Assert.Null(FfxiEventStart.TryParse(p));
    }
}
