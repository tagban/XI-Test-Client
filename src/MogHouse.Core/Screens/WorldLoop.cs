using System.Runtime.InteropServices;
using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Everything that has to happen while somebody is in the world.
///
/// <para>
/// The renderer owns movement and reports where the character ended up; the
/// session owns everything the server has to be told about it. This is the
/// wiring between the two: what the player typed, what they jumped over, what
/// killed them, which zone they walked into, and what the server said back.
/// </para>
///
/// <para>
/// It lived in a view model, which is the wrong place for it - none of it draws
/// anything. Anything that did draw stayed behind, and what is here talks only
/// to the session and to the renderer.
/// </para>
/// </summary>
/// <summary>
/// Why a character stopped being in the world.
///
/// The three want different things afterwards: a closed window ends the
/// client, /shutdown ends it deliberately, and /logout goes back to the
/// character list with the account still signed in - which is what the game
/// does and what the client used to get wrong by ending everything.
/// </summary>
public enum WorldExit
{
    WindowClosed,
    LoggedOut,
    ShutDown,
}

public sealed class WorldLoop
{
    private readonly FfxiGameSession _session;
    private readonly LiveRadar _world;
    private readonly FfxiEntityTracker _tracker;
    private readonly string _who;
    private readonly TextWriter _say;

    private uint _openZone;
    private bool _leaving;
    private bool _drawn;

    /// <summary>Why the world was left, once it has been.</summary>
    public WorldExit Exit { get; private set; } = WorldExit.WindowClosed;

    /// <summary>
    /// Set from the session's thread when the bags change, cleared on the one
    /// that draws. The tracker raises its event as each packet lands, and the
    /// server sends a hundred of them at once on zoning in - so this coalesces
    /// the burst into a single push instead of a hundred.
    /// </summary>
    private volatile bool _inventoryDirty;

    /// <summary>Set when the server sends new job or stat numbers.</summary>
    private volatile bool _statsDirty;

    /// <summary>
    /// Items already described to the renderer. It keeps the icon on an atlas
    /// and the name beside it, so sending either twice is wasted work.
    /// </summary>
    private readonly HashSet<ushort> _itemsSent = [];

    /// <summary>
    /// Names and icons, or an empty table where the retail files are not
    /// installed. Without them the bags still work - a slot knows what it
    /// holds - they just have nothing to draw or to call it.
    /// </summary>
    private FfxiItemTable? _items;

    /// <param name="tracker">
    /// What the renderer draws entities from. Handed in rather than made here
    /// because whoever zoned in has already been feeding it.
    /// </param>
    /// <param name="who">
    /// The character's own name, for echoing back what they say. The session
    /// does not carry it - it knows the character it selected, not what to put
    /// in front of a line of chat.
    /// </param>
    public WorldLoop(FfxiGameSession session, LiveRadar world, FfxiEntityTracker tracker,
                     uint openZone, string who, TextWriter? say = null)
    {
        _session = session;
        _world = world;
        _tracker = tracker;
        _openZone = openZone;
        _who = who;
        _say = say ?? Console.Out;
    }

    /// <summary>
    /// Wires the session's events to the world and runs until the window
    /// closes. Blocking, and not on the thread drawing the world.
    /// </summary>
    public WorldExit Run()
    {
        Attach();

        // What the last session left behind, before anything else: the volume
        // has to be right before the first note rather than after it.
        _world.ShowSettings(MogHouseSettings.Current);

        // A window opened over a corpse. The health packet arrives in the burst
        // the server sends at zone-in, long before anyone was watching, so
        // somebody who logs in dead gets no event to tell them there is a way
        // up.
        _world.ShowDeath(_session.IsDead, _session.HasRaiseOffer);
        _world.ShowZoneLines(_session.ZoneLines);
        _world.ShowWeather(_session.CurrentWeather);

        // Whatever the server already sent, before anyone was listening. The
        // inventory arrives in the zone-in burst like the health packet does.
        _inventoryDirty = true;
        _statsDirty = true;
        PlayMusic(_session.CurrentTrack);
        Welcome();

        try
        {
            Pump();
        }
        finally
        {
            Detach();
        }

        return Exit;
    }

    private void Attach()
    {
        _session.Inventory.Changed += OnInventoryChanged;
        _session.JobChanged += OnJobChanged;
        _session.LookChanged += OnLookChanged;
        _session.ChatReceived += OnChat;
        _session.NpcChoiceOffered += OnNpcChoice;
        _session.ShopOpened += OnShopOpened;
        _session.EntitiesChanged += OnEntities;
        _session.ZoneChanged += OnZoneChanged;
        _session.MusicChanged += PlayMusic;
        _session.WeatherChanged += OnWeatherChanged;
        _session.MovedByServer += OnMovedByServer;
        _session.DeathChanged += OnDeathChanged;
        _session.RaiseOfferChanged += OnRaiseOfferChanged;
    }

    private void Detach()
    {
        _session.Inventory.Changed -= OnInventoryChanged;
        _session.JobChanged -= OnJobChanged;
        _session.LookChanged -= OnLookChanged;
        _items?.Dispose();
        _items = null;
        _session.ChatReceived -= OnChat;
        _session.NpcChoiceOffered -= OnNpcChoice;
        _session.ShopOpened -= OnShopOpened;
        _session.EntitiesChanged -= OnEntities;
        _session.ZoneChanged -= OnZoneChanged;
        _session.MusicChanged -= PlayMusic;
        _session.WeatherChanged -= OnWeatherChanged;
        _session.MovedByServer -= OnMovedByServer;
        _session.DeathChanged -= OnDeathChanged;
        _session.RaiseOfferChanged -= OnRaiseOfferChanged;
    }

    /// <summary>The loop proper, until the window goes.</summary>
    /// <summary>
    /// A word to testers as they arrive, because a reporting tool nobody knows
    /// about collects nothing. Shown as a popup rather than said into chat: the
    /// first thing on screen at zone-in is a burst of the server's own
    /// messages, and a line about /bug would scroll past behind them.
    /// </summary>
    /// <summary>
    /// The bug-reporting notice, said rather than asked.
    ///
    /// It was a box with a button, which stopped the world until it was
    /// dismissed - and being a form, it also swallowed the space bar, so a
    /// character could not jump until it had been clicked away. Neither is
    /// what a reminder is worth.
    ///
    /// A line of chat is how the server itself says this sort of thing, it
    /// stays in the log to be scrolled back to, and it blocks nothing.
    /// </summary>
    private void Welcome() =>
        _world.Say("", "If you find a bug, face it, and type /bug <insert context here> and hit enter.",
                   FfxiChatMessageType.System1);

    /// <summary>
    /// When the world stops, once the server has had its countdown.
    ///
    /// Null until a logout is asked for. The client used to leave the instant
    /// it sent the packet, which is why nothing was ever seen of the thirty
    /// seconds the server spends counting: it applies a Leavegame effect
    /// rather than disconnecting, and we were gone before any of it happened.
    /// </summary>
    private DateTimeOffset? _leaveAt;

    /// The logout, once asked for. Started and not waited on: awaiting it
    /// stops this loop dead, and a loop that is not running cannot count
    /// anything down or draw anybody kneeling.
    private Task? _logout;

    /// The last number said out loud, so each one is said once.
    private int _lastCount;

    private string _leaveWhat = "logged out";

    /// <summary>
    /// Starts leaving: says so, kneels, and sends the packet without waiting
    /// for it.
    ///
    /// Thirty seconds, counted at thirty, twenty and ten the way the game
    /// does. It used to await the logout here, which stopped the world for the
    /// whole wait - so nothing could be counted, nobody knelt, and the only
    /// evidence anything had happened was the window vanishing later.
    /// </summary>
    private void BeginLeaving(string what, FfxiLogoutKind kind)
    {
        if (_logout is not null)
        {
            return;   // already going
        }

        _leaveWhat = what;
        Exit = kind == FfxiLogoutKind.Logout ? WorldExit.LoggedOut : WorldExit.ShutDown;
        _leaveAt = DateTimeOffset.UtcNow.AddSeconds(30);
        _lastCount = 30;
        _say.WriteLine($"leaving: {what}, at {_leaveAt:HH:mm:ss}");
        _world.Say("", $"You will be {what} in 30 seconds.", FfxiChatMessageType.System1);

        // Down on one knee for the wait, which is what the game does. res0 is
        // the clip /heal plays and every race ships it.
        _world.ShowResting(true);
        _logout = _session.LogoutAsync(kind);
    }

    /// <summary>
    /// The countdown: a line at thirty, twenty and ten, and then gone.
    ///
    /// Timed here rather than by the server. The server has been told and runs
    /// its own clock; this is what the player sees, and the two only have to
    /// agree about the end.
    /// </summary>
    private void CountDown()
    {
        if (_leaveAt is not { } leaveAt)
        {
            return;
        }

        int left = (int)Math.Ceiling((leaveAt - DateTimeOffset.UtcNow).TotalSeconds);
        foreach (int mark in new[] { 20, 10 })
        {
            if (left <= mark && _lastCount > mark)
            {
                _lastCount = mark;
                _world.Say("", $"You will be {_leaveWhat} in {mark} seconds.",
                           FfxiChatMessageType.System1);
            }
        }

        if (left <= 0)
        {
            _leaving = true;
        }
    }

    private void OnInventoryChanged() => _inventoryDirty = true;

    private void OnJobChanged() => _statsDirty = true;

    /// <summary>
    /// What we now look like, gear included. Pushed straight through: the
    /// renderer rebuilds the model, and doing that on the thread that drew the
    /// last frame is what it already expects.
    /// </summary>
    private void OnLookChanged(string look)
    {
        // Logged, because what a naked character is supposed to look like is
        // still an open question: the fallback uses model 1 for every slot,
        // which is a whole set of clothes and reads as armour on a Tarutaru
        // who is wearing nothing. Knowing what the server actually sends for
        // an empty slot is what settles it.
        _say.WriteLine($"look: {look}");
        _world.SetPlayer(_who, look);
    }

    /// <summary>
    /// Sends the job, level and stats to the renderer.
    ///
    /// The sub job's level is not a field of its own - the packet carries a
    /// level for every job at once, so the sub's is looked up by its number.
    /// </summary>
    private unsafe void ShowStats()
    {
        if (_session.Job is not { } job)
        {
            return;
        }

        var stats = new NativeCharacterStats
        {
            MainJob = job.MainJob,
            SubJob = job.SubJob,
            MainLevel = job.MainJobLevel,
            SubLevel = job.SubJob < job.JobLevels.Count ? job.JobLevels[job.SubJob] : (byte)0,
            MaxHp = job.MaxHp,
            MaxMp = job.MaxMp,
        };

        for (int i = 0; i < FfxiJobInfo.StatCount; i++)
        {
            stats.BaseStats[i] = job.BaseStats[i];
            stats.StatModifiers[i] = job.StatModifiers[i];
        }

        _world.ShowCharacterStats(stats);
    }

    /// <summary>
    /// Sends the bags to the renderer, and the name and icon of anything in
    /// them it has not been told about yet.
    ///
    /// The sizes come from the server rather than from a constant. A character
    /// starts with thirty inventory slots on some servers and quests for the
    /// rest, so a client that assumed eighty would draw fifty places to put
    /// things that cannot be put anywhere.
    /// </summary>
    private void ShowInventory()
    {
        FfxiInventoryTracker bags = _session.Inventory;

        var slots = new List<NativeInventorySlot>();
        for (int container = 0; container < FfxiContainerSizes.Containers; container++)
        {
            foreach (FfxiInventorySlot held in bags.Contents((FfxiContainer)container))
            {
                slots.Add(new NativeInventorySlot
                {
                    Container = (byte)container,
                    Slot = held.Slot,
                    ItemId = held.ItemId,
                    Count = held.Quantity,
                });
            }
        }

        var sizes = new ushort[FfxiContainerSizes.Containers];
        for (int container = 0; container < sizes.Length; container++)
        {
            sizes[container] = bags.Size((FfxiContainer)container);
        }

        _world.ShowInventory(CollectionsMarshal.AsSpan(slots), sizes);

        // What is worn, as places rather than things: the server names a
        // container and a slot, and what is in it is the answer.
        var wornContainers = new byte[16];
        var wornSlots = new byte[16];
        for (int slot = 0; slot < 16; slot++)
        {
            (FfxiContainer Container, byte Slot)? at = bags.Equipped((FfxiEquipSlot)slot);
            wornContainers[slot] = (byte)(at?.Container ?? FfxiContainer.Inventory);
            wornSlots[slot] = at?.Slot ?? FfxiEquipment.Empty;
        }

        _world.ShowEquipment(wornContainers, wornSlots);

        // Names and icons, once each. Opened on first use rather than in the
        // constructor: a session that never opens its bags never reads a file.
        if (_items is null)
        {
            try
            {
                _items = new FfxiItemTable(new FfxiFileTable(FfxiFileTable.DefaultInstallRoot()));
            }
            catch (Exception)
            {
                _items = FfxiItemTable.Empty;
            }
        }

        foreach (NativeInventorySlot held in slots)
        {
            if (!_itemsSent.Add(held.ItemId))
            {
                continue;
            }

            FfxiItem? item = _items.Item(held.ItemId);
            FfxiItemIcon? icon = _items.Icon(held.ItemId);
            if (item is null || icon is null)
            {
                continue;
            }

            _world.ShowItem(item, icon);
        }
    }

    /// <summary>
    /// Buys whatever was clicked, or closes the shop.
    ///
    /// One at a time: the buy packet carries a quantity, but nothing on screen
    /// asks for one yet, and quietly buying twelve of something because the
    /// button was pressed twice would be worse than buying one.
    /// </summary>
    private void TakeShopChoice()
    {
        if (_world.TakeFormResult() is not { } answered)
        {
            return;
        }

        IReadOnlyList<FfxiShopItem> selling = _selling;
        _selling = [];
        _world.HideForm();

        if (answered.Button < 0 || answered.Button >= selling.Count)
        {
            return;   // Close, or a button that is not an item
        }

        FfxiShopItem bought = selling[answered.Button];
        string name = Items().Item(bought.ItemId)?.Name ?? $"item {bought.ItemId}";
        _world.Say(null, $"Buying {name} for {bought.Price:N0} gil.");
        _ = _session.BuyAsync(bought.ShopIndex);
    }

    private void Pump()
    {
        while (!_world.Closed && !_leaving)
        {
            CountDown();
            TakeNpcAnswer();

            if (_inventoryDirty)
            {
                _inventoryDirty = false;
                ShowInventory();
            }

            if (_statsDirty)
            {
                _statsDirty = false;
                ShowStats();
            }

            // What was clicked in the bags. Nothing is applied locally: the
            // server answers with its own packets, and a client that moved the
            // gear itself would be showing a change that may well be refused.
            while (_world.TakeInventoryAction() is { } asked)
            {
                if (asked.Kind == 1)
                {
                    _session.EquipAsync(asked.Slot, (FfxiEquipSlot)asked.EquipSlot,
                                        (FfxiContainer)asked.Container).GetAwaiter().GetResult();
                }
                else if (asked.Kind == 2)
                {
                    _session.DropAsync((FfxiContainer)asked.Container, asked.Slot)
                            .GetAwaiter().GetResult();
                }
                else if (asked.Kind == 3)
                {
                    _session.RefreshSelfAsync().GetAwaiter().GetResult();
                }
            }

            // Not while a zone is being read. Until it finishes the window is
            // still holding the position it had in the zone being left, and
            // telling the server that is how one zone change becomes several -
            // a position from the old zone can be standing in the line that
            // sent us here.
            if (!_world.IsLoading &&
                _world.Position() is (float x, float vertical, float depth, sbyte direction))
            {
                _session.PlaceAt(x, vertical, depth, direction);
            }

            while (_world.TakeChat() is { Length: > 0 } typed)
            {
                Typed(typed);
            }

            // Somebody clicked on somebody. The renderer decides who was
            // pointed at and hands back their id; this side is the only half
            // with a socket to ask them a question.
            //
            // The click was being collected and dropped: character select read
            // it, the world never did, so clicking an NPC did nothing at all.
            if (_world.TakeTalk() is uint clicked && clicked != 0)
            {
                Talk(clicked);
            }

            // Space: a jump on your feet, a wave lying down. A corpse has no
            // other way to be noticed.
            if (_world.TakeJump())
            {
                Wait(_session.JumpAsync());
            }

            // Whichever button a dead character pressed. The renderer drew the
            // choice; only this half has a socket to say it down.
            switch (_world.TakeDeathChoice())
            {
                case NativeDeathChoice.HomePoint:
                    Wait(_session.ReturnToHomePointAsync());
                    break;

                case NativeDeathChoice.AcceptRaise:
                    Wait(_session.AcceptRaiseAsync());
                    break;
            }

            // Volume and the minimap are changed by keys in the world window,
            // so this side only hears about it afterwards - and writes it out,
            // so the next session starts where this one left off.
            if (_world.TakeSettings() is { } changed)
            {
                MogHouseSettings settings = MogHouseSettings.Current;
                settings.MusicVolume = changed.MusicVolume;
                settings.SoundVolume = changed.SoundVolume;
                settings.UiScale = changed.UiScale;
                settings.RadarTurnsWithPlayer = changed.RadarTurns;
                settings.Save();
            }

            // Somewhere to send a bug from inside the world. The window knows a
            // corner was clicked; opening a browser is this side's job, and
            // works the same on three operating systems.
            switch (_world.TakeLink())
            {
                case NativeLink.Discord:
                    Links.Open(Links.Discord);
                    break;

                case NativeLink.Issues:
                    Links.Open(Links.Issues);
                    break;
            }

            // Pushed rather than raised on change: hit points move constantly,
            // and the panel wants the current number rather than a notification
            // that it moved.
            if (_session.Health is { } vitals)
            {
                _world.ShowVitals(vitals);
            }

            _world.Publish(_tracker);
            Thread.Sleep(50);
        }
    }

    /// <summary>
    /// What a typed line means.
    ///
    /// The client's own commands are answered rather than said: typing /logout
    /// used to be broadcast to the zone as the word "/logout" and do nothing
    /// else, because only the console client ever ran a line past the parser.
    /// </summary>
    private void Typed(string text)
    {
        FfxiClientCommand command = FfxiClientCommands.Parse(text);

        // Logged, because "/logout did nothing" has two very different causes
        // - the line never crossed from the renderer, or it crossed and the
        // command was refused - and they look identical from a chair.
        _say.WriteLine($"typed: {text}  -> {command.Kind}");
        switch (command.Kind)
        {
            case FfxiClientCommandKind.Logout:
                BeginLeaving("logged out", FfxiLogoutKind.Logout);
                return;

            case FfxiClientCommandKind.Shutdown:
                BeginLeaving("shut down", FfxiLogoutKind.Shutdown);
                return;

            case FfxiClientCommandKind.Bug:
                ReportBug(command.Rest);
                return;

            case FfxiClientCommandKind.HomePoint:
                Wait(_session.ReturnToHomePointAsync());
                return;

            // Answered here rather than sent. Sitting is a pose the client
            // holds; the server has no opinion about it and moving ends it,
            // which the renderer decides for itself.
            case FfxiClientCommandKind.Sit:
                _world.ShowSitting(true);
                _world.Say(null, "You sit down.");
                return;

            case FfxiClientCommandKind.Stand:
                _world.ShowSitting(false);
                _world.Say(null, "You stand up.");
                return;

            // A local pose, no server and no target: draw or sheathe the
            // weapon to see the battle stance. /attack is the real engage;
            // this is the quiet version for looking at your gear.
            case FfxiClientCommandKind.Draw:
                _drawn = !_drawn;
                _world.ShowDrawn(_drawn);
                _world.Say(null, _drawn ? "You ready your weapon." : "You put your weapon away.");
                return;

            // Engage the current target - start attacking. The server drives
            // the fight; here we send the action and show the drawn stance.
            case FfxiClientCommandKind.Engage:
            {
                uint targetId = _world.CurrentTarget();
                if (targetId == 0 || _tracker.Find(targetId) is not { } foe)
                {
                    _world.Say(null, "You have no target.");
                    return;
                }

                _drawn = true;
                _world.ShowDrawn(true);
                _world.Say(null, $"You engage {(string.IsNullOrEmpty(foe.Name) ? "the target" : foe.Name)}.");
                Wait(_session.EngageAsync(targetId, foe.ActIndex));
                return;
            }

            case FfxiClientCommandKind.Disengage:
            {
                _drawn = false;
                _world.ShowDrawn(false);
                _world.Say(null, "You disengage.");
                // Disengage targets our own character; the session fills that in.
                if (_world.CurrentTarget() is uint t && t != 0 && _tracker.Find(t) is { } foe)
                {
                    Wait(_session.DisengageAsync(t, foe.ActIndex));
                }
                return;
            }

            // Every channel the real client answers to, and the short forms
            // nobody types the long version of: /s /sh /y /p /l /ls /l2 /u /em.
            case FfxiClientCommandKind.Chat:
                // Echoed in the channel's own colour. Echoing every channel as
                // Say put a linkshell message on screen looking exactly like
                // something shouted at the street, which is the one thing the
                // colour is there to tell you.
                _world.Say(_who, command.Rest, FfxiChatEcho.For(command.Channel));
                Wait(_session.SayAsync(command.Rest, command.Channel));
                return;

            case FfxiClientCommandKind.Tell:
                Wait(_session.TellAsync(command.Recipient, command.Rest));
                // A tell is not echoed back to whoever sent it, so without this
                // the only evidence it went anywhere is the reply.
                _world.Say(">> " + command.Recipient, command.Rest);
                return;

            case FfxiClientCommandKind.Incomplete:
                _world.Say(null, $"/{command.Name} needs more than that.");
                return;

            case FfxiClientCommandKind.Unsupported:
                _world.Say(null, $"/{command.Name} is not something this client does yet.");
                return;
        }

        // Anything starting with '!' is a GM command, which the server routes
        // off the ordinary say path and needs no handling here.
        //
        // Echoed locally either way: the server does not send our own say back
        // to us, so without this talking leaves no trace on screen.
        _world.Say(_who, text);
        Wait(_session.SayAsync(text));
    }

    /// <summary>
    /// Asks whoever was clicked what they have to say.
    ///
    /// The renderer knows only a UniqueNo; the packet wants an ActIndex too,
    /// and the tracker is the one thing holding both. Somebody clicked who we
    /// have never had an update for cannot be addressed, which in practice
    /// means they are out of range or already gone.
    /// </summary>
    private void Talk(uint uniqueNo)
    {
        if (_tracker.Find(uniqueNo) is not { } who)
        {
            _say.WriteLine($"clicked {uniqueNo:X8}, who is not in the tracker");
            return;
        }

        // A tracked entity's name is nullable and plenty of them have none -
        // the server sends NPCs without one and the zone's own name table
        // fills them in later, if at all. Reading it as though it were always
        // there ended the whole session the first time somebody clicked a
        // nameless one, because an exception here unwinds out of the pump and
        // past everything to RunSession's catch.
        string called = string.IsNullOrEmpty(who.Name) ? $"{uniqueNo:X8}" : who.Name;

        try
        {
            _say.WriteLine($"talking to {called}");
            Wait(_session.TalkToAsync(uniqueNo, who.ActIndex));
        }
        catch (Exception failed)
        {
            // Nothing a click can do is worth ending the session over. The
            // world carries on and the log says what happened, which beats a
            // window that stops responding because somebody clicked a rock.
            _say.WriteLine($"could not talk to {called}: {failed}");
        }
    }

    /// <summary>
    /// Writes down what is wrong and where, and sends it on if there is
    /// anywhere to send it.
    ///
    /// <para>
    /// The picture is taken first and waited for briefly: the renderer writes
    /// it on its next frame, and a report of a graphics fault without the
    /// frame it was seen in is most of the way to useless.
    /// </para>
    ///
    /// <para>
    /// Nothing here is allowed to fail loudly. Somebody reporting a bug is
    /// already having a bad time, and a crash while reporting one would be a
    /// poor joke.
    /// </para>
    /// </summary>
    private void ReportBug(string what)
    {
        try
        {
            uint zone = _session.ZoneState?.ZoneNo ?? _openZone;
            string shot = Path.Combine(Path.GetTempPath(),
                                       $"moghouse-bug-{DateTimeOffset.Now:yyyyMMdd-HHmmss}.bmp");
            _world.Capture(shot);

            var now = new FfxiBugReport.Context(
                What: what,
                Who: _who,
                ZoneNo: zone,
                ZoneName: FfxiZoneNames.Label(zone) ?? $"zone {zone}",
                X: _session.PosX,
                Vertical: _session.PosVertical,
                Depth: _session.PosDepth,
                Facing: _session.Facing,
                VanadielHour: _session.VanadielHour,
                GameTime: _session.ZoneState?.GameTime ?? 0,
                Weather: _session.CurrentWeather);

            string? written = FfxiBugReport.Append(now);
            _say.WriteLine($"bug reported: {what}");
            _say.WriteLine($"  written to {written ?? "(nowhere - could not write the file)"}");

            // Give the renderer a few frames to put the picture on disk. It is
            // drawing at sixty a second, so this is a long wait by its
            // standards and an unnoticeable one by anybody else's.
            Thread.Sleep(200);

            // As a PNG. The renderer writes a BMP because that needs no
            // encoder, which is right for a debugging aid and wrong for
            // something to send: a 2560x1440 frame is eleven megabytes
            // uncompressed and Discord will not take it from most accounts.
            // If the conversion fails, send the BMP rather than nothing.
            string? picture = File.Exists(shot) ? (FfxiPng.FromBmp(shot) ?? shot) : null;

            string outcome = FfxiBugReport.SendAsync(now, picture)
                                          .GetAwaiter().GetResult();
            _say.WriteLine($"  {outcome}");
            _world.Say(null, $"Reported: {what} ({outcome}).");
        }
        catch (Exception failed)
        {
            _say.WriteLine($"could not report a bug: {failed}");
            _world.Say(null, "Could not write that report down - it is in the log.");
        }
    }

    private void OnChat(FfxiChatLine line) => _world.Say(line.Sender, line.Text, line.Kind);

    /// <summary>The question an NPC has on screen, or null.</summary>
    private FfxiNpcChoice? _asking;

    /// <summary>What the open shop is selling, in the order its buttons are drawn.</summary>
    private IReadOnlyList<FfxiShopItem> _selling = [];

    /// <summary>
    /// Names and icons for items, opened on first use.
    ///
    /// A session that never opens its bags and never talks to a shopkeeper
    /// never reads the file.
    /// </summary>
    private FfxiItemTable Items()
    {
        if (_items is null)
        {
            try
            {
                _items = new FfxiItemTable(new FfxiFileTable(FfxiFileTable.DefaultInstallRoot()));
            }
            catch (Exception)
            {
                _items = FfxiItemTable.Empty;
            }
        }

        return _items;
    }

    /// <summary>
    /// Puts a shopkeeper's stock on the screen.
    ///
    /// The server sends item ids and prices and no names at all - the name is
    /// the client's own business, out of the item DAT, which is the same place
    /// the bags get theirs. An id with no entry still gets a row: knowing
    /// something is for sale at a price is more use than a gap.
    /// </summary>
    private void OnShopOpened(FfxiShop shop)
    {
        _selling = shop.Items;
        _asking = null;

        var rows = new List<NativeFormRow>();
        foreach (FfxiShopItem selling in _selling)
        {
            string name = Items().Item(selling.ItemId)?.Name ?? $"item {selling.ItemId}";
            rows.Add(NativeFormRow.Button($"{name}  -  {selling.Price:N0} gil"));
        }

        rows.Add(NativeFormRow.Button("Close"));
        _world.ShowForm("Shop", $"{_selling.Count} for sale. One at a time for now.", rows);
    }

    /// <summary>
    /// Puts an NPC's question on the screen with its answers as buttons.
    ///
    /// The line is in the chat log either way - a question you have answered
    /// is still something that was said to you - and this is the copy you can
    /// act on. Only lines that carry choices get a box: making every remark an
    /// NPC passes raise a modal would put a wall in front of the world for
    /// "Good day to you", and retail does not do that either.
    /// </summary>
    private void OnNpcChoice(FfxiNpcChoice choice)
    {
        _asking = choice;

        var rows = new List<NativeFormRow> { NativeFormRow.Label(choice.Text) };
        foreach (string answer in choice.Choices)
        {
            rows.Add(NativeFormRow.Button(answer));
        }

        _world.ShowForm(string.IsNullOrEmpty(choice.Speaker) ? "\u2014" : choice.Speaker, "", rows);
    }

    /// <summary>
    /// Takes the answer, if one has been pressed.
    ///
    /// The answer is not sent anywhere yet. Replying means the event option
    /// packet, and which option value a choice maps to lives in the event
    /// bytecode, which this project has not decoded - see docs/wiki/Events.md.
    /// So the box shows the question, records what you picked, and closes.
    /// Saying so in the log is deliberate: a button that silently does nothing
    /// reads as broken, and a button that says it went nowhere reads as
    /// unfinished, which is what it is.
    /// </summary>
    private void TakeNpcAnswer()
    {
        if (_selling.Count > 0)
        {
            TakeShopChoice();
            return;
        }

        if (_asking is null || _world.TakeFormResult() is not { } answered)
        {
            return;
        }

        FfxiNpcChoice asked = _asking;
        _asking = null;
        _world.HideForm();

        string picked = answered.Button >= 0 && answered.Button < asked.Choices.Count
            ? asked.Choices[answered.Button]
            : "";

        _world.Say(null, picked.Length > 0
            ? $"You chose \"{picked}\" - answering is not wired to the server yet."
            : "Closed without answering.");
    }

    private void OnEntities(IReadOnlyList<FfxiEntityUpdate> entities)
    {
        // The tracker is what the renderer draws from: it holds what an entity
        // looks like, whether the server means it to be seen, and which fields
        // a partial update may change.
        //
        // Anything belonging to another zone is dropped. Zoning clears the
        // tracker, but packets already on their way arrive after it and put
        // the old zone straight back: walking from Southern San d'Oria into
        // Valkurm Dunes brought 76 of San d'Oria's people along, standing in
        // the sand. An entity id carries the zone it belongs to, so it can be
        // asked rather than trusted.
        uint here = _session.ZoneState?.ZoneNo ?? _openZone;
        DateTimeOffset now = DateTimeOffset.UtcNow;
        foreach (FfxiEntityUpdate update in entities)
        {
            if (ZoneOf(update.UniqueNo) != here)
            {
                continue;
            }

            _tracker.Observe(update, now);
        }
    }

    /// <summary>
    /// Which zone an entity id belongs to.
    ///
    /// The server numbers them 0x1000000 | zone &lt;&lt; 12 | index, so the
    /// zone comes back out with a shift - the same scheme the client's own
    /// entity name tables are keyed by.
    /// </summary>
    private static uint ZoneOf(uint uniqueNo) => (uniqueNo >> 12) - 0x1000;

    private void OnDeathChanged(bool dead) => _world.ShowDeath(dead, _session.HasRaiseOffer);

    private void OnRaiseOfferChanged(bool offered) => _world.ShowDeath(_session.IsDead, offered);

    private void OnMovedByServer() =>
        _world.PlaceCharacter(_session.PosX, _session.PosVertical, _session.PosDepth, _session.Facing);

    /// <summary>
    /// The server sends a track number; the file it names has to be found.
    ///
    /// Which sound directory holds it is not fixed - the install spreads them
    /// across sound, sound2 and so on as expansions were added - so this looks
    /// rather than computes, and a track this install does not have goes quiet
    /// rather than failing.
    /// </summary>
    /// <summary>
    /// The weather turning while somebody is standing in it.
    ///
    /// Handed on so the next zone comes up under the right sky. It does not
    /// change the one overhead: a zone's sky is built with the zone, and
    /// swapping it without reloading everything else needs a rebuild path that
    /// does not exist yet.
    /// </summary>
    private void OnWeatherChanged(FfxiWeather weather)
    {
        _say.WriteLine($"the weather turned to {weather}");
        _world.ShowWeather(weather);
    }

    private void PlayMusic(int track)
    {
        string? path = FfxiMusicFile.Resolve(FfxiInstall.Find() ?? "", track);
        if (path is null && track > 0)
        {
            _say.WriteLine($"no file for music track {track}");
        }
        _world.ShowMusic(path);
    }

    /// <summary>
    /// Walking into a zone line, or being sent somewhere.
    ///
    /// The window draws the new zone itself rather than being replaced by one
    /// that does - closing it was what made zoning look like the client
    /// shutting down, because for as long as the new zone took to read there
    /// was nothing on screen at all.
    /// </summary>
    private void OnZoneChanged(uint zone)
    {
        // Target indices are only unique within a zone, so carrying one across
        // would put an old entity's name and kind on whatever now holds that
        // index.
        _tracker.Clear();

        if (zone == _openZone)
        {
            // A teleport inside the zone you are already standing in still
            // arrives as a zone change: !goto and !bring both send you through
            // the zone server even when the destination is where you already
            // are. There is nothing to load, and there is somewhere new to
            // stand - returning without doing anything is what made those two
            // commands look like they did nothing at all.
            _world.PlaceCharacter(_session.PosX, _session.PosVertical, _session.PosDepth, _session.Facing);
            _say.WriteLine($"moved inside zone {zone} to " +
                           $"{_session.PosX:F1} {_session.PosVertical:F1} {_session.PosDepth:F1}");
            return;
        }

        string name = FfxiZoneNames.Label(zone) ?? $"zone {zone}";
        _say.WriteLine($"zoning to {zone} ({name})");

        // Told before the zone is read, because that is when its sky is built.
        _world.ShowWeather(_session.CurrentWeather);

        if (_world.LoadZone((int)zone, name, _session.PosX, _session.PosVertical, _session.PosDepth,
                            _session.Facing))
        {
            _openZone = zone;
            _world.ShowZoneLines(_session.ZoneLines);
            PlayMusic(_session.CurrentTrack);
            return;
        }

        // No DAT for that zone. Saying so beats a window that silently keeps
        // drawing the zone the character has already left.
        _say.WriteLine($"could not read {name}; still showing zone {_openZone}");
        _world.Say(null, $"{name} is not in this installation.");
    }

    /// <summary>
    /// Drives a task to completion on this thread.
    ///
    /// This loop is already on a thread of its own and everything in it is a
    /// step in a sequence, so awaiting would only hand the work to a pool
    /// thread and wait for it anyway.
    /// </summary>
    private static void Wait(Task work) => work.GetAwaiter().GetResult();
}
