using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiActionPacketTests
{
    private static (ushort id, int size, ushort sync, uint uniqueNo, ushort actIndex, ushort actionId) Read(byte[] p)
    {
        (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(p));
        return (id, size,
                BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(2)),
                BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(4)),
                BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(8)),
                BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(10)));
    }

    [Fact]
    public void EngageCarriesTheTargetAndTheEngageAction()
    {
        byte[] p = FfxiActionPacket.BuildEngage(uniqueNo: 0x01234567, actIndex: 0x0042, sync: 7);

        var (id, size, sync, uniqueNo, actIndex, actionId) = Read(p);
        Assert.Equal(FfxiActionPacket.PacketId, id);
        Assert.Equal(FfxiActionPacket.PacketSize, size);
        Assert.Equal(7, sync);
        Assert.Equal(0x01234567u, uniqueNo);
        Assert.Equal(0x0042, actIndex);
        Assert.Equal(FfxiActionPacket.ActionEngage, actionId);
    }

    [Fact]
    public void DisengageIsTheDisengageAction()
    {
        byte[] p = FfxiActionPacket.BuildDisengage(uniqueNo: 1, actIndex: 2, sync: 3);
        Assert.Equal(FfxiActionPacket.ActionDisengage, Read(p).actionId);
    }

    [Theory]
    [InlineData("/attack", FfxiClientCommandKind.Engage)]
    [InlineData("/at", FfxiClientCommandKind.Engage)]
    [InlineData("/attackoff", FfxiClientCommandKind.Disengage)]
    [InlineData("/draw", FfxiClientCommandKind.Draw)]
    [InlineData("/sheathe", FfxiClientCommandKind.Draw)]
    public void TheCommandsParse(string line, FfxiClientCommandKind kind) =>
        Assert.Equal(kind, FfxiClientCommands.Parse(line).Kind);
}
