using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Interop;

/// <summary>What the radar shows, in world coordinates.</summary>
/// <remarks>
/// Blittable and laid out to match MhRadarEntity, so an array of these crosses
/// the boundary as a pointer with no marshalling.
/// </remarks>
/// <summary>
/// One zone line, as somewhere to draw rather than somewhere to stand.
///
/// Matches MhZoneLine. Y is up here, as everywhere across this boundary.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct NativeZoneLine
{
    public float X;
    public float Y;
    public float Z;
    public float Radius;
}

/// <summary>
/// The character's job, level and stats. Matches MhCharacterStats.
///
/// Stats are STR, DEX, VIT, AGI, INT, MND, CHR: the base from the job and
/// level, and what everything worn adds to it.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeCharacterStats
{
    public byte MainJob;
    public byte SubJob;
    public byte MainLevel;
    public byte SubLevel;
    public int MaxHp;
    public int MaxMp;
    public fixed ushort BaseStats[7];
    public fixed short StatModifiers[7];
}

/// <summary>
/// One slot the player holds. Matches MhInventorySlot.
///
/// Container and slot are the server's own numbering, so the pair identifies
/// the same place here, in a packet, and in the renderer.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct NativeInventorySlot
{
    public byte Container;
    public byte Slot;
    public ushort ItemId;
    public uint Count;
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeRadarEntity
{
    public float X;
    public float Z;

    /// <summary>World height, Y up. The radar ignores it; the body does not.</summary>
    public float Y;

    /// <summary>Compass heading in radians, 0 along +z.</summary>
    public float Heading;

    /// <summary>Cast from <see cref="Ffxi.FfxiEntityKind"/>; the values match.</summary>
    public int Kind;

    /// <summary>
    /// Shown over the body. Fixed width and ASCII so the array stays blittable.
    /// </summary>
    public unsafe fixed byte Name[20];

    /// <summary>
    /// The server's id for this entity. The renderer uses it to find a name in
    /// the zone's own name table, because the server sends NPCs without one.
    /// </summary>
    public uint Id;

    /// <summary>
    /// Non-zero when the name should stay hidden until the entity is targeted.
    /// </summary>
    public int NameHidden;

    /// <summary>
    /// Race, face, head, body, hands, legs, feet - slot tags already stripped.
    /// All zero when the server describes this one as a fixed model instead.
    /// </summary>
    public unsafe fixed ushort Look[7];

    /// <summary>GM level, 0 for an ordinary player.</summary>
    public int GmLevel;

    /// <summary>Writes a look in, or leaves it zeroed.</summary>
    /// <summary>
    /// A creature's own model, when the server describes it as one fixed
    /// model rather than as a race wearing equipment. Zero when it has none.
    /// </summary>
    public uint ModelId;

    /// <summary>Health 0-100, or -1 when the server has not said.</summary>
    public int HealthPercent;

    /// <summary>Non-zero when the server will accept a trigger on this.</summary>
    public int Triggerable;

    /// <summary>
    /// How to draw this one: 0 as itself, 1 as a pale half-transparent shape,
    /// 2 as itself but faded until the cursor is over it. Matches
    /// MhRadarEntity.silhouette.
    /// </summary>
    public int Silhouette;

    /// <summary>
    /// 1 small, 2 medium, 3 large; 0 for nobody said, drawn medium. The
    /// server's size plus one, so a struct left at zero reads as medium.
    /// </summary>
    public int Size;

    /// <summary>
    /// How long ago this one turned up, in seconds; negative when nothing is
    /// known. Matches MhRadarEntity.spawned_seconds_ago.
    /// </summary>
    public float SpawnedSecondsAgo;

    public unsafe void SetLook(Ffxi.FfxiEntityLook? look)
    {
        if (look is null)
        {
            return;
        }

        // look_t is a union. A fixed model is not a race wearing anything,
        // so there is nothing to fill the seven slots with - it is one id
        // pointing at one file, and the renderer loads it whole.
        if (!look.IsEquipment)
        {
            ModelId = look.ModelId;
            return;
        }

        fixed (ushort* target = Look)
        {
            target[0] = look.Race;
            target[1] = look.Face;
            target[2] = Ffxi.FfxiEntityLook.ModelOf(look.Head);
            target[3] = Ffxi.FfxiEntityLook.ModelOf(look.Body);
            target[4] = Ffxi.FfxiEntityLook.ModelOf(look.Hands);
            target[5] = Ffxi.FfxiEntityLook.ModelOf(look.Legs);
            target[6] = Ffxi.FfxiEntityLook.ModelOf(look.Feet);
        }
    }

    /// <summary>Writes a name in, truncated and NUL terminated.</summary>
    public unsafe void SetName(string? value)
    {
        fixed (byte* target = Name)
        {
            int written = 0;
            if (!string.IsNullOrEmpty(value))
            {
                // The renderer's font has no lower case and no accents, and it
                // turns anything it does not know into a space. Cutting to
                // ASCII here keeps that decision in one place.
                foreach (char c in value)
                {
                    if (written >= 19)
                    {
                        break;
                    }
                    target[written++] = c < 128 ? (byte)c : (byte)' ';
                }
            }
            target[written] = 0;
        }
    }
}

/// <summary>What a dead player pressed in the box the renderer draws them.</summary>
/// <remarks>Matches MH_DEATH_* in moghouse_interop.h and mh::DeathChoice.</remarks>
public enum NativeDeathChoice
{
    None = 0,
    HomePoint = 1,
    AcceptRaise = 2,
}

/// <summary>A link clicked in the world window's top corner.</summary>
public enum NativeLink
{
    None = 0,
    Discord = 1,
    Issues = 2,
}

/// <summary>What a form row is for. Cast straight across; the values match MhFormRow.</summary>
public enum NativeFormRowKind
{
    /// <summary>Text on its own line. Not focusable.</summary>
    Label = 0,

    /// <summary>A box the player types into.</summary>
    Field = 1,

    /// <summary>A field drawn as dots. Still returned in the clear.</summary>
    Secret = 2,

    /// <summary>Something to press, which ends the form.</summary>
    Button = 3,

    /// <summary>
    /// One option from several, unfolding into a list when pressed. Picking
    /// one hands the form back at once with this row as the button, so the
    /// caller can react to the change before anything else is pressed.
    /// </summary>
    Choice = 4,
}

/// <summary>
/// A row of a form, as callers write it.
///
/// The nested factories read better at a call site than four bools would:
/// <c>NativeFormRow.Secret("Password")</c> says what it is.
/// </summary>
public sealed record NativeFormRow(NativeFormRowKind Kind, string Text, string Value = "", bool Enabled = true)
{
    public static NativeFormRow Label(string text) => new(NativeFormRowKind.Label, text);

    public static NativeFormRow Field(string caption, string value = "", bool enabled = true) =>
        new(NativeFormRowKind.Field, caption, value, enabled);

    public static NativeFormRow Secret(string caption, string value = "", bool enabled = true) =>
        new(NativeFormRowKind.Secret, caption, value, enabled);

    public static NativeFormRow Button(string text, bool enabled = true) =>
        new(NativeFormRowKind.Button, text, "", enabled);

    /// <summary>
    /// A dropdown: a caption, the options, and which is chosen. The value
    /// carries all three as "&lt;selected&gt;;first|second|third", which is
    /// the one shape the renderer and the caller both read.
    /// </summary>
    public static NativeFormRow Choice(string caption, IReadOnlyList<string> options, int selected,
                                       bool enabled = true) =>
        new(NativeFormRowKind.Choice, caption, EncodeChoice(selected, options), enabled);

    internal static string EncodeChoice(int selected, IReadOnlyList<string> options) =>
        $"{Math.Clamp(selected, 0, Math.Max(options.Count - 1, 0))};{string.Join('|', options)}";
}

/// <summary>
/// What came back when a form was submitted: which button, and what every row
/// held at that moment.
/// </summary>
/// <param name="Button">
/// The index of the row that was pressed, in the list that was handed to
/// <see cref="NativeViewer.ShowForm"/> - so the caller gets back the row it
/// supplied rather than a count it would have to keep track of.
/// </param>
/// <param name="Values">
/// One entry per row, in the same order. Labels and buttons come back empty;
/// fields come back with whatever was typed.
/// </param>
public sealed record NativeFormResult(int Button, IReadOnlyList<string> Values)
{
    /// <summary>
    /// What a row held, or an empty string if that row is not one there is a
    /// value for. Saves every caller writing the same bounds check.
    /// </summary>
    public string this[int row] => row >= 0 && row < Values.Count ? Values[row] : string.Empty;

    /// <summary>
    /// Which option a <see cref="NativeFormRowKind.Choice"/> row holds, or
    /// <paramref name="fallback"/> if that row is not a choice or did not come
    /// back in the expected shape.
    /// </summary>
    public int Choice(int row, int fallback = 0)
    {
        string value = this[row];
        int split = value.IndexOf(';');
        return split > 0 && int.TryParse(value.AsSpan(0, split), out int chosen) && chosen >= 0
            ? chosen
            : fallback;
    }
}

/// <summary>Blittable, laid out to match MhFormRow.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeFormRowData
{
    public int Kind;
    public int Enabled;
    public unsafe fixed byte Text[64];
    public unsafe fixed byte Value[128];
}

/// <summary>What to open the viewer on.</summary>
public sealed record NativeViewerOptions
{
    public required string ZonePath { get; init; }
    public required string KeyTablePath { get; init; }
    public required string KeyTable2Path { get; init; }

    /// <summary>"race,face,head,body,hands,legs,feet", or null for no character.</summary>
    public string? Look { get; init; }

    /// <summary>The name to draw over our own character.</summary>
    public string? PlayerName { get; init; }

    /// <summary>The server's Vana'diel clock in seconds; 0 runs the renderer's own.</summary>
    public uint ServerClock { get; init; }

    /// <summary>"x,y,z", or null to let the viewer pick somewhere standable.</summary>
    public string? CharacterAt { get; init; }

    /// <summary>Compass degrees.</summary>
    public string? CharacterFacing { get; init; }

    /// <summary>Shown along the bottom of the radar.</summary>
    public string? ZoneName { get; init; }

    /// <summary>Vana'diel clock as hhmm, or null to let the day run.</summary>
    public int? TimeOfDay { get; init; }

    /// <summary>Writes one frame here and closes. For checking unattended.</summary>
    public string? ScreenshotPath { get; init; }

    /// <summary>Frames to wait before that shot.</summary>
    public int ScreenshotAfterFrames { get; init; }
}

/// <summary>
/// The renderer, loaded into this process rather than run as a second one.
///
/// MogHouse ships as a single application, so the client owns the renderer
/// rather than talking to it. <see cref="Run"/> blocks and owns the window and
/// its event loop, so it wants a thread of its own; everything else here is
/// safe to call from another one while it runs.
/// </summary>
public sealed partial class NativeViewer : IDisposable
{
    private const string LibraryName = "moghouse_interop";

    private IntPtr _handle;
    private bool _disposed;

    static NativeViewer()
    {
        // The native library sits with the renderer build rather than beside
        // the managed assemblies, and it needs Dawn and SDL from that same
        // directory. Resolving it explicitly beats copying four DLLs around
        // and getting one of them stale.
        NativeLibrary.SetDllImportResolver(typeof(NativeViewer).Assembly, Resolve);
    }

    private static IntPtr Resolve(string name, Assembly assembly, DllImportSearchPath? path)
    {
        if (name != LibraryName)
        {
            return IntPtr.Zero;
        }

        foreach (string directory in SearchDirectories())
        {
            string candidate = Path.Combine(directory, OperatingSystem.IsWindows()
                ? $"{LibraryName}.dll"
                : OperatingSystem.IsMacOS() ? $"lib{LibraryName}.dylib" : $"lib{LibraryName}.so");

            if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out IntPtr loaded))
            {
                // The renderer looks for its glyph atlas beside whatever loaded
                // it, and in-process that is a .NET host somewhere else. Only
                // this code knows where the shim actually came from, so it says.
                NativeEnvironment.Set("MOGHOUSE_NATIVE_DIR", directory);
                return loaded;
            }
        }

        // Fall through to the default search, which is right once everything
        // is published into one directory.
        return IntPtr.Zero;
    }

    private static IEnumerable<string> SearchDirectories()
    {
        if (Environment.GetEnvironmentVariable("MOGHOUSE_NATIVE_DIR") is { Length: > 0 } configured)
        {
            yield return configured;
        }

        yield return AppContext.BaseDirectory;

        // A packaged build keeps the runtime out of sight in data\, so the
        // folder a player opens holds an executable and their settings rather
        // than two hundred files they must not touch. The renderer's assets
        // travel with the library - they are looked up beside whatever loaded
        // it - so putting the library here puts the atlas, the interior table
        // and the water here too.
        yield return Path.Combine(AppContext.BaseDirectory, "data");

        // The development layout: the renderer builds into build-renderer at
        // the repository root, which is several levels above bin/Debug/net10.0.
        string? walk = AppContext.BaseDirectory;
        for (int i = 0; i < 6 && walk is not null; i++)
        {
            yield return Path.Combine(walk, "build-renderer", "moghouse_interop");
            walk = Path.GetDirectoryName(walk.TrimEnd(Path.DirectorySeparatorChar));
        }
    }

    public NativeViewer(NativeViewerOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);

        // Passed through the environment rather than the options struct: the
        // renderer already reads its preferences that way, and widening the
        // interop struct is a change worth making deliberately rather than for
        // one float. Set before the create call, which is when it is read.
        NativeEnvironment.Set(
            "MOGHOUSE_BODY_DISTANCE",
            MogHouseSettings.Current.BodyDrawDistance > 0.0f
                ? MogHouseSettings.Current.BodyDrawDistance.ToString(CultureInfo.InvariantCulture)
                : null);

        // Every string is copied on the native side during the create call, so
        // these can be freed the moment it returns.
        IntPtr zone = Utf8(options.ZonePath);
        IntPtr keys = Utf8(options.KeyTablePath);
        IntPtr keys2 = Utf8(options.KeyTable2Path);
        IntPtr look = Utf8(options.Look);
        IntPtr at = Utf8(options.CharacterAt);
        IntPtr facing = Utf8(options.CharacterFacing);
        IntPtr shot = Utf8(options.ScreenshotPath);
        IntPtr zoneName = Utf8(options.ZoneName);
        IntPtr playerName = Utf8(options.PlayerName);

        try
        {
            Options native = new()
            {
                ZonePath = zone,
                KeyTablePath = keys,
                KeyTable2Path = keys2,
                Look = look,
                ServerClock = options.ServerClock,
                CharacterAt = at,
                CharacterFacing = facing,
                ZoneName = zoneName,
                PlayerName = playerName,
                TimeOfDay = options.TimeOfDay ?? -1,
                ScreenshotPath = shot,
                ScreenshotAfterFrames = options.ScreenshotAfterFrames,
            };

            _handle = mh_viewer_create(in native);
            if (_handle == IntPtr.Zero)
            {
                throw new InvalidOperationException("the renderer refused to start");
            }
        }
        finally
        {
            foreach (IntPtr held in new[] { zone, keys, keys2, look, at, facing, shot, zoneName })
            {
                if (held != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(held);
                }
            }
        }
    }

    /// <summary>Opens the window and runs until it closes. Blocking.</summary>
    public int Run()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return mh_viewer_run(_handle);
    }

    /// <summary>Replaces what the radar shows. Safe to call while Run is going.</summary>
    public void SetEntities(ReadOnlySpan<NativeRadarEntity> entities)
    {
        if (_disposed)
        {
            return;
        }
        mh_viewer_set_entities(_handle, entities, entities.Length);
    }

    /// <summary>
    /// Replaces the bags. Sizes are the server's, one per container, and are
    /// what decides how many slots get drawn.
    /// </summary>
    public void SetInventory(ReadOnlySpan<NativeInventorySlot> slots, ReadOnlySpan<ushort> sizes)
    {
        if (_disposed)
        {
            return;
        }
        mh_viewer_set_inventory(_handle, slots, slots.Length, sizes, sizes.Length);
    }

    /// <summary>Whether the character is kneeling, as a logout makes it.</summary>
    public void SetResting(bool resting)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_resting(_handle, resting ? 1 : 0);
        }
    }

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_resting(IntPtr viewer, int resting);

    /// <summary>Whether the character is sitting.</summary>
    public void SetSitting(bool sitting)
    {
        if (!_disposed)
        {
            mh_viewer_set_sitting(_handle, sitting ? 1 : 0);
        }
    }

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_sitting(IntPtr viewer, int sitting);

    /// <summary>Whether the weapon is drawn - the engaged battle stance.</summary>
    public void SetDrawn(bool drawn)
    {
        if (!_disposed)
        {
            mh_viewer_set_drawn(_handle, drawn ? 1 : 0);
        }
    }

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_drawn(IntPtr viewer, int drawn);

    /// <summary>The entity the player currently has selected, or 0.</summary>
    public uint CurrentTarget() =>
        _disposed || _handle == IntPtr.Zero ? 0 : mh_viewer_current_target(_handle);

    [LibraryImport(LibraryName)]
    private static partial uint mh_viewer_current_target(IntPtr viewer);

    /// <summary>The character's job, level and stats, for the equipment screen.</summary>
    public void SetCharacterStats(NativeCharacterStats stats)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_character_stats(_handle, in stats);
        }
    }

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_character_stats(IntPtr viewer,
        in NativeCharacterStats stats);

    /// <summary>
    /// Where each of the sixteen equipment slots is wearing something from.
    /// </summary>
    public void SetEquipment(ReadOnlySpan<byte> containers, ReadOnlySpan<byte> slots)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_equipment(_handle, containers, slots, slots.Length);
        }
    }

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_equipment(IntPtr viewer, ReadOnlySpan<byte> containers,
        ReadOnlySpan<byte> slots, int count);

    /// <summary>
    /// Tells the renderer what an item is called and what it looks like.
    ///
    /// Once per distinct item, not once per slot. The pixels are copied out
    /// before this returns, so the caller can reuse the buffer.
    /// </summary>
    public void PushItem(ushort itemId, string name, string description, ushort type, ushort level,
                         uint slots, ReadOnlySpan<byte> rgba, int width, int height)
    {
        if (_disposed || rgba.IsEmpty)
        {
            return;
        }
        mh_viewer_push_item(_handle, itemId, name, description, type, level, slots, rgba, width, height);
    }

    /// <summary>Replaces the zone lines drawn in the world.</summary>
    public void SetZoneLines(ReadOnlySpan<NativeZoneLine> lines)
    {
        if (_disposed)
        {
            return;
        }
        mh_viewer_set_zone_lines(_handle, lines, lines.Length);
    }

    /// <summary>
    /// Shows one line in the renderer's chat panel.
    /// </summary>
    public void PushChat(string line, int tone = 0)
    {
        if (_disposed || string.IsNullOrEmpty(line))
        {
            return;
        }
        mh_viewer_push_chat(_handle, line, tone);
    }

    /// <summary>
    /// Where the character has walked to, or false before the first frame.
    /// Y is up; FFXI's own vertical is the negation.
    /// </summary>
    public bool TryGetCharacter(out float x, out float y, out float z, out float heading)
    {
        x = y = z = heading = 0;
        if (_disposed)
        {
            return false;
        }
        return mh_viewer_get_character(_handle, out x, out y, out z, out heading) != 0;
    }

    /// <summary>
    /// Whether the player asked to jump since this was last called. Consumes
    /// it, so each jump is reported once.
    /// </summary>
    public bool TakeJump() => !_disposed && _handle != IntPtr.Zero && mh_viewer_take_jump(_handle) != 0;

    /// <summary>Who the player asked to talk to, or 0 if nobody.</summary>
    public uint TakeTalk() => _disposed || _handle == IntPtr.Zero ? 0 : mh_viewer_take_talk(_handle);

    /// <summary>Whether the window is reading a zone right now.</summary>
    public bool IsLoading =>
        !_disposed && _handle != IntPtr.Zero && mh_viewer_is_loading(_handle) != 0;

    /// <summary>Draws a different zone in the window that is already open.</summary>
    public void LoadZone(string datPath, string zoneName, float x, float y, float z, float heading)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_load_zone(_handle, datPath, zoneName, x, y, z, heading);
        }
    }

    /// <summary>Hands the saved preferences to the world window.</summary>
    public void SetSettings(float musicVolume, float soundVolume, float uiScale, bool radarTurns)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_settings(_handle, musicVolume, soundVolume, uiScale, radarTurns ? 1 : 0);
        }
    }

    /// <summary>
    /// What the player changed in the world window since last asked, or null.
    /// </summary>
    public (float MusicVolume, float SoundVolume, float UiScale, bool RadarTurns)? TakeSettings()
    {
        if (_disposed || _handle == IntPtr.Zero ||
            mh_viewer_take_settings(_handle, out float volume, out float sound, out float scale,
                                    out int turns) == 0)
        {
            return null;
        }
        return (volume, sound, scale, turns != 0);
    }

    /// <summary>
    /// The music file the zone wants, or null for silence.
    /// </summary>
    public void SetMusic(string? path)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_music(_handle, path);
        }
    }

    /// <summary>
    /// Which link the player clicked in the world window, or None.
    /// </summary>
    public NativeLink TakeLink() =>
        _disposed || _handle == IntPtr.Zero ? NativeLink.None : (NativeLink)mh_viewer_take_link(_handle);

    /// <summary>
    /// The player's own HP, MP and TP, drawn as a panel in the world window.
    ///
    /// Without it the only sign of being dead is being unable to move, which
    /// looks exactly like a client that has stopped responding.
    /// </summary>
    public void SetVitals(uint hp, uint mp, uint tp, byte hpPercent, byte mpPercent)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_vitals(_handle, hp, mp, tp, hpPercent, mpPercent);
        }
    }

    /// <summary>
    /// Whether the character is down, and whether a raise has been offered.
    /// The renderer draws its death box from these two and nothing else.
    /// </summary>
    public void SetDeath(bool dead, bool raiseOffered)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_death(_handle, dead ? 1 : 0, raiseOffered ? 1 : 0);
        }
    }

    /// <summary>
    /// What the player pressed in that box. Consumed, so each press is acted
    /// on once.
    /// </summary>
    public NativeDeathChoice TakeDeathChoice() =>
        _disposed || _handle == IntPtr.Zero
            ? NativeDeathChoice.None
            : (NativeDeathChoice)mh_viewer_take_death_choice(_handle);

    /// <summary>
    /// The next line the player typed, or null if they have not pressed return
    /// since this was last called.
    /// </summary>
    public unsafe string? TakeChat()
    {
        if (_disposed || _handle == IntPtr.Zero)
        {
            return null;
        }

        // Chat is capped well under this on the wire; the buffer only has to
        // outlast one line.
        byte* buffer = stackalloc byte[256];
        if (mh_viewer_take_chat(_handle, buffer, 256) == 0)
        {
            return null;
        }

        return Marshal.PtrToStringUTF8((IntPtr)buffer);
    }

    /// <summary>
    /// Tells the renderer who the player is, once that is known.
    ///
    /// <para>
    /// The window opens before the sign-in screen is drawn in it, so neither of
    /// these is known when the viewer is made. Either may be left null to keep
    /// what is there.
    /// </para>
    ///
    /// <para>
    /// <paramref name="look"/> is applied at the next zone load rather than at
    /// once - building a character reads a skeleton, its motions and a file per
    /// slot, and before a zone is up there is nowhere for a body to stand.
    /// </para>
    /// </summary>
    public void SetPlayer(string? name = null, string? look = null)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_player(_handle, name, look);
        }
    }

    /// <summary>
    /// Hands the renderer the server's clock, in Earth seconds since the
    /// Vana'diel epoch, so the sky agrees with everyone else's.
    ///
    /// Set at zone-in: the window is open through the whole sign-in, and the
    /// renderer counts from when this arrives rather than from startup.
    /// </summary>
    public void SetClock(uint serverClock)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_clock(_handle, serverClock);
        }
    }

    /// <summary>
    /// The weather the zone is under, as the server numbers it. Which of the
    /// zone's four skies that calls for is the renderer's decision.
    /// </summary>
    public void SetWeather(int weather)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_weather(_handle, weather);
        }
    }

    /// <summary>Writes the next frame to a BMP without stopping the world.</summary>
    public void Capture(string path)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_capture(_handle, path);
        }
    }

    /// <summary>
    /// Puts the character aboard the monorail, or takes them off.
    ///
    /// While aboard the train carries them - walking and gravity are ignored -
    /// so the client should keep reporting the position it reads back rather
    /// than the one it last sent.
    /// </summary>
    public void SetRiding(bool aboard)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_riding(_handle, aboard ? 1 : 0);
        }
    }

    /// <summary>
    /// Whether to draw the game's own furniture - the radar and the chat panel.
    /// Off while the client is showing its own screens.
    /// </summary>
    public void ShowHud(bool on)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_hud(_handle, on ? 1 : 0);
        }
    }

    /// <summary>
    /// Says that the entities on this viewer are a character-select line-up:
    /// stand them in a row on the floor and look at them.
    ///
    /// <para>
    /// The roster goes across as ordinary entities, with the names and looks
    /// the client knows. Where they stand needs the zone's collision and the
    /// camera, so the renderer works that out. Picking one is the ordinary
    /// entity click, so the answer arrives through <see cref="TakeTalk"/> as
    /// the id given to that entity.
    /// </para>
    /// </summary>
    public void SetLineup(bool on)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_lineup(_handle, on ? 1 : 0);
        }
    }

    /// <summary>
    /// Puts a form up in the renderer, replacing whatever was showing.
    ///
    /// <para>
    /// The renderer knows nothing about what a login or a character list is.
    /// It draws rows and reports a press; every decision about what the rows
    /// mean stays here, on the side that already holds the session and the
    /// protocol.
    /// </para>
    ///
    /// <para>
    /// Safe from any thread: the rows are copied into the renderer's own
    /// storage before this returns, so nothing here has to outlive the call.
    /// </para>
    /// </summary>
    public unsafe void ShowForm(string title, string message, IReadOnlyList<NativeFormRow> rows)
    {
        if (_disposed || _handle == IntPtr.Zero)
        {
            return;
        }

        // stackalloc rather than an array: a form is a handful of rows, and
        // this way there is nothing for the collector to pin while the native
        // side reads it.
        NativeFormRowData* data = stackalloc NativeFormRowData[rows.Count];
        for (int i = 0; i < rows.Count; i++)
        {
            NativeFormRow row = rows[i];
            data[i].Kind = (int)row.Kind;
            data[i].Enabled = row.Enabled ? 1 : 0;
            WriteFixed(data[i].Text, 64, row.Text);
            WriteFixed(data[i].Value, 128, row.Value);
        }

        mh_viewer_set_form(_handle, title, message, data, rows.Count);
    }

    /// <summary>Takes the form back down, showing whatever is behind it.</summary>
    public unsafe void HideForm()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_form(_handle, string.Empty, string.Empty, null, 0);
        }
    }

    /// <summary>
    /// Whether forms shown from now on stand against the left edge with the
    /// world left bright beside them, for a screen about something standing
    /// in the world. Off, they sit in the middle over a dimmed world.
    /// </summary>
    public void SetFormAside(bool aside)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_set_form_aside(_handle, aside ? 1 : 0);
        }
    }

    /// <summary>
    /// What the player pressed, or null while they are still filling the form
    /// in. Polled the same way <see cref="TakeChat"/> is, and answers once.
    /// </summary>
    public unsafe NativeFormResult? TakeFormResult()
    {
        if (_disposed || _handle == IntPtr.Zero)
        {
            return null;
        }

        // A single value is capped at 128 by the struct that carried it in, so
        // this holds sixty-odd rows end to end - more than a character list,
        // which is the longest form there is.
        const int capacity = 8192;
        byte* buffer = stackalloc byte[capacity];

        int count = mh_viewer_take_form_result(_handle, out int button, buffer, capacity);
        if (count == 0)
        {
            return null;
        }

        return new NativeFormResult(button, SplitValues(new ReadOnlySpan<byte>(buffer, capacity), count));
    }

    /// <summary>
    /// Unpacks what the renderer wrote: <paramref name="count"/> NUL
    /// terminated values, in the order the rows were given.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Counted rather than terminated. Label and button rows come back empty,
    /// an empty value is a lone NUL, and so a trailing marker would be
    /// indistinguishable from the first caption in the form - the list would
    /// end before the fields anyone actually typed into.
    /// </para>
    /// <para>
    /// Split out from the call above so it can be tested without a viewer, and
    /// so the pointer arithmetic lives in one place.
    /// </para>
    /// </remarks>
    internal static IReadOnlyList<string> SplitValues(ReadOnlySpan<byte> packed, int count)
    {
        var values = new List<string>(Math.Max(count, 0));

        for (int i = 0; i < count && !packed.IsEmpty; i++)
        {
            int end = packed.IndexOf((byte)0);
            if (end < 0)
            {
                // No terminator left, so the buffer was filled to its very end.
                // Take what is there rather than dropping it.
                values.Add(System.Text.Encoding.UTF8.GetString(packed));
                break;
            }

            values.Add(System.Text.Encoding.UTF8.GetString(packed[..end]));
            packed = packed[(end + 1)..];
        }

        return values;
    }

    /// <summary>
    /// Writes a string into one of the fixed-width arrays a form row carries,
    /// truncated to fit and always NUL terminated.
    /// </summary>
    /// <remarks>
    /// Cut to ASCII for the same reason <see cref="NativeRadarEntity.SetName"/>
    /// is: the renderer's font has no accents and turns what it does not know
    /// into a space, so it is better to make that substitution here, in one
    /// place, than to send bytes that quietly come out wrong.
    /// </remarks>
    internal static unsafe void WriteFixed(byte* target, int capacity, string? value)
    {
        int written = 0;
        if (!string.IsNullOrEmpty(value))
        {
            foreach (char c in value)
            {
                if (written >= capacity - 1)
                {
                    break;
                }
                target[written++] = c < 128 ? (byte)c : (byte)' ';
            }
        }
        target[written] = 0;
    }

    /// <summary>
    /// Puts the character somewhere, because the server said so. Y is up here;
    /// the caller converts from the protocol's frame.
    /// </summary>
    public void PlaceCharacter(float x, float y, float z, float heading)
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            mh_viewer_place_character(_handle, x, y, z, heading);
        }
    }

    /// <summary>Asks the viewer to close. Run returns shortly afterwards.</summary>
    public void Stop()
    {
        if (!_disposed)
        {
            mh_viewer_stop(_handle);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;

        if (_handle != IntPtr.Zero)
        {
            mh_viewer_destroy(_handle);
            _handle = IntPtr.Zero;
        }
    }

    private static IntPtr Utf8(string? text) => text is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(text);

    [StructLayout(LayoutKind.Sequential)]
    private struct Options
    {
        public IntPtr ZonePath;
        public IntPtr KeyTablePath;
        public IntPtr KeyTable2Path;
        public IntPtr Look;
        public uint ServerClock;
        public IntPtr CharacterAt;
        public IntPtr CharacterFacing;
        public IntPtr ZoneName;
        public IntPtr PlayerName;
        public int TimeOfDay;
        public IntPtr ScreenshotPath;
        public int ScreenshotAfterFrames;
    }

    [LibraryImport(LibraryName)]
    private static partial IntPtr mh_viewer_create(in Options options);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_run(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_entities(IntPtr viewer, ReadOnlySpan<NativeRadarEntity> entities, int count);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_zone_lines(IntPtr viewer, ReadOnlySpan<NativeZoneLine> lines, int count);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_inventory(IntPtr viewer,
        ReadOnlySpan<NativeInventorySlot> slots, int count, ReadOnlySpan<ushort> sizes, int sizeCount);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_push_item(IntPtr viewer, ushort itemId, string name,
        string description, ushort type, ushort level, uint slots, ReadOnlySpan<byte> rgba,
        int width, int height);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_push_chat(IntPtr viewer, string line, int tone);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_get_character(IntPtr viewer, out float x, out float y, out float z,
                                                       out float heading);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_take_jump(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static partial uint mh_viewer_take_talk(IntPtr viewer);


    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_death(IntPtr viewer, int dead, int raiseOffered);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_vitals(IntPtr viewer, uint hp, uint mp, uint tp,
                                                     byte hpPercent, byte mpPercent);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_take_link(IntPtr viewer);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_set_music(IntPtr viewer, string? path);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_settings(IntPtr viewer, float musicVolume, float soundVolume,
                                                       float uiScale, int radarTurns);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_load_zone(IntPtr viewer, string datPath, string zoneName,
                                                    float x, float y, float z, float heading);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_is_loading(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_take_settings(IntPtr viewer, out float musicVolume,
                                                       out float soundVolume, out float uiScale,
                                                       out int radarTurns);

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_take_death_choice(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static unsafe partial int mh_viewer_take_chat(IntPtr viewer, byte* buffer, int capacity);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_riding(IntPtr viewer, int aboard);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_hud(IntPtr viewer, int on);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_lineup(IntPtr viewer, int on);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_clock(IntPtr viewer, uint serverClock);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_weather(IntPtr viewer, int weather);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_capture(IntPtr viewer, string path);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_viewer_set_player(IntPtr viewer, string? name, string? look);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static unsafe partial void mh_viewer_set_form(IntPtr viewer, string title, string message,
                                                          NativeFormRowData* rows, int count);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_set_form_aside(IntPtr viewer, int aside);

    [LibraryImport(LibraryName)]
    private static unsafe partial int mh_viewer_take_form_result(IntPtr viewer, out int button,
                                                                 byte* values, int capacity);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static unsafe partial int mh_pick_folder(string? defaultLocation, byte* buffer, int capacity);

    /// <summary>
    /// Asks the player for a folder, using the platform's own chooser.
    /// Returns null when they cancel or the chooser cannot be opened.
    ///
    /// <para>
    /// Needs no viewer, because the one thing it is for happens before there
    /// is one: with no game files there are no DATs, no glyph atlas, and
    /// nothing to draw a screen with. This is the only prompt that works on a
    /// first run, which is why it is a native chooser rather than a form.
    /// </para>
    ///
    /// <para>
    /// <b>Must be called on the main thread</b>, and it blocks until answered.
    /// macOS opens no window of any kind off the main thread.
    /// </para>
    /// </summary>
    /// <summary>
    /// A plain message box, shown before anything can be drawn.
    ///
    /// Blocks until it is dismissed. Safe before the renderer exists, which is
    /// the whole point of it.
    /// </summary>
    public static void ShowMessage(string title, string body)
    {
        mh_show_message(title, body);
    }

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void mh_show_message(string title, string body);

    /// <summary>
    /// What the player asked to do with one inventory slot, or null.
    ///
    /// Kind 1 equips and 2 drops. Everything else is the server's numbering
    /// and goes straight into a packet.
    /// </summary>
    public (int Kind, byte Container, byte Slot, byte EquipSlot, uint Count)? TakeInventoryAction()
    {
        if (_disposed || _handle == IntPtr.Zero ||
            mh_viewer_take_inventory_action(_handle, out int kind, out int container, out int slot,
                                            out int equipSlot, out uint count) == 0)
        {
            return null;
        }
        return (kind, (byte)container, (byte)slot, (byte)equipSlot, count);
    }

    [LibraryImport(LibraryName)]
    private static partial int mh_viewer_take_inventory_action(IntPtr viewer, out int kind,
        out int container, out int slot, out int equipSlot, out uint count);

    public static unsafe string? PickFolder(string? defaultLocation = null)
    {
        // No explicit setup call: the static constructor registers the library
        // resolver, and touching this method is what runs it.

        // Long enough for any real path; the native side refuses rather than
        // truncating if somebody manages a longer one.
        const int capacity = 4096;
        byte* buffer = stackalloc byte[capacity];

        int result = mh_pick_folder(defaultLocation, buffer, capacity);
        return result == 1 ? Marshal.PtrToStringUTF8((IntPtr)buffer) : null;
    }

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_place_character(IntPtr viewer, float x, float y, float z, float heading);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_stop(IntPtr viewer);

    [LibraryImport(LibraryName)]
    private static partial void mh_viewer_destroy(IntPtr viewer);
}
