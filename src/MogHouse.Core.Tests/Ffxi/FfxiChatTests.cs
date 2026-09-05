using System.Buffers.Binary;
using System.Text;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiChatPacketTests
{
    /// <summary>
    /// The server does not look for a terminator - it derives the message
    /// length from the declared size as `header.size * 4 - 6`. So the size
    /// field has to be right, and it has to be a multiple of 4.
    /// </summary>
    [Theory]
    [InlineData("hi", 12)]        // 6 + 2 + 1 = 9 -> 12
    [InlineData("hello", 12)]     // 6 + 5 + 1 = 12 -> 12
    [InlineData("hello!", 16)]    // 6 + 6 + 1 = 13 -> 16
    [InlineData("", 8)]           // 6 + 0 + 1 = 7  -> 8
    public void Build_SizeIsPaddedToFourByteUnits(string message, int expectedSize)
    {
        byte[] packet = FfxiChatPacket.Build(FfxiChatKind.Say, message, sync: 1);

        Assert.Equal(expectedSize, packet.Length);
        (ushort id, int declared) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(packet));
        Assert.Equal(FfxiChatPacket.PacketId, id);
        Assert.Equal(expectedSize, declared);
    }

    [Fact]
    public void Build_PlacesKindAndTextAtRealOffsets()
    {
        byte[] packet = FfxiChatPacket.Build(FfxiChatKind.Shout, "hello", sync: 7);

        Assert.Equal(7, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(2, 2)));
        Assert.Equal((byte)FfxiChatKind.Shout, packet[4]);
        Assert.Equal("hello", Encoding.ASCII.GetString(packet, 6, 5));
    }

    /// <summary>
    /// Padding lands inside the range the server reads as the message, so it
    /// has to be NULs rather than arbitrary bytes.
    /// </summary>
    [Fact]
    public void Build_PadsWithNulsNotGarbage()
    {
        byte[] packet = FfxiChatPacket.Build(FfxiChatKind.Say, "hello!", sync: 1);

        Assert.Equal(16, packet.Length);
        Assert.Equal(0, packet[12]);
        Assert.Equal(0, packet[13]);
        Assert.Equal(0, packet[14]);
        Assert.Equal(0, packet[15]);
    }

    [Fact]
    public void Build_OverlongMessageIsTruncatedToFieldSize()
    {
        byte[] packet = FfxiChatPacket.Build(FfxiChatKind.Say, new string('x', 500), sync: 1);

        (_, int declared) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(packet));
        Assert.Equal(packet.Length, declared);
        Assert.True(packet.Length <= 6 + FfxiChatPacket.MaxTextLength + 3);
    }
}

public class FfxiChatMessageTests
{
    /// <summary>
    /// Offsets come from the real compiled struct. The uint16 `Data` field at
    /// body+2 is what pushes sName to body+4 (packet offset 8) and the text to
    /// body+19 (packet offset 23) - not where a by-hand reading would put them.
    /// </summary>
    private static byte[] BuildServerChat(FfxiChatMessageType kind, string sender, string text)
    {
        var packet = new byte[176];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(FfxiChatMessage.PacketId, 176));
        packet[4] = (byte)kind;
        Encoding.ASCII.GetBytes(sender).CopyTo(packet, 8);
        Encoding.ASCII.GetBytes(text).CopyTo(packet, 23);
        return packet;
    }

    [Fact]
    public void TryParse_ReadsKindSenderAndText()
    {
        FfxiChatMessage? chat = FfxiChatMessage.TryParse(
            BuildServerChat(FfxiChatMessageType.Say, "Questria", "hello from MogHouse"));

        Assert.NotNull(chat);
        Assert.Equal(FfxiChatMessageType.Say, chat!.Kind);
        Assert.Equal("Questria", chat.Sender);
        Assert.Equal("hello from MogHouse", chat.Text);
    }

    [Fact]
    public void TryParse_TellIsDistinctFromSay()
    {
        FfxiChatMessage tell = FfxiChatMessage.TryParse(
            BuildServerChat(FfxiChatMessageType.Tell, "Tagban", "psst"))!;

        Assert.Equal(FfxiChatMessageType.Tell, tell.Kind);
    }

    [Fact]
    public void TryParse_WrongPacketId_ReturnsNull()
    {
        var packet = new byte[176];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(0x00D, 176));

        Assert.Null(FfxiChatMessage.TryParse(packet));
    }

    [Fact]
    public void TryParse_TooShort_ReturnsNull()
    {
        Assert.Null(FfxiChatMessage.TryParse(new byte[8]));
    }
}

public class FfxiTellPacketTests
{
    /// <summary>
    /// These two fields are named "unknown" but are validated: the server
    /// requires unknown04 == 3 and unknown05 == 0, and silently drops the
    /// packet otherwise while logging a warning only it can see. Leaving them
    /// zero - the natural default - fails invisibly, which is exactly how this
    /// was originally missed.
    /// </summary>
    [Fact]
    public void Build_SetsTheValidatedUnknownFields()
    {
        byte[] packet = FfxiTellPacket.Build("Tagban", "hello", sync: 1);

        Assert.Equal(3, packet[4]);
        Assert.Equal(0, packet[5]);
    }

    [Fact]
    public void Build_PlacesRecipientAndTextAtRealOffsets()
    {
        byte[] packet = FfxiTellPacket.Build("Tagban", "hello", sync: 1);

        Assert.Equal("Tagban", System.Text.Encoding.ASCII.GetString(packet, 6, 6));
        Assert.Equal("hello", System.Text.Encoding.ASCII.GetString(packet, 21, 5));
    }

    /// <summary>
    /// The server derives message length as `header.size * 4 - 0x15`, so the
    /// declared size must be right and a multiple of 4.
    /// </summary>
    [Theory]
    [InlineData("hi", 24)]      // 21 + 2 + 1 = 24
    [InlineData("hello", 28)]   // 21 + 5 + 1 = 27 -> 28
    [InlineData("", 24)]        // 21 + 0 + 1 = 22 -> 24
    public void Build_SizeIsPaddedToFourByteUnits(string message, int expectedSize)
    {
        byte[] packet = FfxiTellPacket.Build("Tagban", message, sync: 1);

        Assert.Equal(expectedSize, packet.Length);
        (ushort id, int declared) = FfxiZonePacket.UnpackIdAndSize(
            System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(packet));
        Assert.Equal(FfxiTellPacket.PacketId, id);
        Assert.Equal(expectedSize, declared);
    }

    [Fact]
    public void Build_LongRecipientIsTruncatedLeavingRoomForATerminator()
    {
        byte[] packet = FfxiTellPacket.Build(new string('x', 40), "hi", sync: 1);

        Assert.Equal(0, packet[6 + 14]);
    }
}

/// <summary>
/// Telling somebody whose name has a space in it.
///
/// The retail client cannot: it takes the first word as the name and sends the
/// rest, so a character called "Donald Trump" is reachable by nobody. Nothing
/// on the wire objects - the 0x0B6's recipient field is fifteen bytes and a
/// space is a byte like any other - so the whole limitation is in the parsing,
/// and quoting lifts it.
/// </summary>
public class FfxiTwoWordNameTests
{
    [Theory]
    [InlineData("/tell \"Donald Trump\" hello")]
    [InlineData("/tell 'Donald Trump' hello")]
    [InlineData("/t \"Donald Trump\" hello")]
    public void QuotedRecipientKeepsBothWords(string line)
    {
        FfxiClientCommand command = FfxiClientCommands.Parse(line);

        Assert.Equal(FfxiClientCommandKind.Tell, command.Kind);
        Assert.Equal("Donald Trump", command.Recipient);
        Assert.Equal("hello", command.Rest);
    }

    [Fact]
    public void QuotingKeepsTheRestOfTheMessageIntact()
    {
        FfxiClientCommand command = FfxiClientCommands.Parse("/tell \"Donald Trump\" how are you");

        Assert.Equal("Donald Trump", command.Recipient);
        Assert.Equal("how are you", command.Rest);
    }

    /// <summary>The ordinary case is untouched - no quotes, one word, as before.</summary>
    [Fact]
    public void AnUnquotedNameStillEndsAtTheFirstSpace()
    {
        FfxiClientCommand command = FfxiClientCommands.Parse("/tell Tagban hello there");

        Assert.Equal(FfxiClientCommandKind.Tell, command.Kind);
        Assert.Equal("Tagban", command.Recipient);
        Assert.Equal("hello there", command.Rest);
    }

    [Theory]
    [InlineData("/tell \"Donald Trump hello")]   // never closed
    [InlineData("/tell \"Donald Trump\"")]        // nobody said anything
    [InlineData("/tell \"\" hello")]              // quoted nobody
    public void AnIncompleteTellIsRefusedRatherThanSaidOutLoud(string line)
    {
        Assert.Equal(FfxiClientCommandKind.Incomplete, FfxiClientCommands.Parse(line).Kind);
    }

    /// <summary>
    /// And the packet carries it: the space is written into the recipient field
    /// like any other byte, so what the server reads is the whole name.
    /// </summary>
    [Fact]
    public void TheSpaceSurvivesOntoTheWire()
    {
        byte[] packet = FfxiTellPacket.Build("Donald Trump", "hello", sync: 1);

        Assert.Equal("Donald Trump", System.Text.Encoding.ASCII.GetString(packet, 6, 12));
        Assert.Equal(0, packet[6 + 12]);
    }

    [Theory]
    [InlineData(FfxiChatKind.Say, FfxiChatMessageType.Say)]
    [InlineData(FfxiChatKind.Shout, FfxiChatMessageType.Shout)]
    [InlineData(FfxiChatKind.Party, FfxiChatMessageType.Party)]
    [InlineData(FfxiChatKind.Linkshell1, FfxiChatMessageType.Linkshell)]
    [InlineData(FfxiChatKind.Linkshell2, FfxiChatMessageType.Linkshell)]
    [InlineData(FfxiChatKind.LinkshellPvp, FfxiChatMessageType.Linkshell)]
    [InlineData(FfxiChatKind.Emote, FfxiChatMessageType.Emotion)]
    public void AnEchoTakesItsOwnChannelsColour(FfxiChatKind sent, FfxiChatMessageType shown) =>
        Assert.Equal(shown, FfxiChatEcho.For(sent));

    [Fact]
    public void YellReadsAsAShoutBecauseTheServersListHasNoYell()
    {
        // The two enumerations are different lists and the message side stops
        // before Yell and Unity. A shout is what a yell is; anything else
        // falls back rather than inventing a colour.
        Assert.Equal(FfxiChatMessageType.Shout, FfxiChatEcho.For(FfxiChatKind.Yell));
        Assert.Equal(FfxiChatMessageType.Say, FfxiChatEcho.For(FfxiChatKind.Unity));
    }

    [Fact]
    public void TheTwoEnumerationsAreNotInterchangeable()
    {
        // Linkshell1 is 0x05 on the way out and Linkshell is 5 on the way
        // back, which is a coincidence; Yell is 0x1A and has no counterpart at
        // all. Casting between them would be right twice and wrong the rest of
        // the time, so this guards the one that would look safest.
        Assert.NotEqual((int)FfxiChatKind.Yell, (int)FfxiChatEcho.For(FfxiChatKind.Yell));
    }

    [Theory]
    [InlineData("/sit", FfxiClientCommandKind.Sit)]
    [InlineData("/sitchair", FfxiClientCommandKind.Sit)]
    [InlineData("/stand", FfxiClientCommandKind.Stand)]
    public void SittingIsAnsweredRatherThanSaid(string typed, FfxiClientCommandKind kind)
    {
        // A pose the client holds, so it must not reach the zone as speech -
        // which is the fault /logout had, broadcast as the word "/logout".
        Assert.Equal(kind, FfxiClientCommands.Parse(typed).Kind);
    }
}
