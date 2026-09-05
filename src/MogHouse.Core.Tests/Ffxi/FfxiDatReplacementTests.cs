using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// A folder of replacement DATs, searched before the install.
///
/// The retail files are never written to: a mod is something you can delete.
///
/// <para>
/// These pass the folder to the table rather than setting
/// <c>MOGHOUSE_DAT_REPLACEMENTS</c>, because that variable is process-wide and
/// xUnit runs classes in parallel. Setting it here pointed every other table
/// in the process at this class's temp folder, and since every test that
/// builds a fake install writes its file to the same ROM/9/1.DAT, the tests
/// beside this one read this one's bytes: FfxiEntityNamesTests failed in a
/// full run and passed on its own. One test below still exercises the
/// variable's decision, and it does that without setting it.
/// </para>
/// </summary>
public class FfxiDatReplacementTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("moghouse-install").FullName;
    private readonly string _mods = Directory.CreateTempSubdirectory("moghouse-mods").FullName;

    /// <summary>An install holding one file, id 7, at ROM/9/1.DAT.</summary>
    private FfxiFileTable Install(byte[] contents, string? replacements = null)
    {
        Directory.CreateDirectory(Path.Combine(_root, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_root, "ROM", "9", "1.DAT"), contents);

        var vtable = new byte[8];
        vtable[7] = 1;                          // ROM 1, the folder called "ROM"
        var ftable = new byte[16];
        BinaryPrimitives.WriteUInt16LittleEndian(ftable.AsSpan(7 * 2, 2), (9 << 7) | 1);
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), vtable);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), ftable);

        return new FfxiFileTable(_root, replacements);
    }

    /// <summary>Puts a replacement for id 7 in the mods folder.</summary>
    private void Mod(byte[] contents)
    {
        // Mirroring the install's own layout, which is how every existing tool
        // distributes a DAT mod.
        Directory.CreateDirectory(Path.Combine(_mods, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_mods, "ROM", "9", "1.DAT"), contents);
    }

    [Fact]
    public void AReplacementIsUsedInsteadOfTheInstall()
    {
        FfxiFileTable table = Install([1, 2, 3], _mods);
        Mod([9, 9, 9]);

        string? found = table.Path(7);

        Assert.NotNull(found);
        Assert.Equal<byte[]>([9, 9, 9], File.ReadAllBytes(found));
        Assert.StartsWith(_mods, found);
    }

    [Fact]
    public void TheInstallIsUsedWhereNoReplacementExists()
    {
        FfxiFileTable table = Install([1, 2, 3], _mods);

        // The folder is there but holds nothing for this id.
        string? found = table.Path(7);

        Assert.NotNull(found);
        Assert.Equal<byte[]>([1, 2, 3], File.ReadAllBytes(found));
        Assert.StartsWith(_root, found);
    }

    [Fact]
    public void TheRetailFilesAreLeftAlone()
    {
        FfxiFileTable table = Install([1, 2, 3], _mods);
        Mod([9, 9, 9]);

        table.Path(7);

        // Overriding must never mean overwriting - the whole point is that the
        // install can be left exactly as the game shipped it.
        Assert.Equal<byte[]>([1, 2, 3], File.ReadAllBytes(Path.Combine(_root, "ROM", "9", "1.DAT")));
    }

    [Fact]
    public void AnIdTheInstallDoesNotHoldStaysMissing()
    {
        FfxiFileTable table = Install([1, 2, 3], _mods);

        // A replacement cannot add a file the table has no entry for: the id
        // has to resolve before there is a path to replace.
        Assert.Null(table.Path(3));
    }

    [Fact]
    public void TheNamedFolderIsUsedWhenItIsThere()
    {
        // The decision the shared root makes, tested without setting the
        // variable that carries it: that variable is process-wide, and a test
        // that sets it points every other test's table at this folder.
        Assert.Equal(_mods, FfxiFileTable.ReplacementRootFrom(_mods));
    }

    [Fact]
    public void AFolderThatIsNotThereIsNoFolder()
    {
        // Naming somewhere that does not exist is not an error - a player who
        // has never made the folder is the normal case - it just means the
        // install answers every id.
        Assert.Null(FfxiFileTable.ReplacementRootFrom(Path.Combine(_mods, "no-such-folder")));
    }

    public void Dispose()
    {
        Directory.Delete(_root, recursive: true);
        Directory.Delete(_mods, recursive: true);
    }
}
