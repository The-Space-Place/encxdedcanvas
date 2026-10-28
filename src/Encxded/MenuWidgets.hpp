// EncxdedCanvas: Ahmni-style building blocks shared by the main menu and the sessions popup
#pragma once
#include "../GUIStuff/GUIManager.hpp"
#include "../GUIStuff/Elements/SelectableButton.hpp"
#include "../GUIStuff/Elements/LayoutElement.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Encxded { namespace Widgets {

using namespace GUIStuff;

// Element drawn by a callback. Redraws when stateKey changes, so pass something that captures everything the draw depends on.
class CustomDraw : public Element {
    public:
        typedef std::function<void(SkCanvas* canvas, const SkRect& r, UpdateInputData& io, bool skiaAA)> DrawFunc;
        CustomDraw(GUIManager& gui);
        void layout(const Clay_ElementId& id, Clay_SizingAxis w, Clay_SizingAxis h, uint64_t stateKey, const DrawFunc& drawFunc);
        virtual void update() override;
        virtual void clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* command, bool skiaAA) override;
    private:
        DrawFunc drawFunc;
        uint64_t stateKey = 0;
        uint64_t drawnKey = 0;
};

// SVG icon drawn in any color (SVGIcon only uses theme front/back colors)
class TintedIcon : public Element {
    public:
        TintedIcon(GUIManager& gui);
        void layout(const Clay_ElementId& id, const std::string& svgPath, const SkColor4f& color);
        virtual void clay_draw(SkCanvas* canvas, UpdateInputData& io, Clay_RenderCommand* command, bool skiaAA) override;
    private:
        sk_sp<SkSVGDOM> svgDom;
        SkColor4f color;
};

uint64_t color_key(const SkColor4f& c);

void label(GUIManager& gui, std::string_view text, const SkColor4f& color, uint16_t fontId = GUI_FONT_REGULAR, float scale = 1.0f);
void icon(GUIManager& gui, const char* id, const std::string& svgPath, float size, const SkColor4f& color);
void spacer(GUIManager& gui);
void divider(GUIManager& gui, float horizontalInset = 0.0f);

SelectableButton* icon_button(GUIManager& gui, const char* id, const std::string& svgPath, const std::function<void()>& onClick, float size = 34.0f, bool isSelected = false);
// Icon button whose icon is drawn in a fixed color (for icons on accent-colored surfaces)
SelectableButton* tinted_icon_button(GUIManager& gui, const char* id, const std::string& svgPath, const SkColor4f& color, const std::function<void()>& onClick, float size = 30.0f);
SelectableButton* more_button(GUIManager& gui, const char* id, const std::function<void()>& onClick, float size = 30.0f);

struct ButtonStyle {
    enum class Kind {
        ACCENT,  // Filled with the accent color (Start Session, Create)
        SOFT,    // Filled with the card color (Add Canvas, Add Entry, Hide)
        TINT,    // Faint accent fill with accent text (Focus)
        PLAIN    // No fill (sidebar rows, "+ Add Item")
    } kind = Kind::SOFT;
    const char* iconPath = nullptr;
    bool wide = false;
    bool isSelected = false;
    bool centered = true;
    float height = 40.0f;
    float scale = 1.0f;
    uint16_t fontId = GUI_FONT_REGULAR;
    std::optional<SkColor4f> color; // Overrides the text/icon color
};
SelectableButton* button(GUIManager& gui, const char* id, std::string_view text, const ButtonStyle& style, const std::function<void()>& onClick);

// Round checkbox (accent ring, filled with a checkmark when checked)
void round_check(GUIManager& gui, const char* id, bool checked, const std::function<void()>& onClick, float size = 30.0f);
void progress_ring(GUIManager& gui, const char* id, float fraction, float size);
void status_square(GUIManager& gui, const char* id, const SkColor4f& color, float size = 22.0f);
// Row of options where one is highlighted (Board / Templates, Pomodoro / Flowmodoro, 1-5 ratings)
void segmented(GUIManager& gui, const char* id, const std::vector<std::string>& options, size_t selected, const std::function<void(size_t)>& onSelect, Clay_SizingAxis width = CLAY_SIZING_GROW(0), float height = 36.0f);

// Rounded container in the card color
void card(GUIManager& gui, const std::function<void()>& content, Clay_SizingAxis width = CLAY_SIZING_GROW(0), uint16_t padding = 12, uint16_t childGap = 8, Clay_LayoutDirection dir = CLAY_TOP_TO_BOTTOM);

// Round accent button floating in the bottom-right corner of the parent
void floating_action_button(GUIManager& gui, const char* id, const std::string& svgPath, const std::function<void()>& onClick);

struct MenuEntry {
    std::string label;
    std::function<void()> action;
};
// Popup list anchored to a button (used for every "•••" menu). close() is called after an entry runs or on a tap elsewhere.
void popup_menu(GUIManager& gui, const char* id, SelectableButton* anchor, const std::vector<MenuEntry>& entries, const std::function<void()>& close, float width = 220.0f);

// Dimmed full-window layer with a centered panel. onClickOutside runs when the dim area is tapped.
void modal_frame(GUIManager& gui, const char* id, float width, std::optional<float> height, const std::function<void()>& content, const std::function<void()>& onClickOutside);

// Small modal dialogs: text prompt, confirmation, or custom content
class Dialogs {
    public:
        void prompt_text(const std::string& title, const std::string& initialValue, const std::string& confirmLabel, const std::function<void(const std::string&)>& onConfirm);
        void confirm(const std::string& title, const std::string& message, const std::string& confirmLabel, const std::function<void()>& onConfirm);
        void custom(const std::string& title, float width, const std::function<void(GUIManager&)>& content);
        void close();
        bool is_open() const;
        void layout(GUIManager& gui);
    private:
        enum class Kind { NONE, PROMPT, CONFIRM, CUSTOM } kind = Kind::NONE;
        std::string title;
        std::string message;
        std::string value;
        std::string confirmLabel;
        float width = 400.0f;
        bool focusTextBox = false;
        std::function<void(const std::string&)> onConfirmText;
        std::function<void()> onConfirm;
        std::function<void(GUIManager&)> customContent;
};

}}
