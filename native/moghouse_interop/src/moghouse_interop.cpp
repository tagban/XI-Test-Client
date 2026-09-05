#include "moghouse_interop.h"

#include "viewer.h"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
/// Copies a borrowed C string, treating null as absent.
std::optional<std::string> borrow(const char* text)
{
    if (!text)
    {
        return std::nullopt;
    }
    return std::string{text};
}

std::string borrowOr(const char* text, const char* fallback)
{
    return text ? std::string{text} : std::string{fallback};
}

/// Sends this library's stdout and stderr to a file beside the app's log.
///
/// The app is a windowed executable with no console, and redirecting
/// Console.Out on the managed side does nothing for the C runtime in here - so
/// every printf the renderer makes goes to a handle that leads nowhere. Zone
/// loads, texture counts, the reason a model failed to build: all of it exists
/// and none of it is readable.
///
/// Its own file, not the app's. The managed side holds MOGHOUSE_LOG open with
/// a StreamWriter, which shares for reading only, so opening it for writing
/// here fails - and two writers on one file would tread on each other in any
/// case, because each keeps its own idea of where the end is.
///
/// The probe matters more than it looks. freopen closes the stream *before* it
/// tries to open the new target, so a failed freopen leaves stdout shut and
/// every later printf writing to a dead handle - which is exactly what stopped
/// the world window from opening at all. Nothing is redirected unless a plain
/// open has already proved it can be done.
void redirectOutputToLog()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;

    const char* base = std::getenv("MOGHOUSE_RENDERER_LOG");
    std::string path = base ? std::string{base} : std::string{};
    if (path.empty())
    {
        const char* appLog = std::getenv("MOGHOUSE_LOG");
        if (!appLog || !*appLog)
        {
            return;
        }
        path = std::string{appLog} + ".renderer";
    }

    if (FILE* probe = std::fopen(path.c_str(), "w"))
    {
        std::fclose(probe);
    }
    else
    {
        return;                      // cannot write there; leave stdout alone
    }

    if (std::freopen(path.c_str(), "w", stdout))
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf("renderer: output attached to %s\n", path.c_str());
    }
    if (std::freopen(path.c_str(), "a", stderr))
    {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}
} // namespace

/// The options are copied at create time and the link outlives every call, so
/// the caller is free to release its own strings immediately and nothing here
/// reaches back into managed memory.
struct MhViewer
{
    mh::ViewerOptions options;
    mh::ViewerLink link;
};

extern "C" {

MhViewerHandle mh_viewer_create(const MhViewerOptions* options)
{
    if (!options)
    {
        return nullptr;
    }

    redirectOutputToLog();

    auto* viewer = new MhViewer{};
    viewer->options.zonePath = borrowOr(options->zone_path, "");
    viewer->options.keyTablePath = borrowOr(options->key_table_path, "");
    viewer->options.keyTable2Path = borrowOr(options->key_table2_path, "");
    viewer->options.look = borrow(options->look);
    if (options->server_clock != 0)
    {
        viewer->options.serverClock = options->server_clock;
    }
    viewer->options.characterAt = borrow(options->character_at);
    viewer->options.characterFacing = borrow(options->character_facing);
    viewer->options.zoneName = borrow(options->zone_name);
    viewer->options.playerName = borrow(options->player_name);
    if (options->time_of_day >= 0)
    {
        viewer->options.timeOfDay = options->time_of_day;
    }
    viewer->options.screenshotPath = borrow(options->screenshot_path);
    if (options->screenshot_after_frames > 0)
    {
        viewer->options.settleFrames = options->screenshot_after_frames;
    }
    return viewer;
}

int32_t mh_viewer_run(MhViewerHandle viewer)
{
    if (!viewer)
    {
        return 2;
    }

    // Nothing may cross this boundary as an exception. A C++ throw unwinding
    // into managed code arrives as an opaque SEHException with no message,
    // which turns "the key table path was wrong" into "external component has
    // thrown an exception".
    try
    {
        return static_cast<int32_t>(mh::runViewer(viewer->options, &viewer->link));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "renderer: %s\n", error.what());
        std::fflush(stderr);
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "renderer: unknown failure\n");
        std::fflush(stderr);
        return 1;
    }
}

void mh_viewer_set_entities(MhViewerHandle viewer, const MhRadarEntity* entities, int32_t count)
{
    if (!viewer)
    {
        return;
    }

    std::vector<mh::RadarEntity> copied;
    if (entities && count > 0)
    {
        copied.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            // Bounded by the field width in case the caller filled it without
            // a terminator.
            const char* raw = entities[i].name;
            const size_t length = static_cast<size_t>(std::find(raw, raw + sizeof(entities[i].name), '\0') - raw);
            mh::RadarEntity entity{entities[i].x,    entities[i].z,
                                   entities[i].y,    entities[i].heading,
                                   entities[i].kind, std::string{raw, length},
                                   entities[i].id,   entities[i].nameHidden != 0};
            std::memcpy(entity.look, entities[i].look, sizeof(entity.look));
            entity.gmLevel = entities[i].gmLevel;
            entity.modelId = static_cast<uint16_t>(entities[i].modelId);
            entity.healthPercent = entities[i].healthPercent;
            entity.triggerable = entities[i].triggerable != 0;
            entity.silhouette = entities[i].silhouette;
            entity.size = entities[i].size;
            entity.spawnedSecondsAgo = entities[i].spawned_seconds_ago;
            copied.push_back(std::move(entity));
        }
    }
    viewer->link.setEntities(std::move(copied));
}

void mh_viewer_set_zone_lines(MhViewerHandle viewer, const MhZoneLine* lines, int32_t count)
{
    if (!viewer)
    {
        return;
    }

    std::vector<mh::ZoneLineMarker> copied;
    if (lines && count > 0)
    {
        copied.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            copied.push_back(mh::ZoneLineMarker{lines[i].x, lines[i].y, lines[i].z, lines[i].radius});
        }
    }
    viewer->link.setZoneLines(std::move(copied));
}

void mh_viewer_set_death(MhViewerHandle viewer, int32_t dead, int32_t raise_offered)
{
    if (viewer)
    {
        viewer->link.setDeath(dead != 0, raise_offered != 0);
    }
}

void mh_viewer_set_vitals(MhViewerHandle viewer, uint32_t hp, uint32_t mp, uint32_t tp,
                          uint8_t hp_percent, uint8_t mp_percent)
{
    if (viewer)
    {
        viewer->link.setVitals(hp, mp, tp, hp_percent, mp_percent);
    }
}

void mh_viewer_set_music(MhViewerHandle viewer, const char* path)
{
    if (viewer)
    {
        viewer->link.setMusic(path ? std::string{path} : std::string{});
    }
}

void mh_viewer_load_zone(MhViewerHandle viewer, const char* dat_path, const char* zone_name, float x, float y,
                         float z, float heading)
{
    if (viewer && dat_path)
    {
        viewer->link.requestZone({dat_path, zone_name ? zone_name : "", x, y, z, heading});
    }
}

int32_t mh_viewer_is_loading(MhViewerHandle viewer)
{
    return viewer && viewer->link.loading() ? 1 : 0;
}

void mh_viewer_set_settings(MhViewerHandle viewer, float music_volume, float sound_volume, float ui_scale,
                            int32_t radar_turns)
{
    if (viewer)
    {
        viewer->link.applySettings({music_volume, sound_volume, radar_turns != 0, ui_scale});
    }
}

int32_t mh_viewer_take_settings(MhViewerHandle viewer, float* music_volume, float* sound_volume, float* ui_scale,
                                int32_t* radar_turns)
{
    if (!viewer || !viewer->link.settingsChanged())
    {
        return 0;
    }
    const auto settings = viewer->link.settings();
    if (music_volume)
    {
        *music_volume = settings.musicVolume;
    }
    if (sound_volume)
    {
        *sound_volume = settings.soundVolume;
    }
    if (ui_scale)
    {
        *ui_scale = settings.uiScale;
    }
    if (radar_turns)
    {
        *radar_turns = settings.radarTurns ? 1 : 0;
    }
    return 1;
}

int32_t mh_viewer_take_link(MhViewerHandle viewer)
{
    return viewer ? static_cast<int32_t>(viewer->link.takeLink()) : 0;
}

int32_t mh_viewer_take_death_choice(MhViewerHandle viewer)
{
    return viewer ? static_cast<int32_t>(viewer->link.takeDeathChoice()) : MH_DEATH_NONE;
}

uint32_t mh_viewer_take_talk(MhViewerHandle viewer)
{
    if (!viewer)
    {
        return 0;
    }

    uint32_t entityId = 0;
    return viewer->link.takeTalk(entityId) ? entityId : 0;
}

void mh_viewer_push_chat(MhViewerHandle viewer, const char* line, int32_t tone)
{
    if (!viewer || !line)
    {
        return;
    }
    viewer->link.pushChat(std::string{line}, static_cast<mh::ChatTone>(tone));
}

void mh_viewer_set_inventory(MhViewerHandle viewer, const MhInventorySlot* slots, int32_t count,
                             const uint16_t* sizes, int32_t size_count)
{
    if (!viewer || (count > 0 && !slots))
    {
        return;
    }

    // The two layouts are the same fields in the same order, but they are
    // separate types on purpose: the C header is the contract and the C++
    // struct is free to change. Copied field by field rather than cast.
    std::vector<mh::ViewerLink::InventorySlot> converted;
    converted.reserve(static_cast<size_t>(count > 0 ? count : 0));
    for (int32_t i = 0; i < count; ++i)
    {
        converted.push_back(mh::ViewerLink::InventorySlot{slots[i].container, slots[i].slot,
                                                          slots[i].item_id, slots[i].count});
    }

    viewer->link.setInventory(converted.data(), static_cast<int>(converted.size()),
                              sizes, sizes ? size_count : 0);
}

int32_t mh_viewer_take_inventory_action(MhViewerHandle viewer, int32_t* kind, int32_t* container,
                                        int32_t* slot, int32_t* equip_slot, uint32_t* count)
{
    mh::ViewerLink::InventoryAction action;
    if (!viewer || !viewer->link.takeInventoryAction(action))
    {
        return 0;
    }

    if (kind)
    {
        *kind = static_cast<int32_t>(action.kind);
    }
    if (container)
    {
        *container = action.container;
    }
    if (slot)
    {
        *slot = action.slot;
    }
    if (equip_slot)
    {
        *equip_slot = action.equipSlot;
    }
    if (count)
    {
        *count = action.count;
    }
    return 1;
}

void mh_viewer_set_resting(MhViewerHandle viewer, int32_t resting)
{
    if (viewer)
    {
        viewer->link.setResting(resting != 0);
    }
}

void mh_viewer_set_sitting(MhViewerHandle viewer, int32_t sitting)
{
    if (viewer)
    {
        viewer->link.setSitting(sitting != 0);
    }
}

void mh_viewer_set_character_stats(MhViewerHandle viewer, const MhCharacterStats* stats)
{
    if (!viewer || !stats)
    {
        return;
    }

    mh::ViewerLink::CharacterStats held;
    held.mainJob = stats->main_job;
    held.subJob = stats->sub_job;
    held.mainLevel = stats->main_level;
    held.subLevel = stats->sub_level;
    held.maxHp = stats->max_hp;
    held.maxMp = stats->max_mp;
    for (int i = 0; i < 7; ++i)
    {
        held.base[i] = stats->base_stats[i];
        held.modifier[i] = stats->stat_modifiers[i];
    }
    held.known = true;
    viewer->link.setCharacterStats(held);
}

void mh_viewer_set_equipment(MhViewerHandle viewer, const uint8_t* containers, const uint8_t* slots,
                            int32_t count)
{
    if (viewer && containers && slots)
    {
        viewer->link.setEquipment(containers, slots, count);
    }
}

void mh_viewer_push_item(MhViewerHandle viewer, uint16_t item_id, const char* name,
                         const char* description, uint16_t type, uint16_t level, uint32_t slots,
                         const uint8_t* rgba, int32_t width, int32_t height)
{
    if (!viewer || width <= 0 || height <= 0 || !rgba)
    {
        return;
    }

    mh::ViewerLink::ItemFace face;
    face.itemId = item_id;
    face.name = name ? name : "";
    face.description = description ? description : "";
    face.type = type;
    face.level = level;
    face.slots = slots;
    face.width = width;
    face.height = height;
    face.rgba.assign(rgba, rgba + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
    viewer->link.pushItemFace(std::move(face));
}

int32_t mh_viewer_get_character(MhViewerHandle viewer, float* x, float* y, float* z, float* heading)
{
    if (!viewer || !x || !y || !z || !heading)
    {
        return 0;
    }
    return viewer->link.character(*x, *y, *z, *heading) ? 1 : 0;
}

int32_t mh_viewer_take_jump(MhViewerHandle viewer)
{
    return viewer && viewer->link.takeJump() ? 1 : 0;
}

void mh_viewer_place_character(MhViewerHandle viewer, float x, float y, float z, float heading)
{
    if (viewer)
    {
        viewer->link.placeCharacter(x, y, z, heading);
    }
}

int32_t mh_viewer_take_chat(MhViewerHandle viewer, char* buffer, int32_t capacity)
{
    if (!viewer || !buffer || capacity <= 0)
    {
        return 0;
    }

    std::optional<std::string> line = viewer->link.takeChat();
    if (!line)
    {
        return 0;
    }

    const size_t room = static_cast<size_t>(capacity) - 1;
    const size_t length = line->size() < room ? line->size() : room;
    std::memcpy(buffer, line->data(), length);
    buffer[length] = '\0';
    return 1;
}

void mh_viewer_stop(MhViewerHandle viewer)
{
    if (viewer)
    {
        viewer->link.stop();
    }
}

void mh_viewer_set_player(MhViewerHandle viewer, const char* name, const char* look)
{
    if (viewer == nullptr)
    {
        return;
    }

    if (name != nullptr)
    {
        viewer->link.setPlayerName(name);
    }

    if (look != nullptr)
    {
        viewer->link.setLook(look);
    }
}

void mh_viewer_set_clock(MhViewerHandle viewer, uint32_t server_clock)
{
    if (viewer != nullptr)
    {
        viewer->link.setServerClock(server_clock);
    }
}

void mh_viewer_set_weather(MhViewerHandle viewer, int32_t weather)
{
    if (viewer != nullptr)
    {
        viewer->link.setWeather(weather);
    }
}

void mh_viewer_capture(MhViewerHandle viewer, const char* path)
{
    if (viewer != nullptr && path != nullptr)
    {
        viewer->link.requestCapture(path);
    }
}

void mh_viewer_set_lineup(MhViewerHandle viewer, int32_t on)
{
    if (viewer != nullptr)
    {
        viewer->link.setLineup(on != 0);
    }
}

void mh_viewer_set_hud(MhViewerHandle viewer, int32_t on)
{
    if (viewer != nullptr)
    {
        viewer->link.setHud(on != 0);
    }
}

void mh_viewer_set_riding(MhViewerHandle viewer, int32_t aboard)
{
    if (viewer != nullptr)
    {
        viewer->link.setRiding(aboard != 0);
    }
}

// Pinned because the C# side declares this layout a second time, by hand, and
// a mismatch would not fail to build on either side - it would just read the
// wrong bytes. If one of these fires, NativeFormRowData needs the same edit.
static_assert(sizeof(MhFormRow) == 200, "MhFormRow layout changed; update NativeFormRowData");
static_assert(offsetof(MhFormRow, kind) == 0, "MhFormRow.kind moved");
static_assert(offsetof(MhFormRow, enabled) == 4, "MhFormRow.enabled moved");
static_assert(offsetof(MhFormRow, text) == 8, "MhFormRow.text moved");
static_assert(offsetof(MhFormRow, value) == 72, "MhFormRow.value moved");

void mh_viewer_set_form(MhViewerHandle viewer, const char* title, const char* message,
                        const MhFormRow* rows, int32_t count)
{
    if (viewer == nullptr)
    {
        return;
    }

    mh::Form form;
    form.title = title ? title : "";
    form.message = message ? message : "";
    form.aside = viewer->link.formAside();

    if (rows != nullptr && count > 0)
    {
        form.rows.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            const MhFormRow& from = rows[i];

            mh::FormRow row;
            row.kind = static_cast<mh::FormRowKind>(from.kind);
            row.enabled = from.enabled != 0;

            // strnlen rather than trusting the terminator: the arrays are
            // fixed width and a caller that filled one exactly would leave no
            // NUL to find.
            row.text.assign(from.text, strnlen(from.text, sizeof(from.text)));
            row.value.assign(from.value, strnlen(from.value, sizeof(from.value)));

            form.rows.push_back(std::move(row));
        }
    }

    viewer->link.setForm(std::move(form));
}

void mh_viewer_set_form_aside(MhViewerHandle viewer, int32_t aside)
{
    if (viewer != nullptr)
    {
        viewer->link.setFormAside(aside != 0);
    }
}

int32_t mh_viewer_take_form_result(MhViewerHandle viewer, int32_t* button, char* values,
                                   int32_t capacity)
{
    if (viewer == nullptr || button == nullptr || values == nullptr || capacity <= 0)
    {
        return 0;
    }

    int pressed = -1;
    std::vector<std::string> taken;
    if (!viewer->link.takeFormResult(pressed, taken))
    {
        return 0;
    }

    // Packed one after another rather than returned one at a time: a form is
    // read once when a button is pressed, and a call per field would mean
    // holding the answer across several crossings while the player carries on
    // typing into it.
    //
    // The count is returned rather than marked in the buffer. An empty value is
    // a lone NUL and every label and button row produces one, so a terminator
    // would end the list at the first caption instead of after the last field.
    int32_t at = 0;
    int32_t written = 0;
    for (const std::string& value : taken)
    {
        const int32_t needed = static_cast<int32_t>(value.size()) + 1;
        if (at + needed > capacity)
        {
            // Out of room. Better to hand back what fits and say the press
            // happened than to lose the press entirely - the alternative is a
            // button that visibly does nothing.
            std::printf("form result did not fit in %d bytes; %zu values were dropped\n", capacity,
                        taken.size() - static_cast<size_t>(written));
            break;
        }
        std::memcpy(values + at, value.c_str(), static_cast<size_t>(needed));
        at += needed;
        ++written;
    }

    *button = pressed;
    return written;
}

void mh_viewer_destroy(MhViewerHandle viewer) { delete viewer; }

namespace
{
/// What the folder chooser produced, filled in by SDL's callback.
struct FolderChoice
{
    bool done{false};
    bool chosen{false};
    std::string path;
};

void SDLCALL onFolderChosen(void* userdata, const char* const* filelist, int /*filter*/)
{
    auto* choice = static_cast<FolderChoice*>(userdata);
    if (choice == nullptr)
    {
        return;
    }

    // A null list is the chooser failing; an empty one is the player pressing
    // cancel. Both leave `chosen` false, and only the first is worth a
    // different answer to the caller - which it gets from the return value
    // below rather than from here.
    if (filelist != nullptr && filelist[0] != nullptr)
    {
        choice->path = filelist[0];
        choice->chosen = true;
    }
    choice->done = true;
}
} // namespace

void mh_show_message(const char* title, const char* body)
{
    if (!title || !body)
    {
        return;
    }

    // Same reason the chooser starts video: on a first run nothing has yet.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        std::printf("could not start SDL video for a message: %s\n", SDL_GetError());
        return;
    }

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, nullptr);
}

int32_t mh_pick_folder(const char* default_location, char* out, int32_t out_size)
{
    if (out == nullptr || out_size <= 0)
    {
        return -1;
    }
    out[0] = '\0';

    // Video is what owns a chooser, and on a first run nothing has started it
    // yet. Init is reference counted, so asking again when the renderer is
    // already up costs nothing and does not tear anything down on the way out.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        std::printf("could not start SDL video for the folder chooser: %s\n", SDL_GetError());
        return -1;
    }

    FolderChoice choice;

    // Titled, because this is the very first thing a new player sees and the
    // bare chooser says only "Select Folder". What to pick was written to the
    // log instead, where nobody looks - the first tester to meet it asked what
    // it wanted. The accept button says so too, for the platforms that use it.
    // Properties are best-effort per platform; if any are refused the dialog
    // still opens, which is why nothing here is checked for failure.
    SDL_PropertiesID props = SDL_CreateProperties();
    if (props)
    {
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                              "MogHouse XI - where is Final Fantasy XI? Pick the folder holding FTABLE.DAT and ROM");
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Use this folder");
        if (default_location != nullptr)
        {
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_location);
        }
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, onFolderChosen, &choice, props);
        SDL_DestroyProperties(props);
    }
    else
    {
        SDL_ShowOpenFolderDialog(onFolderChosen, &choice, nullptr, default_location, false);
    }

    // SDL answers through the callback, and on a first run there is no other
    // loop to deliver it - so this one pumps until the answer arrives.
    while (!choice.done)
    {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    if (!choice.chosen)
    {
        return 0;
    }

    if (static_cast<int32_t>(choice.path.size()) >= out_size)
    {
        std::printf("the chosen folder's path is longer than the caller's buffer\n");
        return -1;
    }

    std::memcpy(out, choice.path.c_str(), choice.path.size() + 1);
    return 1;
}

} // extern "C"
