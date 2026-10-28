// EncxdedCanvas: main menu with an Ahmni-style sidebar (Notes, Schedule, Goal Cycle, Sessions, Backup, Settings, Trash).
// Replaces InfiniPaint's FileSelectScreen when the mobile UI is used.
#pragma once
#include "../Screens/Screen.hpp"
#include "../Screens/CanvasEditorUI/ProgramSettingsGUI.hpp"
#include "MenuData.hpp"
#include "MenuWidgets.hpp"
#include <mutex>
#include <unordered_map>

namespace Encxded {

class App;

class MainMenuScreen : public Screen {
    public:
        MainMenuScreen(MainProgram& m);
        ~MainMenuScreen();
        virtual void gui_layout_run() override;
        virtual void update() override;
        virtual void draw(SkCanvas* canvas) override;
        virtual void input_open_infinipaint_file_callback(const CustomEvents::OpenInfiniPaintFileEvent& openFile) override;
        virtual void input_global_back_button_callback() override;
        virtual void input_app_about_to_go_to_background_callback() override;
        virtual bool input_keybind_callback(const Vector2ui32& newKey) override;
        virtual void input_mobile_import_canvas_callback(const CustomEvents::MobileImportCanvasEvent& mobileImport) override;

        enum class Section : int {
            NOTES,
            SCHEDULE,
            GOALS,
            BACKUP,
            SETTINGS,
            TRASH
        };

    private:
        App& app;
        MenuData& data();
        void changed(); // Marks the menu data for saving and relayouts

        std::filesystem::path savePath;
        std::filesystem::path trashPath;

        Section section = Section::NOTES;
        void set_section(Section s);

        Widgets::Dialogs dialogs;
        void global_log();

        // Shell
        void sidebar();
        void sidebar_entry(const char* id, const char* iconPath, const char* text, Section s);
        bool sidebar_is_overlay();
        void content_area();
        void page_header(const std::string& title, const std::function<void()>& leftContent, const std::function<void()>& rightContent);
        void sidebar_toggle_if_hidden();

        // Shared "•••" menu: whichever menu is open and the button it hangs off
        struct OpenMenu {
            enum class Kind { NONE, NOTES_HEADER, NOTES_ENTRY, TRASH_ENTRY, SCHEDULE_HEADER, SCHEDULE_COLUMN, SCHEDULE_CARD, SCHEDULE_TEMPLATE, GOALS_HEADER, GOAL_COLUMN, GOAL_CARD, GOAL_DETAIL, GOAL_ENTRY, GOAL_TEMPLATE } kind = Kind::NONE;
            ItemID id = 0;
            std::string path;
            GUIStuff::SelectableButton* anchor = nullptr;
        } openMenu;
        void open_menu(OpenMenu::Kind kind, ItemID id = 0, const std::string& path = "");
        bool menu_is_open(OpenMenu::Kind kind, ItemID id = 0, const std::string& path = "") const;
        void anchor_menu(GUIStuff::SelectableButton* b, OpenMenu::Kind kind, ItemID id = 0, const std::string& path = "");
        void open_menu_layout();
        void close_menu();

        // Canvases and folders
        std::string to_relative(const std::filesystem::path& p);
        std::filesystem::path to_absolute(const std::string& rel);
        std::string display_name(const std::string& relPath);
        bool canvas_exists(const std::string& relPath);
        void open_canvas(const std::string& relPath);
        void create_canvas_in(const std::string& folderRel);
        // Creates an empty canvas, reports its path, then opens it
        void create_and_open_canvas(const std::string& folderRel, const std::function<void(const std::string& relPath)>& onCreated);
        std::vector<std::string> all_canvases();
        std::vector<std::string> all_folders();
        // Lets the user choose a canvas (or make a new one); allowNone adds a "no canvas" option
        void pick_canvas(const std::string& title, bool allowNone, const std::function<void(const std::optional<std::string>& relPath, bool createNew)>& onPick);

        // Notes
        struct NotesEntry {
            std::string name;
            std::string relPath;
            bool isFolder = false;
            uintmax_t size = 0;
            size_t childCount = 0;
            std::filesystem::file_time_type modified;
        };
        std::vector<NotesEntry> notesEntries;
        std::vector<std::string> notesBackHistory;
        std::vector<std::string> notesForwardHistory;
        void refresh_notes();
        void navigate_notes(const std::string& folderRel, bool recordHistory = true);
        void notes_view();
        void notes_row(const NotesEntry& e);
        void notes_entry_menu();
        void notes_header_menu();
        void rename_entry(const std::string& relPath, bool isFolder);
        void move_entry_dialog(const std::string& relPath, bool isFolder);
        void move_entry(const std::string& relPath, bool isFolder, const std::string& toFolderRel);
        void duplicate_canvas(const std::string& relPath);
        void new_folder();

        // Trash
        void load_trash_index();
        void trash_view();
        void trash_entry_menu();
        void move_to_trash(const std::string& relPath, bool isFolder);
        void restore_from_trash(const std::string& trashName);
        void delete_from_trash(const std::string& trashName);

        // Kanban boards (schedule and goal cycle share the drag handling)
        struct DragState {
            enum class Kind { NONE, SCHEDULE_CARD, GOAL } kind = Kind::NONE;
            ItemID id = 0;
            std::string title;
            Vector2f pressPos = {0, 0};
            Vector2f pos = {0, 0};
            Vector2f grabOffset = {0, 0};
            Vector2f cardSize = {0, 0};
            SDL_Time pressTime = 0;
            bool pressIsTouch = false;
            bool moved = false;
            bool dragging = false;
            ItemID targetColumn = 0;
            size_t targetIndex = 0;
        } drag;
        struct BoardHit {
            ItemID columnId;
            ItemID itemId;
            SCollision::AABB<float> bb;
        };
        std::unordered_map<ItemID, SCollision::AABB<float>> boardColumnBBs;
        std::vector<BoardHit> boardItemBBs;
        std::unordered_map<ItemID, GUIStuff::SelectableButton*> boardMoreButtons;
        void board_press(DragState::Kind kind, ItemID id, const std::string& title, GUIStuff::LayoutElement* l, const Vector2f& pos, bool isTouch);
        void board_pointer_motion(const Vector2f& pos);
        void board_pointer_up(const Vector2f& pos);
        void board_update();
        void board_update_target();
        void board_tap(DragState::Kind kind, ItemID id);
        void board_drag_ghost();
        float board_column_width(size_t columnCount);

        // Schedule
        bool scheduleTemplatesTab = false;
        void schedule_view();
        void schedule_column(ScheduleColumn& col, float width);
        void schedule_card(ScheduleCard& card);
        void schedule_templates();
        void schedule_card_menu();
        void schedule_column_menu();
        void schedule_header_menu();
        void schedule_template_menu();
        void create_schedule_card(ItemID columnId);

        // Goal cycle
        bool goalTemplatesTab = false;
        std::optional<ItemID> openGoal;
        void goals_view();
        void goal_column(GoalColumn& col, float width);
        void goal_card(Goal& g);
        void goal_detail(Goal& g);
        void goal_entry_row(Goal& g, GoalEntry& e);
        void goal_templates();
        void goal_column_menu();
        void goal_card_menu();
        void goals_header_menu();
        void goal_detail_menu();
        void goal_entry_menu();
        void goal_template_menu();
        void add_goal_entry_dialog(ItemID goalId);
        std::string newEntryTitle;
        std::string newEntrySubtitle;

        // Backup
        // Native file dialogs can answer on another thread (the Android UI thread), so the result is parked here and
        // handled in update(). Shared so a dialog that outlives the screen doesn't write into freed memory.
        struct PendingFileChoice {
            enum class Kind { SAVE_BACKUP, RESTORE_BACKUP };
            std::mutex mutex;
            std::optional<std::pair<Kind, std::string>> result; // Chosen path (or content:// URI)
        };
        std::shared_ptr<PendingFileChoice> pendingFileChoice = std::make_shared<PendingFileChoice>();
        void show_backup_file_dialog(PendingFileChoice::Kind kind);
        void handle_pending_file_choice();

        std::filesystem::path backupsPath; // Safety backups made before each restore
        std::vector<std::filesystem::path> safetyBackups;
        void refresh_safety_backups();
        void backup_view();
        // Writes everything (canvases, menu data, settings, themes) into one zip. Returns an error message or "".
        std::string write_backup(const std::filesystem::path& zipPath);
        void create_backup(const std::string& destination);
        // rollBack (safety backups): the backup's files and menu data replace the current ones instead of merging
        void confirm_restore(const std::string& zipPath, bool rollBack = false);
        void restore_backup(const std::string& zipPath, bool rollBack);
        std::string backupStatus;

        // Settings
        ProgramSettingsGUI settingsGUI;
        void settings_view();
};

}
