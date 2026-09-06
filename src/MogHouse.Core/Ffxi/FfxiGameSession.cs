using System.Net;

namespace MogHouse.Core.Ffxi;

/// <summary>A chat line as the UI wants it: who, what, and which channel.</summary>
public sealed record FfxiChatLine(DateTimeOffset At, FfxiChatMessageType Kind, string Sender, string Text);

/// <summary>
/// Drives a whole session - auth, roster, character select, zone connection,
/// and the position heartbeat - and surfaces the parts a user interface cares
/// about: chat, nearby entities, and where we are.
///
/// This exists so the UI never has to know the protocol. Everything here is
/// already covered by the lower-level clients; the value added is sequencing,
/// keeping the connection alive, and raising events on arrival rather than
/// making a caller poll.
/// </summary>
public sealed class FfxiGameSession : IDisposable
{
    private readonly FfxiHuffman? _codec;
    private FfxiRosterClient? _roster;
    private FfxiZoneClient? _zone;
    private IPEndPoint? _zoneEndpoint;
    private CancellationTokenSource? _holdCts;
    private Task? _holdTask;

    private readonly string? _navMeshDirectory;
    private readonly string? _zoneDataDirectory;

    /// <summary>Zone lines for the current zone, if the data was available.</summary>
    public IReadOnlyList<FfxiZoneLine> ZoneLines { get; private set; } = [];

    /// <param name="navMeshDirectory">
    /// Where the server's `.nav` files live. Optional: without it the character
    /// can still move, but at a fixed height and with nothing stopping it
    /// walking off a ledge.
    /// </param>
    /// <param name="zoneDataDirectory">
    /// The server's `data/zones` directory, which holds each zone's zone lines.
    /// Optional: without it the character can walk over a zone line and nothing
    /// happens, because only the client ever initiates a zone change.
    /// </param>
    public FfxiGameSession(FfxiHuffman? codec = null, string? navMeshDirectory = null,
                           string? zoneDataDirectory = null, FfxiFileTable? files = null)
    {
        _codec = codec;
        _navMeshDirectory = navMeshDirectory;
        _zoneDataDirectory = zoneDataDirectory;
        _files = files;
    }

    // What NPCs say. The server sends line ids, so without the install we
    // can see that an NPC spoke but not what it said.
    private readonly FfxiFileTable? _files;
    /// <summary>An NPC has asked something with a list of answers under it.</summary>
    public event Action<FfxiNpcChoice>? NpcChoiceOffered;

    /// <summary>A shopkeeper has finished describing what is for sale.</summary>
    public event Action<FfxiShop>? ShopOpened;

    /// <summary>What the last shopkeeper we spoke to is selling.</summary>
    public FfxiShop Shop { get; } = new();

    private FfxiDialogueTable _dialogue = FfxiDialogueTable.Empty;
    private FfxiEntityNames _entityNames = FfxiEntityNames.Empty;
    private int _entityNamesZone = -1;
    private uint _dialogueZone = uint.MaxValue;

    private FfxiEventTable _events = FfxiEventTable.Empty;
    private uint _eventZone = uint.MaxValue;

    /// <summary>Our own health, as the server last reported it.</summary>
    public FfxiCharacterHealth? Health { get; private set; }

    /// <summary>
    /// Our bags and what we are wearing, kept up to date from the server's
    /// one-slot-at-a-time updates.
    /// </summary>
    public FfxiInventoryTracker Inventory { get; } = new();

    /// <summary>
    /// Our job, level and stats, as the server last sent them.
    ///
    /// Arrives in the batch the server withholds until the zone-in handshake
    /// finishes, and again whenever the numbers change - which gear does, so
    /// this is what an equipment screen reads its columns from.
    /// </summary>
    public FfxiJobInfo? Job { get; private set; }

    /// <summary>Raised when <see cref="Job"/> changes.</summary>
    public event Action? JobChanged;

    /// <summary>
    /// What our own character looks like, in the seven numbers the character
    /// loader takes, or null before the server has said.
    /// </summary>
    public string? Look { get; private set; }

    /// How many of our own updates arrived describing no appearance.
    private int _looklessSelfUpdates;

    /// <summary>Raised when <see cref="Look"/> changes - which gear does.</summary>
    public event Action<string>? LookChanged;

    /// <summary>
    /// Whether the character is dead. Movement is refused while it is true:
    /// the server will not accept it, and walking a corpse around is the
    /// most obvious way for a client to be lying to the person using it.
    /// </summary>
    public bool IsDead => Health?.IsDead ?? false;

    /// <summary>Raised when the character dies, and again when they get up.</summary>
    public event Action<bool>? DeathChanged;

    /// <summary>
    /// Whether somebody has offered a raise, which is the only condition under
    /// which the server will accept one.
    ///
    /// It arrives as a packet of its own - see <see cref="FfxiRaiseOffer"/> -
    /// because nothing else changes when a raise is cast over a corpse. Held
    /// until the character is on their feet again.
    /// </summary>
    public bool HasRaiseOffer { get; private set; }

    /// <summary>Raised when a raise is offered, and again when it lapses.</summary>
    public event Action<bool>? RaiseOfferChanged;

    private void SetRaiseOffer(bool offered)
    {
        if (HasRaiseOffer == offered)
        {
            return;
        }

        HasRaiseOffer = offered;
        RaiseOfferChanged?.Invoke(offered);
    }

    /// <summary>Raised for every chat message received.</summary>
    public event Action<FfxiChatLine>? ChatReceived;

    /// <summary>Raised after each batch of entity updates, with the current snapshot.</summary>
    public event Action<IReadOnlyList<FfxiEntityUpdate>>? EntitiesChanged;

    /// <summary>Raised with a human-readable note whenever the session's state changes.</summary>
    public event Action<string>? Status;

    public FfxiZoneLoginReply? ZoneState { get; private set; }

    /// <summary>
    /// Where we currently tell the server we are. Movement is horizontal only:
    /// the vertical stays at whatever the server reported on zone-in, because
    /// this client has no terrain height. Other players still see the
    /// character correctly, since the game client draws them on the ground -
    /// but the server persists position, so walking far from the starting
    /// ground level can leave a height in the character record that puts a
    /// real client underground next time it logs in there.
    /// </summary>
    public float PosX { get; private set; }
    public float PosVertical { get; private set; }
    public float PosDepth { get; private set; }
    public sbyte Facing { get; private set; }

    /// <summary>Raised after the position changes, so a UI can redraw.</summary>
    public event Action? Moved;

    /// <summary>
    /// The zone's navmesh, when one was found. With it, movement follows the
    /// ground and refuses to leave walkable surfaces - which is collision, for
    /// free, since walls and ledges are simply absent from the mesh. Without
    /// it, movement is horizontal-only at a fixed height.
    /// </summary>
    public FfxiNavMesh? NavMesh { get; private set; }

    /// <summary>True when the last move was refused because the destination was off the navmesh.</summary>
    public bool LastMoveBlocked { get; private set; }

    /// <summary>The route currently being walked, if any. World-space waypoints.</summary>
    public IReadOnlyList<(float X, float Vertical, float Depth)> CurrentPath { get; private set; } = [];

    private CancellationTokenSource? _walkCts;

    /// <summary>
    /// Walks to a point using the server's own navmesh for routing, so the path
    /// goes around walls rather than into them.
    ///
    /// Cancels any walk already in progress: a second destination means the
    /// first is no longer wanted.
    /// </summary>
    public async Task WalkToAsync(float targetX, float targetDepth)
    {
        if (NavMesh is null)
        {
            Status?.Invoke("No navmesh for this zone - cannot route.");
            return;
        }

        _walkCts?.Cancel();
        _walkCts = new CancellationTokenSource();
        CancellationToken ct = _walkCts.Token;

        if (!NavMesh.TryGetGroundHeight(targetX, PosVertical, targetDepth, out float targetVertical))
        {
            Status?.Invoke("Nothing walkable there.");
            return;
        }

        IReadOnlyList<(float X, float Vertical, float Depth)> route =
            NavMesh.FindPath(PosX, PosVertical, PosDepth, targetX, targetVertical, targetDepth);

        if (route.Count == 0)
        {
            Status?.Invoke("No route to there.");
            return;
        }

        CurrentPath = route;
        Status?.Invoke($"Walking {route.Count} waypoints to ({targetX:F1}, {targetDepth:F1}).");

        try
        {
            // Skip the first waypoint - it's where we already are.
            for (int i = 1; i < route.Count && !ct.IsCancellationRequested; i++)
            {
                (float wx, _, float wz) = route[i];

                // Step toward each corner at a walking pace rather than
                // teleporting: the server tracks movement and a jump would
                // look like a speed hack.
                while (!ct.IsCancellationRequested)
                {
                    float dx = wx - PosX;
                    float dz = wz - PosDepth;
                    float distance = MathF.Sqrt((dx * dx) + (dz * dz));

                    if (distance < 0.5f)
                    {
                        break;
                    }

                    float step = MathF.Min(StepDistance, distance);
                    Move(dx / distance * step, dz / distance * step);

                    await Task.Delay(StepInterval, ct);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // A newer destination replaced this walk; nothing to report.
        }

        CurrentPath = [];
    }

    /// <summary>Stops any walk in progress.</summary>
    public void StopWalking()
    {
        _walkCts?.Cancel();
        CurrentPath = [];
    }

    /// <summary>Roughly a normal running pace once combined with the interval below.</summary>
    private const float StepDistance = 0.6f;
    private static readonly TimeSpan StepInterval = TimeSpan.FromMilliseconds(120);

    /// <summary>Steps the character horizontally and turns it to face the direction of travel.</summary>
    /// <summary>
    /// Reports where the character actually is, rather than asking to be moved.
    ///
    /// <see cref="Move"/> walks against the navmesh, which is a coarse thing
    /// built for pathfinding. A renderer has the zone's own collision mesh and
    /// has already decided what the character could and could not walk into, so
    /// it says where it ended up and this follows. The zone-line check still
    /// runs - walking across one is the whole point of knowing where you are.
    /// </summary>
    public void PlaceAt(float x, float vertical, float depth, sbyte facing)
    {
        // Reported while dead too, which it did not used to be.
        //
        // The old refusal said the server ignores a corpse's position. It does
        // not: LandSandBoat's 0x015_pos.cpp checks only that the character is
        // not disappearing or shutting down, then takes the position and tells
        // everyone nearby. What the refusal was really protecting against was
        // the renderer walking a corpse around the zone - and the renderer now
        // refuses that itself, at the keys, so the only movement that can
        // reach here while dead is the deliberate drag on the jump key and the
        // fall that settles the body onto the ground.
        //
        // Both of those are worth sending. A body that shifts is the only
        // thing a dead player can do that someone across a clearing might
        // notice.

        // Not while we are between zones.
        //
        // The renderer showing the zone we are leaving keeps reporting a
        // position in it, and the server takes the last one it is told.
        // Walking into the line to West Sarutabaruta put the character at
        // the line's own coordinates in West Sarutabaruta - which is the
        // middle of the zone - rather than where the server had already
        // put them, because we overwrote its placement with ours.
        if (_placementSuspended)
        {
            return;
        }

        bool shifted = x != PosX || vertical != PosVertical || depth != PosDepth;

        PosX = x;
        PosVertical = vertical;
        PosDepth = depth;
        Facing = facing;

        if (!shifted)
        {
            return;
        }

        Moved?.Invoke();

        // Not the zone lines, though. Dying on top of one and then shuffling
        // would ask to change zone from the floor, and a corpse arriving in a
        // new zone is a mess nobody asked for - the raise you were waiting on
        // is in the zone you just left.
        if (!IsDead)
        {
            _ = CheckZoneLinesAsync();
        }
    }

    public void Move(float dx, float dz)
    {
        if (dx == 0 && dz == 0)
        {
            return;
        }

        float targetX = PosX + dx;
        float targetZ = PosDepth + dz;
        LastMoveBlocked = false;

        if (NavMesh is not null)
        {
            // Raycast along the path rather than testing the destination:
            // a destination test finds the floor on the far side of a wall and
            // waves the move through.
            if (NavMesh.TryMove(PosX, PosVertical, PosDepth, targetX, targetZ,
                                out float newX, out float newVertical, out float newZ, out bool blocked))
            {
                LastMoveBlocked = blocked;
                PosX = newX;
                PosVertical = newVertical;
                PosDepth = newZ;
                Facing = (sbyte)(Math.Atan2(-dx, -dz) * (128.0 / Math.PI));
                Moved?.Invoke();
                _ = CheckZoneLinesAsync();
                return;
            }

            // Not on the mesh at all - fall through and move anyway rather
            // than freezing the character somewhere it can never leave.
        }

        PosX = targetX;
        PosDepth = targetZ;

        // FFXI packs a full turn into one signed byte, so a heading is
        // atan2 scaled by 256/2pi rather than by 360.
        Facing = (sbyte)(Math.Atan2(-dx, -dz) * (128.0 / Math.PI));
        Moved?.Invoke();
        _ = CheckZoneLinesAsync();
    }
    public FfxiZoneHandoff? Handoff { get; private set; }
    public uint OwnCharId => Handoff?.ContentId ?? 0;

    /// <summary>
    /// Whether an id in a packet means us.
    ///
    /// There are three answers to "who am I" and they are not always the same
    /// number: the content id from the handoff, the unique id the zone login
    /// reply gave back, and the server id. Which one a packet carries depends
    /// on the packet, so asking about one of them and getting it wrong makes
    /// the whole message look like it is about somebody else - which is how a
    /// GM teleport arrived, matched nothing, and moved nobody.
    /// </summary>
    public bool IsSelf(uint uniqueNo) =>
        uniqueNo != 0 &&
        (uniqueNo == OwnCharId ||
         uniqueNo == (ZoneState?.UniqueNo ?? 0) ||
         uniqueNo == (Handoff?.ServerId ?? 0));

    /// <summary>Everything we have been told about, for drawing or listing.</summary>
    public IReadOnlyList<FfxiEntityUpdate> KnownEntities() => _zone?.KnownEntities() ?? [];

    /// <summary>How many entities we currently know about. Diagnostic.</summary>
    public int KnownEntityCount => KnownEntities().Count;
    public bool IsConnected => _holdTask is { IsCompleted: false };

    /// <summary>Authenticates and returns the character roster.</summary>
    public async Task<(FfxiLoginResponse Login, IReadOnlyList<FfxiCharacter> Characters)> LoginAsync(
        FfxiServerProfile profile, string? otp = null, CancellationToken ct = default)
    {
        Status?.Invoke($"Connecting to {profile.Host}:{profile.AuthPort}...");

        var client = new FfxiLoginClient();
        (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters, FfxiRosterClient? roster) =
            await client.LoginAsync(profile, otp, ct: ct);

        _roster = roster;
        Status?.Invoke(login.Result == FfxiLoginResult.Success
            ? $"Logged in - {characters.Count(c => !string.IsNullOrWhiteSpace(c.Name))} character(s)."
            : $"Login failed: {login.Result}");

        return (login, characters);
    }

    /// <summary>
    /// Selects a character and connects to its zone server, completing the
    /// zone-in handshake. Movement is deliberately not started - see
    /// <see cref="StartHeartbeatAsync"/>.
    /// </summary>
    /// <summary>
    /// Makes a character on the lobby connection this session already holds.
    /// Returns null when it worked, or what to tell the person when it did not.
    /// </summary>
    public async Task<string?> CreateCharacterAsync(FfxiNewCharacter character, byte[] sessionHash,
                                                    CancellationToken ct = default)
    {
        if (_roster is null)
        {
            return "Not connected to the lobby - log in first.";
        }

        return await _roster.CreateCharacterAsync(character, sessionHash, ct);
    }

    public async Task ConnectToZoneAsync(FfxiCharacter character, byte[] sessionHash, string host, CancellationToken ct = default)
    {
        if (_roster is null)
        {
            throw new InvalidOperationException("Call LoginAsync first.");
        }

        // Before any of it arrives, not after.
        //
        // A zone change makes the server resend every bag from scratch, so
        // dropping what we have keeps a slot emptied on the other side from
        // lingering. This used to sit at the end of this method, inside the
        // zone-line loader, which put it *after* the server's login burst had
        // already been read - and the equipment list is in that burst and is
        // sent exactly once. So the bags refilled from the packets that kept
        // arriving and the equipment did not, which looked like an equip that
        // never worked rather than a wipe that should not have happened.
        Inventory.Clear();

        Status?.Invoke($"Selecting {character.Name}...");
        Handoff = await _roster.SelectCharacterAsync(character, sessionHash, ct);

        string zoneHost = FfxiRosterClient.FormatIpAddress(Handoff.ZoneServerIp);
        if (zoneHost == "0.0.0.0")
        {
            zoneHost = host;
        }

        _zoneEndpoint = new IPEndPoint(IPAddress.Parse(zoneHost), (int)Handoff.ZoneServerPort);
        _zone = new FfxiZoneClient(Handoff, _codec);

        Status?.Invoke($"Connecting to zone server {_zoneEndpoint}...");
        FfxiZoneReply? reply = await _zone.LoginAsync(
            _zoneEndpoint, Handoff.ContentId, Handoff.CharacterName, "", clientVersion: 99, clientLanguage: 2, ct: ct);

        if (reply?.Plaintext is null)
        {
            throw new InvalidOperationException("Zone server did not answer, or its reply could not be decoded.");
        }

        foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
        {
            AdoptBags(reply.Plaintext.AsSpan(offset, size));

            if (id == FfxiZoneLoginReply.PacketId && size == FfxiZoneLoginReply.PacketSize)
            {
                ZoneState = FfxiZoneLoginReply.Parse(reply.Plaintext.AsSpan(offset, size));
                AdoptZoneMusic();
                AdoptZoneWeather();
            }
        }

        // Adopt the reported position before loading zone lines: the arrival
        // guard compares against where we are, and at this point the heartbeat
        // has not started to set it.
        if (ZoneState is not null)
        {
            PosX = ZoneState.X;
            PosVertical = ZoneState.Vertical;
            PosDepth = ZoneState.Depth;
            Facing = ZoneState.Direction;
        }

        // Without this the server never sends the initialization batch, so the
        // character arrives knowing nothing about itself.
        await _zone.SendGameOkAsync(_zoneEndpoint, ct);

        // The login message has to be asked for; it is not pushed with
        // everything else at zone-in. A client that never sends this sees
        // nothing, which looks exactly like a server with nothing to say.
        await _zone.SendServerMessageRequestAsync(_zoneEndpoint, 0, ct);

        // An event the character was already in when we arrived.
        //
        // Answering the 0x032 the server sends only covers events it starts
        // while we are listening. A character who was parked in one - anyone
        // who logged out mid-cutscene, or was put somewhere by other means -
        // is already in it at zone-in, and the server does not announce it
        // again. It is in the login reply, which says what event is open.
        //
        // Nobody sees a character in an event, so this is the difference
        // between existing and not. EVENTEND is validated on the event id
        // alone, so our own ids are fine even when the event belongs to an
        // NPC.
        // The server fills these four in only when it has an event open
        // (0x00a_login.cpp: `if (csid != -1)`), and sets the character's
        // animation to Event in the same breath - so all four zero means no
        // event, and anything else means we are standing in one.
        //
        // EventNum is the zone and EventPara the event id, exactly as in the
        // 0x032 we would have answered had we been listening, so they go back
        // as they arrived.
        if (ZoneState.EventNo != 0 || ZoneState.EventNum != 0 ||
            ZoneState.EventPara != 0 || ZoneState.EventMode != 0)
        {
            Status?.Invoke($"Arrived inside event {ZoneState.EventPara} - ending it.");
            ChatReceived?.Invoke(new FfxiChatLine(
                DateTimeOffset.Now,
                FfxiChatMessageType.System1,
                "",
                $"[cutscene {ZoneState.EventPara} was already playing and has been skipped]"));
            _endedEvents.Add((ZoneState.UniqueNo, ZoneState.EventPara));
            await _zone.SendEventEndAsync(_zoneEndpoint, ZoneState.UniqueNo, ZoneState.ActIndex,
                                          ZoneState.EventNum, ZoneState.EventPara, ct);
        }

        TryLoadNavMesh();
        TryLoadZoneLines();
        Status?.Invoke($"In {(ZoneState is null ? "zone" : $"zone {ZoneState.ZoneNo}")} as {Handoff.CharacterName}.");
    }

    /// <summary>
    /// Starts the position heartbeat that keeps the session alive, reporting
    /// whatever <see cref="PosX"/>/<see cref="PosDepth"/> currently say - so
    /// <see cref="Move"/> steers a live character.
    /// </summary>
    public Task StartHeartbeatAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            throw new InvalidOperationException("Call ConnectToZoneAsync first.");
        }

        PosX = ZoneState.X;
        PosVertical = ZoneState.Vertical;
        PosDepth = ZoneState.Depth;
        Facing = ZoneState.Direction;

        _holdCts = new CancellationTokenSource();
        _holdTask = RunSessionAsync(_holdCts.Token);
        return Task.CompletedTask;
    }

    /// <summary>
    /// Keeps the heartbeat running, and carries the session across zone
    /// changes. A zone change ends one heartbeat and starts another against a
    /// different server, so the loop is the natural shape: hold until
    /// something interrupts, and if what interrupted was a handoff, follow it.
    /// </summary>
    private async Task RunSessionAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && _zone is not null && _zoneEndpoint is not null)
        {
            _zoneCts = CancellationTokenSource.CreateLinkedTokenSource(ct);

            await _zone.HoldWithPositionAsync(
                _zoneEndpoint,
                x: PosX,
                vertical: PosVertical,
                depth: PosDepth,
                direction: Facing,
                duration: TimeSpan.FromHours(12),
                interval: TimeSpan.FromMilliseconds(400),
                onReply: OnReply,
                positionProvider: () => (PosX, PosVertical, PosDepth, Facing),
                ct: _zoneCts.Token);

            IPEndPoint? destination = _pendingZone;
            _pendingZone = null;

            if (destination is null || ct.IsCancellationRequested)
            {
                break;
            }

            try
            {
                await CompleteZoneChangeAsync(destination);
            }
            catch (Exception ex)
            {
                Status?.Invoke($"Zone change failed: {ex.Message}");
                break;
            }
        }
    }

    private CancellationTokenSource? _zoneCts;

    /// <summary>
    /// How far the server's idea of our position may differ from ours before we
    /// treat it as a teleport rather than ordinary lag. Comfortably larger than
    /// a movement step, comfortably smaller than any real warp.
    /// </summary>
    private const float TeleportThreshold = 5.0f;

    private void AdoptServerPosition(FfxiEntityUpdate self)
    {
        float dx = self.X - PosX;
        float dz = self.Depth - PosDepth;

        if ((dx * dx) + (dz * dz) < TeleportThreshold * TeleportThreshold)
        {
            return;
        }

        PosX = self.X;
        PosVertical = self.Vertical;
        PosDepth = self.Depth;
        Facing = self.Direction;

        Status?.Invoke($"Moved by the server to x {PosX:F1}  y {PosVertical:F1}  z {PosDepth:F1}.");
        Moved?.Invoke();
        MovedByServer?.Invoke();
    }

    /// <summary>
    /// The server has put us somewhere, and the renderer has to be told.
    ///
    /// Separate from <see cref="Moved"/> on purpose. Moved fires for our own
    /// walking too, and the renderer is where that walking came from - feeding
    /// it back would have the character fighting itself every frame. This
    /// fires only when the move came from the other end.
    /// </summary>
    public event Action? MovedByServer;

    /// <summary>
    /// The track the zone wants, by slot. Filled from the zone login reply and
    /// then kept current by GP_SERV_COMMAND_MUSIC.
    /// </summary>
    private readonly ushort[] _musicSlots = new ushort[8];

    /// <summary>Raised when the music the zone wants has changed.</summary>
    public event Action<int>? MusicChanged;

    /// <summary>
    /// Which track should be playing now.
    ///
    /// Day and night are separate slots and the server does not say which
    /// applies - the client is expected to know the time, which it does. The
    /// other slots are situations we do not model yet, so this is the zone's
    /// tune and nothing else.
    /// </summary>
    public int CurrentTrack
    {
        get
        {
            bool night = VanadielHour is < 6 or >= 18;
            ushort wanted = night ? _musicSlots[(int)FfxiMusicSlot.ZoneNight] : _musicSlots[(int)FfxiMusicSlot.ZoneDay];

            // A zone with one tune leaves the other slot empty rather than
            // repeating itself.
            if (wanted == 0)
            {
                wanted = _musicSlots[(int)FfxiMusicSlot.ZoneDay];
            }
            return wanted;
        }
    }

    /// <summary>
    /// Takes the tracks the zone login reply carries.
    ///
    /// Called from both ways of arriving somewhere. It used to be called from
    /// the zone-change path alone, which is every arrival except the first
    /// one - so logging straight into a zone left every slot at zero and the
    /// client played nothing at all until you walked through a zone line.
    /// </summary>
    private void AdoptZoneMusic()
    {
        if (ZoneState?.Music is not { Count: > 1 } tracks)
        {
            return;
        }

        for (int slot = 0; slot < _musicSlots.Length && slot < tracks.Count; ++slot)
        {
            _musicSlots[slot] = tracks[slot];
        }

        Status?.Invoke($"music: day {tracks[0]}, night {tracks[1]}" +
                       (tracks.Count > 4 ? $", mount {tracks[4]}" : ""));
        MusicChanged?.Invoke(CurrentTrack);
    }

    /// <summary>Raised when the sky the zone is under has changed.</summary>
    public event Action<FfxiWeather>? WeatherChanged;

    /// <summary>
    /// The weather this zone is under.
    ///
    /// Comes from two places for the same reason the music does: the zone
    /// login reply says what it is on arrival, and 0x057 says when it turns.
    /// Listening only for the change would leave a client under the last
    /// zone's sky until the weather happened to move.
    /// </summary>
    public FfxiWeather CurrentWeather { get; private set; } = FfxiWeather.None;

    /// <summary>
    /// Takes the weather the zone login reply carries, and says so if it is
    /// different from what we were standing under.
    /// </summary>
    private void AdoptZoneWeather()
    {
        FfxiWeather arrived = ZoneState?.Weather ?? FfxiWeather.None;
        if (arrived == CurrentWeather)
        {
            return;
        }

        CurrentWeather = arrived;
        Status?.Invoke($"weather: {arrived}");
        WeatherChanged?.Invoke(arrived);
    }

    /// <summary>Vana'diel's hour, which decides day music from night.</summary>
    public int VanadielHour =>
        ZoneState is null ? 12 : (int)(((ulong)ZoneState.GameTime * 25 / 3600) % 24);

    /// <summary>Raised when the server moves us to a different zone.</summary>
    public event Action<uint>? ZoneChanged;

    /// <summary>
    /// Handles GP_SERV_COMMAND_LOGOUT. Despite the name, state 2 is an
    /// ordinary zone change - walking a zone line, or a GM teleporting us
    /// somewhere else. The server rotates its Blowfish key straight after
    /// sending this, so ignoring it means talking to the wrong address with
    /// the wrong key, which from the outside looks like being logged off for
    /// no reason.
    /// </summary>
    private void HandleZoneTransition(FfxiZoneTransition transition)
    {
        if (_zone is null)
        {
            return;
        }

        if (!transition.IsZoneChange)
        {
            Status?.Invoke($"Server ended the session: {transition.State}.");

            // This is the answer LogoutAsync is waiting for. The server decides
            // how long leaving takes - LandSandBoat runs a Leavegame effect
            // first, and cuts it short for a GM - so waiting for it to say so
            // beats guessing a duration.
            _loggedOut?.TrySetResult(transition.State);

            _holdCts?.Cancel();
            return;
        }

        string ip = FfxiRosterClient.FormatIpAddress(transition.ZoneServerIp);
        if (ip == "0.0.0.0" && _zoneEndpoint is not null)
        {
            ip = _zoneEndpoint.Address.ToString();
        }

        _pendingZone = new IPEndPoint(IPAddress.Parse(ip), (int)transition.ZoneServerPort);
        Status?.Invoke($"Zoning to {_pendingZone}...");

        // Ends the current heartbeat so RunSessionAsync can pick the handoff up.
        _zoneCts?.Cancel();
    }

    private IPEndPoint? _pendingZone;

    /// <summary>Completed when the server confirms we have actually left.</summary>
    private TaskCompletionSource<FfxiLogoutState>? _loggedOut;

    /// <summary>
    /// Set from the moment a zone change is asked for until the renderer has
    /// been reopened on the other side. Anything reporting a position during
    /// that window is describing the zone we are leaving.
    /// </summary>
    private bool _placementSuspended;


    /// <summary>
    /// Completes a zone change: rotate the key the way the server just did,
    /// restart the counters, and introduce ourselves to the new zone server
    /// with a fresh 0x00A and GAMEOK.
    /// </summary>
    private async Task CompleteZoneChangeAsync(IPEndPoint destination)
    {
        if (_zone is null || Handoff is null)
        {
            return;
        }

        _zone.AdvanceKey();
        _zone.ResetCountersForNewZone();
        _zoneEndpoint = destination;

        FfxiZoneReply? reply = await _zone.LoginAsync(
            destination, Handoff.ContentId, Handoff.CharacterName, "", clientVersion: 99, clientLanguage: 2);

        if (reply?.Plaintext is null)
        {
            Status?.Invoke("New zone server did not answer.");
            return;
        }

        foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
        {
            AdoptBags(reply.Plaintext.AsSpan(offset, size));

            if (id == FfxiZoneLoginReply.PacketId && size == FfxiZoneLoginReply.PacketSize)
            {
                ZoneState = FfxiZoneLoginReply.Parse(reply.Plaintext.AsSpan(offset, size));
            }
        }

        await _zone.SendGameOkAsync(destination);
        await _zone.SendServerMessageRequestAsync(destination);

        if (ZoneState is not null)
        {
            PosX = ZoneState.X;
            PosVertical = ZoneState.Vertical;
            PosDepth = ZoneState.Depth;
            Facing = ZoneState.Direction;
            NavMesh = null;
            TryLoadNavMesh();
            TryLoadZoneLines();
            // Event ids only mean anything within a zone.
            _endedEvents.Clear();
            // ZoneChanged reopens the renderer on the far side, so by the
            // time it returns anything reporting a position is describing
            // the zone we are now actually in.
            ZoneChanged?.Invoke(ZoneState.ZoneNo);
            _placementSuspended = false;
            AdoptZoneMusic();
            AdoptZoneWeather();

            Status?.Invoke($"Now in zone {ZoneState.ZoneNo}.");
        }
    }

    /// <summary>Which zone line we have already asked about, so one step does not fire a dozen requests.</summary>
    /// <summary>Built up across fragments, then split into lines once the last lands.</summary>
    /// <summary>
    /// Events already answered. A zone change clears it: ids are only unique
    /// within a zone, and the same one means something else on the other side.
    /// </summary>
    private readonly HashSet<(uint Entity, ushort Event)> _endedEvents = [];

    private readonly System.Text.StringBuilder _serverMessage = new();

    private uint? _zoneLineRequested;

    /// <summary>
    /// The bags, what is worn out of them, and the numbers they change.
    ///
    /// <para>
    /// Its own method because more than one loop reads packets. The zone login
    /// reply is walked where it arrives, looking only for the login packet
    /// itself, and everything after it is walked by the heartbeat. The
    /// equipment list is sent exactly once, in the login burst, so a handler
    /// that only ran in the second loop never saw it - the bags filled from
    /// the packets that kept coming and the equipment stayed empty, which
    /// looked like an equip that did nothing.
    /// </para>
    ///
    /// <para>
    /// The server sends the bags one slot at a time with no count in front, so
    /// the tracker keeps the running picture and 0x01D says when it is
    /// complete.
    /// </para>
    /// </summary>
    private void AdoptBags(ReadOnlySpan<byte> subPacket)
    {
        if (FfxiContainerSizes.TryParse(subPacket) is { } sizes)
        {
            Inventory.Apply(sizes);
        }

        if (FfxiInventoryItem.TryParse(subPacket) is { } carried)
        {
            Inventory.Apply(carried);
        }

        if (FfxiInventoryItemDetail.TryParse(subPacket) is { } detailed)
        {
            Inventory.Apply(detailed);
        }

        if (FfxiInventoryCount.TryParse(subPacket) is { } counted)
        {
            Inventory.Apply(counted);
        }

        if (FfxiInventoryReady.TryParse(subPacket) is { } ready)
        {
            Inventory.Apply(ready);
        }

        if (FfxiEquipment.TryParse(subPacket) is { } worn)
        {
            Inventory.Apply(worn);
        }

        // Our job, level and stats. Sent with the zone-in batch, and again
        // whenever a piece of gear changes what they are.
        if (FfxiJobInfo.TryParse(subPacket) is { } job)
        {
            Job = job;
            JobChanged?.Invoke();
        }
    }

    private void TryLoadZoneLines()
    {
        ZoneLines = [];
        _zoneLineRequested = null;

        if (_zoneDataDirectory is null || ZoneState is null)
        {
            return;
        }

        string? zoneName = FfxiZoneNames.Get(ZoneState.ZoneNo);
        if (zoneName is null)
        {
            return;
        }

        // Zone data directories are the zone name lowercased.
        string path = Path.Combine(_zoneDataDirectory, zoneName.ToLowerInvariant(), "zone.yaml");

        try
        {
            ZoneLines = FfxiZoneLineReader.Read(path);
            if (ZoneLines.Count > 0)
            {
                Status?.Invoke($"{ZoneLines.Count} zone lines loaded for {zoneName}.");
            }

            // Zoning drops us onto the arrival zone line, which is the return
            // trip. Treat whichever line we land inside as already requested,
            // so we do not bounce straight back where we came from.
            foreach (FfxiZoneLine line in ZoneLines)
            {
                if (line.DistanceSquaredTo(PosX, PosDepth) <= line.Radius * line.Radius)
                {
                    _zoneLineRequested = line.Id;
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            Status?.Invoke($"Zone lines for {zoneName} could not be read: {ex.Message}");
        }
    }

    /// <summary>
    /// Asks the server to zone if we are standing on a zone line. Only the
    /// client ever starts a zone change - the server waits to be asked - so
    /// without this a character can walk over a zone line all day and stay put.
    /// </summary>
    private async Task CheckZoneLinesAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null || ZoneLines.Count == 0 || _pendingZone is not null)
        {
            return;
        }

        FfxiZoneLine? touching = null;
        foreach (FfxiZoneLine candidate in ZoneLines)
        {
            if (candidate.DistanceSquaredTo(PosX, PosDepth) <= candidate.Radius * candidate.Radius)
            {
                touching = candidate;
                break;
            }
        }

        // Clear the guard once clear of every line, so the same one can be
        // used again on a later crossing.
        if (touching is null)
        {
            _zoneLineRequested = null;
            return;
        }

        foreach (FfxiZoneLine line in new[] { touching })
        {
            if (_zoneLineRequested == line.Id)
            {
                return;
            }

            Status?.Invoke($"Touched zone line {line.Token} to {line.Destination} - requesting zone change.");

            // One request per touch. The heartbeat calls this several times a
            // second, and the server takes a moment to answer, so without a
            // guard a single step onto a line fires a dozen requests.
            _zoneLineRequested = line.Id;
            _placementSuspended = true;
            await _zone.SendZoneLineAsync(_zoneEndpoint, line.Id, PosX, PosVertical, PosDepth, ZoneState.ActIndex);
            return;
        }
    }

    private void TryLoadNavMesh()
    {
        if (_navMeshDirectory is null || ZoneState is null)
        {
            return;
        }

        string? zoneName = FfxiZoneNames.Get(ZoneState.ZoneNo);
        if (zoneName is null)
        {
            Status?.Invoke($"No name known for zone {ZoneState.ZoneNo} - movement will not follow terrain.");
            return;
        }

        try
        {
            NavMesh = FfxiNavMesh.TryLoadZone(_navMeshDirectory, zoneName);
            Status?.Invoke(NavMesh is null
                ? $"No navmesh for {zoneName} - movement will not follow terrain."
                : $"Navmesh loaded for {zoneName} - movement follows the ground.");
        }
        catch (Exception ex)
        {
            Status?.Invoke($"Navmesh for {zoneName} could not be read: {ex.Message}");
        }
    }

    private void OnReply(FfxiZoneReply reply)
    {
        if (reply.Plaintext is null)
        {
            return;
        }

        bool sawEntity = false;

        foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
        {
            // Whatever event the server has started, answered at once.
            //
            // A character the server thinks is in an event is not spawned for
            // anyone else: present, addressable, receiving everyone's
            // positions, and invisible to all of them. A brand new character
            // zones in owing a whole opening cutscene, so this is the
            // difference between a new character existing and not.
            //
            // Echoed back as it arrived - the ids belong to whatever the event
            // is about, usually an NPC rather than us, and the server checks
            // them.
            FfxiEventStart? started = FfxiEventStart.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (started is not null && _zoneEndpoint is not null && _zone is not null)
            {
                // Once each. The server repeats the start until it is answered
                // and several arrive together, so answering every copy sends a
                // handful of EVENTENDs in the same millisecond - the first ends
                // the event and the server logs "Not in an event" for the rest.
                var which = (started.UniqueNo, started.EventId);
                if (_endedEvents.Add(which))
                {
                    _zone.SendEventEndAsync(_zoneEndpoint, started.UniqueNo, started.ActIndex, started.ZoneNo,
                                            started.EventId).GetAwaiter().GetResult();

                    // Say that it happened, and say what was found.
                    //
                    // The script is in the client's own DATs and can now be
                    // located: 5820 + zone, the block whose first word is this
                    // entity, and the offset the index gives for this id. What
                    // cannot be done yet is run it, so the event is still
                    // ended - but ending it silently is indistinguishable from
                    // an NPC ignoring you.
                    string found = "not in this zone's scripts";
                    if (Events().For(started.UniqueNo) is { } scripts)
                    {
                        found = scripts.Has((ushort)started.EventId)
                            ? $"script found, {scripts.Script.Length} bytes"
                            : $"entity has {scripts.EventIds.Count} events, not this one";
                    }

                    ChatReceived?.Invoke(new FfxiChatLine(
                        DateTimeOffset.Now,
                        FfxiChatMessageType.System1,
                        Speaker(started.UniqueNo),
                        $"[cutscene {started.EventId}: {found}]"));
                }
            }

            // The login message, a fragment at a time. Each says where it sits
            // in the whole and the next has to be asked for - the server
            // answers exactly what it was asked for and nothing more.
            FfxiServerMessageFragment? fragment =
                FfxiServerMessageFragment.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (fragment is not null)
            {
                _serverMessage.Append(fragment.Text);
                if (fragment.NextOffset is int next && _zoneEndpoint is not null)
                {
                    // Inline: this runs on the receive path, and the packet
                    // counter it shares is not safe to advance from elsewhere.
                    _zone?.SendServerMessageRequestAsync(_zoneEndpoint, next).GetAwaiter().GetResult();
                }
                else
                {
                    foreach (string line in _serverMessage.ToString().Replace("\r\n", "\n").Split('\n'))
                    {
                        if (line.Trim().Length > 0)
                        {
                            ChatReceived?.Invoke(new FfxiChatLine(DateTimeOffset.Now, FfxiChatMessageType.System1,
                                                                  "Server", line.Trim()));
                        }
                    }

                    _serverMessage.Clear();
                }
            }

            FfxiChatMessage? chat = FfxiChatMessage.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (chat is not null && chat.Text.Length > 0)
            {
                ChatReceived?.Invoke(new FfxiChatLine(DateTimeOffset.Now, chat.Kind, chat.Sender, chat.Text));
            }

            // The zone changing its mind about the music - a battle
            // starting, night falling, stepping onto a chocobo.
            FfxiMusicChange? tune = FfxiMusicChange.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (tune is not null && (int)tune.Slot < _musicSlots.Length)
            {
                int before = CurrentTrack;
                _musicSlots[(int)tune.Slot] = tune.Track;
                if (CurrentTrack != before)
                {
                    MusicChanged?.Invoke(CurrentTrack);
                }
            }

            // The zone's weather turning. Sent to everyone standing in it, on
            // the zone's own schedule rather than in answer to anything, so
            // this is the only notice a client gets.
            FfxiWeatherChange? sky = FfxiWeatherChange.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (sky is not null && sky.Weather != CurrentWeather)
            {
                CurrentWeather = sky.Weather;
                Status?.Invoke($"weather: {sky.Weather}");
                WeatherChanged?.Invoke(sky.Weather);
            }

            // Being put somewhere. GP_SERV_COMMAND_POS, which is what every
            // GM teleport arrives as - !pos, !goto, !bring - and what a
            // homepoint return uses. Going unread meant those commands moved
            // the character on the server and nowhere else: the renderer kept
            // drawing them where they were and kept telling the server so,
            // fifty milliseconds later, which put them back.
            FfxiServerPosition? placed = FfxiServerPosition.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (placed is not null && !IsSelf(placed.UniqueNo))
            {
                // Worth saying rather than dropping: if a teleport ever stops
                // working again, this is the line that says why.
                Status?.Invoke($"Ignored a placement for {placed.UniqueNo:X8}, which is not us.");
            }
            else if (placed is not null)
            {
                PosX = placed.X;
                PosVertical = placed.Vertical;
                PosDepth = placed.Depth;
                Facing = placed.Direction;
                Status?.Invoke($"Placed by the server at x {PosX:F1}  y {PosVertical:F1}  z {PosDepth:F1}.");
                Moved?.Invoke();
                MovedByServer?.Invoke();
            }

            // Our own hit points. Nothing else carries them.
            FfxiCharacterHealth? health = FfxiCharacterHealth.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (health is not null && health.UniqueNo == (ZoneState?.UniqueNo ?? 0))
            {
                bool wasDead = IsDead;
                Health = health;
                if (wasDead != health.IsDead)
                {
                    // Standing up is the end of any offer, taken or not. The
                    // server does not withdraw one - it simply stops honouring
                    // it - so a client that kept the flag would go on showing a
                    // live Accept Raise button to somebody already on their
                    // feet.
                    if (!health.IsDead)
                    {
                        SetRaiseOffer(false);
                    }

                    DeathChanged?.Invoke(health.IsDead);
                }
            }

            AdoptBags(reply.Plaintext.AsSpan(offset, size));

            // Somebody has cast Raise. Nothing about the corpse says so.
            FfxiRaiseOffer? offer = FfxiRaiseOffer.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (offer is not null && offer.UniqueNo == (ZoneState?.UniqueNo ?? 0) &&
                offer.Kind == FfxiResurrectionKind.Raise)
            {
                SetRaiseOffer(true);
            }

            // A shop. The count comes first and the stock follows in batches
            // of up to nineteen, so nothing can be shown until the last one is
            // in - see FfxiShop.
            if (Shop.Open(reply.Plaintext.AsSpan(offset, size)) && Shop.Complete)
            {
                ShopOpened?.Invoke(Shop);
            }
            else if (Shop.AddBatch(reply.Plaintext.AsSpan(offset, size)) && Shop.Complete)
            {
                ShopOpened?.Invoke(Shop);
            }

            // What an NPC said, which arrives as a line id to look up.
            FfxiNpcMessage? spoken = FfxiNpcMessage.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (spoken is not null)
            {
                string? said = Dialogue().Line(spoken.MessageId);
                string speaker = spoken.Nameless ? "" : Speaker(spoken.UniqueNo);

                // The server asks for some lines to appear with nobody in
                // front of them - narration, and anything a player character
                // says through this path.
                ChatReceived?.Invoke(new FfxiChatLine(
                    DateTimeOffset.Now,
                    FfxiChatMessageType.System1,
                    speaker,
                    said ?? $"(line {spoken.MessageId})"));

                // A line that carries choices is a question, and a question
                // belongs on the screen rather than in the log where it
                // scrolls away. The choices are not held anywhere separate -
                // 0x0b opens the list inside the line's own text and each 0x07
                // starts the next one - so the dialogue table has already
                // pulled them out by the time we get here.
                IReadOnlyList<string> choices = Dialogue().Options(spoken.MessageId);
                if (choices.Count > 0 && said is not null)
                {
                    NpcChoiceOffered?.Invoke(new FfxiNpcChoice(
                        spoken.UniqueNo, spoken.MessageId, speaker, said, choices));
                }
            }

            FfxiZoneTransition? transition = FfxiZoneTransition.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (transition is not null)
            {
                HandleZoneTransition(transition);
            }

            if (FfxiEntityUpdate.IsEntityUpdate(id))
            {
                sawEntity = true;

                // The server is authoritative for anything that moves us
                // without us asking - a GM !bring, a warp, a knockback. If it
                // places us somewhere far from where we have been claiming to
                // be, adopt its answer; otherwise our next heartbeat would
                // simply assert the old position and undo the teleport.
                FfxiEntityUpdate? self = FfxiEntityUpdate.TryParse(reply.Plaintext.AsSpan(offset, size));
                if (self is not null && IsSelf(self.UniqueNo))
                {
                    AdoptServerPosition(self);

                    // And what we look like, which is the only place it comes
                    // from. The equipment model ids are not in the item files
                    // at all - the server sends them in the look, so a client
                    // that reads its own bags still cannot tell what the gear
                    // in them looks like on a body. Only re-announced when it
                    // changes, because rebuilding the model is not free.
                    // Logged either way: an update that arrives without a
                    // look is a different problem from one that never arrives,
                    // and the difference is invisible from the outside.
                    if (self.Look is null)
                    {
                        _looklessSelfUpdates++;
                        if (_looklessSelfUpdates % 50 == 1)
                        {
                            Status?.Invoke($"self update {_looklessSelfUpdates} carried no look");
                        }
                    }

                    if (self.Look is { IsEquipment: true } look)
                    {
                        string wearing = look.ToLookString();
                        if (wearing != Look)
                        {
                            Look = wearing;
                            LookChanged?.Invoke(wearing);
                        }
                    }
                }
            }
        }

        if (sawEntity && _zone is not null)
        {
            EntitiesChanged?.Invoke(_zone.KnownEntities());
        }
    }

    /// <summary>
    /// Talks to an NPC, which is the only way to make one say anything.
    ///
    /// The server runs an NPC's onTrigger when asked and not before, and
    /// answers with either a line of dialogue as a TALKNUM id or an event.
    /// Both now reach the chat window.
    /// </summary>
    public async Task TalkToAsync(uint uniqueNo, ushort actIndex)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendActionAsync(_zoneEndpoint, uniqueNo, actIndex);
    }

    /// <summary>
    /// Engage the target - start attacking it. The server does the rest: it
    /// turns us to face the mob, closes to melee and swings on its own timer.
    /// Our part is this packet and the drawn-weapon stance the renderer shows
    /// once the server confirms we are engaged.
    /// </summary>
    public async Task EngageAsync(uint uniqueNo, ushort actIndex)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendActionAsync(_zoneEndpoint, uniqueNo, actIndex, FfxiActionPacket.ActionEngage);
    }

    /// <summary>Disengage - stop attacking and sheathe. The target is our own.</summary>
    public async Task DisengageAsync(uint uniqueNo, ushort actIndex)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendActionAsync(_zoneEndpoint, uniqueNo, actIndex, FfxiActionPacket.ActionDisengage);
    }

    /// <summary>
    /// Accepts the home point after dying, which is the only way back up.
    ///
    /// A character at zero HP is not going anywhere on their own: the server
    /// waits to be told, and until it is told the character lies there while
    /// this client happily keeps walking them around.
    /// </summary>
    public async Task ReturnToHomePointAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            return;
        }

        await _zone.SendActionAsync(_zoneEndpoint, ZoneState.UniqueNo, ZoneState.ActIndex,
                                    FfxiActionPacket.ActionHomePointMenu);
    }

    /// <summary>
    /// Jumps - or waves, when there is nothing left to jump with.
    ///
    /// A dead character in FFXI has no way to say anything. Movement, chat,
    /// actions and abilities are all refused, so somebody lying on the floor
    /// waiting for a raise cannot signal the party wandering past that they
    /// are there. The two packets the server does still take from a corpse are
    /// the jump and the emote - LandSandBoat's handlers for both check only
    /// that you are not mid-event, and rebroadcast to everyone in range - and
    /// of the two, a wave is the one that means "over here" rather than "I am
    /// hopping".
    ///
    /// So the key is the same key, and what it sends depends on whether you
    /// are on your feet. The renderer plays the local half either way: alive
    /// it is the jump clip, dead it restarts the death clip, so the body
    /// flinches where it lies.
    /// </summary>
    public async Task JumpAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            return;
        }

        if (IsDead)
        {
            // Motion only. The text form would put a line in everyone's log
            // every time, and someone waving for a raise will press this more
            // than once.
            await _zone.SendEmoteAsync(_zoneEndpoint, ZoneState.UniqueNo, ZoneState.ActIndex,
                                       FfxiMotionPacket.EmoteWave, FfxiEmoteMode.Motion);
            return;
        }

        await _zone.SendJumpAsync(_zoneEndpoint, ZoneState.UniqueNo, ZoneState.ActIndex);
    }

    /// <summary>
    /// Wears the item in a bag slot, or takes off what is worn when passed
    /// <see cref="FfxiEquipment.Empty"/>.
    ///
    /// Nothing is applied locally: the server answers with its own 0x050 and
    /// a fresh look, and a client that moved the gear itself would be showing
    /// a change the server may well have refused.
    /// </summary>
    public async Task EquipAsync(byte itemSlot, FfxiEquipSlot slot,
                                 FfxiContainer container = FfxiContainer.Inventory)
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            return;
        }

        await _zone.SendEquipAsync(_zoneEndpoint, itemSlot, slot, container);
    }

    /// <summary>
    /// Buys from the shop that is open.
    ///
    /// The index is the shop's own, which is what the server indexes its
    /// container by - not the position in whatever packet carried the item.
    /// Nothing is applied locally: the server answers with the gil and the
    /// inventory, and a client that added the item itself would be showing a
    /// purchase that may well be refused.
    /// </summary>
    public async Task BuyAsync(byte shopItemIndex, uint quantity = 1)
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null || quantity == 0)
        {
            return;
        }

        await _zone.SendShopBuyAsync(_zoneEndpoint, quantity, shopItemIndex);
    }

    /// <summary>
    /// Asks the server what we look like now.
    ///
    /// Gear changes what a character looks like, and the ids that say so live
    /// only in the server's entity update. It sends one eventually; this asks
    /// for it, so closing a bag is not followed by wearing the old thing until
    /// something else happens to prompt it.
    /// </summary>
    public async Task RefreshSelfAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            return;
        }

        Status?.Invoke($"refresh: asking the server about targid {ZoneState.ActIndex}");
        await _zone.SendCharacterRequestAsync(_zoneEndpoint, ZoneState.ActIndex);
    }

    /// <summary>
    /// Throws an item away.
    ///
    /// The quantity has to be the whole stack - the server compares it against
    /// what it holds and refuses anything else - so this is taken from the
    /// tracker rather than from the caller, which cannot be out of step with
    /// what the server last said.
    /// </summary>
    public async Task DropAsync(FfxiContainer container, byte slot)
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            return;
        }

        if (Inventory.At(container, slot) is not { } held)
        {
            return;
        }

        await _zone.SendDropAsync(_zoneEndpoint, held.Quantity, container, slot);
    }

    /// <summary>
    /// Takes the raise somebody has offered, which is the other way up.
    ///
    /// The server drops this unless it has already sent the offer, so it is
    /// guarded on having seen one rather than sent hopefully: an unanswered
    /// action packet looks exactly like a working one from here.
    /// </summary>
    public async Task AcceptRaiseAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null || !HasRaiseOffer)
        {
            return;
        }

        await _zone.SendActionAsync(_zoneEndpoint, ZoneState.UniqueNo, ZoneState.ActIndex,
                                    FfxiActionPacket.ActionRaiseMenu);
    }

    public async Task SayAsync(string message, FfxiChatKind kind = FfxiChatKind.Say)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendChatAsync(_zoneEndpoint, kind, message);
    }

    public async Task TellAsync(string recipient, string message)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendTellAsync(_zoneEndpoint, recipient, message);
    }

    /// <summary>Asks the server to log out, then stops the heartbeat.</summary>
    /// <summary>
    /// Leaves the world. Logout goes back to the character list; Shutdown ends
    /// the session outright, which is what /shutdown does in the real client.
    ///
    /// Either way the server applies a Leavegame effect for a few seconds and
    /// acts when it expires - neither is instant, and a client that closes its
    /// socket straight afterwards is back to being reaped on a timeout.
    /// </summary>
    public async Task LogoutAsync(FfxiLogoutKind kind = FfxiLogoutKind.Logout)
    {
        if (_zone is not null && _zoneEndpoint is not null)
        {
            try
            {
                _loggedOut = new TaskCompletionSource<FfxiLogoutState>(
                    TaskCreationOptions.RunContinuationsAsynchronously);

                await _zone.SendLogoutAsync(_zoneEndpoint, kind);
                Status?.Invoke("Logout requested. Waiting for the server...");

                // Wait for the server to say it is done, rather than for a
                // number we picked.
                //
                // REQLOGOUT is a request, not a goodbye: LandSandBoat starts a
                // Leavegame effect and clears the session when it expires,
                // which is seconds for a GM and the best part of half a minute
                // for anyone else. A fixed wait is wrong in both directions -
                // too short and the logout never completes, so the character
                // sits on the server until it is reaped about a minute later
                // and the next login fails on 201; too long and leaving always
                // feels broken.
                //
                // The cap is a backstop for a server that never answers, not
                // the expected path.
                using var giveUp = new CancellationTokenSource(TimeSpan.FromSeconds(45));
                try
                {
                    FfxiLogoutState state = await _loggedOut.Task.WaitAsync(giveUp.Token);
                    Status?.Invoke($"Logged out: {state}.");
                }
                catch (OperationCanceledException)
                {
                    Status?.Invoke("The server never confirmed the logout; leaving anyway.");
                }
                finally
                {
                    _loggedOut = null;
                }
            }
            catch (Exception ex)
            {
                Status?.Invoke($"Logout request failed: {ex.Message}");
            }
        }

        await _holdCts?.CancelAsync()!;
    }

    public void Dispose()
    {
        _holdCts?.Cancel();
        _holdCts?.Dispose();
        _zone?.Dispose();
        _roster?.Dispose();
    }
    /// <summary>
    /// The current zone's lines, loaded once per zone.
    ///
    /// Each zone has its own dialogue file and its own numbering, so line
    /// 4133 is a shopkeeper here and something unrelated one zone over.
    /// </summary>
    /// <summary>
    /// The current zone's event scripts, loaded once per zone.
    ///
    /// Numbered per zone the same way the dialogue is: event 7 here is a
    /// different cutscene one zone over.
    /// </summary>
    private FfxiEventTable Events()
    {
        uint zone = ZoneState?.ZoneNo ?? uint.MaxValue;
        if (_files is not null && zone != _eventZone)
        {
            _events = FfxiEventTable.Load(_files, (int)zone);
            _eventZone = zone;
        }

        return _events;
    }

    private FfxiDialogueTable Dialogue()
    {
        uint zone = ZoneState?.ZoneNo ?? uint.MaxValue;
        if (_files is not null && zone != _dialogueZone)
        {
            _dialogue = FfxiDialogueTable.Load(_files, (int)zone);
            _dialogueZone = zone;
        }

        return _dialogue;
    }

    /// <summary>Who said it, if we have seen them; otherwise nobody.</summary>
    private string Speaker(uint uniqueNo)
    {
        foreach (FfxiEntityUpdate entity in _zone.KnownEntities())
        {
            if (entity.UniqueNo == uniqueNo && !string.IsNullOrEmpty(entity.Name))
            {
                return entity.Name;
            }
        }

        // The server sends most NPCs with an empty name and expects the client
        // to know it. Without this an NPC spoke into the chat log with nothing
        // in front of the colon, which reads as the game not knowing who is
        // talking - and it is the same table the renderer already reads for
        // nameplates.
        return EntityNames().Lookup(uniqueNo) ?? "";
    }

    private FfxiEntityNames EntityNames()
    {
        uint zone = ZoneState?.ZoneNo ?? 0;
        if (_entityNamesZone != (int)zone)
        {
            _entityNamesZone = (int)zone;
            _entityNames = FfxiEntityNames.Load(_files, (int)zone);
        }

        return _entityNames;
    }
}
