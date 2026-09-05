using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// A folder of replacement DATs, searched before the install.
///
/// The retail files are never written to: a mod is something you can delete.
/// </summary>
public class FfxiDatReplacementTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("moghouse-install").FullName;
    private readonly string _mods = Directory.CreateTempSubdirectory("moghouse-mods").FullName;
    private readonly string? _wasSet = Environment.GetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS");

    /// <summary>An install holding one file, id 7, at ROM/9/1.DAT.</summary>
    private FfxiFileTable Install(byte[] contents)
    {
        Directory.CreateDirectory(Path.Combine(_root, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_root, "ROM", "9", "1.DAT"), contents);

        var vtable = new byte[8];
        vtable[7] = 1;                          // ROM 1, the folder called "ROM"
        var ftable = new byte[16];
        BinaryPrimitives.WriteUInt16LittleEndian(ftable.AsSpan(7 * 2, 2), (9 << 7) | 1);
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), vtable);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), ftable);

        return new FfxiFileTable(_root);
    }

    [Fact]
    public void AReplacementIsUsedInsteadOfTheInstall()
    {
        FfxiFileTable table = Install([1, 2, 3]);
        Environment.SetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS", _mods);

        // Mirroring the install's own layout, which is how every existing tool
        // distributes a DAT mod.
        Directory.CreateDirectory(Path.Combine(_mods, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_mods, "ROM", "9", "1.DAT"), [9, 9, 9]);

        string? found = table.Path(7);

        Assert.NotNull(found);
        Assert.Equal<byte[]>([9, 9, 9], File.ReadAllBytes(found));
        Assert.StartsWith(_mods, found);
    }

    [Fact]
    public void TheInstallIsUsedWhereNoReplacementExists()
    {
        FfxiFileTable table = Install([1, 2, 3]);
        Environment.SetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS", _mods);

        // The folder is there but holds nothing for this id.
        string? found = table.Path(7);

        Assert.NotNull(found);
        Assert.Equal<byte[]>([1, 2, 3], File.ReadAllBytes(found));
        Assert.StartsWith(_root, found);
    }

    [Fact]
    public void TheRetailFilesAreLeftAlone()
    {
        FfxiFileTable table = Install([1, 2, 3]);
        Environment.SetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS", _mods);
        Directory.CreateDirectory(Path.Combine(_mods, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_mods, "ROM", "9", "1.DAT"), [9, 9, 9]);

        table.Path(7);

        // Overriding must never mean overwriting - the whole point is that the
        // install can be left exactly as the game shipped it.
        Assert.Equal<byte[]>([1, 2, 3], File.ReadAllBytes(Path.Combine(_root, "ROM", "9", "1.DAT")));
    }

    [Fact]
    public void AnIdTheInstallDoesNotHoldStaysMissing()
    {
        FfxiFileTable table = Install([1, 2, 3]);
        Environment.SetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS", _mods);

        // A replacement cannot add a file the table has no entry for: the id
        // has to resolve before there is a path to replace.
        Assert.Null(table.Path(3));
    }

    public void Dispose()
    {
        Environment.SetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS", _wasSet);
        Directory.Delete(_root, recursive: true);
        Directory.Delete(_mods, recursive: true);
    }
}
