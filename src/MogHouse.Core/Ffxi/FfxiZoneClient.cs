using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// What the server sent back to an 0x00A, after decryption and integrity
/// checking. <see cref="Payload"/> is still compressed - see
/// <see cref="FfxiZoneClient"/>'s remarks.
/// </summary>
/// <param name="Payload">
/// The decrypted body, still compressed, with its trailing MD5 stripped. Its
/// last four bytes are the declared bit count - see <see cref="DeclaredBits"/>.
/// </param>
/// <param name="DeclaredBits">
/// The compressed size the server stamped into the packet, in bits and
/// including the compression marker's 8 bits. Null when the payload is too
/// short to hold one.
/// </param>
/// <param name="Plaintext">
/// The decompressed body, when a Huffman codec was supplied and decoding
/// succeeded - otherwise null. This is the sequence of sub-packets the game
/// actually runs on.
/// </param>
public sealed record FfxiZoneReply(
    byte[] Datagram,
    byte[] Payload,
    bool ChecksumValid,
    ushort ServerCounter,
    ushort AcknowledgedClientCounter,
    uint? DeclaredBits,
    byte[]? Plaintext);

/// <summary>
/// The UDP zone/map connection (the address and port come from
/// <see cref="FfxiZoneHandoff"/>). This is the third and last transport in
/// the login chain and shares nothing with the other two: the auth session is
/// TLS+JSON, the data/view sessions are plain TCP, and this is UDP with its
/// own framing, its own integrity check, and Blowfish encryption.
///
/// Grounded in LandSandBoat/server's src/map/map_networking.cpp (branch
/// `base`, 2026-08-28).
///
/// Send path: the opening 0x00A is plaintext, and it's the only packet that
/// ever is - the server tries a plaintext MD5 first and requires any packet
/// that validates that way to be an 0x00A.
///
/// Receive path: strip the header, Blowfish-decrypt the body, then MD5-check
/// it. That order matters and is genuinely useful here - the checksum is
/// computed over the *decrypted* bytes, so a passing checksum proves the
/// cipher, the key schedule and the key derivation are all correct. It's the
/// server's own test for "did this decrypt?", which is how it decides whether
/// to retry with the previous key mid-zone-transition.
///
/// What this class deliberately does NOT do yet is decompress the payload.
/// The server's compression is not zlib despite the file being named zlib.cpp
/// - it's a static-Huffman codec driven by two lookup tables loaded at
/// startup from `res/compress.dat` and `res/decompress.dat`, with a bit
/// length (not byte length) as its size field. Implementing it needs those
/// tables, so <see cref="FfxiZoneReply.Payload"/> hands back the decrypted
/// but still-compressed body rather than pretending to parse it.
/// </summary>
public sealed class FfxiZoneClient : IDisposable
{
    private readonly UdpClient _udp = new();

    // When the server last answered, and whether the silence since then has
    // been written down. The zone is polled several times a second, and a
    // server with nothing to say - the normal state of a quiet zone - answers
    // only some of them, so a single missed poll means nothing and a line per
    // miss buries everything else in the log. A silence this long is worth
    // one line, because at that point the question is the network.
    private static readonly TimeSpan QuietWorthNoting = TimeSpan.FromSeconds(5);
    private DateTime _lastHeard = DateTime.UtcNow;
    private bool _quietReported;
    private readonly FfxiHuffman? _codec;

    /// <summary>Current session key and the cipher derived from it. Both advance together - see <see cref="TryAdvanceKey"/>.</summary>
    private uint[] _sessionKey;
    private FfxiBlowfish _blowfish;

    /// <summary>How many times the key has rotated this session. Purely diagnostic.</summary>
    public int KeyRotations { get; private set; }

    /// <summary>
    /// Last position we were told about for each entity, keyed by charid.
    /// Lets a client place itself relative to somebody else without anyone
    /// having to agree on what the axes are called - the numbers come off the
    /// wire in the same order for everyone.
    /// </summary>
    private readonly Dictionary<uint, (ushort PacketId, float X, float Vertical, float Depth)> _knownPositions = [];

    public bool TryGetKnownPosition(uint charId, out (ushort PacketId, float X, float Vertical, float Depth) position) =>
        _knownPositions.TryGetValue(charId, out position);

    /// <summary>
    /// Snapshot of every entity we have been told about, for callers that want
    /// to draw or list them. Copied rather than exposed live, since the
    /// receive loop mutates the underlying map.
    /// </summary>
    public IReadOnlyList<FfxiEntityUpdate> KnownEntities()
    {
        lock (_entityLock)
        {
            return _knownEntities.Values.ToList();
        }
    }

    private readonly Dictionary<uint, FfxiEntityUpdate> _knownEntities = [];
    private readonly object _entityLock = new();

    /// <summary>Our own charid, so "somebody else" is answerable without the caller passing it around.</summary>
    private uint _ownCharId;

    /// <summary>Our own outgoing packet counter (header offset 0).</summary>
    private ushort _ownCounter;

    /// <param name="codec">
    /// Optional. Without it, replies still decrypt and verify but their bodies
    /// stay compressed - useful because the compression tables aren't bundled
    /// (see <see cref="FfxiHuffmanTables"/>), so the transport is testable
    /// without them.
    /// </param>
    public FfxiZoneClient(FfxiZoneHandoff handoff, FfxiHuffman? codec = null)
    {
        _sessionKey = handoff.SessionKey.ToArray();
        _blowfish = FfxiBlowfish.FromSessionKey(_sessionKey);
        _codec = codec;
    }

    /// <summary>
    /// Tries the next key in the rotation against a datagram that failed to
    /// decrypt, adopting it if it works.
    ///
    /// The server rotates its key whenever it sends a 0x00B, and does so
    /// silently from our point of view: after it rotates, the 0x00B itself is
    /// undecryptable to us, so there is no message to react to - the only
    /// evidence is that everything stops working at once, in both directions.
    /// Probing forward on failure is the client-side mirror of the server's
    /// own `prev_blowfish` fallback.
    ///
    /// Bounded to a few steps so a genuinely corrupt packet costs a little
    /// wasted work rather than an unbounded search.
    /// </summary>
    /// <summary>
    /// Rotates to the next session key, as the server does immediately after
    /// sending a 0x00B. Used when we have actually seen that packet, rather
    /// than inferring the rotation from a decrypt failure.
    /// </summary>
    public void AdvanceKey()
    {
        _sessionKey = FfxiBlowfish.NextSessionKey(_sessionKey);
        _blowfish = FfxiBlowfish.FromSessionKey(_sessionKey);
        KeyRotations++;
    }

    /// <summary>
    /// Resets the packet counters for a new zone server. They are per-session
    /// on the server side and it zeroes them when it accepts a fresh 0x00A, so
    /// ours have to restart too - otherwise every sub-packet we send looks
    /// stale and is silently skipped.
    /// </summary>
    public void ResetCountersForNewZone()
    {
        _ownCounter = 0;
        _lastServerCounter = 0;
    }

    private bool TryAdvanceKey(byte[] datagram, int maxSteps = 4)
    {
        uint[] candidateKey = _sessionKey;

        for (int step = 0; step < maxSteps; step++)
        {
            candidateKey = FfxiBlowfish.NextSessionKey(candidateKey);
            var candidate = FfxiBlowfish.FromSessionKey(candidateKey);

            if (Decode(datagram, candidate, _codec).ChecksumValid)
            {
                _sessionKey = candidateKey;
                _blowfish = candidate;
                KeyRotations++;
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// Sends the opening GP_CLI_COMMAND_LOGIN. `uniqueNo` is the character id;
    /// the server will only accept it if a *pending session* exists for that
    /// id, which the login server creates by IPC as part of character select -
    /// so this must follow a real SelectCharacterAsync, not stand alone.
    /// </summary>
    public async Task SendLoginAsync(IPEndPoint zoneServer, uint uniqueNo, string characterName, string accountName, uint clientVersion, ushort clientLanguage, CancellationToken ct = default)
    {
        if (_ownCounter == 0)
        {
            _ownCounter = 1;
        }

        byte[] datagram = FfxiZoneLoginPacket.BuildDatagram(
            uniqueNo: uniqueNo,
            characterName: characterName,
            accountName: accountName,
            ticket: default,
            clientVersion: clientVersion,
            clientLanguage: clientLanguage,
            ownCounter: _ownCounter,
            sync: _ownCounter,
            timestamp: (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds());

        LastSentDatagram = datagram;
        await _udp.SendAsync(datagram, zoneServer, ct);
    }

    /// <summary>The last datagram handed to the socket, kept for diagnostics.</summary>
    public byte[]? LastSentDatagram { get; private set; }

    /// <summary>
    /// Sends the login packet and waits for the server's answer, retransmitting
    /// until one arrives. Retransmission is not a convenience here, it's
    /// required by how the server is built - a first 0x00A from an unknown
    /// address is answered by *nothing*, twice over:
    ///
    ///  1. `handle_incoming_packet` looks its session up by address, gets null,
    ///     and passes that null pointer to `recv_parse` **by value**.
    ///     `recv_parse` does create the session (adopting the pending session
    ///     the login server set up during character select), but it assigns it
    ///     to its own local parameter, so the caller's pointer is still null on
    ///     return - and the very next line is `if (PSession == nullptr) return;`.
    ///     The session now exists; nothing was sent back.
    ///  2. Independently, the pending session that step 1 consumes is created
    ///     by an IPC message that travels login server -> world server -> map
    ///     server after the handoff packet is already on its way to us. A
    ///     client fast enough to answer the handoff immediately can beat it.
    ///
    /// Either way the fix is the same and matches what the protocol expects of
    /// a UDP client: keep sending until answered. The second attempt finds the
    /// session registered against our address and gets a real reply.
    ///
    /// Each retransmit reuses the same counter and sync deliberately - it's the
    /// same logical packet, and the server's `parse` loop drops any sub-packet
    /// whose sync exceeds the outer header counter.
    /// </summary>
    public async Task<FfxiZoneReply?> LoginAsync(
        IPEndPoint zoneServer,
        uint uniqueNo,
        string characterName,
        string accountName,
        uint clientVersion,
        ushort clientLanguage,
        int attempts = 5,
        TimeSpan? retryInterval = null,
        CancellationToken ct = default)
    {
        TimeSpan interval = retryInterval ?? TimeSpan.FromSeconds(2);

        for (int attempt = 1; attempt <= attempts; attempt++)
        {
                        _ownCharId = uniqueNo;
await SendLoginAsync(zoneServer, uniqueNo, characterName, accountName, clientVersion, clientLanguage, ct);

            FfxiZoneReply? reply = await ReceiveAsync(interval, ct);
            if (reply is not null)
            {
                return reply;
            }
        }

        return null;
    }

    /// <summary>
    /// Waits for a reply, then decrypts and integrity-checks it. Returns null
    /// if nothing arrives before <paramref name="timeout"/> - a silent drop is
    /// the normal way this protocol rejects a packet it doesn't like, so a
    /// timeout is an expected outcome worth reporting rather than an
    /// exception.
    /// </summary>
    public async Task<FfxiZoneReply?> ReceiveAsync(TimeSpan timeout, CancellationToken ct = default)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(timeout);

        UdpReceiveResult result;
        try
        {
            result = await _udp.ReceiveAsync(timeoutCts.Token);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            TimeSpan quiet = DateTime.UtcNow - _lastHeard;
            if (!_quietReported && quiet >= QuietWorthNoting)
            {
                _quietReported = true;
                Console.WriteLine($"zone: nothing has arrived for {quiet.TotalSeconds:0}s");
            }
            return null;
        }

        if (_quietReported)
        {
            Console.WriteLine($"zone: heard from the server again after {(DateTime.UtcNow - _lastHeard).TotalSeconds:0}s");
        }
        _lastHeard = DateTime.UtcNow;
        _quietReported = false;
        FfxiZoneReply reply = Decode(result.Buffer, _blowfish, _codec);

        // A failed checksum here is the only signal that the server rotated
        // its key - see TryAdvanceKey.
        if (!reply.ChecksumValid && TryAdvanceKey(result.Buffer))
        {
            reply = Decode(result.Buffer, _blowfish, _codec);
        }

        // Said out loud because the two ways this fails are indistinguishable
        // from the outside: LoginAsync returns null when nothing ever arrives,
        // and a reply with no plaintext when something arrived and could not be
        // read. Both surface as "Zone server did not answer, or its reply could
        // not be decoded", which is the wrong sentence to be debugging from -
        // one means look at the network, the other means look at the key.
        if (reply.Plaintext is null)
        {
            Console.WriteLine(
                $"zone: {result.Buffer.Length} bytes from {result.RemoteEndPoint}, checksum " +
                $"{(reply.ChecksumValid ? "ok" : "FAILED")}, declared bits {reply.DeclaredBits?.ToString() ?? "none"}, " +
                "no plaintext");
        }

        // Track the server's counter so our outgoing headers can acknowledge it.
        if (reply.ChecksumValid || reply.ServerCounter > _lastServerCounter)
        {
            _lastServerCounter = reply.ServerCounter;
        }

        if (reply.Plaintext is not null)
        {
            foreach ((ushort _, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
            {
                FfxiEntityUpdate? entity = FfxiEntityUpdate.TryParse(reply.Plaintext.AsSpan(offset, size));
                if (entity is not null)
                {
                    _knownPositions[entity.UniqueNo] = (entity.PacketId, entity.X, entity.Vertical, entity.Depth);

                    lock (_entityLock)
                    {
                        // Partial updates carry position but no name, so keep
                        // the richer earlier record and refresh its position
                        // rather than overwriting a named entity with a blank.
                        if (entity.Name is null && _knownEntities.TryGetValue(entity.UniqueNo, out FfxiEntityUpdate? existing))
                        {
                            _knownEntities[entity.UniqueNo] = existing with { X = entity.X, Vertical = entity.Vertical, Depth = entity.Depth, Direction = entity.Direction };
                        }
                        else
                        {
                            _knownEntities[entity.UniqueNo] = entity;
                        }
                    }
                }
            }
        }

        return reply;
    }

    /// <summary>
    /// Holds an established session open by resending the login packet on an
    /// interval, until <paramref name="duration"/> elapses or the caller
    /// cancels. Returns how many keepalives were sent.
    ///
    /// This works because a repeat 0x00A on a session the server already knows
    /// takes a different path than the first one: `recv_parse` finds the
    /// session by address, so it skips the pending-session adoption and the
    /// character reload, and `handle_incoming_packet` then runs `parse`, whose
    /// first act is `tapLastUpdate()` for any session no longer in the
    /// WAITING or PENDING_ZONE state - which is exactly what an accepted
    /// session is. That tap is what the cleanup sweep checks, so the session
    /// stops being reaped ~60s after login.
    ///
    /// It also survives `GP_CLI_COMMAND_LOGIN::validate`'s "Player already
    /// logged in" rejection, which only fires once `hasDecryptedPacket` is
    /// set - and that flag is only set by a successfully *decrypted* packet,
    /// which this client cannot send yet. So this is a keepalive that happens
    /// to work rather than the mechanism the real client uses; the real one
    /// holds its session open with ordinary encrypted traffic. Replace this
    /// once the compressor lands and real packets can be sent.
    /// </summary>
    public async Task<int> HoldSessionAsync(
        IPEndPoint zoneServer,
        uint uniqueNo,
        string characterName,
        string accountName,
        uint clientVersion,
        ushort clientLanguage,
        TimeSpan duration,
        TimeSpan? interval = null,
        CancellationToken ct = default)
    {
        TimeSpan gap = interval ?? TimeSpan.FromSeconds(20);
        DateTimeOffset until = DateTimeOffset.UtcNow + duration;
        int sent = 0;

        while (DateTimeOffset.UtcNow < until && !ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(gap, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            await SendLoginAsync(zoneServer, uniqueNo, characterName, accountName, clientVersion, clientLanguage, ct);
            sent++;
        }

        return sent;
    }

    /// <summary>
    /// <remarks>
    /// WARNING: the vertical this sends is fixed - whatever the caller passes,
    /// typically echoed from the server's own reported position. FFXI terrain
    /// is not flat, so moving horizontally without adjusting height walks the
    /// character into the ground: still present, still able to chat and receive
    /// tells, but buried and invisible to everyone. The server also persists
    /// position periodically, so a bad height gets saved. Leave walkRadius at 0
    /// and followCharId unset unless the caller genuinely knows the terrain
    /// height along the path.
    /// </remarks>
    /// Holds a session open the way a real client does: by sending position
    /// updates. Unlike <see cref="HoldSessionAsync"/>'s 0x00A resend trick,
    /// this is the actual mechanism - the server's 0x015 handler sets
    /// UPDATE_POS and `requestedInfoSync`, so the character stops looking
    /// timed out to other players.
    ///
    /// Sends at <paramref name="interval"/> (default 1s, near what a real
    /// client does) until <paramref name="duration"/> elapses. Returns the
    /// number of updates sent.
    /// </summary>
    /// <summary>
    /// Requests a clean logout (GP_CLI_COMMAND_REQLOGOUT). Preferable to just
    /// dropping the socket: an abandoned session is reaped on a timeout about
    /// a minute later and its accounts_sessions row survives until then,
    /// blocking the next login for that character with error 201.
    ///
    /// The server applies a Leavegame effect rather than disconnecting
    /// immediately, and refuses outright while the character is in an event,
    /// crafting or otherwise action-blocked - so this can fail, and the caller
    /// should not assume the session has ended.
    /// </summary>
    public async Task SendLogoutAsync(IPEndPoint zoneServer, FfxiLogoutKind kind = FfxiLogoutKind.Logout,
                                      CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiLogoutPacket.Build((ushort)(_ownCounter + 1), FfxiLogoutMode.LogoutOn, kind), ct);

    /// <summary>
    /// Sends GP_CLI_COMMAND_GAMEOK - the "finished loading, send me everything"
    /// step of zoning in. Must follow a successful 0x00A: it travels encrypted
    /// like all steady-state traffic, so it needs the codec.
    /// </summary>
    /// <summary>
    /// Sends a chat message with a correct sync number.
    ///
    /// The sync matters and is easy to get wrong: the server's parse loop
    /// silently skips any sub-packet whose sync is at or below the session's
    /// last client counter. A hardcoded value works for the first packet of a
    /// session and then never again - the message goes out, is accepted by the
    /// transport, and is dropped without a word. Only this class knows the
    /// current counter, so building chat packets belongs here rather than in
    /// a caller.
    /// </summary>
    public async Task SendChatAsync(IPEndPoint zoneServer, FfxiChatKind kind, string message, CancellationToken ct = default) =>
        await SendEncryptedAsync(zoneServer, FfxiChatPacket.Build(kind, message, (ushort)(_ownCounter + 1)), ct);

    /// <summary>
    /// Acts on a target - talking to an NPC, for now. Same sync caveat as
    /// chat: a stale counter is skipped silently by the server's parse loop.
    /// </summary>
    public async Task SendActionAsync(IPEndPoint zoneServer, uint uniqueNo, ushort actIndex,
                                      ushort actionId = FfxiActionPacket.ActionTalk,
                                      CancellationToken ct = default) =>
        await SendEncryptedAsync(zoneServer, FfxiActionPacket.Build(uniqueNo, actIndex, actionId,
                                                                    (ushort)(_ownCounter + 1)), ct);

    /// <summary>Sends a tell, with the same sync caveat as <see cref="SendChatAsync"/>.</summary>
    public async Task SendTellAsync(IPEndPoint zoneServer, string recipient, string message, CancellationToken ct = default) =>
        await SendEncryptedAsync(zoneServer, FfxiTellPacket.Build(recipient, message, (ushort)(_ownCounter + 1)), ct);

    /// <summary>
    /// Requests a zone change after touching a zone line (0x05E). Same sync
    /// caveat as chat: a hardcoded value is silently skipped by the server's
    /// parse loop once the session counter has moved past it, with no warning
    /// logged anywhere - the request simply never happens.
    /// </summary>
    public async Task SendZoneLineAsync(IPEndPoint zoneServer, uint rectId, float x, float vertical, float depth, ushort actIndex, CancellationToken ct = default) =>
        await SendEncryptedAsync(zoneServer, FfxiZoneLinePacket.Build(rectId, x, vertical, depth, actIndex, (ushort)(_ownCounter + 1)), ct);

    /// <summary>
    /// Performs an emote (GP_CLI_COMMAND_MOTION). The only channel the protocol
    /// has for an animation other people will see - see FfxiMotionPacket.
    /// </summary>
    public async Task SendEmoteAsync(IPEndPoint zoneServer, uint uniqueNo, ushort actIndex, byte number,
                                     FfxiEmoteMode mode = FfxiEmoteMode.Motion, CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiMotionPacket.Build(uniqueNo, actIndex, number, (ushort)(_ownCounter + 1), mode), ct);

    /// <summary>
    /// Asks for the server's login message (GP_CLI_COMMAND_FRAGMENTS). The
    /// server does not volunteer it - a client that never asks never sees it.
    /// Call again with the next offset until the fragment says it is complete.
    /// </summary>
    public async Task SendServerMessageRequestAsync(IPEndPoint zoneServer, int offset = 0,
                                                    CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiFragmentsPacket.BuildServerMessageRequest((ushort)(_ownCounter + 1), offset), ct);

    /// <summary>
    /// Jumps (GP_CLI_COMMAND_JUMP). The server rebroadcasts it to everyone in
    /// range, which is what makes the animation visible to anyone but us.
    /// </summary>
    public async Task SendJumpAsync(IPEndPoint zoneServer, uint uniqueNo, ushort actIndex,
                                    CancellationToken ct = default) =>
        await SendEncryptedAsync(zoneServer, FfxiJumpPacket.Build(uniqueNo, actIndex, (ushort)(_ownCounter + 1)), ct);

    /// <summary>
    /// Changes a piece of equipment (GP_CLI_COMMAND_EQUIP_SET). Pass
    /// <see cref="FfxiEquipment.Empty"/> as the item slot to take something off.
    /// </summary>
    public async Task SendEquipAsync(IPEndPoint zoneServer, byte itemSlot, FfxiEquipSlot slot,
                                     FfxiContainer container, CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiEquipPacket.Build(itemSlot, slot, container, (ushort)(_ownCounter + 1)), ct);

    /// <summary>
    /// Throws an item away (GP_CLI_COMMAND_ITEM_DUMP). There is no undo.
    /// </summary>
    public async Task SendDropAsync(IPEndPoint zoneServer, uint quantity, FfxiContainer container,
                                    byte slot, CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiDropPacket.Build(quantity, container, slot, (ushort)(_ownCounter + 1)), ct);

    /// <summary>
    /// Buys from an open shop (GP_CLI_COMMAND_SHOP_BUY).
    ///
    /// ShopNo is sent as zero: the server reads only the item index, using it
    /// straight into the container it built when the shop was opened.
    /// </summary>
    public async Task SendShopBuyAsync(IPEndPoint zoneServer, uint quantity, ushort shopItemIndex,
                                       CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiShop.BuildBuy(quantity, 0, shopItemIndex, (ushort)(_ownCounter + 1)), ct);

    /// <summary>Asks the server to describe an entity again (GP_CLI_COMMAND_CHARREQ).</summary>
    public async Task SendCharacterRequestAsync(IPEndPoint zoneServer, ushort actIndex,
                                                CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiCharacterRequestPacket.Build(actIndex, (ushort)(_ownCounter + 1)), ct);

    public async Task SendGameOkAsync(IPEndPoint zoneServer, CancellationToken ct = default) =>
        await SendEncryptedAsync(zoneServer, FfxiGameOkPacket.Build((ushort)(_ownCounter + 1)), ct);

    /// <summary>How many events this session has ended. Diagnostic only.</summary>
    public int EventsEnded => eventsEnded;

    private int eventsEnded;

    /// <summary>
    /// Ends an event the server started, so the character stops being
    /// invisible to everyone else. See FfxiEventEndPacket.
    /// </summary>
    public async Task SendEventEndAsync(IPEndPoint zoneServer, uint uniqueNo, ushort actIndex, ushort eventNum,
                                        ushort eventPara, CancellationToken ct = default) =>
        await SendEncryptedAsync(
            zoneServer, FfxiEventEndPacket.Build(uniqueNo, actIndex, eventNum, eventPara, (ushort)(_ownCounter + 1)), ct);

    /// <param name="onReply">
    /// Called for every decoded reply. Handy for tracing what the server is
    /// actually telling us during a session.
    /// </param>
    public async Task<int> HoldWithPositionAsync(
        IPEndPoint zoneServer,
        float x,
        float vertical,
        float depth,
        sbyte direction,
        TimeSpan duration,
        TimeSpan? interval = null,
        Action<FfxiZoneReply>? onReply = null,
        float walkRadius = 0f,
        string? sayEvery = null,
        FfxiChatKind sayKind = FfxiChatKind.Say,
        int sayIntervalUpdates = 25,
        uint? followCharId = null,
        string? gmCommand = null,
        string? stopFile = null,
        Func<(float X, float Vertical, float Depth, sbyte Direction)>? positionProvider = null,
        string? tellTo = null,
        string? tellText = null,
        Func<bool>? jumpRequested = null,
        Func<string?>? chatToSend = null,
        uint selfUniqueNo = 0,
        Func<ushort>? selfActIndex = null,
        CancellationToken ct = default)
    {
        TimeSpan gap = interval ?? TimeSpan.FromSeconds(1);
        DateTimeOffset until = DateTimeOffset.UtcNow + duration;
        int sent = 0;

        while (DateTimeOffset.UtcNow < until && !ct.IsCancellationRequested)
        {
            // A stop file is how an outside caller asks for a *graceful* exit.
            // Killing the process instead skips the logout entirely, leaving
            // the server to reap the session on a timeout and holding the
            // character's session row for about a minute afterwards.
            if (stopFile is not null && File.Exists(stopFile))
            {
                break;
            }

            // Reporting a byte-identical position every tick makes the
            // server's own `moved` check false forever, so it never calls
            // onEntityMoved and never sets UPDATE_POS. Tracing a small circle
            // keeps the character genuinely in motion, which is what a real
            // client always is.
            float offsetX = 0f;
            float offsetZ = 0f;
            if (walkRadius > 0f)
            {
                double angle = sent * 0.4;
                offsetX = walkRadius * (float)Math.Cos(angle);
                offsetZ = walkRadius * (float)Math.Sin(angle);
            }

            // Following takes the target's position verbatim off the wire, so
            // it needs no agreement about which float is "up" - a small offset
            // keeps us beside them rather than inside them.
            // A provider lets the caller steer while the heartbeat runs; without
            // one the character simply reports where it started.
            sbyte baseDirection = direction;
            float baseX = x, baseVertical = vertical, baseDepth = depth;
            if (positionProvider is not null)
            {
                (baseX, baseVertical, baseDepth, baseDirection) = positionProvider();
            }
            {
                // Prefer the requested target, but fall back to any other
                // player we can see - which charid a person is logged in as
                // is not something worth making the caller guess.
                (ushort PacketId, float X, float Vertical, float Depth)? theirs = null;

                if (followCharId is uint target && _knownPositions.TryGetValue(target, out var requested))
                {
                    theirs = requested;
                }
                else if (followCharId is not null)
                {
                    foreach ((uint charId, var known) in _knownPositions)
                    {
                        if (known.PacketId == FfxiEntityUpdate.PlayerPacketId && charId != _ownCharId)
                        {
                            theirs = known;
                            break;
                        }
                    }
                }

                if (theirs is not null)
                {
                    baseX = theirs.Value.X + 0.5f;
                    baseVertical = theirs.Value.Vertical;
                    baseDepth = theirs.Value.Depth + 0.5f;
                }
            }

            byte[] pos = FfxiPositionPacket.Build(
                sync: (ushort)(_ownCounter + 1),
                x: baseX + offsetX,
                vertical: baseVertical,
                depth: baseDepth + offsetZ,
                direction: baseDirection,
                moveFrame: (ushort)(sent & 0xFFFF),
                modes: walkRadius > 0f ? FfxiPositionPacket.ModeFlags.Run : FfxiPositionPacket.ModeFlags.None,
                timeNow: (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds());

            await SendEncryptedAsync(zoneServer, pos, ct);
            sent++;

            // Chat rides the same encrypted transport as everything else, so
            // it goes out as its own datagram between position updates.
            // GM commands ride the ordinary chat packet: the server routes any
            // message starting with '!' to its command handler. Sent once,
            // a few updates in, so the session is settled first - and once
            // only, since most of these toggle.
            if (gmCommand is not null && sent == 5)
            {
                byte[] command = FfxiChatPacket.Build(FfxiChatKind.Say, gmCommand.StartsWith('!') ? gmCommand : $"!{gmCommand}", (ushort)(_ownCounter + 1));
                await SendEncryptedAsync(zoneServer, command, ct);
            }

            // A jump the player asked for in the renderer. FFXI has a packet
            // of its own for this rather than an emote, and the server
            // rebroadcasts it - which is the only way anyone else sees it. Both
            // ids are checked against the sender, so it is skipped until the
            // server has told us our own targid.
            if (jumpRequested is not null && selfUniqueNo != 0 && jumpRequested())
            {
                ushort targid = selfActIndex?.Invoke() ?? 0;
                if (targid != 0)
                {
                    await SendEncryptedAsync(
                        zoneServer, FfxiJumpPacket.Build(selfUniqueNo, targid, (ushort)(_ownCounter + 1)), ct);
                }
            }

            // Whatever the player typed. A line starting with '!' is a GM
            // command and rides the ordinary chat packet - the server routes it
            // - which is how a character moves between zones for now.
            if (chatToSend is not null)
            {
                while (chatToSend() is { Length: > 0 } line)
                {
                    await SendEncryptedAsync(
                        zoneServer, FfxiChatPacket.Build(FfxiChatKind.Say, line, (ushort)(_ownCounter + 1)), ct);
                }
            }

            if (sayEvery is not null && sent % sayIntervalUpdates == 0)
            {
                byte[] chat = FfxiChatPacket.Build(sayKind, $"{sayEvery} #{sent / sayIntervalUpdates}", (ushort)(_ownCounter + 1));
                await SendEncryptedAsync(zoneServer, chat, ct);
            }

            if (tellTo is not null && sent % sayIntervalUpdates == 0)
            {
                byte[] tell = FfxiTellPacket.Build(tellTo, $"{tellText} #{sent / sayIntervalUpdates}", (ushort)(_ownCounter + 1));
                await SendEncryptedAsync(zoneServer, tell, ct);
            }

            // Spend the rest of the interval draining replies, so the server's
            // counter stays tracked and the socket buffer doesn't fill - but
            // still leave the interval when it goes quiet.
            //
            // Draining must not double as the pacing, which is what an earlier
            // version did: ReceiveAsync returns the moment a datagram is
            // waiting, so with a busy server the loop never waited at all and
            // sent hundreds of thousands of updates a minute instead of one a
            // second. Deadline first, receive second.
            DateTimeOffset deadline = DateTimeOffset.UtcNow + gap;
            while (DateTimeOffset.UtcNow < deadline && !ct.IsCancellationRequested)
            {
                TimeSpan remaining = deadline - DateTimeOffset.UtcNow;
                if (remaining <= TimeSpan.Zero)
                {
                    break;
                }

                FfxiZoneReply? incoming = await ReceiveAsync(remaining, ct);
                if (incoming is null)
                {
                    break;
                }

                onReply?.Invoke(incoming);

                // Answer any cutscene the server starts.
                //
                // A character the server considers to be in an event is never
                // spawned for other players, so an unanswered event makes this
                // client invisible for as long as it lasts. Which event, and
                // when, is not something to guess at: the ids are script
                // decisions - the opening Bastok cutscene is 0 and then 7, and
                // a zone-in on a character who has already seen it was 22 - so
                // the only durable answer is to reply to whatever arrives.
                foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(incoming.Plaintext))
                {
                    if (id != FfxiEventStart.PacketId && id != FfxiEventStart.PacketIdStr &&
                        id != FfxiEventStart.PacketIdNum)
                    {
                        continue;
                    }

                    FfxiEventStart? started = FfxiEventStart.TryParse(incoming.Plaintext.AsSpan(offset, size));
                    if (started is null)
                    {
                        continue;
                    }

                    // Echoed back as it arrived. UniqueNo and ActIndex belong
                    // to whatever the event is about, usually an NPC rather
                    // than us, and the server checks them.
                    await SendEventEndAsync(zoneServer, started.UniqueNo, started.ActIndex, started.ZoneNo,
                                            started.EventId, ct);
                    eventsEnded++;
                }
            }

            // If replies kept arriving right up to the deadline the loop above
            // exits with no time left; if they stopped early, wait out the rest.
            TimeSpan leftover = deadline - DateTimeOffset.UtcNow;
            if (leftover > TimeSpan.Zero)
            {
                try
                {
                    await Task.Delay(leftover, ct);
                }
                catch (OperationCanceledException)
                {
                    break;
                }
            }
        }

        return sent;
    }

    /// <summary>
    /// Sends one or more sub-packets as a normal encrypted datagram - the
    /// wrapping every packet after the plaintext 0x00A uses. This is
    /// `send_parse`/`compressPacket`/`finalizePacket` run in reverse:
    /// compress the body, append its bit count, append an MD5 over both,
    /// then Blowfish the whole thing from byte 28 onward.
    ///
    /// Requires a codec - there is no uncompressed form of these packets.
    /// </summary>
    public async Task SendEncryptedAsync(IPEndPoint zoneServer, byte[] subPackets, CancellationToken ct = default)
    {
        if (_codec is null)
        {
            throw new InvalidOperationException(
                "Sending encrypted packets needs the Huffman tables - construct FfxiZoneClient with a codec. See FfxiHuffmanTables.");
        }

        _ownCounter++;

        byte[] datagram = BuildEncryptedDatagram(
            subPackets,
            _ownCounter,
            _lastServerCounter,
            (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            _codec,
            _blowfish);

        LastSentDatagram = datagram;
        await _udp.SendAsync(datagram, zoneServer, ct);
    }

    /// <summary>Highest counter seen from the server, echoed back in our header so it can tell what we've received.</summary>
    private ushort _lastServerCounter;

    /// <summary>
    /// Builds a compressed+encrypted datagram. Split out from
    /// <see cref="SendEncryptedAsync"/> so it can be tested without a socket.
    ///
    /// Body layout after the 28-byte header, mirroring `compressPacket` +
    /// `finalizePacket`: compressed bytes, then a uint32 holding the compressed
    /// size in bits (marker included), then a 16-byte MD5 over everything
    /// before it. Encryption then covers whole 64-bit blocks only - a trailing
    /// partial block is left in the clear, exactly as the receive path assumes.
    /// </summary>
    internal static byte[] BuildEncryptedDatagram(
        ReadOnlySpan<byte> subPackets,
        ushort ownCounter,
        ushort peerCounter,
        uint timestamp,
        FfxiHuffman codec,
        FfxiBlowfish blowfish)
    {
        var compressed = new byte[subPackets.Length * 2 + 64];
        int bits = codec.Compress(subPackets, compressed);
        if (bits < 0)
        {
            throw new InvalidOperationException("Compression failed - output buffer too small.");
        }

        int compressedBytes = FfxiHuffman.CompressedByteLength(bits);
        int bodyLength = compressedBytes + 4 + FfxiZonePacket.ChecksumSize;

        var datagram = new byte[FfxiZonePacket.HeaderSize + bodyLength];

        BinaryPrimitives.WriteUInt16LittleEndian(datagram.AsSpan(FfxiZonePacket.OffsetOwnCounter, 2), ownCounter);
        BinaryPrimitives.WriteUInt16LittleEndian(datagram.AsSpan(FfxiZonePacket.OffsetPeerCounter, 2), peerCounter);
        BinaryPrimitives.WriteUInt32LittleEndian(datagram.AsSpan(FfxiZonePacket.OffsetTimestamp, 4), timestamp);

        Span<byte> body = datagram.AsSpan(FfxiZonePacket.HeaderSize);
        compressed.AsSpan(0, compressedBytes).CopyTo(body);
        BinaryPrimitives.WriteUInt32LittleEndian(body.Slice(compressedBytes, 4), (uint)bits);

        System.Security.Cryptography.MD5.HashData(
            body[..(compressedBytes + 4)],
            body.Slice(compressedBytes + 4, FfxiZonePacket.ChecksumSize));

        // `cypherSize = (PacketSize / 4) & -2` - whole 4-byte words, rounded
        // down to an even count, since Blowfish works on 64-bit pairs.
        int words = (bodyLength / 4) & ~1;
        if (words > 0)
        {
            Span<uint> block = new uint[words];
            for (int i = 0; i < words; i++)
            {
                block[i] = BinaryPrimitives.ReadUInt32LittleEndian(body.Slice(i * 4, 4));
            }

            blowfish.EncipherBlocks(block);

            for (int i = 0; i < words; i++)
            {
                BinaryPrimitives.WriteUInt32LittleEndian(body.Slice(i * 4, 4), block[i]);
            }
        }

        return datagram;
    }

    /// <summary>
    /// Decrypts and verifies a received datagram. Split out from
    /// <see cref="ReceiveAsync"/> so it can be tested against captured bytes
    /// without a socket.
    /// </summary>
    internal static FfxiZoneReply Decode(byte[] datagram, FfxiBlowfish blowfish, FfxiHuffman? codec = null)
    {
        var working = (byte[])datagram.Clone();

        // map_decipher_packet: the cipher covers whole 64-bit blocks starting
        // at byte 28 (`(uint32*)buff + 7`), and an odd trailing 4-byte word is
        // left in the clear (`tmp -= tmp % 2`).
        int words = (working.Length - FfxiZonePacket.HeaderSize) / 4;
        words -= words % 2;

        if (words > 0)
        {
            Span<uint> body = new uint[words];
            for (int i = 0; i < words; i++)
            {
                body[i] = BitConverter.ToUInt32(working, FfxiZonePacket.HeaderSize + i * 4);
            }

            blowfish.DecipherBlocks(body);

            for (int i = 0; i < words; i++)
            {
                BitConverter.TryWriteBytes(working.AsSpan(FfxiZonePacket.HeaderSize + i * 4, 4), body[i]);
            }
        }

        int payloadLength = working.Length - FfxiZonePacket.HeaderSize - FfxiZonePacket.ChecksumSize;
        bool checksumValid = false;
        byte[] payload = [];

        if (payloadLength > 0)
        {
            payload = working.AsSpan(FfxiZonePacket.HeaderSize, payloadLength).ToArray();
            byte[] expected = System.Security.Cryptography.MD5.HashData(payload);
            checksumValid = working.AsSpan(working.Length - FfxiZonePacket.ChecksumSize).SequenceEqual(expected);
        }

        // The server stamps the compressed size, in bits, into the four bytes
        // immediately before the MD5 - so within `payload` (which excludes the
        // hash) it's the last word. See `compressPacket`, which writes it at
        // `zlib_compressed_size(packetSize)`, i.e. right past the compressed
        // bytes.
        uint? declaredBits = null;
        byte[]? plaintext = null;

        if (payload.Length >= 4)
        {
            declaredBits = BitConverter.ToUInt32(payload, payload.Length - 4);

            if (codec is not null && checksumValid)
            {
                plaintext = TryDecompress(payload, declaredBits.Value, codec);
            }
        }

        return new FfxiZoneReply(
            Datagram: working,
            Payload: payload,
            ChecksumValid: checksumValid,
            ServerCounter: BitConverter.ToUInt16(working, FfxiZonePacket.OffsetOwnCounter),
            AcknowledgedClientCounter: BitConverter.ToUInt16(working, FfxiZonePacket.OffsetPeerCounter),
            DeclaredBits: declaredBits,
            Plaintext: plaintext);
    }

    /// <summary>
    /// The declared size counts the compression marker's 8 bits, so the actual
    /// bitstream is eight bits shorter.
    ///
    /// The server passes the declared value to its own decompressor *unadjusted*,
    /// even though that function measures bits from after the marker - so it
    /// walks eight bits past the real end and decodes a stray trailing symbol
    /// or two. Harmless there, because sub-packet iteration is bounded by each
    /// sub-packet's own size field and stops before reaching the garbage. This
    /// subtracts the 8 instead, to produce exactly the bytes the server
    /// compressed rather than replicating an off-by-one that only adds noise.
    /// </summary>
    private static byte[]? TryDecompress(byte[] payload, uint declaredBits, FfxiHuffman codec)
    {
        if (declaredBits < 8)
        {
            return null;
        }

        int bitstreamBits = (int)declaredBits - 8;
        int compressedBytes = 1 + FfxiHuffman.CompressedByteLength(bitstreamBits);
        if (compressedBytes > payload.Length - 4)
        {
            return null;
        }

        // A Huffman-coded byte can't take fewer than one bit, so the bit count
        // is its own upper bound on the symbol count.
        var output = new byte[bitstreamBits];
        int written = codec.Decompress(payload.AsSpan(0, compressedBytes), bitstreamBits, output);

        return written < 0 ? null : output.AsSpan(0, written).ToArray();
    }

    public void Dispose() => _udp.Dispose();
}
