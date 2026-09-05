#include "filetable.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
// For the registry, which is where PlayOnline records the install path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ffxi
{
namespace
{
std::vector<uint8_t> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file)
    {
        throw std::runtime_error("could not open " + path.string());
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> buffer(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    return buffer;
}
} // namespace

FileTable::FileTable(std::filesystem::path installRoot) : root_(std::move(installRoot))
{
    vtable_ = readWholeFile(root_ / "VTABLE.DAT");
    ftable_ = readWholeFile(root_ / "FTABLE.DAT");
    if (ftable_.size() != vtable_.size() * 2)
    {
        throw std::runtime_error("VTABLE and FTABLE disagree on how many ids there are");
    }

    // Then each expansion over the top, later ones winning - see the header.
    // An absent pair is an expansion that is not installed, which is normal;
    // a pair the wrong shape is skipped rather than fatal, so one broken
    // expansion does not take the whole install with it.
    for (int rom = 2; rom <= kLastRom; ++rom)
    {
        const std::filesystem::path folder = root_ / ("ROM" + std::to_string(rom));
        const std::filesystem::path vPath = folder / ("VTABLE" + std::to_string(rom) + ".DAT");
        const std::filesystem::path fPath = folder / ("FTABLE" + std::to_string(rom) + ".DAT");
        std::error_code ignored;
        if (!std::filesystem::exists(vPath, ignored) || !std::filesystem::exists(fPath, ignored))
        {
            continue;
        }

        const std::vector<uint8_t> vtable = readWholeFile(vPath);
        const std::vector<uint8_t> ftable = readWholeFile(fPath);
        if (vtable.size() != vtable_.size() || ftable.size() != ftable_.size())
        {
            std::printf("filetable: ROM%d's tables do not match the base install's; ignoring them\n", rom);
            continue;
        }

        for (size_t id = 0; id < vtable.size(); ++id)
        {
            if (vtable[id] != 0)
            {
                vtable_[id] = vtable[id];
                ftable_[id * 2] = ftable[id * 2];
                ftable_[id * 2 + 1] = ftable[id * 2 + 1];
            }
        }
    }
}

std::optional<std::filesystem::path> FileTable::path(size_t fileId) const
{
    if (fileId >= vtable_.size())
    {
        return std::nullopt;
    }
    const uint8_t rom = vtable_[fileId];
    if (rom == 0)
    {
        return std::nullopt;
    }

    uint16_t packed = 0;
    std::memcpy(&packed, ftable_.data() + fileId * 2, sizeof(packed));

    // ROM 1 lives in a folder called plain "ROM"; the rest carry their number.
    const std::string folder = rom == 1 ? "ROM" : "ROM" + std::to_string(rom);
    const std::filesystem::path relative =
        std::filesystem::path{folder} / std::to_string(packed >> 7) / (std::to_string(packed & 0x7F) + ".DAT");

    // A replacement, if somebody has put one there. See replacementRoot().
    if (const std::filesystem::path& over = replacementRoot(); !over.empty())
    {
        std::error_code ignored;
        const std::filesystem::path candidate = over / relative;
        if (std::filesystem::exists(candidate, ignored))
        {
            return candidate;
        }
    }

    return root_ / relative;
}

const std::filesystem::path& FileTable::replacementRoot()
{
    // Read once. A miss costs a stat per file id otherwise, and the answer
    // cannot change while the client runs.
    static const std::filesystem::path root = [] {
        const char* set = std::getenv("MOGHOUSE_DAT_REPLACEMENTS");
        if (set == nullptr || *set == '\0')
        {
            return std::filesystem::path{};
        }

        std::error_code ignored;
        std::filesystem::path given{set};
        if (!std::filesystem::is_directory(given, ignored))
        {
            std::printf("no DAT replacements at %s\n", set);
            return std::filesystem::path{};
        }

        std::printf("DAT replacements: %s\n", set);
        return given;
    }();

    return root;
}

#ifdef _WIN32
namespace
{
/// The install path PlayOnline recorded, or empty.
///
/// Under InstallFolder the values are numbered rather than named: 0001 is Final
/// Fantasy XI and 1000 is the PlayOnline viewer. Both the US and the JP/EU keys
/// are worth asking, and a 32-bit installer on a 64-bit machine lands under
/// WOW6432Node - which is where it actually is on a normal install.
std::filesystem::path installFromRegistry()
{
    static const wchar_t* kKeys[] = {
        L"SOFTWARE\\WOW6432Node\\PlayOnlineUS\\InstallFolder",
        L"SOFTWARE\\WOW6432Node\\PlayOnline\\InstallFolder",
        L"SOFTWARE\\PlayOnlineUS\\InstallFolder",
        L"SOFTWARE\\PlayOnline\\InstallFolder",
    };

    for (const wchar_t* key : kKeys)
    {
        HKEY handle = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle) != ERROR_SUCCESS)
        {
            continue;
        }

        wchar_t value[MAX_PATH] = {};
        DWORD size = sizeof(value);
        DWORD type = 0;
        const LSTATUS read = RegQueryValueExW(handle, L"0001", nullptr, &type,
                                              reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(handle);

        if (read == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0')
        {
            std::filesystem::path found{value};
            if (std::filesystem::exists(found))
            {
                return found;
            }
        }
    }

    return {};
}
} // namespace
#endif

bool looksLikeInstall(const std::filesystem::path& root)
{
    // The two index files and the data they index. Checking for the directory
    // alone accepts an empty folder someone made by hand, and then every later
    // failure is about a missing DAT rather than about the wrong folder.
    std::error_code ignored;
    return std::filesystem::exists(root / "FTABLE.DAT", ignored) &&
           std::filesystem::exists(root / "VTABLE.DAT", ignored) &&
           std::filesystem::is_directory(root / "ROM", ignored);
}

std::filesystem::path defaultInstallRoot()
{
    // Told outright. The app sets this when someone picks a folder, so it has
    // to win over anything found by looking.
    if (const char* fromEnv = std::getenv("MOGHOUSE_FFXI_INSTALL"))
    {
        return std::filesystem::path{fromEnv};
    }

#ifdef _WIN32
    // Ask PlayOnline where it put the game rather than guessing.
    if (std::filesystem::path recorded = installFromRegistry(); !recorded.empty())
    {
        return recorded;
    }
#endif

    // And otherwise look. There is no registry off Windows, and plenty of
    // people have the files as a folder they copied rather than an install -
    // including everyone running this under Wine, where the game sits inside a
    // prefix at a path Windows would recognise but the host does not.
    std::vector<std::filesystem::path> candidates;

    const auto addHome = [&candidates](const char* variable, std::initializer_list<const char*> tails) {
        const char* home = std::getenv(variable);
        if (!home)
        {
            return;
        }
        for (const char* tail : tails)
        {
            candidates.push_back(std::filesystem::path{home} / tail);
        }
    };

    candidates.emplace_back("C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI");
    candidates.emplace_back("C:/Program Files/PlayOnline/SquareEnix/FINAL FANTASY XI");

    addHome("HOME", {
                        "FINAL FANTASY XI",
                        "Games/FINAL FANTASY XI",
                        "Library/Application Support/FINAL FANTASY XI",
                        ".wine/drive_c/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI",
                        ".wine/drive_c/Program Files/PlayOnline/SquareEnix/FINAL FANTASY XI",
                        "Library/Application Support/CrossOver/Bottles/FFXI/drive_c/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI",
                    });
    addHome("USERPROFILE", {"FINAL FANTASY XI", "Games/FINAL FANTASY XI"});

    // Beside the client itself, which is how a folder of files travels.
    std::error_code ignored;
    if (std::filesystem::path here = std::filesystem::current_path(ignored); !ignored)
    {
        candidates.push_back(here / "FINAL FANTASY XI");
        candidates.push_back(here / "ffxi");
    }

    for (const std::filesystem::path& candidate : candidates)
    {
        if (looksLikeInstall(candidate))
        {
            return candidate;
        }
    }

    // Nothing found. Returning the old constant keeps the failure where it was
    // - a missing file, named - rather than turning it into an empty path.
    return std::filesystem::path{"C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI"};
}
} // namespace ffxi
