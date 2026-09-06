using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// The look string, which is the whole description of a character in one line.
///
/// The renderer parses this on the other side of the interop boundary, so the
/// order and the count are a contract between two languages and worth pinning.
/// </summary>
public class FfxiEntityLookTests
{
    private static FfxiEntityLook Look(ushort main = 0, ushort sub = 0, ushort ranged = 0) =>
        new(FfxiLookKind.Equipped, ModelId: 0, Race: 3, Face: 5,
            Head: 0x1000 + 12, Body: 0x2000 + 34, Hands: 0x3000 + 56,
            Legs: 0x4000 + 78, Feet: 0x5000 + 90,
            Main: main, Sub: sub, Ranged: ranged);

    [Fact]
    public void TheSlotTagComesOffEveryPiece()
    {
        // The server tags each id with the slot in the high nibble. Only the
        // low twelve bits mean anything to the file table.
        Assert.Equal("3,5,12,34,56,78,90,1,0,0,0", Look().ToLookString());
    }

    [Fact]
    public void WhatIsHeldComesAfterTheSize()
    {
        // After, not beside the armour, so that a look string written before
        // weapons existed still parses as the seven it has.
        Assert.Equal("3,5,12,34,56,78,90,1,268,36,40", Look(main: 268, sub: 36, ranged: 40).ToLookString());
    }

    [Fact]
    public void AnEmptyHandIsZero()
    {
        // No weapon in the item table has model zero, so zero can only mean
        // nothing is held - and the renderer draws nothing for it rather than
        // the mesh at the foot of the weapon window.
        string worn = Look(main: 268).ToLookString();

        Assert.EndsWith(",268,0,0", worn);
    }

    [Fact]
    public void AWeaponsTagIsStrippedLikeAnyOther()
    {
        // Weapons carry tag zero almost always, but a handful in the item
        // table do not, and an unstripped one lands outside the window.
        Assert.EndsWith(",268,0,0", Look(main: 0x6000 + 268).ToLookString());
    }
}
