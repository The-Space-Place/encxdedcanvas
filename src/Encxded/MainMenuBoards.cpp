// EncxdedCanvas: Schedule (retrieval kanban) and Goal Cycle boards, plus the card dragging they share
#include "MainMenuScreen.hpp"
#include "EncxdedApp.hpp"
#include "FsUtil.hpp"
#include "../MainProgram.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "../GUIStuff/Elements/NumberSlider.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include <Helpers/Logger.hpp>
#include <format>

namespace Encxded {

using namespace GUIStuff;
using namespace GUIStuff::ElementHelpers;
using namespace Widgets;

namespace {
    constexpr int64_t NS_PER_MS = 1000000LL;
    constexpr int64_t LONG_PRESS_NS = 350 * NS_PER_MS;
    constexpr int64_t MAX_TAP_NS = 600 * NS_PER_MS;
    constexpr float MOUSE_DRAG_START_DISTANCE = 8.0f;
    constexpr float TOUCH_SCROLL_DISTANCE = 14.0f;
    constexpr float SIDEBAR_WIDTH = 240.0f;

    const SkColor4f STATUS_GREEN = {0.25f, 0.78f, 0.36f, 1.0f};
    const SkColor4f STATUS_YELLOW = {0.96f, 0.74f, 0.18f, 1.0f};
    const SkColor4f STATUS_RED = {0.90f, 0.30f, 0.30f, 1.0f};

    void text_field(GUIManager& gui, const char* id, std::string* val, const std::function<void()>& onEdit = nullptr) {
        auto& theme = *gui.io.theme;
        CLAY_AUTO_ID({
            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(8)},
            .backgroundColor = convert_vec4<Clay_Color>(color_mul_alpha(theme.backColor0, 0.6f)),
            .cornerRadius = CLAY_CORNER_RADIUS(6)
        }) {
            input_text(gui, id, val, {.onEdit = onEdit});
        }
    }

    // Swaps an element of v with its neighbour among elements sharing its columnId (for "Move up/down")
    template <typename T> bool move_within_column(std::vector<T>& v, ItemID id, int dir) {
        auto it = std::find_if(v.begin(), v.end(), [id](const T& a) { return a.id == id; });
        if(it == v.end())
            return false;
        ItemID col = it->columnId;
        if(dir < 0) {
            for(auto j = std::make_reverse_iterator(it); j != v.rend(); ++j) {
                if(j->columnId == col) {
                    std::iter_swap(it, j);
                    return true;
                }
            }
        }
        else {
            for(auto j = it + 1; j != v.end(); ++j) {
                if(j->columnId == col) {
                    std::iter_swap(it, j);
                    return true;
                }
            }
        }
        return false;
    }

    template <typename T> void move_column(std::vector<T>& cols, ItemID id, int dir) {
        auto it = std::find_if(cols.begin(), cols.end(), [id](const T& c) { return c.id == id; });
        if(it == cols.end())
            return;
        size_t i = it - cols.begin();
        size_t j = static_cast<size_t>(static_cast<int64_t>(i) + dir);
        if(j < cols.size())
            std::swap(cols[i], cols[j]);
    }
}

// ---------- Dragging ----------

float MainMenuScreen::board_column_width(size_t columnCount) {
    auto& io = main.g.gui.io;
    float available = io.safeWindowRect.width() - 40.0f - (data().sidebarOpen && !sidebar_is_overlay() ? SIDEBAR_WIDTH : 0.0f);
    float w = (available - 14.0f * (columnCount - 1)) / std::max<size_t>(columnCount, 1);
    return std::clamp(w, 270.0f, 420.0f);
}

void MainMenuScreen::board_press(DragState::Kind kind, ItemID id, const std::string& title, LayoutElement* l, const Vector2f& pos, bool isTouch) {
    if(drag.kind != DragState::Kind::NONE || dialogs.is_open() || openMenu.kind != OpenMenu::Kind::NONE)
        return;
    auto more = boardMoreButtons.find(id);
    if(more != boardMoreButtons.end() && more->second->mouseHovering)
        return;
    if(!l->get_bb().has_value())
        return;
    auto& bb = l->get_bb().value();
    drag = DragState{
        .kind = kind,
        .id = id,
        .title = title,
        .pressPos = pos,
        .pos = pos,
        .grabOffset = pos - bb.min,
        .cardSize = bb.dim(),
        .pressTime = now_time(),
        .pressIsTouch = isTouch
    };
}

void MainMenuScreen::board_pointer_motion(const Vector2f& pos) {
    if(drag.kind == DragState::Kind::NONE)
        return;
    drag.pos = pos;
    if(!drag.dragging) {
        float dist = (pos - drag.pressPos).norm();
        if(drag.pressIsTouch) {
            if(dist > TOUCH_SCROLL_DISTANCE) // Moving before the long press means the finger is scrolling
                drag = DragState();
        }
        else if(dist > MOUSE_DRAG_START_DISTANCE) {
            drag.dragging = true;
            main.g.gui.pointerDragCapture = true;
        }
    }
    if(drag.dragging) {
        board_update_target();
        main.g.gui.set_to_layout();
    }
}

void MainMenuScreen::board_update() {
    if(drag.kind != DragState::Kind::NONE && !drag.dragging && drag.pressIsTouch && now_time() - drag.pressTime >= LONG_PRESS_NS) {
        drag.dragging = true;
        main.g.gui.pointerDragCapture = true;
        board_update_target();
        main.g.gui.set_to_layout();
    }
}

void MainMenuScreen::board_update_target() {
    std::optional<ItemID> best;
    float bestDist = std::numeric_limits<float>::max();
    for(auto& [colId, bb] : boardColumnBBs) {
        float dist = 0.0f;
        if(drag.pos.x() < bb.min.x())
            dist = bb.min.x() - drag.pos.x();
        else if(drag.pos.x() > bb.max.x())
            dist = drag.pos.x() - bb.max.x();
        if(dist < bestDist) {
            bestDist = dist;
            best = colId;
        }
    }
    if(!best.has_value())
        return;
    drag.targetColumn = best.value();
    drag.targetIndex = 0;
    for(auto& hit : boardItemBBs) {
        if(hit.columnId == drag.targetColumn && hit.itemId != drag.id && hit.bb.center().y() < drag.pos.y())
            drag.targetIndex++;
    }
}

void MainMenuScreen::board_pointer_up(const Vector2f& pos) {
    if(drag.kind == DragState::Kind::NONE)
        return;
    DragState d = drag;
    drag = DragState();
    main.g.gui.pointerDragCapture = false;
    if(d.dragging) {
        if(d.targetColumn != 0) {
            if(d.kind == DragState::Kind::SCHEDULE_CARD)
                data().move_card_to_column(d.id, d.targetColumn, d.targetIndex);
            else
                data().move_goal_to_column(d.id, d.targetColumn, d.targetIndex);
            changed();
        }
    }
    else if(now_time() - d.pressTime <= MAX_TAP_NS && (pos - d.pressPos).norm() <= TOUCH_SCROLL_DISTANCE)
        board_tap(d.kind, d.id);
    main.g.gui.set_to_layout();
}

void MainMenuScreen::board_tap(DragState::Kind kind, ItemID id) {
    if(kind == DragState::Kind::SCHEDULE_CARD) {
        ScheduleCard* card = data().find_schedule_card(id);
        if(!card)
            return;
        if(canvas_exists(card->canvasPath))
            open_canvas(card->canvasPath);
        else
            open_menu(OpenMenu::Kind::SCHEDULE_CARD, id);
    }
    else if(kind == DragState::Kind::GOAL) {
        openGoal = id;
        main.g.gui.set_to_layout();
    }
}

void MainMenuScreen::board_drag_ghost() {
    if(!drag.dragging)
        return;
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    gui.set_z_index(gui.get_z_index() + 150, [&] {
        gui.new_id("drag ghost", [&] {
            Vector2f topLeft = drag.pos - drag.grabOffset;
            CLAY_AUTO_ID({
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIXED(drag.cardSize.x()), .height = CLAY_SIZING_FIXED(drag.cardSize.y())},
                    .padding = {.left = 14, .right = 14},
                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}
                },
                .backgroundColor = convert_vec4<Clay_Color>(theme.cardColor),
                .cornerRadius = CLAY_CORNER_RADIUS(8),
                .floating = {.offset = {topLeft.x(), topLeft.y()}, .zIndex = gui.get_z_index(), .attachTo = CLAY_ATTACH_TO_ROOT},
                .border = {.color = convert_vec4<Clay_Color>(theme.accentColor), .width = CLAY_BORDER_OUTSIDE(2)}
            }) {
                label(gui, drag.title, theme.frontColor1, GUI_FONT_BOLD, 1.05f);
            }
        });
    });
}

// Shared pieces of a board column
namespace {
    void drop_indicator(GUIManager& gui) {
        CLAY_AUTO_ID({
            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(4)}},
            .backgroundColor = convert_vec4<Clay_Color>(gui.io.theme->accentColor),
            .cornerRadius = CLAY_CORNER_RADIUS(2)
        }) {}
    }
}

// ---------- Schedule ----------

void MainMenuScreen::schedule_view() {
    auto& gui = main.g.gui;
    page_header("Scheduling", nullptr, [&] {
        anchor_menu(more_button(gui, "schedule more", [&] { open_menu(OpenMenu::Kind::SCHEDULE_HEADER); }, 40.0f), OpenMenu::Kind::SCHEDULE_HEADER);
    });
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
        segmented(gui, "schedule tabs", {"Board", "Templates"}, scheduleTemplatesTab ? 1 : 0, [&](size_t i) {
            scheduleTemplatesTab = i == 1;
            drag = DragState();
            main.g.gui.set_to_layout();
        }, CLAY_SIZING_FIXED(std::min(640.0f, gui.io.windowSize.x() - 60.0f)), 40.0f);
    }
    if(scheduleTemplatesTab) {
        schedule_templates();
        return;
    }
    auto& cols = data().scheduleColumns;
    float colWidth = board_column_width(cols.size());
    gui.element<LayoutElement>("schedule board", [&](LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {
            gui.clipping_element<ScrollArea>("schedule columns", ScrollArea::Options{
                .scrollHorizontal = true,
                .clipHorizontal = true,
                .scrollbarX = ScrollArea::ScrollbarType::NORMAL,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_GROW(0)}, .padding = {.bottom = 8}, .childGap = 14, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                        for(auto& col : cols)
                            gui.new_id(static_cast<int64_t>(col.id), [&] { schedule_column(col, colWidth); });
                    }
                }
            });
        }
    }, LayoutElement::Callbacks{
        .onClick = [&](LayoutElement*, const InputManager::MouseButtonCallbackArgs& b) {
            if(!b.down)
                board_pointer_up(b.pos);
        },
        .onMotion = [&](LayoutElement*, const InputManager::MouseMotionCallbackArgs& m) {
            board_pointer_motion(m.pos);
        }
    });
}

void MainMenuScreen::schedule_column(ScheduleColumn& col, float width) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID colId = col.id;
    gui.element<LayoutElement>("column", [&](LayoutElement* l, const Clay_ElementId& lId) {
        if(l->get_bb().has_value())
            boardColumnBBs[colId] = l->get_bb().value();
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_GROW(0)},
                .padding = CLAY_PADDING_ALL(8),
                .childGap = 8,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = convert_vec4<Clay_Color>(theme.sidebarColor),
            .cornerRadius = CLAY_CORNER_RADIUS(12)
        }) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(44)}, .padding = {.left = 40}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                    label(gui, col.name, theme.frontColor1, GUI_FONT_BOLD, 1.2f);
                    label(gui, col.intervalDays == 1 ? "Due after 1 day" : "Due after " + std::to_string(col.intervalDays) + " days", theme.frontColor2, GUI_FONT_REGULAR, 0.8f);
                }
                anchor_menu(more_button(gui, "column more", [&, colId] { open_menu(OpenMenu::Kind::SCHEDULE_COLUMN, colId); }, 40.0f), OpenMenu::Kind::SCHEDULE_COLUMN, colId);
            }
            gui.clipping_element<ScrollArea>("cards", ScrollArea::Options{
                .scrollVertical = true,
                .clipVertical = true,
                .scrollbarY = ScrollArea::ScrollbarType::NO_INTERACTION,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .childGap = 8, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                        size_t index = 0;
                        for(auto& card : data().scheduleCards) {
                            if(card.columnId != colId)
                                continue;
                            if(drag.dragging && drag.targetColumn == colId && drag.targetIndex == index && card.id != drag.id)
                                drop_indicator(gui);
                            gui.new_id(static_cast<int64_t>(card.id), [&] { schedule_card(card); });
                            if(card.id != drag.id)
                                index++;
                        }
                        if(drag.dragging && drag.targetColumn == colId && drag.targetIndex >= index)
                            drop_indicator(gui);
                    }
                }
            });
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
                button(gui, "create", "Create", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/plus.svg", .height = 44.0f, .scale = 1.1f}, [&, colId] { create_schedule_card(colId); });
            }
        }
    });
}

void MainMenuScreen::schedule_card(ScheduleCard& card) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID id = card.id;
    ItemID colId = card.columnId;
    std::string title = card.title;
    bool isDragged = drag.dragging && drag.id == id;
    int64_t days = data().card_days_until_due(card);
    SkColor4f status = days < 0 ? STATUS_RED : (days <= 1 ? STATUS_YELLOW : STATUS_GREEN);
    bool linked = !card.canvasPath.empty();
    bool missing = linked && !canvas_exists(card.canvasPath);

    gui.element<LayoutElement>("card", [&](LayoutElement* l, const Clay_ElementId& lId) {
        if(l->get_bb().has_value())
            boardItemBBs.push_back({colId, id, l->get_bb().value()});
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(52)},
                .padding = {.left = 14, .right = 4, .top = 8, .bottom = 8},
                .childGap = 8,
                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            },
            .backgroundColor = convert_vec4<Clay_Color>(isDragged ? color_mul_alpha(theme.cardColor, 0.35f) : theme.cardColor),
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 2, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                label(gui, title, isDragged ? theme.frontColor2 : theme.frontColor1, GUI_FONT_BOLD, 1.08f);
                if(missing)
                    label(gui, "Canvas missing", theme.errorColor, GUI_FONT_REGULAR, 0.8f);
                else if(!linked)
                    label(gui, "No canvas", theme.frontColor2, GUI_FONT_REGULAR, 0.8f);
            }
            label(gui, std::to_string(days), theme.frontColor2, GUI_FONT_REGULAR, 0.95f);
            status_square(gui, "status", status, 22.0f);
            SelectableButton* more = more_button(gui, "more", [&, id] { open_menu(OpenMenu::Kind::SCHEDULE_CARD, id); }, 36.0f);
            boardMoreButtons[id] = more;
            anchor_menu(more, OpenMenu::Kind::SCHEDULE_CARD, id);
        }
    }, LayoutElement::Callbacks{
        .onClick = [&, id, title](LayoutElement* l, const InputManager::MouseButtonCallbackArgs& b) {
            if(b.down && l->mouseHovering && b.button == InputManager::MouseButton::LEFT)
                board_press(DragState::Kind::SCHEDULE_CARD, id, title, l, b.pos, b.deviceType == InputManager::MouseDeviceType::TOUCH);
        }
    });
}

void MainMenuScreen::create_schedule_card(ItemID columnId) {
    pick_canvas("New card", true, [this, columnId](const std::optional<std::string>& rel, bool createNew) {
        auto addCard = [this, columnId](const std::string& title, const std::string& canvasPath) {
            ScheduleCard card;
            card.id = data().new_id();
            card.columnId = data().find_schedule_column(columnId) ? columnId : data().scheduleColumns.front().id;
            card.title = title;
            card.canvasPath = canvasPath;
            card.enteredColumnTime = now_time();
            data().scheduleCards.push_back(card);
            changed();
        };
        if(rel.has_value())
            addCard(display_name(rel.value()), rel.value());
        else if(createNew)
            create_and_open_canvas(app.notesFolder, [this, addCard](const std::string& newRel) { addCard(display_name(newRel), newRel); });
        else {
            dialogs.prompt_text("Card title", "", "Create", [addCard](const std::string& title) {
                addCard(title.empty() ? "Untitled" : title, "");
            });
        }
    });
}

void MainMenuScreen::schedule_card_menu() {
    ItemID id = openMenu.id;
    ScheduleCard* card = data().find_schedule_card(id);
    if(!card) {
        close_menu();
        return;
    }
    std::vector<MenuEntry> entries;
    if(canvas_exists(card->canvasPath)) {
        std::string path = card->canvasPath;
        entries.push_back({"Open canvas", [&, path] { open_canvas(path); }});
    }
    entries.push_back({"Mark retrieved", [&, id] {
        data().mark_card_retrieved(id);
        changed();
    }});
    entries.push_back({"Rename", [&, id] {
        ScheduleCard* c = data().find_schedule_card(id);
        if(!c) return;
        dialogs.prompt_text("Rename card", c->title, "Rename", [&, id](const std::string& v) {
            if(ScheduleCard* c2 = data().find_schedule_card(id)) {
                c2->title = v;
                changed();
            }
        });
    }});
    entries.push_back({card->canvasPath.empty() ? "Link canvas..." : "Change canvas...", [&, id] {
        pick_canvas("Link a canvas", false, [this, id](const std::optional<std::string>& rel, bool createNew) {
            auto link = [this, id](const std::string& r) {
                if(ScheduleCard* c = data().find_schedule_card(id)) {
                    c->canvasPath = r;
                    changed();
                }
            };
            if(rel.has_value())
                link(rel.value());
            else if(createNew)
                create_and_open_canvas(app.notesFolder, link);
        });
    }});
    if(!card->canvasPath.empty()) {
        entries.push_back({"Unlink canvas", [&, id] {
            if(ScheduleCard* c = data().find_schedule_card(id)) {
                c->canvasPath.clear();
                changed();
            }
        }});
    }
    entries.push_back({"Delete", [&, id] {
        std::erase_if(data().scheduleCards, [id](auto& c) { return c.id == id; });
        changed();
    }});
    popup_menu(main.g.gui, "schedule card menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::schedule_column_menu() {
    ItemID id = openMenu.id;
    ScheduleColumn* col = data().find_schedule_column(id);
    if(!col) {
        close_menu();
        return;
    }
    std::vector<MenuEntry> entries;
    entries.push_back({"Rename", [&, id] {
        ScheduleColumn* c = data().find_schedule_column(id);
        if(!c) return;
        dialogs.prompt_text("Rename column", c->name, "Rename", [&, id](const std::string& v) {
            if(ScheduleColumn* c2 = data().find_schedule_column(id)) {
                c2->name = v;
                changed();
            }
        });
    }});
    entries.push_back({"Days until due...", [&, id] {
        ScheduleColumn* c = data().find_schedule_column(id);
        if(!c) return;
        dialogs.prompt_text("Days until a card in \"" + c->name + "\" is due", std::to_string(c->intervalDays), "Save", [&, id](const std::string& v) {
            int days = 0;
            try { days = std::stoi(v); } catch(...) { return; }
            if(ScheduleColumn* c2 = data().find_schedule_column(id)) {
                c2->intervalDays = std::clamp(days, 0, 3650);
                changed();
            }
        });
    }});
    entries.push_back({"Move left", [&, id] { move_column(data().scheduleColumns, id, -1); changed(); }});
    entries.push_back({"Move right", [&, id] { move_column(data().scheduleColumns, id, 1); changed(); }});
    entries.push_back({"Delete column", [&, id] {
        dialogs.confirm("Delete this column?", "Its cards move to the first column.", "Delete", [&, id] {
            data().delete_schedule_column(id);
            changed();
        });
    }});
    popup_menu(main.g.gui, "schedule column menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::schedule_header_menu() {
    std::vector<MenuEntry> entries;
    entries.push_back({"Add column", [&] {
        dialogs.prompt_text("New column", "", "Add", [&](const std::string& name) {
            data().scheduleColumns.push_back({data().new_id(), name.empty() ? "New Column" : name, 14});
            changed();
        });
    }});
    entries.push_back({"Save board as template", [&] {
        dialogs.prompt_text("Template name", "My board", "Save", [&](const std::string& name) {
            ScheduleTemplate t;
            t.id = data().new_id();
            t.name = name.empty() ? "My board" : name;
            for(auto& c : data().scheduleColumns)
                t.columns.push_back({c.name, c.intervalDays});
            data().scheduleTemplates.push_back(t);
            changed();
        });
    }});
    popup_menu(main.g.gui, "schedule header menu", openMenu.anchor, entries, [&] { close_menu(); }, 250.0f);
}

void MainMenuScreen::schedule_templates() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    gui.clipping_element<ScrollArea>("schedule templates", ScrollArea::Options{
        .scrollVertical = true,
        .clipVertical = true,
        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
        .innerContent = [&](const ScrollArea::InnerContentParameters&) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 8}, .childGap = 10, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                label(gui, "Templates are column layouts. Applying one replaces the board's columns; cards keep their position.", theme.frontColor2);
                for(auto& t : data().scheduleTemplates) {
                    ItemID id = t.id;
                    gui.new_id(static_cast<int64_t>(id), [&] {
                        card(gui, [&] {
                            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                                label(gui, t.name, theme.frontColor1, GUI_FONT_BOLD, 1.1f);
                                std::string summary;
                                for(auto& c : t.columns)
                                    summary += (summary.empty() ? "" : "   ") + c.name + " (" + std::to_string(c.intervalDays) + "d)";
                                label(gui, summary, theme.frontColor2, GUI_FONT_REGULAR, 0.9f);
                            }
                            button(gui, "apply", "Apply", {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, [&, id] {
                                dialogs.confirm("Apply this template?", "The board's columns are replaced. Cards stay at the same column position.", "Apply", [&, id] {
                                    auto& ts = data().scheduleTemplates;
                                    auto it = std::find_if(ts.begin(), ts.end(), [id](auto& x) { return x.id == id; });
                                    if(it != ts.end()) {
                                        ScheduleTemplate copy = *it;
                                        data().apply_schedule_template(copy);
                                        scheduleTemplatesTab = false;
                                        changed();
                                    }
                                });
                            });
                            anchor_menu(more_button(gui, "more", [&, id] { open_menu(OpenMenu::Kind::SCHEDULE_TEMPLATE, id); }, 36.0f), OpenMenu::Kind::SCHEDULE_TEMPLATE, id);
                        }, CLAY_SIZING_GROW(0), 14, 12, CLAY_LEFT_TO_RIGHT);
                    });
                }
                button(gui, "save board", "Save current board as template", {.kind = ButtonStyle::Kind::SOFT, .iconPath = "data/icons/plus.svg"}, [&] {
                    open_menu(OpenMenu::Kind::NONE);
                    dialogs.prompt_text("Template name", "My board", "Save", [&](const std::string& name) {
                        ScheduleTemplate t;
                        t.id = data().new_id();
                        t.name = name.empty() ? "My board" : name;
                        for(auto& c : data().scheduleColumns)
                            t.columns.push_back({c.name, c.intervalDays});
                        data().scheduleTemplates.push_back(t);
                        changed();
                    });
                });
            }
        }
    });
}

void MainMenuScreen::schedule_template_menu() {
    ItemID id = openMenu.id;
    std::vector<MenuEntry> entries;
    entries.push_back({"Rename", [&, id] {
        auto& ts = data().scheduleTemplates;
        auto it = std::find_if(ts.begin(), ts.end(), [id](auto& x) { return x.id == id; });
        if(it == ts.end()) return;
        dialogs.prompt_text("Rename template", it->name, "Rename", [&, id](const std::string& v) {
            for(auto& t : data().scheduleTemplates)
                if(t.id == id) t.name = v;
            changed();
        });
    }});
    entries.push_back({"Delete", [&, id] {
        std::erase_if(data().scheduleTemplates, [id](auto& t) { return t.id == id; });
        changed();
    }});
    popup_menu(main.g.gui, "schedule template menu", openMenu.anchor, entries, [&] { close_menu(); });
}

// ---------- Goal cycle ----------

void MainMenuScreen::goals_view() {
    auto& gui = main.g.gui;
    if(openGoal.has_value()) {
        if(Goal* g = data().find_goal(openGoal.value())) {
            goal_detail(*g);
            return;
        }
        openGoal = std::nullopt;
    }
    page_header("Goal Cycle", nullptr, [&] {
        anchor_menu(more_button(gui, "goals more", [&] { open_menu(OpenMenu::Kind::GOALS_HEADER); }, 40.0f), OpenMenu::Kind::GOALS_HEADER);
    });
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
        segmented(gui, "goal tabs", {"Board", "Templates"}, goalTemplatesTab ? 1 : 0, [&](size_t i) {
            goalTemplatesTab = i == 1;
            drag = DragState();
            main.g.gui.set_to_layout();
        }, CLAY_SIZING_FIXED(std::min(640.0f, gui.io.windowSize.x() - 60.0f)), 40.0f);
    }
    if(goalTemplatesTab) {
        goal_templates();
        return;
    }
    auto& cols = data().goalColumns;
    float colWidth = board_column_width(cols.size());
    gui.element<LayoutElement>("goal board", [&](LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {
            gui.clipping_element<ScrollArea>("goal columns", ScrollArea::Options{
                .scrollHorizontal = true,
                .clipHorizontal = true,
                .scrollbarX = ScrollArea::ScrollbarType::NORMAL,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_GROW(0)}, .padding = {.bottom = 8}, .childGap = 14, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                        for(auto& col : cols)
                            gui.new_id(static_cast<int64_t>(col.id), [&] { goal_column(col, colWidth); });
                    }
                }
            });
        }
    }, LayoutElement::Callbacks{
        .onClick = [&](LayoutElement*, const InputManager::MouseButtonCallbackArgs& b) {
            if(!b.down)
                board_pointer_up(b.pos);
        },
        .onMotion = [&](LayoutElement*, const InputManager::MouseMotionCallbackArgs& m) {
            board_pointer_motion(m.pos);
        }
    });
}

void MainMenuScreen::goal_column(GoalColumn& col, float width) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID colId = col.id;
    gui.element<LayoutElement>("column", [&](LayoutElement* l, const Clay_ElementId& lId) {
        if(l->get_bb().has_value())
            boardColumnBBs[colId] = l->get_bb().value();
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_GROW(0)},
                .padding = CLAY_PADDING_ALL(8),
                .childGap = 8,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = convert_vec4<Clay_Color>(theme.sidebarColor),
            .cornerRadius = CLAY_CORNER_RADIUS(12)
        }) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(44)}, .padding = {.left = 40}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
                    label(gui, col.name, theme.frontColor1, GUI_FONT_BOLD, 1.2f);
                }
                anchor_menu(more_button(gui, "column more", [&, colId] { open_menu(OpenMenu::Kind::GOAL_COLUMN, colId); }, 40.0f), OpenMenu::Kind::GOAL_COLUMN, colId);
            }
            gui.clipping_element<ScrollArea>("goals", ScrollArea::Options{
                .scrollVertical = true,
                .clipVertical = true,
                .scrollbarY = ScrollArea::ScrollbarType::NO_INTERACTION,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .childGap = 8, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                        size_t index = 0;
                        for(auto& g : data().goals) {
                            if(g.columnId != colId)
                                continue;
                            if(drag.dragging && drag.targetColumn == colId && drag.targetIndex == index && g.id != drag.id)
                                drop_indicator(gui);
                            gui.new_id(static_cast<int64_t>(g.id), [&] { goal_card(g); });
                            if(g.id != drag.id)
                                index++;
                        }
                        if(drag.dragging && drag.targetColumn == colId && drag.targetIndex >= index)
                            drop_indicator(gui);
                    }
                }
            });
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
                button(gui, "create", "Create", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/plus.svg", .height = 44.0f, .scale = 1.1f}, [&, colId] {
                    dialogs.prompt_text("New goal", "", "Create", [&, colId](const std::string& title) {
                        Goal g;
                        g.id = data().new_id();
                        auto& goalCols = data().goalColumns;
                        g.columnId = std::any_of(goalCols.begin(), goalCols.end(), [colId](auto& c) { return c.id == colId; }) ? colId : goalCols.front().id;
                        g.title = title.empty() ? "New goal" : title;
                        data().goals.push_back(g);
                        openGoal = g.id;
                        changed();
                    });
                });
            }
        }
    });
}

void MainMenuScreen::goal_card(Goal& g) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID id = g.id;
    ItemID colId = g.columnId;
    std::string title = g.title;
    bool isDragged = drag.dragging && drag.id == id;
    size_t done = std::count_if(g.entries.begin(), g.entries.end(), [](const GoalEntry& e) { return e.done; });

    gui.element<LayoutElement>("goal", [&](LayoutElement* l, const Clay_ElementId& lId) {
        if(l->get_bb().has_value())
            boardItemBBs.push_back({colId, id, l->get_bb().value()});
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(56)},
                .padding = {.left = 14, .right = 4, .top = 10, .bottom = 10},
                .childGap = 10,
                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            },
            .backgroundColor = convert_vec4<Clay_Color>(isDragged ? color_mul_alpha(theme.cardColor, 0.35f) : theme.cardColor),
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 3, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                label(gui, title, isDragged ? theme.frontColor2 : theme.frontColor1, GUI_FONT_BOLD, 1.05f);
                if(!g.entries.empty())
                    label(gui, std::to_string(done) + " of " + std::to_string(g.entries.size()) + " entries done", theme.frontColor2, GUI_FONT_REGULAR, 0.8f);
            }
            progress_ring(gui, "ring", g.progress / 100.0f, 28.0f);
            SelectableButton* more = more_button(gui, "more", [&, id] { open_menu(OpenMenu::Kind::GOAL_CARD, id); }, 36.0f);
            boardMoreButtons[id] = more;
            anchor_menu(more, OpenMenu::Kind::GOAL_CARD, id);
        }
    }, LayoutElement::Callbacks{
        .onClick = [&, id, title](LayoutElement* l, const InputManager::MouseButtonCallbackArgs& b) {
            if(b.down && l->mouseHovering && b.button == InputManager::MouseButton::LEFT)
                board_press(DragState::Kind::GOAL, id, title, l, b.pos, b.deviceType == InputManager::MouseDeviceType::TOUCH);
        }
    });
}

void MainMenuScreen::goal_detail(Goal& g) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID goalId = g.id;

    page_header("", [&] {
        button(gui, "done", "Done", {.kind = ButtonStyle::Kind::SOFT, .height = 40.0f}, [&] {
            openGoal = std::nullopt;
            main.g.gui.set_to_layout();
        });
    }, [&] {
        anchor_menu(more_button(gui, "goal more", [&, goalId] { open_menu(OpenMenu::Kind::GOAL_DETAIL, goalId); }, 40.0f), OpenMenu::Kind::GOAL_DETAIL, goalId);
    });

    gui.clipping_element<ScrollArea>("goal detail", ScrollArea::Options{
        .scrollVertical = true,
        .clipVertical = true,
        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
        .innerContent = [&](const ScrollArea::InnerContentParameters&) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 8}, .childGap = 10, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                card(gui, [&] {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 12, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                        label(gui, "Goal:", theme.frontColor2, GUI_FONT_REGULAR, 1.1f);
                        text_field(gui, "goal title", &g.title, [&] { app.store.mark_dirty(); });
                    }
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 14, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                        label(gui, "Progress:", theme.frontColor2, GUI_FONT_REGULAR, 1.1f);
                        gui.element<NumberSlider<int>>("progress slider", &g.progress, 0, 100, NumberSliderData{.onChange = [&] { changed(); }});
                        label(gui, std::to_string(g.progress) + "%", theme.frontColor1, GUI_FONT_BOLD);
                        progress_ring(gui, "progress ring", g.progress / 100.0f, 44.0f);
                    }
                }, CLAY_SIZING_GROW(0), 16, 14);
                for(auto& e : g.entries)
                    gui.new_id(static_cast<int64_t>(e.id), [&] { goal_entry_row(g, e); });
                card(gui, [&] {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 40, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                        button(gui, "add canvas", "Add Canvas", {.kind = ButtonStyle::Kind::SOFT, .iconPath = "data/icons/encxded/compose.svg", .height = 50.0f, .scale = 1.1f}, [&, goalId] {
                            pick_canvas("Add a canvas to this goal", false, [this, goalId](const std::optional<std::string>& rel, bool createNew) {
                                auto addEntry = [this, goalId](const std::string& r) {
                                    if(Goal* g2 = data().find_goal(goalId)) {
                                        g2->entries.push_back({data().new_id(), display_name(r), "Canvas", now_time(), false, r});
                                        changed();
                                    }
                                };
                                if(rel.has_value())
                                    addEntry(rel.value());
                                else if(createNew)
                                    create_and_open_canvas(app.notesFolder, addEntry);
                            });
                        });
                        button(gui, "add entry", "Add Entry", {.kind = ButtonStyle::Kind::SOFT, .iconPath = "data/icons/encxded/keyboard.svg", .height = 50.0f, .scale = 1.1f}, [&, goalId] {
                            add_goal_entry_dialog(goalId);
                        });
                    }
                }, CLAY_SIZING_GROW(0), 16);
            }
        }
    });
}

void MainMenuScreen::goal_entry_row(Goal& g, GoalEntry& e) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    ItemID goalId = g.id;
    ItemID entryId = e.id;
    std::string canvasPath = e.canvasPath;
    card(gui, [&] {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT(0)}, .childGap = 4, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
            icon(gui, "kind", canvasPath.empty() ? "data/icons/encxded/keyboard.svg" : "data/icons/encxded/compose.svg", 22.0f, theme.frontColor2);
            round_check(gui, "done", e.done, [&, goalId, entryId] {
                if(Goal* g2 = data().find_goal(goalId)) {
                    if(GoalEntry* e2 = data().find_goal_entry(*g2, entryId)) {
                        e2->done = !e2->done;
                        changed();
                    }
                }
            }, 28.0f);
        }
        auto texts = [&] {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 6, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                label(gui, e.title, theme.frontColor1, GUI_FONT_BOLD, 1.08f);
                if(!e.subtitle.empty())
                    label(gui, e.subtitle, theme.frontColor2, GUI_FONT_REGULAR, 1.02f);
            }
        };
        if(canvasPath.empty())
            texts();
        else {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(48)}}}) {
                gui.element<SelectableButton>("open", SelectableButton::Data{
                    .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                    .onClick = [&, canvasPath] { open_canvas(canvasPath); },
                    .innerContent = [&](const SelectableButton::InnerContentCallbackParameters&) { texts(); }
                });
            }
        }
        label(gui, short_date_string(e.date), theme.frontColor2, GUI_FONT_REGULAR, 1.02f);
        anchor_menu(more_button(gui, "more", [&, entryId] { open_menu(OpenMenu::Kind::GOAL_ENTRY, entryId); }, 36.0f), OpenMenu::Kind::GOAL_ENTRY, entryId);
    }, CLAY_SIZING_GROW(0), 14, 14, CLAY_LEFT_TO_RIGHT);
}

void MainMenuScreen::add_goal_entry_dialog(ItemID goalId) {
    newEntryTitle.clear();
    newEntrySubtitle.clear();
    dialogs.custom("Add entry", 480.0f, [this, goalId](GUIManager& gui) {
        auto& theme = *gui.io.theme;
        label(gui, "Title", theme.frontColor2);
        text_field(gui, "entry title", &newEntryTitle);
        label(gui, "Subtitle", theme.frontColor2);
        text_field(gui, "entry subtitle", &newEntrySubtitle);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 10, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
            button(gui, "cancel", "Cancel", {.kind = ButtonStyle::Kind::SOFT}, [this] { dialogs.close(); });
            button(gui, "add", "Add", {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, [this, goalId] {
                if(Goal* g = data().find_goal(goalId)) {
                    g->entries.push_back({data().new_id(), newEntryTitle.empty() ? "New entry" : newEntryTitle, newEntrySubtitle, now_time(), false, ""});
                    changed();
                }
                dialogs.close();
            });
        }
    });
}

void MainMenuScreen::goal_entry_menu() {
    ItemID entryId = openMenu.id;
    Goal* goal = openGoal.has_value() ? data().find_goal(openGoal.value()) : nullptr;
    GoalEntry* entry = goal ? data().find_goal_entry(*goal, entryId) : nullptr;
    if(!entry) {
        close_menu();
        return;
    }
    ItemID goalId = goal->id;
    std::vector<MenuEntry> entries;
    if(!entry->canvasPath.empty()) {
        std::string path = entry->canvasPath;
        entries.push_back({"Open canvas", [&, path] { open_canvas(path); }});
    }
    entries.push_back({"Edit", [&, goalId, entryId] {
        Goal* g = data().find_goal(goalId);
        GoalEntry* e = g ? data().find_goal_entry(*g, entryId) : nullptr;
        if(!e) return;
        newEntryTitle = e->title;
        newEntrySubtitle = e->subtitle;
        dialogs.custom("Edit entry", 480.0f, [this, goalId, entryId](GUIManager& gui) {
            auto& theme = *gui.io.theme;
            label(gui, "Title", theme.frontColor2);
            text_field(gui, "entry title", &newEntryTitle);
            label(gui, "Subtitle", theme.frontColor2);
            text_field(gui, "entry subtitle", &newEntrySubtitle);
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 10, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                button(gui, "cancel", "Cancel", {.kind = ButtonStyle::Kind::SOFT}, [this] { dialogs.close(); });
                button(gui, "save", "Save", {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, [this, goalId, entryId] {
                    Goal* g2 = data().find_goal(goalId);
                    if(GoalEntry* e2 = g2 ? data().find_goal_entry(*g2, entryId) : nullptr) {
                        e2->title = newEntryTitle;
                        e2->subtitle = newEntrySubtitle;
                        changed();
                    }
                    dialogs.close();
                });
            }
        });
    }});
    auto move = [&, goalId, entryId](int dir) {
        Goal* g = data().find_goal(goalId);
        if(!g) return;
        auto it = std::find_if(g->entries.begin(), g->entries.end(), [entryId](auto& e) { return e.id == entryId; });
        if(it == g->entries.end()) return;
        size_t i = it - g->entries.begin();
        size_t j = static_cast<size_t>(static_cast<int64_t>(i) + dir);
        if(j < g->entries.size()) {
            std::swap(g->entries[i], g->entries[j]);
            changed();
        }
    };
    entries.push_back({"Move up", [move] { move(-1); }});
    entries.push_back({"Move down", [move] { move(1); }});
    entries.push_back({"Delete", [&, goalId, entryId] {
        if(Goal* g = data().find_goal(goalId)) {
            std::erase_if(g->entries, [entryId](auto& e) { return e.id == entryId; });
            changed();
        }
    }});
    popup_menu(main.g.gui, "goal entry menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::goal_card_menu() {
    ItemID id = openMenu.id;
    if(!data().find_goal(id)) {
        close_menu();
        return;
    }
    std::vector<MenuEntry> entries;
    entries.push_back({"Open", [&, id] { openGoal = id; }});
    entries.push_back({"Move up", [&, id] { if(move_within_column(data().goals, id, -1)) changed(); }});
    entries.push_back({"Move down", [&, id] { if(move_within_column(data().goals, id, 1)) changed(); }});
    entries.push_back({"Delete", [&, id] {
        Goal* g = data().find_goal(id);
        if(!g) return;
        dialogs.confirm("Delete \"" + g->title + "\"?", "The goal and its entries are deleted. Linked canvases are kept.", "Delete", [&, id] {
            std::erase_if(data().goals, [id](auto& x) { return x.id == id; });
            changed();
        });
    }});
    popup_menu(main.g.gui, "goal card menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::goal_detail_menu() {
    ItemID id = openMenu.id;
    Goal* goal = data().find_goal(id);
    if(!goal) {
        close_menu();
        return;
    }
    std::vector<MenuEntry> entries;
    for(auto& col : data().goalColumns) {
        if(col.id == goal->columnId)
            continue;
        ItemID colId = col.id;
        entries.push_back({"Move to " + col.name, [&, id, colId] {
            data().move_goal_to_column(id, colId);
            changed();
        }});
    }
    entries.push_back({"Save as template", [&, id] {
        Goal* g = data().find_goal(id);
        if(!g) return;
        GoalTemplate t;
        t.id = data().new_id();
        t.name = g->title;
        for(auto& e : g->entries)
            t.entries.push_back({e.title, e.subtitle});
        data().goalTemplates.push_back(t);
        changed();
        Logger::get().log(Logger::LogType::USERINFO, "Saved \"" + t.name + "\" as a template");
    }});
    entries.push_back({"Delete goal", [&, id] {
        Goal* g = data().find_goal(id);
        if(!g) return;
        dialogs.confirm("Delete \"" + g->title + "\"?", "The goal and its entries are deleted. Linked canvases are kept.", "Delete", [&, id] {
            std::erase_if(data().goals, [id](auto& x) { return x.id == id; });
            openGoal = std::nullopt;
            changed();
        });
    }});
    popup_menu(main.g.gui, "goal detail menu", openMenu.anchor, entries, [&] { close_menu(); }, 240.0f);
}

void MainMenuScreen::goal_column_menu() {
    ItemID id = openMenu.id;
    std::vector<MenuEntry> entries;
    entries.push_back({"Rename", [&, id] {
        auto& cols = data().goalColumns;
        auto it = std::find_if(cols.begin(), cols.end(), [id](auto& c) { return c.id == id; });
        if(it == cols.end()) return;
        dialogs.prompt_text("Rename column", it->name, "Rename", [&, id](const std::string& v) {
            for(auto& c : data().goalColumns)
                if(c.id == id) c.name = v;
            changed();
        });
    }});
    entries.push_back({"Move left", [&, id] { move_column(data().goalColumns, id, -1); changed(); }});
    entries.push_back({"Move right", [&, id] { move_column(data().goalColumns, id, 1); changed(); }});
    entries.push_back({"Delete column", [&, id] {
        dialogs.confirm("Delete this column?", "Its goals move to the first column.", "Delete", [&, id] {
            data().delete_goal_column(id);
            changed();
        });
    }});
    popup_menu(main.g.gui, "goal column menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::goals_header_menu() {
    std::vector<MenuEntry> entries;
    entries.push_back({"Add column", [&] {
        dialogs.prompt_text("New column", "", "Add", [&](const std::string& name) {
            data().goalColumns.push_back({data().new_id(), name.empty() ? "New Column" : name});
            changed();
        });
    }});
    popup_menu(main.g.gui, "goals header menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::goal_templates() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    gui.clipping_element<ScrollArea>("goal templates", ScrollArea::Options{
        .scrollVertical = true,
        .clipVertical = true,
        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
        .innerContent = [&](const ScrollArea::InnerContentParameters&) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 8}, .childGap = 10, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                if(data().goalTemplates.empty())
                    label(gui, "No templates yet. Open a goal and choose \"Save as template\" from its menu.", theme.frontColor2);
                for(auto& t : data().goalTemplates) {
                    ItemID id = t.id;
                    gui.new_id(static_cast<int64_t>(id), [&] {
                        card(gui, [&] {
                            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                                label(gui, t.name, theme.frontColor1, GUI_FONT_BOLD, 1.1f);
                                label(gui, t.entries.size() == 1 ? "1 entry" : std::to_string(t.entries.size()) + " entries", theme.frontColor2, GUI_FONT_REGULAR, 0.9f);
                            }
                            button(gui, "use", "New goal", {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, [&, id] {
                                auto& ts = data().goalTemplates;
                                auto it = std::find_if(ts.begin(), ts.end(), [id](auto& x) { return x.id == id; });
                                if(it == ts.end()) return;
                                Goal g;
                                g.id = data().new_id();
                                g.columnId = data().goalColumns.front().id;
                                g.title = it->name;
                                for(auto& e : it->entries)
                                    g.entries.push_back({data().new_id(), e.title, e.subtitle, now_time(), false, ""});
                                data().goals.push_back(g);
                                goalTemplatesTab = false;
                                openGoal = g.id;
                                changed();
                            });
                            anchor_menu(more_button(gui, "more", [&, id] { open_menu(OpenMenu::Kind::GOAL_TEMPLATE, id); }, 36.0f), OpenMenu::Kind::GOAL_TEMPLATE, id);
                        }, CLAY_SIZING_GROW(0), 14, 12, CLAY_LEFT_TO_RIGHT);
                    });
                }
            }
        }
    });
}

void MainMenuScreen::goal_template_menu() {
    ItemID id = openMenu.id;
    std::vector<MenuEntry> entries;
    entries.push_back({"Rename", [&, id] {
        auto& ts = data().goalTemplates;
        auto it = std::find_if(ts.begin(), ts.end(), [id](auto& x) { return x.id == id; });
        if(it == ts.end()) return;
        dialogs.prompt_text("Rename template", it->name, "Rename", [&, id](const std::string& v) {
            for(auto& t : data().goalTemplates)
                if(t.id == id) t.name = v;
            changed();
        });
    }});
    entries.push_back({"Delete", [&, id] {
        std::erase_if(data().goalTemplates, [id](auto& t) { return t.id == id; });
        changed();
    }});
    popup_menu(main.g.gui, "goal template menu", openMenu.anchor, entries, [&] { close_menu(); });
}

}
