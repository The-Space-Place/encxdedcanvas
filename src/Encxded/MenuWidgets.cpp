#include "MenuWidgets.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include <include/core/SkColorFilter.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkPathBuilder.h>
#include <cmath>

namespace Encxded { namespace Widgets {

using namespace GUIStuff::ElementHelpers;

namespace {
    Clay_Color clay_color(const SkColor4f& c) {
        return convert_vec4<Clay_Color>(c);
    }

    uint64_t hash_combine(uint64_t a, uint64_t b) {
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
}

uint64_t color_key(const SkColor4f& c) {
    return (static_cast<uint64_t>(c.fR * 255.0f) << 24) | (static_cast<uint64_t>(c.fG * 255.0f) << 16) | (static_cast<uint64_t>(c.fB * 255.0f) << 8) | static_cast<uint64_t>(c.fA * 255.0f);
}

CustomDraw::CustomDraw(GUIManager& gui): Element(gui) {}

void CustomDraw::layout(const Clay_ElementId& id, Clay_SizingAxis w, Clay_SizingAxis h, uint64_t newStateKey, const DrawFunc& newDrawFunc) {
    drawFunc = newDrawFunc;
    stateKey = newStateKey;
    CLAY(id, {
        .layout = {.sizing = {.width = w, .height = h}},
        .custom = {.customData = this}
    }) {}
}

void CustomDraw::update() {
    if(stateKey != drawnKey)
        gui.invalidate_draw_element(this);
}

void CustomDraw::clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* command, bool skiaAA) {
    drawnKey = stateKey;
    if(drawFunc && boundingBox.has_value()) {
        auto& bb = boundingBox.value();
        drawFunc(canvas, SkRect::MakeLTRB(bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()), io, skiaAA);
    }
}

TintedIcon::TintedIcon(GUIManager& gui): Element(gui) {}

void TintedIcon::layout(const Clay_ElementId& id, const std::string& svgPath, const SkColor4f& newColor) {
    auto it = gui.io.svgData.find(svgPath);
    if(it == gui.io.svgData.end())
        throw std::runtime_error("[TintedIcon::layout] Could not load icon " + svgPath);
    if(svgDom != it->second || color != newColor)
        gui.invalidate_draw_element(this);
    svgDom = it->second;
    color = newColor;
    CLAY(id, {
        .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
        .custom = {.customData = this}
    }) {}
}

void TintedIcon::clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* command, bool skiaAA) {
    if(!svgDom || !boundingBox.has_value())
        return;
    auto& bb = boundingBox.value();
    canvas->save();
    canvas->translate(bb.min.x(), bb.min.y());
    SkRect r = SkRect::MakeXYWH(0.0f, 0.0f, bb.width(), bb.height());
    canvas->clipRect(r);
    SkPaint blendingPaint;
    blendingPaint.setColorFilter(SkColorFilters::Blend(color, SkColorSpace::MakeSRGB(), SkBlendMode::kSrcIn));
    canvas->saveLayer(r, &blendingPaint);
    canvas->scale(bb.width() / svgDom->containerSize().width(), bb.height() / svgDom->containerSize().height());
    svgDom->render(canvas);
    canvas->restore();
    canvas->restore();
}

void label(GUIManager& gui, std::string_view text, const SkColor4f& color, uint16_t fontId, float scale) {
    CLAY_TEXT(gui.strArena.std_str_to_clay_str(text), CLAY_TEXT_CONFIG({.textColor = clay_color(color), .fontId = fontId, .fontSize = static_cast<uint16_t>(gui.io.fontSize * scale)}));
}

void icon(GUIManager& gui, const char* id, const std::string& svgPath, float size, const SkColor4f& color) {
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size), .height = CLAY_SIZING_FIXED(size)}}}) {
        gui.element<TintedIcon>(id, svgPath, color);
    }
}

void spacer(GUIManager& gui) {
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {}
}

void divider(GUIManager& gui, float horizontalInset) {
    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(1)},
        .padding = {.left = static_cast<uint16_t>(horizontalInset), .right = static_cast<uint16_t>(horizontalInset)}
    }}) {
        CLAY_AUTO_ID({
            .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(1)}},
            .backgroundColor = clay_color(color_mul_alpha(gui.io.theme->frontColor2, 0.18f))
        }) {}
    }
}

SelectableButton* icon_button(GUIManager& gui, const char* id, const std::string& svgPath, const std::function<void()>& onClick, float size, bool isSelected) {
    return svg_icon_button(gui, id, svgPath, {
        .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
        .isSelected = isSelected,
        .size = size,
        .onClick = onClick
    });
}

SelectableButton* tinted_icon_button(GUIManager& gui, const char* id, const std::string& svgPath, const SkColor4f& color, const std::function<void()>& onClick, float size) {
    SelectableButton* toRet = nullptr;
    gui.new_id(id, [&] {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size), .height = CLAY_SIZING_FIXED(size)}}}) {
            toRet = gui.element<SelectableButton>("button", SelectableButton::Data{
                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                .onClick = onClick,
                .innerContent = [&](const SelectableButton::InnerContentCallbackParameters&) {
                    gui.element<TintedIcon>("icon", svgPath, color);
                }
            });
        }
    });
    return toRet;
}

SelectableButton* more_button(GUIManager& gui, const char* id, const std::function<void()>& onClick, float size) {
    return icon_button(gui, id, "data/icons/RemixIcon/more-fill.svg", onClick, size);
}

SelectableButton* button(GUIManager& gui, const char* id, std::string_view text, const ButtonStyle& style, const std::function<void()>& onClick) {
    auto& theme = *gui.io.theme;
    SkColor4f bg{0.0f, 0.0f, 0.0f, 0.0f};
    SkColor4f fg = theme.frontColor1;
    switch(style.kind) {
        case ButtonStyle::Kind::ACCENT:
            bg = theme.accentColor;
            fg = theme.onAccentColor;
            break;
        case ButtonStyle::Kind::SOFT:
            bg = theme.cardColor;
            break;
        case ButtonStyle::Kind::TINT:
            bg = color_mul_alpha(theme.accentColor, 0.2f);
            fg = theme.accentColor;
            break;
        case ButtonStyle::Kind::PLAIN:
            if(style.isSelected)
                bg = theme.cardColor;
            break;
    }
    if(style.color.has_value())
        fg = style.color.value();
    SelectableButton* toRet = nullptr;
    std::string textCopy(text);
    gui.new_id(id, [&] {
        CLAY_AUTO_ID({
            .layout = {.sizing = {.width = style.wide ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIXED(style.height)}},
            .backgroundColor = clay_color(bg),
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            toRet = gui.element<SelectableButton>("button", SelectableButton::Data{
                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                .onClick = onClick,
                .innerContent = [&gui, &style, textCopy, fg](const SelectableButton::InnerContentCallbackParameters&) {
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                            .padding = {.left = 12, .right = 14},
                            .childGap = 10,
                            .childAlignment = {.x = style.centered ? CLAY_ALIGN_X_CENTER : CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                            .layoutDirection = CLAY_LEFT_TO_RIGHT
                        }
                    }) {
                        if(style.iconPath)
                            icon(gui, "icon", style.iconPath, std::round(style.height * 0.5f), fg);
                        if(!textCopy.empty())
                            label(gui, textCopy, fg, style.fontId, style.scale);
                    }
                }
            });
        }
    });
    return toRet;
}

void round_check(GUIManager& gui, const char* id, bool checked, const std::function<void()>& onClick, float size) {
    SkColor4f accent = gui.io.theme->accentColor;
    SkColor4f onAccent = gui.io.theme->onAccentColor;
    gui.new_id(id, [&] {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size + 8.0f), .height = CLAY_SIZING_FIXED(size + 8.0f)}}}) {
            gui.element<SelectableButton>("button", SelectableButton::Data{
                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                .onClick = onClick,
                .innerContent = [&](const SelectableButton::InnerContentCallbackParameters&) {
                    uint64_t key = hash_combine(hash_combine(checked ? 1 : 2, color_key(accent)), color_key(onAccent));
                    gui.element<CustomDraw>("check", CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size), key, [checked, accent, onAccent](SkCanvas* canvas, const SkRect& r, UpdateInputData&, bool aa) {
                        float radius = std::min(r.width(), r.height()) * 0.5f - 1.5f;
                        SkPaint p;
                        p.setAntiAlias(aa);
                        p.setColor4f(accent);
                        if(checked) {
                            canvas->drawCircle(r.centerX(), r.centerY(), radius, p);
                            SkPathBuilder check;
                            check.moveTo(r.centerX() - radius * 0.45f, r.centerY() + radius * 0.02f);
                            check.lineTo(r.centerX() - radius * 0.1f, r.centerY() + radius * 0.37f);
                            check.lineTo(r.centerX() + radius * 0.48f, r.centerY() - radius * 0.3f);
                            SkPaint cp;
                            cp.setAntiAlias(aa);
                            cp.setColor4f(onAccent);
                            cp.setStyle(SkPaint::kStroke_Style);
                            cp.setStrokeWidth(std::max(2.0f, radius * 0.18f));
                            cp.setStrokeCap(SkPaint::kRound_Cap);
                            cp.setStrokeJoin(SkPaint::kRound_Join);
                            canvas->drawPath(check.detach(), cp);
                        }
                        else {
                            p.setStyle(SkPaint::kStroke_Style);
                            p.setStrokeWidth(2.0f);
                            canvas->drawCircle(r.centerX(), r.centerY(), radius - 0.5f, p);
                        }
                    });
                }
            });
        }
    });
}

void progress_ring(GUIManager& gui, const char* id, float fraction, float size) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    SkColor4f accent = gui.io.theme->accentColor;
    SkColor4f track = gui.io.theme->backColor2;
    uint64_t key = hash_combine(hash_combine(static_cast<uint64_t>(fraction * 10000.0f), color_key(accent)), color_key(track));
    gui.element<CustomDraw>(id, CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size), key, [fraction, accent, track](SkCanvas* canvas, const SkRect& r, UpdateInputData&, bool aa) {
        float stroke = std::max(3.0f, r.width() * 0.13f);
        SkRect ring = r.makeInset(stroke * 0.5f + 1.0f, stroke * 0.5f + 1.0f);
        SkPaint p;
        p.setAntiAlias(aa);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(stroke);
        p.setColor4f(track);
        canvas->drawOval(ring, p);
        if(fraction > 0.0f) {
            p.setColor4f(accent);
            p.setStrokeCap(SkPaint::kRound_Cap);
            canvas->drawArc(ring, -90.0f, std::max(fraction * 360.0f, 1.0f), false, p);
        }
    });
}

void status_square(GUIManager& gui, const char* id, const SkColor4f& color, float size) {
    gui.element<CustomDraw>(id, CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size), color_key(color), [color](SkCanvas* canvas, const SkRect& r, UpdateInputData&, bool aa) {
        SkPaint p;
        p.setAntiAlias(aa);
        p.setColor4f(color);
        canvas->drawRoundRect(r, r.width() * 0.18f, r.width() * 0.18f, p);
    });
}

void segmented(GUIManager& gui, const char* id, const std::vector<std::string>& options, size_t selected, const std::function<void(size_t)>& onSelect, Clay_SizingAxis width, float height) {
    auto& theme = *gui.io.theme;
    gui.new_id(id, [&] {
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = width, .height = CLAY_SIZING_FIXED(height)},
                .padding = CLAY_PADDING_ALL(3),
                .childGap = 3,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            },
            .backgroundColor = clay_color(color_mul_alpha(theme.backColor0, 0.55f)),
            .cornerRadius = CLAY_CORNER_RADIUS(8)
        }) {
            for(size_t i = 0; i < options.size(); i++) {
                gui.new_id(static_cast<int64_t>(i), [&] {
                    CLAY_AUTO_ID({
                        .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                        .backgroundColor = clay_color(i == selected ? color_mul_alpha(theme.fillColor2, 0.7f) : SkColor4f{0.0f, 0.0f, 0.0f, 0.0f}),
                        .cornerRadius = CLAY_CORNER_RADIUS(6)
                    }) {
                        std::string text = options[i];
                        gui.element<SelectableButton>("option", SelectableButton::Data{
                            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                            .onClick = [onSelect, i] { if(onSelect) onSelect(i); },
                            .innerContent = [&gui, text, isSel = i == selected](const SelectableButton::InnerContentCallbackParameters&) {
                                label(gui, text, gui.io.theme->frontColor1, isSel ? GUI_FONT_BOLD : GUI_FONT_REGULAR, 0.9f);
                            }
                        });
                    }
                });
            }
        }
    });
}

void card(GUIManager& gui, const std::function<void()>& content, Clay_SizingAxis width, uint16_t padding, uint16_t childGap, Clay_LayoutDirection dir) {
    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = width, .height = CLAY_SIZING_FIT(0)},
            .padding = CLAY_PADDING_ALL(padding),
            .childGap = childGap,
            .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
            .layoutDirection = dir
        },
        .backgroundColor = clay_color(gui.io.theme->cardColor),
        .cornerRadius = CLAY_CORNER_RADIUS(10)
    }) {
        content();
    }
}

void floating_action_button(GUIManager& gui, const char* id, const std::string& svgPath, const std::function<void()>& onClick) {
    constexpr float SIZE = 60.0f;
    gui.set_z_index(gui.get_z_index() + 5, [&] {
        gui.new_id(id, [&] {
            CLAY_AUTO_ID({
                .layout = {.sizing = {.width = CLAY_SIZING_FIXED(SIZE), .height = CLAY_SIZING_FIXED(SIZE)}},
                .backgroundColor = clay_color(gui.io.theme->accentColor),
                .cornerRadius = CLAY_CORNER_RADIUS(SIZE * 0.5f),
                .floating = {
                    .offset = {-28, -28},
                    .zIndex = gui.get_z_index(),
                    .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM, .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM},
                    .attachTo = CLAY_ATTACH_TO_PARENT
                }
            }) {
                gui.element<SelectableButton>("button", SelectableButton::Data{
                    .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                    .onClick = onClick,
                    .innerContent = [&](const SelectableButton::InnerContentCallbackParameters&) {
                        icon(gui, "icon", svgPath, 28.0f, gui.io.theme->onAccentColor);
                    }
                });
            }
        });
    });
}

void popup_menu(GUIManager& gui, const char* id, SelectableButton* anchor, const std::vector<MenuEntry>& entries, const std::function<void()>& close, float width) {
    if(!anchor || !anchor->get_bb().has_value())
        return;
    gui.set_z_index(gui.get_z_index() + 50, [&] {
        dropdown_many_element_popup_layout(gui, id, {
            .button = anchor,
            .clickUpAwayCallback = [&gui, close] {
                gui.set_post_callback_func_high_priority([close] { if(close) close(); });
            },
            .entrySize = {width, 42.0f},
            .entryCount = entries.size(),
            .entryLayout = [&gui, entries, close](size_t i) {
                auto action = entries[i].action;
                text_button(gui, "entry", entries[i].label, {
                    .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                    .wide = true,
                    .growHeight = true,
                    .centered = false,
                    .onClick = [close, action] {
                        if(close) close();
                        if(action) action();
                    }
                });
            }
        });
    });
}

void modal_frame(GUIManager& gui, const char* id, float width, std::optional<float> height, const std::function<void()>& content, const std::function<void()>& onClickOutside) {
    auto& io = gui.io;
    float panelWidth = std::max(std::min(width, io.windowSize.x() - 24.0f), 100.0f);
    float maxHeight = std::max(io.safeWindowRect.height() - 24.0f, 100.0f);
    gui.set_z_index(gui.get_z_index() + 100, [&] {
        gui.element<LayoutElement>(id, [&](LayoutElement*, const Clay_ElementId& lId) {
            CLAY(lId, {
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIXED(io.windowSize.x()), .height = CLAY_SIZING_FIXED(io.windowSize.y())},
                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}
                },
                .backgroundColor = {0.0f, 0.0f, 0.0f, 0.55f},
                .floating = {.zIndex = gui.get_z_index(), .attachTo = CLAY_ATTACH_TO_ROOT}
            }) {
                gui.element<LayoutElement>("panel", [&](LayoutElement*, const Clay_ElementId& pId) {
                    CLAY(pId, {
                        .layout = {
                            .sizing = {.width = CLAY_SIZING_FIXED(panelWidth), .height = height.has_value() ? CLAY_SIZING_FIXED(std::min(height.value(), maxHeight)) : CLAY_SIZING_FIT(0, maxHeight)},
                            .padding = CLAY_PADDING_ALL(16),
                            .childGap = 12,
                            .layoutDirection = CLAY_TOP_TO_BOTTOM
                        },
                        .backgroundColor = clay_color(io.theme->pageColor),
                        .cornerRadius = CLAY_CORNER_RADIUS(14),
                        .border = {.color = clay_color(color_mul_alpha(io.theme->frontColor2, 0.15f)), .width = CLAY_BORDER_OUTSIDE(1)}
                    }) {
                        content();
                    }
                });
            }
        }, LayoutElement::Callbacks{
            .onClick = [&gui, onClickOutside](LayoutElement* l, const InputManager::MouseButtonCallbackArgs& b) {
                if(b.down && l->mouseHovering && !l->childMouseHovering && onClickOutside) {
                    gui.set_post_callback_func_high_priority(onClickOutside);
                    gui.set_to_layout();
                }
            }
        });
    });
}

void Dialogs::prompt_text(const std::string& newTitle, const std::string& initialValue, const std::string& newConfirmLabel, const std::function<void(const std::string&)>& newOnConfirm) {
    kind = Kind::PROMPT;
    title = newTitle;
    value = initialValue;
    confirmLabel = newConfirmLabel;
    onConfirmText = newOnConfirm;
    width = 420.0f;
    focusTextBox = true;
}

void Dialogs::confirm(const std::string& newTitle, const std::string& newMessage, const std::string& newConfirmLabel, const std::function<void()>& newOnConfirm) {
    kind = Kind::CONFIRM;
    title = newTitle;
    message = newMessage;
    confirmLabel = newConfirmLabel;
    onConfirm = newOnConfirm;
    width = 420.0f;
}

void Dialogs::custom(const std::string& newTitle, float newWidth, const std::function<void(GUIManager&)>& content) {
    kind = Kind::CUSTOM;
    title = newTitle;
    width = newWidth;
    customContent = content;
}

void Dialogs::close() {
    kind = Kind::NONE;
    onConfirm = nullptr;
    onConfirmText = nullptr;
    customContent = nullptr;
}

bool Dialogs::is_open() const {
    return kind != Kind::NONE;
}

void Dialogs::layout(GUIManager& gui) {
    if(kind == Kind::NONE)
        return;
    auto& theme = *gui.io.theme;
    auto confirmText = [this] {
        auto f = onConfirmText;
        std::string v = value;
        close();
        if(f) f(v);
    };
    auto confirmPlain = [this] {
        auto f = onConfirm;
        close();
        if(f) f();
    };
    modal_frame(gui, "dialog", width, std::nullopt, [&] {
        label(gui, title, theme.frontColor1, GUI_FONT_BOLD, 1.15f);
        switch(kind) {
            case Kind::PROMPT: {
                CLAY_AUTO_ID({
                    .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(8)},
                    .backgroundColor = clay_color(color_mul_alpha(theme.backColor0, 0.6f)),
                    .cornerRadius = CLAY_CORNER_RADIUS(6)
                }) {
                    TextBox<std::string>* tb = input_text(gui, "dialog text", &value, {.onEnter = confirmText});
                    if(focusTextBox && tb->get_bb().has_value()) {
                        focusTextBox = false;
                        gui.set_post_callback_func([tb] { tb->select(); });
                    }
                }
                break;
            }
            case Kind::CONFIRM:
                label(gui, message, theme.frontColor2);
                break;
            case Kind::CUSTOM:
                if(customContent)
                    customContent(gui);
                break;
            case Kind::NONE:
                break;
        }
        if(kind == Kind::PROMPT || kind == Kind::CONFIRM) {
            CLAY_AUTO_ID({.layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                .childGap = 10,
                .childAlignment = {.x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }}) {
                button(gui, "cancel", "Cancel", {.kind = ButtonStyle::Kind::SOFT}, [this] { close(); });
                if(kind == Kind::PROMPT)
                    button(gui, "ok", confirmLabel, {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, confirmText);
                else
                    button(gui, "ok", confirmLabel, {.kind = ButtonStyle::Kind::ACCENT, .fontId = GUI_FONT_BOLD}, confirmPlain);
            }
        }
    }, [this] { close(); });
}

}}
