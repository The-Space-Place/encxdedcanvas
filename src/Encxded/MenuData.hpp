// EncxdedCanvas: data behind the main menu (schedule, goal cycle, sessions, trash index).
// Stored as one JSON file in the config directory. Canvas links are paths relative to the saves folder,
// using '/' separators, so they survive moving the config directory and restoring backups.
#pragma once
#include <nlohmann/json.hpp>
#include <SDL3/SDL_time.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Encxded {

typedef uint64_t ItemID;

SDL_Time now_time();
// Days since 1970-01-01 in local time, so "due today" flips at local midnight
int64_t local_day_number(SDL_Time t);
int64_t today_day_number();
// "Sep 3"
std::string short_date_string(SDL_Time t);
// "Sep 3, 2026"
std::string long_date_string(SDL_Time t);

struct ScheduleColumn {
    ItemID id = 0;
    std::string name;
    int intervalDays = 1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ScheduleColumn, id, name, intervalDays)
};

struct ScheduleCard {
    ItemID id = 0;
    ItemID columnId = 0;
    std::string title;
    std::string canvasPath; // Relative to the saves folder, empty if not linked
    SDL_Time enteredColumnTime = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ScheduleCard, id, columnId, title, canvasPath, enteredColumnTime)
};

struct TemplateColumn {
    std::string name;
    int intervalDays = 1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TemplateColumn, name, intervalDays)
};

// A saved set of columns that can be applied to the board
struct ScheduleTemplate {
    ItemID id = 0;
    std::string name;
    std::vector<TemplateColumn> columns;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ScheduleTemplate, id, name, columns)
};

struct GoalEntry {
    ItemID id = 0;
    std::string title;
    std::string subtitle;
    SDL_Time date = 0;
    bool done = false;
    std::string canvasPath; // Set for entries added with "Add Canvas"
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GoalEntry, id, title, subtitle, date, done, canvasPath)
};

struct GoalColumn {
    ItemID id = 0;
    std::string name;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GoalColumn, id, name)
};

struct Goal {
    ItemID id = 0;
    ItemID columnId = 0;
    std::string title;
    int progress = 0; // 0 to 100
    std::vector<GoalEntry> entries;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Goal, id, columnId, title, progress, entries)
};

struct GoalTemplateEntry {
    std::string title;
    std::string subtitle;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GoalTemplateEntry, title, subtitle)
};

// A goal title plus entries that a new goal can be created from
struct GoalTemplate {
    ItemID id = 0;
    std::string name;
    std::vector<GoalTemplateEntry> entries;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GoalTemplate, id, name, entries)
};

enum class SessionItemType : int {
    CHECK,
    TEXT,   // Label plus a text field, e.g. "Set Study Goal"
    RATING, // Label plus 1-5, e.g. "Rate Energy"
    TIMER   // Pomodoro / Flowmodoro picker. Only one, in the Before list
};

enum class SessionPhase : int {
    BEFORE,
    FOCUSED,
    AFTER
};

enum class TimerMode : int {
    POMODORO,  // Counts down from pomodoroMinutes
    FLOWMODORO // Counts up until stopped; break suggestion is a fifth of the focus time
};

struct SessionItem {
    ItemID id = 0;
    SessionItemType type = SessionItemType::CHECK;
    std::string label;
    bool checked = false;
    std::string text;
    int rating = 0; // 0 = not rated
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SessionItem, id, type, label, checked, text, rating)
};

struct ActiveSession {
    bool active = false;
    SessionPhase phase = SessionPhase::BEFORE;
    TimerMode mode = TimerMode::POMODORO;
    int minutes = 25;
    SDL_Time focusStart = 0;
    SDL_Time focusEnd = 0;       // Set once the focus phase ends
    int64_t pausedNs = 0;        // Total time spent paused before the current pause
    bool paused = false;
    SDL_Time pauseStart = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ActiveSession, active, phase, mode, minutes, focusStart, focusEnd, pausedNs, paused, pauseStart)

    int64_t focused_ns(SDL_Time now) const;
};

struct SessionRecord {
    SDL_Time start = 0;
    SDL_Time end = 0;
    int64_t focusedNs = 0;
    TimerMode mode = TimerMode::POMODORO;
    std::string goal;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SessionRecord, start, end, focusedNs, mode, goal)
};

struct SessionsData {
    std::vector<SessionItem> before;
    std::vector<SessionItem> focused;
    std::vector<SessionItem> after;
    TimerMode timerMode = TimerMode::POMODORO;
    int pomodoroMinutes = 25;
    bool sound = true;
    ActiveSession active;
    std::vector<SessionRecord> history;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SessionsData, before, focused, after, timerMode, pomodoroMinutes, sound, active, history)

    std::vector<SessionItem>& list(SessionPhase p);
};

struct TrashEntry {
    std::string trashName;    // File or folder name inside the trash folder
    std::string originalPath; // Relative to the saves folder
    bool isFolder = false;
    SDL_Time trashTime = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TrashEntry, trashName, originalPath, isFolder, trashTime)
};

enum class NotesSort : int {
    NAME_ASCENDING,
    NAME_DESCENDING,
    MODIFIED_NEWEST,
    MODIFIED_OLDEST
};

// Video tab of the export panel. One set for the whole app: the last used options.
struct VideoExportOptions {
    int shortSide = 1080; // 720 / 1080 / 1440 / 2160; the long side follows the recording's aspect ratio
    int fps = 30;
    bool includeUI = true; // UI layer, tool overlays and pen indicator
    bool smoothCamera = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(VideoExportOptions, shortSide, fps, includeUI, smoothCamera)
};

struct MenuData {
    ItemID nextId = 1;

    std::vector<ScheduleColumn> scheduleColumns;
    std::vector<ScheduleCard> scheduleCards;
    std::vector<ScheduleTemplate> scheduleTemplates;

    std::vector<GoalColumn> goalColumns;
    std::vector<Goal> goals;
    std::vector<GoalTemplate> goalTemplates;

    SessionsData sessions;

    std::vector<TrashEntry> trash;

    NotesSort notesSort = NotesSort::NAME_ASCENDING;
    bool sidebarOpen = true;
    bool recordMic = false; // Last mic choice for recordings
    VideoExportOptions videoExport;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MenuData, nextId, scheduleColumns, scheduleCards, scheduleTemplates, goalColumns, goals, goalTemplates, sessions, trash, notesSort, sidebarOpen, recordMic, videoExport)

    ItemID new_id();
    void set_defaults();
    void fill_missing_defaults();

    ScheduleColumn* find_schedule_column(ItemID id);
    ScheduleCard* find_schedule_card(ItemID id);
    Goal* find_goal(ItemID id);
    GoalEntry* find_goal_entry(Goal& g, ItemID id);
    SessionItem* find_session_item(ItemID id, SessionPhase* phaseOut = nullptr);

    // Days until the card is due (negative = overdue)
    int64_t card_days_until_due(const ScheduleCard& card);
    // Moves the card to the next column (or keeps it in the last one) and restarts its interval
    void mark_card_retrieved(ItemID cardId);
    void move_card_to_column(ItemID cardId, ItemID columnId, std::optional<size_t> indexInColumn = std::nullopt);
    void delete_schedule_column(ItemID columnId);
    void apply_schedule_template(const ScheduleTemplate& t);

    void move_goal_to_column(ItemID goalId, ItemID columnId, std::optional<size_t> indexInColumn = std::nullopt);
    void delete_goal_column(ItemID columnId);

    // Keeps schedule and goal links pointing at a canvas (or everything inside a folder) after it moves
    void rename_canvas_links(const std::string& oldPath, const std::string& newPath, bool isFolder);

    // Restoring a backup: adds the other data's cards, goals, templates, session items and history alongside these.
    // An item with the same ID and title as an existing one is treated as the same item and skipped (the current version wins).
    // Items in a column that doesn't exist here go to the same-named column, or that column is appended.
    // The trash index and the active session aren't merged. Returns the number of items added.
    size_t merge_from(const MenuData& other);
};

NLOHMANN_JSON_SERIALIZE_ENUM(SessionItemType, {
    {SessionItemType::CHECK, "check"},
    {SessionItemType::TEXT, "text"},
    {SessionItemType::RATING, "rating"},
    {SessionItemType::TIMER, "timer"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(SessionPhase, {
    {SessionPhase::BEFORE, "before"},
    {SessionPhase::FOCUSED, "focused"},
    {SessionPhase::AFTER, "after"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(TimerMode, {
    {TimerMode::POMODORO, "pomodoro"},
    {TimerMode::FLOWMODORO, "flowmodoro"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(NotesSort, {
    {NotesSort::NAME_ASCENDING, "nameAscending"},
    {NotesSort::NAME_DESCENDING, "nameDescending"},
    {NotesSort::MODIFIED_NEWEST, "modifiedNewest"},
    {NotesSort::MODIFIED_OLDEST, "modifiedOldest"},
})

// Owns the menu data and writes it to disk shortly after it changes
class MenuDataStore {
    public:
        static constexpr const char* FILE_NAME = "encxdedMenu.json";

        void load(const std::filesystem::path& configPath);
        void save();
        void mark_dirty();
        void update(); // Call every frame; saves once changes have settled

        MenuData data;
        std::filesystem::path filePath;
    private:
        bool dirty = false;
        SDL_Time dirtySince = 0;
};

}
