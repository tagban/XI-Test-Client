#pragma once

// Resolves FFXI file ids to paths, the way the client does. See
// docs/file-index.md.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace ffxi
{
/// The install's own index: a flat file id maps to a path through two tables,
/// one entry per id in each.
///
///     VTABLE.DAT   u8   the ROM number holding it, 0 if it is not installed
///     FTABLE.DAT   u16  (directory << 7) | file
///
/// The pair at the install root describes the original game. Each expansion
/// brings a pair of its own - ROM2/VTABLE2.DAT and FTABLE2.DAT, up through
/// ROM9 - covering the same id range with a non-zero entry only for the files
/// that expansion holds. The client reads them all, later ones overriding
/// earlier, which is how a patch in a later ROM replaces a file from an
/// earlier one. Reading only the base pair makes every expansion zone and
/// model "not installed": Yhoator Jungle's map is file 223, and that is in
/// ROM2.
///
/// MogHouse.Core has the same lookup in C#, and tools/filetable.py in
/// Python. Change one, change all three.
class FileTable
{
public:
    /// The highest-numbered expansion folder looked for.
    static constexpr int kLastRom = 9;

    /// Throws std::runtime_error if the tables are missing or disagree.
    explicit FileTable(std::filesystem::path installRoot);

    size_t size() const { return vtable_.size(); }

    /// The path for a file id, or nothing if that id is not installed.
    std::optional<std::filesystem::path> path(size_t fileId) const;

    /// Where a modder's replacement DATs live, or empty.
    ///
    /// MOGHOUSE_DAT_REPLACEMENTS names a folder mirroring the install's own
    /// layout - `ROM/1/31.DAT`, `ROM9/0/7.DAT` - and a file present there is
    /// used instead of the one in the install. The retail files are never
    /// written to, which is the point: a mod is something you can delete.
    ///
    /// The layout is the install's rather than our own file ids on purpose.
    /// It is what the existing tools distribute, so a mod written for any of
    /// them drops in here unchanged, and it is what the wiki's own paths look
    /// like. See docs/wiki/File-Ids.md.
    static const std::filesystem::path& replacementRoot();

private:
    std::filesystem::path root_;
    std::vector<uint8_t> vtable_;
    std::vector<uint8_t> ftable_;
};

/// Where the retail client is installed, from MOGHOUSE_FFXI_INSTALL, falling
/// back to the usual Windows location.
/// Whether this folder is really an FFXI install: both index files and the
/// data they index. A directory that merely exists is not enough - accept one
/// and every later failure reads as a missing DAT rather than as the wrong
/// folder, which is a much worse thing to hand someone.
bool looksLikeInstall(const std::filesystem::path& root);

/// Where the game is.
///
/// MOGHOUSE_FFXI_INSTALL first, so an app that asked someone to pick a folder
/// wins. Then the registry on Windows, where PlayOnline records it. Then a list
/// of the places a copied folder tends to sit, including Wine and CrossOver
/// prefixes - there is no registry off Windows, and plenty of people have the
/// files without having run an installer at all.
std::filesystem::path defaultInstallRoot();
} // namespace ffxi
