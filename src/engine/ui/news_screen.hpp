#pragma once
// Port addition (@orig none): the main menu's online part (GitHub issue #36),
// see core/news.hpp for what is fetched and when.
//
// MenuOnline: what the main menu asks the game (Game implements it).
//
// UpdatesQuestion: while the player has not answered (GameConfig::
// check_updates is -1), the main menu opens over itself a small window in the
// enhancements window's look: "ONLINE UPDATES", "CHECK ONLINE FOR NEW
// VERSIONS / AND NEWS OF THE PORT?", "YES" and "NO" side by side (YES
// focused). Left / right ('4' / '6', up / down too) move the focus, Enter
// answers it, Escape and N answer no, Y (O, J) yes; a mouse press on a button
// answers it. The enhancements window's "game" page changes the answer later.
//
// UpdateDialog: what a press on the "UPDATE TO x.y.z" line opens
// when the game can install that version itself (MenuOnline::can_install, core/
// self_update.hpp), in the question's look. It asks "INSTALL IT NOW? THE GAME
// RESTARTS AFTERWARDS" with INSTALL, WEB PAGE (the release's page, as before)
// and LATER; INSTALL goes on to the download, with its percentage and CANCEL,
// "INSTALLING..." and, once done, "RESTARTING...": the program ends and
// starts again (MenuOnline::restart). A failure says why, with WEB PAGE and
// BACK. Left / right move the focus, Enter presses the focused button, Escape
// is LATER, CANCEL or BACK; a mouse press on a button presses it. The menu
// passes the time (update) so the dialog follows the install.
//
// NewsScreen: the news, newest first, in a window as large as the
// enhancements one: for each, its day (DD.MM.YYYY) with a red "NEW" after it
// when it is not read yet, its title in green, its text in
// dim green, wrapped to the window. Up / down scroll a line, left / right a
// page, Enter and Escape close, as does a press on "BACK"; presses on the
// arrows shown at the top and bottom of the text when it does not fit scroll
// a page. The main menu marks the news read once it closes.

#include "core/font.hpp"
#include "core/news.hpp"
#include "core/self_update.hpp"
#include "core/surface.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crusader {

class MenuOnline {
public:
    virtual ~MenuOnline() = default;
    // The player has not said whether the game may go online.
    virtual bool ask() const = 0;
    virtual void answer(bool check) = 0;
    // The newer version of the port released ("1.79.0"), empty when none is
    // known.
    virtual std::string new_version() const = 0;
    // Opens the page of that release in the browser.
    virtual void open_release() = 0;
    // The game can install that version itself here: set up by the program,
    // an archive for this system in the release, a folder it can write to.
    virtual bool can_install() = 0;
    // Starts it (a new try after a failure or a cancel starts again); the
    // status says how far it is.
    virtual void install() = 0;
    virtual void cancel_install() = 0;
    virtual UpdateStatus install_status() const = 0;
    // Installed: the program ends and starts again.
    virtual void restart() = 0;
    // The news to show, newest first (select_news), and which are read;
    // mark_news_read() reads them all.
    virtual const std::vector<NewsItem>& news() const = 0;
    virtual NewsRead news_read() const = 0;
    virtual void mark_news_read() = 0;
};

// MISC.PAL colours of the port's menu windows.
namespace menu_colour {
inline constexpr std::uint8_t kFrame = 0x57, kGreen = 0x0C, kDimGreen = 0x75, kRed = 0x18, kWhite = 0xFF;
}

class UpdatesQuestion {
public:
    static constexpr int kFont = 14;
    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };
    explicit UpdatesQuestion(const FontSet& fonts) : fonts_(&fonts) {}

    // Over what the screen holds.
    void draw(Surface8& screen) const;
    void key(std::uint16_t code);
    void mouse_down(int x, int y);

    // Set once answered.
    std::optional<bool> result() const { return result_; }
    bool yes_focused() const { return yes_focused_; }
    Rect button(bool yes) const;

private:
    const FontSet* fonts_;
    bool yes_focused_ = true;
    std::optional<bool> result_;
};

class UpdateDialog {
public:
    static constexpr int kFont = 14;
    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };
    enum class Step { kAsk, kRunning, kFailed };
    enum class Action { kInstall, kPage, kLater, kCancel, kBack };
    // `title`: "UPDATE TO x.y.z", translated.
    UpdateDialog(const FontSet& fonts, MenuOnline& online, std::string title);

    // Over what the screen holds.
    void draw(Surface8& screen) const;
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    // At every pass of the menu: follows the install.
    void update();
    bool closed() const { return closed_; }

    Step step() const { return step_; }
    const UpdateStatus& status() const { return status_; }
    // The buttons of the step, left to right, and the one with the focus.
    const std::vector<Action>& buttons() const { return buttons_; }
    int focused() const { return focused_; }
    // Each button as wide as its name needs (at least kButtonWidth).
    Rect button(int index) const;
    std::string button_label(Action action) const;
    // Wider than the question's: the German names of the buttons ("INSTALLIEREN")
    // and the lines ("DIE INSTALLATION IST FEHLGESCHLAGEN") need it.
    static constexpr int kWindowX0 = 40, kWindowX1 = 599;
    static constexpr int kButtonWidth = 132, kButtonPad = 14, kButtonGap = 14;

private:
    void press(Action action);
    void set_step(Step step);

    const FontSet* fonts_;
    MenuOnline* online_;
    std::string title_;
    Step step_ = Step::kAsk;
    UpdateStatus status_;
    std::vector<Action> buttons_;
    int focused_ = 0;
    bool closed_ = false;
    bool restarted_ = false;
};

class NewsScreen {
public:
    static constexpr int kFont = 14;
    static constexpr int kLineHeight = 20;
    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };
    // `read`: the news not read are marked new.
    NewsScreen(const FontSet& fonts, const std::vector<NewsItem>& news, const NewsRead& read);

    void draw(Surface8& screen) const;
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    bool closed() const { return closed_; }

    struct Line {
        enum class Kind { kDay, kTitle, kBody, kGap };
        Kind kind = Kind::kGap;
        std::string text;
        bool unread = false;  // kDay: the news is new
    };
    const std::vector<Line>& lines() const { return lines_; }
    int first_line() const { return first_; }
    static int visible_lines();
    Rect back_button() const;
    Rect arrow(bool up) const;

private:
    void scroll(int lines);

    const FontSet* fonts_;
    std::vector<Line> lines_;
    int first_ = 0;
    bool closed_ = false;
};

}  // namespace crusader
