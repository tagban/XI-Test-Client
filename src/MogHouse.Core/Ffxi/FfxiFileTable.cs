namespace MogHouse.Core.Ffxi;

/// <summary>
/// Resolves FFXI file ids to paths, the way the client does.
///
/// The retail install indexes its content by a flat file id rather than by
/// path. Two tables map one to the other, with one entry per id in each:
///
///     VTABLE.DAT   u8   the ROM number holding it, 0 if it is not installed
///     FTABLE.DAT   u16  (directory &lt;&lt; 7) | file
///
/// That pair at the install root describes the original game. Each expansion
/// brings a pair of its own - <c>ROM2/VTABLE2.DAT</c> and <c>FTABLE2.DAT</c>,
/// up through <c>ROM9</c> - covering the same id range, with a non-zero entry
/// only for the files that expansion holds. The client reads them all, later
/// ones overriding earlier, which is how a patch in a later ROM replaces a
/// file from an earlier one. Reading only the base pair - which this did for
/// some time - makes every expansion zone and model "not installed": Yhoator
/// Jungle is zone 123, its map is file 223, and file 223 is in ROM2.
///
/// The renderer has its own copy of this in C++, and tools/filetable.py a
/// third. Duplicated rather than exposed across the interop boundary on
/// purpose: it is thirty lines either side, and widening the C ABI to carry a
/// file system is a worse trade than writing the lookup three times. Change
/// one, change all three.
/// </summary>
public sealed class FfxiFileTable
{
    private readonly string _root;

    /// This table's own replacement folder, or null to use the shared one.
    private readonly string? _replacementRootHere;
    private readonly byte[] _vtable;
    private readonly byte[] _ftable;

    /// <summary>A zone's map data sits at its zone id plus this.</summary>
    public const int ZoneFileIdOffset = 100;

    /// <summary>The highest-numbered expansion folder looked for.</summary>
    private const int LastRom = 9;

    /// <param name="installRoot">Where the retail client is installed.</param>
    /// <param name="replacementRoot">
    /// A folder of replacement DATs to search first, overriding
    /// <see cref="ReplacementRoot"/> for this table alone.
    ///
    /// Null means "use the shared one", which is what the client passes. It is
    /// here so that a test can point one table somewhere without pointing the
    /// whole process there: the shared root reads an environment variable, and
    /// a test that sets it changes what every other table in the process
    /// resolves - including tables in tests running beside it.
    /// </param>
    public FfxiFileTable(string installRoot, string? replacementRoot = null)
    {
        _root = installRoot;
        _replacementRootHere = replacementRoot;
        // Qualified: this class has its own Path method, which shadows the one
        // in System.IO inside the class body.
        _vtable = File.ReadAllBytes(System.IO.Path.Combine(installRoot, "VTABLE.DAT"));
        _ftable = File.ReadAllBytes(System.IO.Path.Combine(installRoot, "FTABLE.DAT"));

        if (_ftable.Length != _vtable.Length * 2)
        {
            throw new InvalidDataException("VTABLE and FTABLE disagree on how many ids there are");
        }

        for (int rom = 2; rom <= LastRom; rom++)
        {
            Overlay(rom);
        }
    }

    /// <summary>
    /// Lays an expansion's tables over what has been read so far. Missing is
    /// normal - an install without that expansion has no such folder - and a
    /// pair that does not match the base is skipped rather than fatal, since
    /// one broken expansion should not take the whole install with it.
    /// </summary>
    private void Overlay(int rom)
    {
        string folder = System.IO.Path.Combine(_root, $"ROM{rom}");
        string vPath = System.IO.Path.Combine(folder, $"VTABLE{rom}.DAT");
        string fPath = System.IO.Path.Combine(folder, $"FTABLE{rom}.DAT");
        if (!File.Exists(vPath) || !File.Exists(fPath))
        {
            return;
        }

        byte[] vtable = File.ReadAllBytes(vPath);
        byte[] ftable = File.ReadAllBytes(fPath);
        if (vtable.Length != _vtable.Length || ftable.Length != _ftable.Length)
        {
            Console.WriteLine($"filetable: ROM{rom}'s tables do not match the base install's; ignoring them");
            return;
        }

        for (int id = 0; id < vtable.Length; id++)
        {
            if (vtable[id] != 0)
            {
                _vtable[id] = vtable[id];
                _ftable[id * 2] = ftable[id * 2];
                _ftable[id * 2 + 1] = ftable[id * 2 + 1];
            }
        }
    }

    public int Count => _vtable.Length;

    /// <summary>The path for a file id, or null if that id is not installed.</summary>
    public string? Path(int fileId)
    {
        if (fileId < 0 || fileId >= _vtable.Length)
        {
            return null;
        }

        byte rom = _vtable[fileId];
        if (rom == 0)
        {
            return null;
        }

        ushort packed = BitConverter.ToUInt16(_ftable, fileId * 2);

        // ROM 1 lives in a folder called plain "ROM"; the rest carry a number.
        string folder = rom == 1 ? "ROM" : $"ROM{rom}";
        string relative = System.IO.Path.Combine(folder, (packed >> 7).ToString(), $"{packed & 0x7F}.DAT");

        // A replacement, if somebody has put one there.
        if ((_replacementRootHere ?? ReplacementRoot) is { Length: > 0 } over)
        {
            string candidate = System.IO.Path.Combine(over, relative);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        return System.IO.Path.Combine(_root, relative);
    }

    private static string? _replacementRoot;
    private static string? _replacementRootFor;

    /// <summary>
    /// Where a modder's replacement DATs live, or null.
    ///
    /// <para>
    /// A folder mirroring the install's own layout - <c>ROM/1/31.DAT</c>,
    /// <c>ROM9/0/7.DAT</c> - whose files are used instead of the install's.
    /// The retail files are never written to, which is the whole point: a mod
    /// is something you can delete.
    /// </para>
    ///
    /// <para>
    /// The layout is the install's rather than our own file ids on purpose. It
    /// is what the existing tools distribute, so a mod written for any of them
    /// drops in unchanged, and it is what the wiki's paths already look like.
    /// </para>
    ///
    /// <para>
    /// <c>MOGHOUSE_DAT_REPLACEMENTS</c> names it; otherwise it is "DAT
    /// Replacements" beside the settings, which is a folder the player owns
    /// rather than one inside the game they do not. Setting the variable also
    /// hands it to the renderer, which reads the same name.
    /// </para>
    /// </summary>
    /// <summary>
    /// The folder <paramref name="named"/> asks for, or null if there is not
    /// one there. Empty and null both mean "the default place", which is "DAT
    /// Replacements" beside the settings.
    ///
    /// Split out of <see cref="ReplacementRoot"/> so that the decision can be
    /// tested without setting <c>MOGHOUSE_DAT_REPLACEMENTS</c>. That variable
    /// is process-wide and the test runner runs classes in parallel, so a test
    /// that sets it changes what every table in every other test resolves -
    /// which is exactly the failure this was written after.
    /// </summary>
    public static string? ReplacementRootFrom(string? named)
    {
        string root = named is { Length: > 0 }
            ? named
            : System.IO.Path.Combine(FfxiServerProfileStore.DefaultConfigDirectory(), "DAT Replacements");

        return Directory.Exists(root) ? root : null;
    }

    public static string? ReplacementRoot
    {
        get
        {
            // Cached against the value it was read from rather than by a
            // once-only flag. The answer cannot change while the client runs,
            // but a test that points this somewhere else has to be able to say
            // so - and a flag makes the first test to run decide for all of
            // them.
            string? named = Environment.GetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS");
            if (_replacementRootFor == named)
            {
                return _replacementRoot;
            }

            _replacementRootFor = named;
            if (ReplacementRootFrom(named) is not { } root)
            {
                _replacementRoot = null;
                return null;
            }

            // Handed to the renderer through the environment, so the two sides
            // cannot disagree about where mods live. Recorded as what we read,
            // so setting it does not invalidate the cache we just filled.
            Environment.SetEnvironmentVariable("MOGHOUSE_DAT_REPLACEMENTS", root);
            _replacementRootFor = root;
            _replacementRoot = root;
            return root;
        }
    }

    /// <summary>The map data for a zone, or null if it is not installed.</summary>
    public string? ZonePath(int zoneId) => Path(zoneId + ZoneFileIdOffset);

    /// <summary>
    /// Where the retail client is installed, from MOGHOUSE_FFXI_INSTALL or the
    /// usual Windows location.
    /// </summary>
    public static string DefaultInstallRoot() =>
        Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_INSTALL") is { Length: > 0 } configured
            ? configured
            : @"C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI";
}
