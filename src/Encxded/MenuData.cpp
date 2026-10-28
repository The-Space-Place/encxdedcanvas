#include "MenuData.hpp"
#include <Helpers/StringHelpers.hpp>
#include <Helpers/Logger.hpp>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <array>

namespace Encxded {

namespace {
    constexpr int64_t NS_PER_SECOND = 1000000000LL;
    constexpr std::array<const char*, 12> MONTH_NAMES = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    // Howard Hinnant's days_from_civil
    int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int64_t>(doe) - 719468;
    }

    template <typename T> T* find_by_id(std::vector<T>& v, ItemID id) {
        auto it = std::find_if(v.begin(), v.end(), [id](const T& a) { return a.id == id; });
        return it == v.end() ? nullptr : &(*it);
    }

    // Moves the element with id to sit at indexInGroup among elements with the same groupId (or to the end of the group)
    template <typename T> void move_to_group(std::vector<T>& v, ItemID id, ItemID groupId, std::optional<size_t> indexInGroup) {
        auto it = std::find_if(v.begin(), v.end(), [id](const T& a) { return a.id == id; });
        if(it == v.end())
            return;
        T moved = *it;
        moved.columnId = groupId;
        v.erase(it);
        size_t seen = 0;
        auto insertAt = v.end();
        for(auto i = v.begin(); i != v.end(); ++i) {
            if(i->columnId != groupId)
                continue;
            if(indexInGroup.has_value() && seen == indexInGroup.value()) {
                insertAt = i;
                break;
            }
            seen++;
            insertAt = i + 1;
        }
        if(!indexInGroup.has_value()) {
            // After the last element in the group, or at the end if the group is empty
            auto last = std::find_if(v.rbegin(), v.rend(), [groupId](const T& a) { return a.columnId == groupId; });
            insertAt = last == v.rend() ? v.end() : last.base();
        }
        v.insert(insertAt, moved);
    }

    bool path_is_inside(const std::string& path, const std::string& folder) {
        return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
    }

    void rename_link(std::string& link, const std::string& oldPath, const std::string& newPath, bool isFolder) {
        if(link.empty())
            return;
        if(link == oldPath)
            link = newPath;
        else if(isFolder && path_is_inside(link, oldPath))
            link = newPath + link.substr(oldPath.size());
    }
}

SDL_Time now_time() {
    SDL_Time t = 0;
    SDL_GetCurrentTime(&t);
    return t;
}

int64_t local_day_number(SDL_Time t) {
    SDL_DateTime dt;
    if(!SDL_TimeToDateTime(t, &dt, true))
        return t / (NS_PER_SECOND * 86400);
    return days_from_civil(dt.year, static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day));
}

int64_t today_day_number() {
    return local_day_number(now_time());
}

std::string short_date_string(SDL_Time t) {
    SDL_DateTime dt;
    if(!SDL_TimeToDateTime(t, &dt, true))
        return "";
    return std::string(MONTH_NAMES[std::clamp(dt.month, 1, 12) - 1]) + " " + std::to_string(dt.day);
}

std::string long_date_string(SDL_Time t) {
    SDL_DateTime dt;
    if(!SDL_TimeToDateTime(t, &dt, true))
        return "";
    return short_date_string(t) + ", " + std::to_string(dt.year);
}

int64_t ActiveSession::focused_ns(SDL_Time now) const {
    if(!active)
        return 0;
    SDL_Time endPoint = phase == SessionPhase::FOCUSED ? (paused ? pauseStart : now) : focusEnd;
    return std::max<int64_t>(0, endPoint - focusStart - pausedNs);
}

std::vector<SessionItem>& SessionsData::list(SessionPhase p) {
    switch(p) {
        case SessionPhase::BEFORE: return before;
        case SessionPhase::FOCUSED: return focused;
        case SessionPhase::AFTER: return after;
    }
    return before;
}

ItemID MenuData::new_id() {
    return nextId++;
}

void MenuData::set_defaults() {
    *this = MenuData();
    fill_missing_defaults();
}

void MenuData::fill_missing_defaults() {
    if(scheduleColumns.empty()) {
        scheduleColumns.push_back({new_id(), "1 Day Retrieval", 1});
        scheduleColumns.push_back({new_id(), "1 Week Retrieval", 7});
        scheduleColumns.push_back({new_id(), "1 Month Retrieval", 30});
    }
    if(scheduleTemplates.empty()) {
        scheduleTemplates.push_back({new_id(), "Day / Week / Month", {{"1 Day Retrieval", 1}, {"1 Week Retrieval", 7}, {"1 Month Retrieval", 30}}});
        scheduleTemplates.push_back({new_id(), "Expanding (1-3-7-21)", {{"1 Day", 1}, {"3 Days", 3}, {"1 Week", 7}, {"3 Weeks", 21}}});
    }
    if(goalColumns.empty()) {
        goalColumns.push_back({new_id(), "Todo"});
        goalColumns.push_back({new_id(), "In Cycle"});
        goalColumns.push_back({new_id(), "Monitoring"});
        goalColumns.push_back({new_id(), "Out"});
    }
    auto& s = sessions;
    if(s.before.empty() && s.focused.empty() && s.after.empty()) {
        auto item = [&](SessionItemType type, const char* label) {
            SessionItem i;
            i.id = new_id();
            i.type = type;
            i.label = label;
            return i;
        };
        s.before.push_back(item(SessionItemType::CHECK, "Wear noise canceling headphones"));
        s.before.push_back(item(SessionItemType::CHECK, "Turn on do-not-disturb"));
        s.before.push_back(item(SessionItemType::CHECK, "Perform Focus Ritual"));
        s.before.push_back(item(SessionItemType::TEXT, "Set Study Goal:"));
        s.before.push_back(item(SessionItemType::RATING, "Rate Energy:"));
        s.before.push_back(item(SessionItemType::TIMER, "Set Timer:"));
        s.focused.push_back(item(SessionItemType::CHECK, "Stay on one canvas"));
        s.focused.push_back(item(SessionItemType::CHECK, "Write distractions down instead of acting on them"));
        s.after.push_back(item(SessionItemType::RATING, "Rate Focus:"));
        s.after.push_back(item(SessionItemType::TEXT, "What did you learn?"));
        s.after.push_back(item(SessionItemType::CHECK, "Schedule a retrieval"));
    }
}

ScheduleColumn* MenuData::find_schedule_column(ItemID id) { return find_by_id(scheduleColumns, id); }
ScheduleCard* MenuData::find_schedule_card(ItemID id) { return find_by_id(scheduleCards, id); }
Goal* MenuData::find_goal(ItemID id) { return find_by_id(goals, id); }
GoalEntry* MenuData::find_goal_entry(Goal& g, ItemID id) { return find_by_id(g.entries, id); }

SessionItem* MenuData::find_session_item(ItemID id, SessionPhase* phaseOut) {
    for(SessionPhase p : {SessionPhase::BEFORE, SessionPhase::FOCUSED, SessionPhase::AFTER}) {
        if(SessionItem* i = find_by_id(sessions.list(p), id)) {
            if(phaseOut)
                *phaseOut = p;
            return i;
        }
    }
    return nullptr;
}

int64_t MenuData::card_days_until_due(const ScheduleCard& card) {
    ScheduleColumn* col = find_schedule_column(card.columnId);
    int interval = col ? col->intervalDays : 0;
    return local_day_number(card.enteredColumnTime) + interval - today_day_number();
}

void MenuData::mark_card_retrieved(ItemID cardId) {
    ScheduleCard* card = find_schedule_card(cardId);
    if(!card)
        return;
    auto colIt = std::find_if(scheduleColumns.begin(), scheduleColumns.end(), [&](auto& c) { return c.id == card->columnId; });
    if(colIt != scheduleColumns.end() && colIt + 1 != scheduleColumns.end())
        move_card_to_column(cardId, (colIt + 1)->id);
    else if(ScheduleCard* c = find_schedule_card(cardId))
        c->enteredColumnTime = now_time();
}

void MenuData::move_card_to_column(ItemID cardId, ItemID columnId, std::optional<size_t> indexInColumn) {
    ScheduleCard* before = find_schedule_card(cardId);
    bool columnChanged = before && before->columnId != columnId;
    move_to_group(scheduleCards, cardId, columnId, indexInColumn);
    // Reordering within a column keeps the due date; entering a new column restarts the interval
    if(ScheduleCard* c = find_schedule_card(cardId); c && columnChanged)
        c->enteredColumnTime = now_time();
}

void MenuData::delete_schedule_column(ItemID columnId) {
    std::erase_if(scheduleColumns, [&](auto& c) { return c.id == columnId; });
    // Cards in the deleted column go to the first remaining column instead of being lost
    if(scheduleColumns.empty())
        scheduleColumns.push_back({new_id(), "1 Day Retrieval", 1});
    for(auto& card : scheduleCards) {
        if(card.columnId == columnId)
            card.columnId = scheduleColumns.front().id;
    }
}

void MenuData::apply_schedule_template(const ScheduleTemplate& t) {
    if(t.columns.empty())
        return;
    std::vector<ScheduleColumn> oldColumns = scheduleColumns;
    scheduleColumns.clear();
    for(auto& c : t.columns)
        scheduleColumns.push_back({new_id(), c.name, c.intervalDays});
    // Keep each card at the same column position where possible
    for(auto& card : scheduleCards) {
        auto it = std::find_if(oldColumns.begin(), oldColumns.end(), [&](auto& c) { return c.id == card.columnId; });
        size_t index = it == oldColumns.end() ? 0 : static_cast<size_t>(it - oldColumns.begin());
        card.columnId = scheduleColumns[std::min(index, scheduleColumns.size() - 1)].id;
    }
}

void MenuData::move_goal_to_column(ItemID goalId, ItemID columnId, std::optional<size_t> indexInColumn) {
    move_to_group(goals, goalId, columnId, indexInColumn);
}

void MenuData::delete_goal_column(ItemID columnId) {
    std::erase_if(goalColumns, [&](auto& c) { return c.id == columnId; });
    if(goalColumns.empty())
        goalColumns.push_back({new_id(), "Todo"});
    for(auto& g : goals) {
        if(g.columnId == columnId)
            g.columnId = goalColumns.front().id;
    }
}

void MenuData::rename_canvas_links(const std::string& oldPath, const std::string& newPath, bool isFolder) {
    for(auto& card : scheduleCards)
        rename_link(card.canvasPath, oldPath, newPath, isFolder);
    for(auto& g : goals) {
        for(auto& e : g.entries)
            rename_link(e.canvasPath, oldPath, newPath, isFolder);
    }
}

namespace {
    template <typename T> bool has_same_item(const std::vector<T>& v, const T& item, std::string T::*titleMember) {
        return std::any_of(v.begin(), v.end(), [&](const T& a) { return a.id == item.id && a.*titleMember == item.*titleMember; });
    }

    // Column in `columns` matching `otherColumnId` from `otherColumns` by name, appended if there is none
    template <typename Col> ItemID map_column(MenuData& d, std::vector<Col>& columns, const std::vector<Col>& otherColumns, ItemID otherColumnId) {
        auto other = std::find_if(otherColumns.begin(), otherColumns.end(), [&](const Col& c) { return c.id == otherColumnId; });
        if(other == otherColumns.end())
            return columns.front().id;
        auto same = std::find_if(columns.begin(), columns.end(), [&](const Col& c) { return c.name == other->name; });
        if(same != columns.end())
            return same->id;
        Col added = *other;
        added.id = d.new_id();
        columns.push_back(added);
        return added.id;
    }
}

size_t MenuData::merge_from(const MenuData& other) {
    size_t added = 0;
    fill_missing_defaults(); // Columns must exist to map into

    for(const ScheduleCard& card : other.scheduleCards) {
        if(has_same_item(scheduleCards, card, &ScheduleCard::title))
            continue;
        ScheduleCard c = card;
        c.columnId = map_column(*this, scheduleColumns, other.scheduleColumns, card.columnId);
        c.id = new_id();
        scheduleCards.push_back(c);
        added++;
    }
    for(const ScheduleTemplate& t : other.scheduleTemplates) {
        if(has_same_item(scheduleTemplates, t, &ScheduleTemplate::name))
            continue;
        ScheduleTemplate n = t;
        n.id = new_id();
        scheduleTemplates.push_back(n);
        added++;
    }
    for(const Goal& goal : other.goals) {
        if(has_same_item(goals, goal, &Goal::title))
            continue;
        Goal g = goal;
        g.columnId = map_column(*this, goalColumns, other.goalColumns, goal.columnId);
        g.id = new_id();
        for(GoalEntry& e : g.entries)
            e.id = new_id();
        goals.push_back(g);
        added++;
    }
    for(const GoalTemplate& t : other.goalTemplates) {
        if(has_same_item(goalTemplates, t, &GoalTemplate::name))
            continue;
        GoalTemplate n = t;
        n.id = new_id();
        goalTemplates.push_back(n);
        added++;
    }
    SessionsData otherSessions = other.sessions;
    for(SessionPhase p : {SessionPhase::BEFORE, SessionPhase::FOCUSED, SessionPhase::AFTER}) {
        std::vector<SessionItem>& list = sessions.list(p);
        for(const SessionItem& item : otherSessions.list(p)) {
            if(has_same_item(list, item, &SessionItem::label))
                continue;
            // Only one timer is allowed, in the Before list
            if(item.type == SessionItemType::TIMER && std::any_of(list.begin(), list.end(), [](const SessionItem& i) { return i.type == SessionItemType::TIMER; }))
                continue;
            SessionItem n = item;
            n.id = new_id();
            list.push_back(n);
            added++;
        }
    }
    for(const SessionRecord& r : other.sessions.history) {
        bool exists = std::any_of(sessions.history.begin(), sessions.history.end(), [&](const SessionRecord& a) { return a.start == r.start && a.end == r.end; });
        if(!exists) {
            sessions.history.push_back(r);
            added++;
        }
    }
    std::sort(sessions.history.begin(), sessions.history.end(), [](const SessionRecord& a, const SessionRecord& b) { return a.start < b.start; });
    return added;
}

void MenuDataStore::load(const std::filesystem::path& configPath) {
    filePath = configPath / FILE_NAME;
    try {
        nlohmann::json j = nlohmann::json::parse(read_file_to_string(filePath));
        j.get_to(data);
    }
    catch(...) {
        data = MenuData();
    }
    data.fill_missing_defaults();
}

void MenuDataStore::save() {
    if(filePath.empty())
        return;
    try {
        std::string s = nlohmann::json(data).dump();
        if(!SDL_SaveFile(filePath.string().c_str(), s.c_str(), s.size()))
            Logger::get().log(Logger::LogType::INFO, std::string("[MenuDataStore::save] ") + SDL_GetError());
    }
    catch(const std::exception& e) {
        Logger::get().log(Logger::LogType::INFO, std::string("[MenuDataStore::save] ") + e.what());
    }
    dirty = false;
}

void MenuDataStore::mark_dirty() {
    if(!dirty)
        dirtySince = now_time();
    dirty = true;
}

void MenuDataStore::update() {
    if(dirty && now_time() - dirtySince > NS_PER_SECOND)
        save();
}

}
