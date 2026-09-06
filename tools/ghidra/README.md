# Reading the client binary with Ghidra, headless

The decompiler is the tool for reading logic in `FFXiMain.dll`, but it does not
need the Ghidra GUI or any MCP bridge. Once the unpacked DLL
(`tools/unpackmain.py`) has been imported into a Ghidra project once - the GUI
does this, and analysis is saved - the decompiler runs from the command line.

## One-time: make the project

Open Ghidra, import `FFXiMain.unpacked.dll`, let auto-analysis finish, save.
That leaves a project (e.g. `~/xiasdw.gpr` / `~/xiasdw.rep`) with the analysis
baked in.

## Decompiling functions

`DecompDump.java` reads a list of hex addresses (one per line, `#` comments
allowed) from `$MOGHOUSE_TARGETS` and writes their decompiled C to
`$MOGHOUSE_OUT`:

```sh
printf '100aeb90\n100ae1f0\n' > /tmp/targets.txt
JAVA_HOME=/opt/homebrew/opt/openjdk@21 \
  /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless \
  ~ xiasdw -process FFXiMain.unpacked.dll -noanalysis \
  -scriptPath tools/ghidra -postScript DecompDump.java
```

(`~ xiasdw` is the project's directory and name; `-noanalysis` reuses the saved
analysis rather than redoing it.)

## Finding the addresses

`tools/dllstrings.py` finds them without Ghidra at all - strings, absolute
cross-references, and float constants:

```sh
tools/dllstrings.py <unpacked.dll> strings "EventData"     # where a string is
tools/dllstrings.py <unpacked.dll> xref 0x1035d0ac         # who points at it
tools/dllstrings.py <unpacked.dll> float 85.0              # where a float lives
```

Call-site callers (relative `E8` calls, which the absolute xref above misses)
are a short scan; see the event-system trace in
[../../docs/wiki/Cutscenes.md](../../docs/wiki/Cutscenes.md) for how the two were
used together to walk from the event file loader to the opcode VM.

The unpacked DLL and the Ghidra project are derived from a proprietary binary -
keep both out of the repository.
