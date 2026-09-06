#include "entitynames.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
/// 28 bytes of name then a 4-byte entity id.
constexpr size_t kRecord = 32;
constexpr size_t kNameLength = 28;

const std::string kNoName;
} // namespace

ffxi::EntityNames ffxi::EntityNames::load(const FileTable& table, uint16_t zoneId)
{
    EntityNames names;

    const auto path = table.path(kEntityNameBase + zoneId);
    if (!path)
    {
        return names;
    }

    std::ifstream file{*path, std::ios::binary};
    if (!file)
    {
        return names;
    }

    std::vector<char> data{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    for (size_t offset = 0; offset + kRecord <= data.size(); offset += kRecord)
    {
        const char* record = data.data() + offset;

        uint32_t id = 0;
        std::memcpy(&id, record + kNameLength, sizeof(id));
        if (id == 0)
        {
            continue;
        }

        // The name runs to the first NUL, and plenty of records are blank
        // placeholders rather than absent - the first entry of every zone is
        // literally "none".
        size_t length = 0;
        while (length < kNameLength && record[length] != '\0')
        {
            ++length;
        }
        if (length == 0)
        {
            continue;
        }

        names.names_.emplace(id, std::string{record, length});
    }

    std::printf("entity names: %zu for zone %u\n", names.names_.size(), zoneId);
    return names;
}

const std::string& ffxi::EntityNames::lookup(uint32_t entityId) const
{
    const auto found = names_.find(entityId);
    return found == names_.end() ? kNoName : found->second;
}
