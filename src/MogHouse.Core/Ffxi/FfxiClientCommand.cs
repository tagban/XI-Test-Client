namespace MogHouse.Core.Ffxi;

/// <summary>What a typed line turned out to be.</summary>
public enum FfxiClientCommandKind
{
    /// <summary>Not a command. Say it.</summary>
    None,

    /// <summary>Leave the world and go back to the character list.</summary>
    Logout,

    /// <summary>End the session and close the client.</summary>
    Shutdown,

    /// <summary>Say something on a particular channel.</summary>
    Chat,

    /// <summary>Say something to one person.</summary>
    Tell,

    /// <summary>Talk to the nearest NPC, or one named after the command.</summary>
    Talk,

    /// <summary>Accept the home point after dying.</summary>
    HomePoint,

    /// <summary>Sit down. A pose the client holds, not a state the server keeps.</summary>
    Sit,

    /// <summary>Stand back up. Walking does it too.</summary>
    Stand,

    /// <summary>
    /// Engage the current target - start attacking. The server drives the
    /// fight; the client sends the action and shows the drawn stance. Retail
    /// also does this on a second Enter with an attackable target selected.
    /// </summary>
    Engage,

    /// <summary>Disengage - stop attacking and sheathe.</summary>
    Disengage,

    /// <summary>
    /// Draw or sheathe the weapon without engaging anything - a purely local
    /// pose, for seeing the drawn stance without a mob to fight. Not a retail
    /// command; a convenience.
    /// </summary>
    Draw,

    /// <summary>
    /// Report something wrong, from where it is wrong.
    ///
    /// The words are the easy half. What makes a report worth having is the
    /// zone, the spot, the facing, the hour and the weather - all of which the
    /// client knows and a person should not have to type.
    /// </summary>
    Bug,

    /// <summary>Recognised, but the line was missing something it needed.</summary>
    Incomplete,

    /// <summary>Recognised, but not something this client does yet.</summary>
    Unsupported,
}

/// <summary>The command a line was, and anything after it.</summary>
/// <param name="Channel">Which channel a Chat goes out on.</param>
/// <param name="Recipient">Who a Tell is addressed to.</param>
public sealed record FfxiClientCommand(
    FfxiClientCommandKind Kind,
    string Name,
    string Rest,
    FfxiChatKind Channel = FfxiChatKind.Say,
    string Recipient = "");

/// <summary>
/// Slash commands the client answers itself.
///
/// Most of what someone types is chat, and a few things are not. '!' commands
/// are not here at all - the server routes those, so they travel as ordinary
/// chat and this leaves them alone. '/' commands are the client's own, and the
/// two that matter are the two ways of leaving: /logout goes back to the
/// character list, /shutdown ends the session.
/// </summary>
public static class FfxiClientCommands
{
    /// <summary>
    /// Works out what a line is. Anything not recognised is chat, including a
    /// slash command we have not implemented - saying it out loud is wrong, so
    /// those come back as Unsupported rather than None.
    /// </summary>
    public static FfxiClientCommand Parse(string line)
    {
        string trimmed = line.Trim();
        if (trimmed.Length < 2 || trimmed[0] != '/')
        {
            return new FfxiClientCommand(FfxiClientCommandKind.None, "", line);
        }

        int space = trimmed.IndexOf(' ');
        string name = (space < 0 ? trimmed[1..] : trimmed[1..space]).ToLowerInvariant();
        string rest = space < 0 ? "" : trimmed[(space + 1)..].Trim();

        return name switch
        {
            "logout" => new FfxiClientCommand(FfxiClientCommandKind.Logout, name, rest),
            "shutdown" or "quit" => new FfxiClientCommand(FfxiClientCommandKind.Shutdown, name, rest),
            "homepoint" or "hp" or "return" => new FfxiClientCommand(FfxiClientCommandKind.HomePoint, name, rest),
            "talk" or "trigger" => new FfxiClientCommand(FfxiClientCommandKind.Talk, name, rest),

            // Said rather than sent: a bug report goes to the people building
            // this, not to the zone.
            "bug" or "report" => rest.Length == 0
                ? new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name,
                                        $"needs something to report, as /{name} the torch is not lit")
                : new FfxiClientCommand(FfxiClientCommandKind.Bug, name, rest),

            // The channels, under the names the real client answers to. Every
            // one has a short form because nobody types /linkshell twice.
            "say" or "s" => Speak(name, rest, FfxiChatKind.Say),
            "shout" or "sh" => Speak(name, rest, FfxiChatKind.Shout),
            "yell" or "y" => Speak(name, rest, FfxiChatKind.Yell),
            "party" or "p" => Speak(name, rest, FfxiChatKind.Party),
            "linkshell" or "l" or "ls" => Speak(name, rest, FfxiChatKind.Linkshell1),
            "linkshell2" or "l2" or "ls2" => Speak(name, rest, FfxiChatKind.Linkshell2),
            "unity" or "u" => Speak(name, rest, FfxiChatKind.Unity),
            "emote" or "em" or "me" => Speak(name, rest, FfxiChatKind.Emote),

            // A tell needs someone to tell. The name is the first word and
            // everything after it is the message, which is why this cannot go
            // through Speak.
            // A pose rather than anything the server is told about. /stand
            // is the way back out for somebody who would rather type it than
            // walk, which is how the real client does it too.
            "sit" or "sitchair" => new FfxiClientCommand(FfxiClientCommandKind.Sit, name, rest),
            "attack" or "at" or "engage" => new FfxiClientCommand(FfxiClientCommandKind.Engage, name, rest),
            "attackoff" or "disengage" => new FfxiClientCommand(FfxiClientCommandKind.Disengage, name, rest),
            "draw" or "unsheathe" or "sheathe" => new FfxiClientCommand(FfxiClientCommandKind.Draw, name, rest),
            "stand" => new FfxiClientCommand(FfxiClientCommandKind.Stand, name, rest),

            "tell" or "t" or "whisper" or "w" or "send" => Whisper(name, rest),

            _ => new FfxiClientCommand(FfxiClientCommandKind.Unsupported, name, rest),
        };
    }

    /// <summary>A channel command, or a complaint if there was nothing to say.</summary>
    private static FfxiClientCommand Speak(string name, string rest, FfxiChatKind channel) =>
        rest.Length == 0
            ? new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name, "needs something to say")
            : new FfxiClientCommand(FfxiClientCommandKind.Chat, name, rest, channel);

    /// <summary>
    /// A tell, split into who and what.
    ///
    /// <para>
    /// The name is the first word, and everything after it is the message -
    /// which is the rule the retail client follows and the reason a character
    /// whose name has a space in it can never be told anything there. Such
    /// characters do exist: a GM command makes one without going near the rules
    /// the creation screen applies, and the server carries the space happily
    /// enough - the 15-byte recipient field in a 0x0B6 has no trouble with it.
    /// It is only the typing that has nowhere to put it.
    /// </para>
    ///
    /// <para>
    /// So the name may also be quoted, which gives this client a way to say
    /// something retail cannot:
    /// </para>
    ///
    /// <code>/tell "Donald Trump" hello</code>
    ///
    /// <para>
    /// Quoting costs the ordinary case nothing, because a quote is not a
    /// character any name may contain.
    /// </para>
    /// </summary>
    private static FfxiClientCommand Whisper(string name, string rest)
    {
        string recipient;
        string message;

        if (rest.Length > 0 && (rest[0] == '"' || rest[0] == '\''))
        {
            char quote = rest[0];
            int close = rest.IndexOf(quote, 1);
            if (close < 0)
            {
                return new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name,
                                             $"is missing the closing {quote} after the name");
            }

            recipient = rest[1..close];
            message = rest[(close + 1)..].Trim();
        }
        else
        {
            int space = rest.IndexOf(' ');
            if (space <= 0)
            {
                return new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name,
                                             $"needs a name and a message, as /{name} Someone hello - " +
                                             $"or /{name} \"Two Words\" hello for a name with a space in it");
            }

            recipient = rest[..space];
            message = rest[(space + 1)..].Trim();
        }

        if (recipient.Length == 0)
        {
            return new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name, "needs somebody to tell");
        }

        return message.Length == 0
            ? new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name, "needs a message after the name")
            : new FfxiClientCommand(FfxiClientCommandKind.Tell, name, message, FfxiChatKind.Say, recipient);
    }
}
