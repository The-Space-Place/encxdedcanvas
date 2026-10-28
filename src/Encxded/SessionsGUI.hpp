// EncxdedCanvas: the "New Session" popup (Before / Focused / After checklists, Pomodoro / Flowmodoro timer)
// and the floating timer pill shown on every screen while a session runs.
#pragma once
#include "MenuData.hpp"
#include "MenuWidgets.hpp"
#include <SDL3/SDL_audio.h>

class MainProgram;

namespace Encxded {

class SessionsGUI {
    public:
        SessionsGUI(MainProgram& main, MenuDataStore& store);
        // Opens on the tab that fits the running session, or Before if there is none
        void open();
        void hide();
        bool is_open() const;
        void update();
        void layout();
        bool input_global_back_button();
    private:
        void popup_layout();
        void pill_layout();
        void header_layout();
        void tab_bar_layout();
        void focus_timer_card();
        void bottom_action_layout();
        void item_card(SessionItem& item, SessionPhase phase);
        void timer_item_controls(SessionItem& item);
        void item_menu(SessionItem& item, SessionPhase phase);
        void add_item_dialog();

        void start_session();
        void end_focus(bool timerRanOut);
        void finish_session();
        void cancel_session();
        void toggle_pause();
        void play_chime();
        std::string timer_text(SDL_Time now);

        MainProgram& main;
        MenuDataStore& store;
        SessionsData& s();

        bool popupOpen = false;
        SessionPhase tab = SessionPhase::BEFORE;
        Widgets::Dialogs dialogs;

        std::optional<ItemID> itemMenuOpen;
        GUIStuff::SelectableButton* itemMenuAnchor = nullptr;

        std::string newItemName;
        size_t newItemType = 0;

        int64_t lastShownSecond = -1;
        SDL_AudioStream* chimeStream = nullptr;
};

}
