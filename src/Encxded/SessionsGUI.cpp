#include "SessionsGUI.hpp"
#include "../MainProgram.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "../GUIStuff/Elements/NumberSlider.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include <SDL3/SDL_init.h>
#include <cmath>
#include <format>
#include <numbers>

namespace Encxded {

using namespace GUIStuff;
using namespace GUIStuff::ElementHelpers;
using namespace Widgets;

namespace {
    constexpr int64_t NS_PER_SECOND = 1000000000LL;
    constexpr size_t MAX_HISTORY = 500;
    constexpr int MIN_POMODORO_MINUTES = 1;
    constexpr int MAX_POMODORO_MINUTES = 120;

    std::string format_duration(int64_t seconds) {
        seconds = std::max<int64_t>(seconds, 0);
        if(seconds >= 3600)
            return std::format("{}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
        return std::format("{:02}:{:02}", seconds / 60, seconds % 60);
    }

    const char* phase_icon(SessionPhase p) {
        switch(p) {
            case SessionPhase::BEFORE: return "data/icons/encxded/play-circle.svg";
            case SessionPhase::FOCUSED: return "data/icons/encxded/sessions.svg";
            case SessionPhase::AFTER: return "data/icons/encxded/check.svg";
        }
        return "";
    }

    const char* phase_name(SessionPhase p) {
        switch(p) {
            case SessionPhase::BEFORE: return "Before";
            case SessionPhase::FOCUSED: return "Focused";
            case SessionPhase::AFTER: return "After";
        }
        return "";
    }
}

SessionsGUI::SessionsGUI(MainProgram& initMain, MenuDataStore& initStore):
    main(initMain),
    store(initStore)
{}

SessionsData& SessionsGUI::s() {
    return store.data.sessions;
}

void SessionsGUI::open() {
    auto& a = s().active;
    tab = a.active ? a.phase : SessionPhase::BEFORE;
    popupOpen = true;
    main.g.gui.set_to_layout();
}

void SessionsGUI::hide() {
    popupOpen = false;
    itemMenuOpen = std::nullopt;
    dialogs.close();
    main.g.gui.set_to_layout();
}

bool SessionsGUI::is_open() const {
    return popupOpen;
}

bool SessionsGUI::input_global_back_button() {
    if(dialogs.is_open()) {
        dialogs.close();
        main.g.gui.set_to_layout();
        return true;
    }
    if(popupOpen) {
        hide();
        return true;
    }
    return false;
}

void SessionsGUI::update() {
    auto& a = s().active;
    if(a.active && a.phase == SessionPhase::FOCUSED) {
        int64_t focusedSeconds = a.focused_ns(now_time()) / NS_PER_SECOND;
        if(a.mode == TimerMode::POMODORO && focusedSeconds >= static_cast<int64_t>(a.minutes) * 60)
            end_focus(true);
        else if(focusedSeconds != lastShownSecond) {
            lastShownSecond = focusedSeconds;
            main.g.gui.set_to_layout();
        }
    }
    if(chimeStream && SDL_GetAudioStreamQueued(chimeStream) == 0) {
        SDL_DestroyAudioStream(chimeStream);
        chimeStream = nullptr;
    }
}

std::string SessionsGUI::timer_text(SDL_Time now) {
    auto& a = s().active;
    int64_t focusedSeconds = a.focused_ns(now) / NS_PER_SECOND;
    if(a.mode == TimerMode::POMODORO)
        return format_duration(static_cast<int64_t>(a.minutes) * 60 - focusedSeconds);
    return format_duration(focusedSeconds);
}

void SessionsGUI::start_session() {
    auto& sd = s();
    sd.active = ActiveSession{
        .active = true,
        .phase = SessionPhase::FOCUSED,
        .mode = sd.timerMode,
        .minutes = std::clamp(sd.pomodoroMinutes, MIN_POMODORO_MINUTES, MAX_POMODORO_MINUTES),
        .focusStart = now_time()
    };
    lastShownSecond = -1;
    popupOpen = false;
    tab = SessionPhase::FOCUSED;
    store.mark_dirty();
    main.g.gui.set_to_layout();
}

void SessionsGUI::toggle_pause() {
    auto& a = s().active;
    if(!a.active || a.phase != SessionPhase::FOCUSED)
        return;
    SDL_Time now = now_time();
    if(a.paused) {
        a.pausedNs += now - a.pauseStart;
        a.paused = false;
    }
    else {
        a.paused = true;
        a.pauseStart = now;
    }
    store.mark_dirty();
    main.g.gui.set_to_layout();
}

void SessionsGUI::end_focus(bool timerRanOut) {
    auto& a = s().active;
    if(!a.active || a.phase != SessionPhase::FOCUSED)
        return;
    SDL_Time now = now_time();
    if(a.paused) {
        a.pausedNs += now - a.pauseStart;
        a.paused = false;
    }
    if(timerRanOut) // End exactly when the countdown hit zero, even if the app was in the background
        a.focusEnd = a.focusStart + a.pausedNs + static_cast<int64_t>(a.minutes) * 60 * NS_PER_SECOND;
    else
        a.focusEnd = now;
    a.phase = SessionPhase::AFTER;
    tab = SessionPhase::AFTER;
    popupOpen = true;
    if(timerRanOut && s().sound)
        play_chime();
    store.mark_dirty();
    main.g.gui.set_to_layout();
}

void SessionsGUI::finish_session() {
    auto& sd = s();
    auto& a = sd.active;
    if(a.active) {
        SessionRecord r{
            .start = a.focusStart,
            .end = a.focusEnd,
            .focusedNs = a.focused_ns(now_time()),
            .mode = a.mode
        };
        for(auto& item : sd.before) {
            if(item.type == SessionItemType::TEXT && !item.text.empty()) {
                r.goal = item.text;
                break;
            }
        }
        sd.history.push_back(r);
        if(sd.history.size() > MAX_HISTORY)
            sd.history.erase(sd.history.begin(), sd.history.begin() + (sd.history.size() - MAX_HISTORY));
    }
    for(SessionPhase p : {SessionPhase::BEFORE, SessionPhase::FOCUSED, SessionPhase::AFTER}) {
        for(auto& item : sd.list(p)) {
            item.checked = false;
            item.rating = 0;
            item.text.clear();
        }
    }
    a = ActiveSession();
    tab = SessionPhase::BEFORE;
    popupOpen = false;
    store.mark_dirty();
    main.g.gui.set_to_layout();
}

void SessionsGUI::cancel_session() {
    s().active = ActiveSession();
    tab = SessionPhase::BEFORE;
    store.mark_dirty();
    main.g.gui.set_to_layout();
}

void SessionsGUI::play_chime() {
    if(!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        return;
    constexpr int RATE = 44100;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, RATE};
    if(!chimeStream) {
        chimeStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if(!chimeStream)
            return;
    }
    // Three rising notes with a soft attack and decay
    std::vector<float> samples;
    auto note = [&](float freq, float seconds) {
        int count = static_cast<int>(seconds * RATE);
        for(int i = 0; i < count; i++) {
            float t = static_cast<float>(i) / RATE;
            float envelope = std::min(1.0f, t / 0.01f) * std::exp(-4.0f * t / seconds);
            samples.push_back(0.25f * envelope * std::sin(2.0f * std::numbers::pi_v<float> * freq * t));
        }
        samples.insert(samples.end(), static_cast<size_t>(0.06f * RATE), 0.0f);
    };
    note(880.0f, 0.22f);
    note(1174.66f, 0.22f);
    note(1567.98f, 0.5f);
    SDL_PutAudioStreamData(chimeStream, samples.data(), static_cast<int>(samples.size() * sizeof(float)));
    SDL_ResumeAudioStreamDevice(chimeStream);
}

void SessionsGUI::layout() {
    auto& a = s().active;
    if(a.active && !popupOpen)
        pill_layout();
    if(popupOpen)
        popup_layout();
}

void SessionsGUI::pill_layout() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    auto& a = s().active;
    gui.set_z_index(gui.get_z_index() + 90, [&] {
        gui.element<LayoutElement>("encxded session pill", [&](LayoutElement*, const Clay_ElementId& lId) {
            CLAY(lId, {
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIXED(40)},
                    .padding = {.left = 12, .right = 6},
                    .childGap = 6,
                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                },
                .backgroundColor = convert_vec4<Clay_Color>(theme.accentColor),
                .cornerRadius = CLAY_CORNER_RADIUS(20),
                .floating = {
                    .offset = {0, gui.io.safeWindowRect.min.y() + 8.0f},
                    .zIndex = gui.get_z_index(),
                    .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP, .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                    .attachTo = CLAY_ATTACH_TO_ROOT
                }
            }) {
                SkColor4f fg = theme.onAccentColor;
                if(a.phase == SessionPhase::FOCUSED) {
                    std::string text = timer_text(now_time());
                    button(gui, "pill time", text, {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/encxded/timer.svg", .height = 34.0f, .fontId = GUI_FONT_BOLD, .color = fg}, [&] { open(); });
                    tinted_icon_button(gui, "pill pause", a.paused ? "data/icons/encxded/play.svg" : "data/icons/encxded/pause.svg", fg, [&] { toggle_pause(); });
                    tinted_icon_button(gui, "pill stop", "data/icons/encxded/stop.svg", fg, [&] { end_focus(false); });
                }
                else
                    button(gui, "pill review", "Review session", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/encxded/check.svg", .height = 34.0f, .fontId = GUI_FONT_BOLD, .color = fg}, [&] { open(); });
            }
        });
    });
}

void SessionsGUI::popup_layout() {
    auto& gui = main.g.gui;
    modal_frame(gui, "encxded sessions popup", 920.0f, gui.io.safeWindowRect.height() - 24.0f, [&] {
        header_layout();
        gui.clipping_element<ScrollArea>("session items", ScrollArea::Options{
            .scrollVertical = true,
            .clipVertical = true,
            .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
            .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                CLAY_AUTO_ID({.layout = {
                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                    .padding = {.right = 6},
                    .childGap = 8,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }}) {
                    if(tab == SessionPhase::FOCUSED)
                        focus_timer_card();
                    auto& items = s().list(tab);
                    for(size_t i = 0; i < items.size(); i++)
                        gui.new_id(static_cast<int64_t>(items[i].id), [&] { item_card(items[i], tab); });
                    bottom_action_layout();
                }
            }
        });
        tab_bar_layout();
    }, [&] { hide(); });

    if(itemMenuOpen.has_value()) {
        SessionPhase p;
        SessionItem* item = store.data.find_session_item(itemMenuOpen.value(), &p);
        if(item)
            item_menu(*item, p);
        else
            itemMenuOpen = std::nullopt;
    }
    dialogs.layout(gui);
}

void SessionsGUI::header_layout() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    auto& a = s().active;
    const char* title = "New Session";
    if(tab == SessionPhase::FOCUSED)
        title = "Focus";
    else if(tab == SessionPhase::AFTER)
        title = "Session Review";
    else if(a.active)
        title = "Session";

    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        .layoutDirection = CLAY_LEFT_TO_RIGHT
    }}) {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}}}) {
            button(gui, "add item", "Add Item", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/plus.svg"}, [&] { add_item_dialog(); });
        }
        label(gui, title, theme.frontColor1, GUI_FONT_BOLD, 1.2f);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER}}}) {
            button(gui, "hide", "Hide", {.kind = ButtonStyle::Kind::SOFT, .height = 36.0f}, [&] { hide(); });
        }
    }
}

void SessionsGUI::tab_bar_layout() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(48)},
        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
        .layoutDirection = CLAY_LEFT_TO_RIGHT
    }}) {
        for(SessionPhase p : {SessionPhase::BEFORE, SessionPhase::FOCUSED, SessionPhase::AFTER}) {
            gui.new_id(static_cast<int64_t>(p), [&] {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}}) {
                    SkColor4f c = tab == p ? theme.accentColor : theme.frontColor2;
                    gui.element<SelectableButton>("tab", SelectableButton::Data{
                        .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                        .onClick = [&, p] {
                            tab = p;
                            itemMenuOpen = std::nullopt;
                        },
                        .innerContent = [&](const SelectableButton::InnerContentCallbackParameters&) {
                            CLAY_AUTO_ID({.layout = {
                                .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)},
                                .childGap = 8,
                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                .layoutDirection = CLAY_LEFT_TO_RIGHT
                            }}) {
                                icon(gui, "icon", phase_icon(p), 22.0f, c);
                                label(gui, phase_name(p), c, tab == p ? GUI_FONT_BOLD : GUI_FONT_REGULAR);
                            }
                        }
                    });
                }
            });
        }
    }
}

void SessionsGUI::focus_timer_card() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    auto& a = s().active;
    card(gui, [&] {
        CLAY_AUTO_ID({.layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
            .padding = CLAY_PADDING_ALL(8),
            .childGap = 10,
            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }}) {
            if(a.active && a.phase == SessionPhase::FOCUSED) {
                label(gui, a.mode == TimerMode::POMODORO ? "Pomodoro" : "Flowmodoro", theme.frontColor2);
                label(gui, timer_text(now_time()), a.paused ? theme.frontColor2 : theme.frontColor1, GUI_FONT_EXTRA_BOLD, 3.0f);
                if(a.paused)
                    label(gui, "Paused", theme.frontColor2);
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}, .childGap = 10, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                    button(gui, "pause", a.paused ? "Resume" : "Pause", {.kind = ButtonStyle::Kind::SOFT, .iconPath = a.paused ? "data/icons/encxded/play.svg" : "data/icons/encxded/pause.svg"}, [&] { toggle_pause(); });
                    button(gui, "end focus", "End Focus", {.kind = ButtonStyle::Kind::ACCENT, .iconPath = "data/icons/encxded/stop.svg", .fontId = GUI_FONT_BOLD}, [&] { end_focus(false); });
                }
            }
            else if(a.active)
                label(gui, "Focus finished: " + format_duration(a.focused_ns(now_time()) / NS_PER_SECOND), theme.frontColor2);
            else
                label(gui, "Start a session from the Before tab. These are the checks to keep in mind while focusing.", theme.frontColor2);
        }
    });
}

void SessionsGUI::bottom_action_layout() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    auto& a = s().active;
    card(gui, [&] {
        CLAY_AUTO_ID({.layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
            .padding = CLAY_PADDING_ALL(10),
            .childGap = 10,
            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }}) {
            switch(tab) {
                case SessionPhase::BEFORE:
                    if(!a.active)
                        button(gui, "start session", "Start Session", {.kind = ButtonStyle::Kind::ACCENT, .iconPath = "data/icons/encxded/play-circle.svg", .height = 48.0f, .fontId = GUI_FONT_BOLD, }, [&] { start_session(); });
                    else
                        button(gui, "go to session", "Session in progress", {.kind = ButtonStyle::Kind::SOFT, .iconPath = "data/icons/encxded/timer.svg"}, [&] { tab = a.phase; });
                    break;
                case SessionPhase::FOCUSED:
                    if(a.active) {
                        button(gui, "cancel session", "Cancel session", {.kind = ButtonStyle::Kind::PLAIN}, [&] {
                            dialogs.confirm("Cancel this session?", "The timer stops and nothing is recorded.", "Cancel Session", [&] { cancel_session(); });
                        });
                    }
                    break;
                case SessionPhase::AFTER:
                    if(a.active && a.phase == SessionPhase::AFTER) {
                        int64_t focusedMinutes = a.focused_ns(now_time()) / (NS_PER_SECOND * 60);
                        label(gui, std::format("You focused for {} minute{}.", focusedMinutes, focusedMinutes == 1 ? "" : "s"), theme.frontColor1);
                        if(a.mode == TimerMode::FLOWMODORO)
                            label(gui, std::format("Suggested break: {} minutes", std::max<int64_t>(1, (focusedMinutes + 4) / 5)), theme.frontColor2);
                        button(gui, "finish session", "Finish Session", {.kind = ButtonStyle::Kind::ACCENT, .iconPath = "data/icons/encxded/check.svg", .height = 48.0f, .fontId = GUI_FONT_BOLD}, [&] { finish_session(); });
                    }
                    else if(a.active)
                        label(gui, "Finish focusing to review the session.", theme.frontColor2);
                    else
                        label(gui, "After a session, review it here.", theme.frontColor2);
                    break;
            }
        }
    });
}

void SessionsGUI::item_card(SessionItem& item, SessionPhase phase) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID id = item.id;
    card(gui, [&] {
        CLAY_AUTO_ID({.layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
            .childGap = 10,
            .childAlignment = {.y = item.type == SessionItemType::CHECK ? CLAY_ALIGN_Y_CENTER : CLAY_ALIGN_Y_TOP},
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }}) {
            round_check(gui, "check", item.checked, [&, id] {
                if(SessionItem* i = store.data.find_session_item(id)) {
                    i->checked = !i->checked;
                    store.mark_dirty();
                }
            });
            CLAY_AUTO_ID({.layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                .padding = {.top = static_cast<uint16_t>(item.type == SessionItemType::CHECK ? 0 : 8)},
                .childGap = 8,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }}) {
                label(gui, item.label, theme.frontColor1, item.type == SessionItemType::CHECK ? GUI_FONT_REGULAR : GUI_FONT_BOLD);
                switch(item.type) {
                    case SessionItemType::CHECK:
                        break;
                    case SessionItemType::TEXT:
                        CLAY_AUTO_ID({
                            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(8)},
                            .backgroundColor = convert_vec4<Clay_Color>(color_mul_alpha(theme.backColor0, 0.6f)),
                            .cornerRadius = CLAY_CORNER_RADIUS(6),
                            .border = {.color = convert_vec4<Clay_Color>(color_mul_alpha(theme.frontColor2, 0.35f)), .width = CLAY_BORDER_OUTSIDE(1)}
                        }) {
                            input_text(gui, "text", &item.text, {.onEdit = [&, id] {
                                if(SessionItem* i = store.data.find_session_item(id)) {
                                    i->checked = !i->text.empty();
                                    store.mark_dirty();
                                }
                            }});
                        }
                        break;
                    case SessionItemType::RATING:
                        segmented(gui, "rating", {"1", "2", "3", "4", "5"}, item.rating > 0 ? static_cast<size_t>(item.rating - 1) : SIZE_MAX, [&, id](size_t i) {
                            if(SessionItem* it = store.data.find_session_item(id)) {
                                it->rating = static_cast<int>(i) + 1;
                                it->checked = true;
                                store.mark_dirty();
                            }
                        });
                        break;
                    case SessionItemType::TIMER:
                        timer_item_controls(item);
                        break;
                }
            }
            SelectableButton* more = more_button(gui, "more", [&, id] {
                itemMenuOpen = id;
            });
            if(itemMenuOpen == id)
                itemMenuAnchor = more;
        }
    });
}

void SessionsGUI::timer_item_controls(SessionItem& item) {
    auto& gui = main.g.gui;
    auto& sd = s();
    ItemID id = item.id;
    auto touch = [&, id] {
        if(SessionItem* i = store.data.find_session_item(id))
            i->checked = true;
        store.mark_dirty();
    };
    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
        .childGap = 8,
        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        .layoutDirection = CLAY_LEFT_TO_RIGHT
    }}) {
        segmented(gui, "timer mode", {"Pomodoro Timer", "Flowmodoro Stopwatch"}, sd.timerMode == TimerMode::POMODORO ? 0 : 1, [&, touch](size_t i) {
            s().timerMode = i == 0 ? TimerMode::POMODORO : TimerMode::FLOWMODORO;
            touch();
        });
        icon_button(gui, "sound", sd.sound ? "data/icons/encxded/volume.svg" : "data/icons/encxded/volume-off.svg", [&] {
            s().sound = !s().sound;
            store.mark_dirty();
        });
    }
    if(sd.timerMode == TimerMode::POMODORO) {
        CLAY_AUTO_ID({.layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
            .childGap = 12,
            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }}) {
            CLAY_AUTO_ID({
                .layout = {.sizing = {.width = CLAY_SIZING_FIXED(64), .height = CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(8)},
                .backgroundColor = convert_vec4<Clay_Color>(gui.io.theme->backColor2),
                .cornerRadius = CLAY_CORNER_RADIUS(6)
            }) {
                input_scalar<int>(gui, "minutes", &sd.pomodoroMinutes, MIN_POMODORO_MINUTES, MAX_POMODORO_MINUTES, {.onEdit = touch});
            }
            gui.element<NumberSlider<int>>("minutes slider", &sd.pomodoroMinutes, MIN_POMODORO_MINUTES, 90, NumberSliderData{.onChange = touch});
        }
    }
    else
        label(gui, "Counts up until you stop. Take a break of about a fifth of the time you focused.", gui.io.theme->frontColor2, GUI_FONT_REGULAR, 0.9f);
}

void SessionsGUI::item_menu(SessionItem& item, SessionPhase phase) {
    auto& gui = main.g.gui;
    ItemID id = item.id;
    std::vector<MenuEntry> entries;
    entries.push_back({"Rename", [&, id] {
        SessionItem* i = store.data.find_session_item(id);
        if(!i) return;
        dialogs.prompt_text("Rename item", i->label, "Rename", [&, id](const std::string& v) {
            if(SessionItem* i2 = store.data.find_session_item(id)) {
                i2->label = v;
                store.mark_dirty();
            }
        });
    }});
    auto move = [&, id, phase](int dir) {
        auto& list = s().list(phase);
        auto it = std::find_if(list.begin(), list.end(), [id](auto& x) { return x.id == id; });
        if(it == list.end()) return;
        size_t i = it - list.begin();
        size_t j = static_cast<size_t>(static_cast<int64_t>(i) + dir);
        if(j < list.size()) {
            std::swap(list[i], list[j]);
            store.mark_dirty();
        }
    };
    entries.push_back({"Move up", [move] { move(-1); }});
    entries.push_back({"Move down", [move] { move(1); }});
    entries.push_back({"Delete", [&, id, phase] {
        std::erase_if(s().list(phase), [id](auto& x) { return x.id == id; });
        store.mark_dirty();
    }});
    popup_menu(gui, "session item menu", itemMenuAnchor, entries, [&] {
        itemMenuOpen = std::nullopt;
        main.g.gui.set_to_layout();
    });
}

void SessionsGUI::add_item_dialog() {
    newItemName.clear();
    newItemType = 0;
    dialogs.custom("Add item to " + std::string(phase_name(tab)), 460.0f, [&](GUIManager& gui) {
        auto& theme = *gui.io.theme;
        label(gui, "Name", theme.frontColor2);
        CLAY_AUTO_ID({
            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(8)},
            .backgroundColor = convert_vec4<Clay_Color>(color_mul_alpha(theme.backColor0, 0.6f)),
            .cornerRadius = CLAY_CORNER_RADIUS(6)
        }) {
            input_text(gui, "new item name", &newItemName);
        }
        label(gui, "Type", theme.frontColor2);
        segmented(gui, "new item type", {"Checkbox", "Text field", "Rating 1-5"}, newItemType, [&](size_t i) { newItemType = i; });
        CLAY_AUTO_ID({.layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
            .childGap = 10,
            .childAlignment = {.x = CLAY_ALIGN_X_RIGHT},
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }}) {
            button(gui, "cancel", "Cancel", {.kind = ButtonStyle::Kind::SOFT}, [&] { dialogs.close(); });
            button(gui, "add", "Add", {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, [&] {
                SessionItem item;
                item.id = store.data.new_id();
                item.label = newItemName.empty() ? "New item" : newItemName;
                item.type = newItemType == 1 ? SessionItemType::TEXT : (newItemType == 2 ? SessionItemType::RATING : SessionItemType::CHECK);
                s().list(tab).push_back(item);
                store.mark_dirty();
                dialogs.close();
            });
        }
    });
}

}
