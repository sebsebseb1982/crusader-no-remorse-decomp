#include "engine/ui/news_screen.hpp"

#include "core/language.hpp"
#include "engine/ui/enhancements_screen.hpp"
#include "engine/ui/gump_keys.hpp"

#include <algorithm>

namespace crusader {

using namespace menu_colour;

namespace {

constexpr int kTextDrop = 17;  // from the top of a line to the pen position

// The question's window.
constexpr int kAskX0 = 80, kAskY0 = 150, kAskX1 = 559, kAskY1 = 329;
constexpr int kAskTitleY = 186, kAskLineY = 222, kAskLineStep = 22;
constexpr int kAskButtonY0 = 276, kAskButtonHeight = 24, kAskButtonWidth = 130, kAskButtonGap = 20;

// The news window.
constexpr int kPanelX0 = 60, kPanelY0 = 34, kPanelX1 = 579, kPanelY1 = 465;
constexpr int kTitleY = 66;
constexpr int kTextX0 = 90, kTextX1 = 549, kTextY0 = 84, kTextY1 = 420;
constexpr int kArrowX0 = 556, kArrowSize = 12;
constexpr int kBackX0 = 230, kBackX1 = 409, kBackY0 = 432, kBackHeight = 24;
constexpr std::uint16_t kPageUp = 0x149, kPageDown = 0x151;

int text_width(const FontSet& fonts, int font, const std::string& text) {
    return TextWriter(fonts, font).measure(text).width;
}

// A button of the port's windows: its frame, its name centred.
void draw_button(Surface8& screen, const FontSet& fonts, int font, int x0, int y0, int x1, int y1,
                 const std::string& label, bool focused) {
    const std::uint8_t tint = focused ? kGreen : kDimGreen;
    draw_rounded_box(screen, x0, y0, x1, y1, -1, tint);
    const int width = text_width(fonts, font, label);
    print_tinted(screen, fonts, font, x0 + (x1 - x0 + 1 - width) / 2, y0 + kTextDrop, label, tint);
}

// `text` cut into lines of at most `width` pixels, at its spaces when it
// can, at '\n' always.
std::vector<std::string> wrap(const FontSet& fonts, int font, const std::string& text, int width) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string paragraph = text.substr(start, end - start);
        std::string line;
        std::size_t pos = 0;
        while (pos < paragraph.size()) {
            const std::size_t space = paragraph.find(' ', pos);
            std::string word = paragraph.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
            pos = space == std::string::npos ? paragraph.size() : space + 1;
            if (word.empty()) continue;
            const std::string joined = line.empty() ? word : line + " " + word;
            if (text_width(fonts, font, joined) <= width) {
                line = joined;
                continue;
            }
            if (!line.empty()) lines.push_back(line);
            // A word wider than a line, cut where it must.
            while (text_width(fonts, font, word) > width && word.size() > 1) {
                std::size_t n = word.size() - 1;
                while (n > 1 && text_width(fonts, font, word.substr(0, n)) > width) --n;
                lines.push_back(word.substr(0, n));
                word.erase(0, n);
            }
            line = word;
        }
        lines.push_back(line);
        if (end >= text.size()) break;
        start = end + 1;
    }
    return lines;
}

}  // namespace

// --- UpdatesQuestion

// @orig none: port's online news
UpdatesQuestion::Rect UpdatesQuestion::button(bool yes) const {
    const int total = 2 * kAskButtonWidth + kAskButtonGap;
    const int x0 = (kAskX0 + kAskX1 + 1 - total) / 2 + (yes ? 0 : kAskButtonWidth + kAskButtonGap);
    return {x0, kAskButtonY0, x0 + kAskButtonWidth - 1, kAskButtonY0 + kAskButtonHeight - 1};
}

// @orig none: port's online news
void UpdatesQuestion::draw(Surface8& screen) const {
    draw_rounded_box(screen, kAskX0, kAskY0, kAskX1, kAskY1, 0, kFrame);
    draw_rounded_box(screen, kAskX0 + 3, kAskY0 + 3, kAskX1 - 3, kAskY1 - 3, -1, kFrame);
    const auto centred = [&](int y, const std::string& text, std::uint8_t tint) {
        print_tinted(screen, *fonts_, kFont, (kAskX0 + kAskX1 + 1 - text_width(*fonts_, kFont, text)) / 2, y, text,
                     tint);
    };
    centred(kAskTitleY, tr("ONLINE UPDATES"), kGreen);
    centred(kAskLineY, tr("CHECK ONLINE FOR NEW VERSIONS"), kDimGreen);
    centred(kAskLineY + kAskLineStep, tr("AND NEWS OF THE PORT?"), kDimGreen);
    for (const bool yes : {true, false}) {
        const Rect r = button(yes);
        draw_button(screen, *fonts_, kFont, r.x0, r.y0, r.x1, r.y1, yes ? tr("YES") : tr("NO"), yes == yes_focused_);
    }
}

// @orig none: port's online news
void UpdatesQuestion::key(std::uint16_t code) {
    if (result_) return;
    switch (code) {
    case gump_key::kLeft:
    case gump_key::kRight:
    case gump_key::kUp:
    case gump_key::kDown:
    case '4':
    case '6':
    case '8':
    case '2': yes_focused_ = !yes_focused_; break;
    case gump_key::kEnter: result_ = yes_focused_; break;
    case gump_key::kEscape:
    case 'n':
    case 'N': result_ = false; break;
    case 'y':
    case 'Y':
    case 'o':
    case 'O':
    case 'j':
    case 'J': result_ = true; break;
    default: break;
    }
}

// @orig none: port's online news
void UpdatesQuestion::mouse_down(int x, int y) {
    if (result_) return;
    for (const bool yes : {true, false}) {
        if (button(yes).contains(x, y)) {
            yes_focused_ = yes;
            result_ = yes;
        }
    }
}

// --- NewsScreen

// @orig none: port's online news
NewsScreen::NewsScreen(const FontSet& fonts, const std::vector<NewsItem>& news, const NewsRead& read) : fonts_(&fonts) {
    const int width = kTextX1 - kTextX0 + 1;
    for (std::size_t i = 0; i < news.size(); ++i) {
        const NewsItem& item = news[i];
        if (i > 0) lines_.push_back({Line::Kind::kGap, {}, false});
        lines_.push_back({Line::Kind::kDay, format_day(item.date), !read.has(item)});
        for (std::string& l : wrap(fonts, kFont, item.title, width)) lines_.push_back({Line::Kind::kTitle, std::move(l)});
        if (!item.body.empty()) {
            for (std::string& l : wrap(fonts, kFont, item.body, width)) lines_.push_back({Line::Kind::kBody, std::move(l)});
        }
    }
}

int NewsScreen::visible_lines() { return (kTextY1 - kTextY0) / kLineHeight; }

NewsScreen::Rect NewsScreen::back_button() const {
    return {kBackX0, kBackY0, kBackX1, kBackY0 + kBackHeight - 1};
}

NewsScreen::Rect NewsScreen::arrow(bool up) const {
    const int y0 = up ? kTextY0 : kTextY0 + visible_lines() * kLineHeight - kArrowSize;
    return {kArrowX0, y0, kArrowX0 + kArrowSize - 1, y0 + kArrowSize - 1};
}

// @orig none: port's online news
void NewsScreen::scroll(int lines) {
    const int last = std::max(static_cast<int>(lines_.size()) - visible_lines(), 0);
    first_ = std::clamp(first_ + lines, 0, last);
}

// @orig none: port's online news
void NewsScreen::draw(Surface8& screen) const {
    screen.clear(0);
    draw_rounded_box(screen, kPanelX0, kPanelY0, kPanelX1, kPanelY1, -1, kFrame);
    draw_rounded_box(screen, kPanelX0 + 3, kPanelY0 + 3, kPanelX1 - 3, kPanelY1 - 3, -1, kFrame);
    const std::string title = tr("NEWS");
    print_tinted(screen, *fonts_, kFont, (screen.width - text_width(*fonts_, kFont, title)) / 2, kTitleY, title, kGreen);

    const int shown = visible_lines();
    for (int i = 0; i < shown && first_ + i < static_cast<int>(lines_.size()); ++i) {
        const Line& line = lines_[static_cast<std::size_t>(first_ + i)];
        const int y = kTextY0 + i * kLineHeight + kTextDrop;
        switch (line.kind) {
        case Line::Kind::kDay:
            print_tinted(screen, *fonts_, kFont, kTextX0, y, line.text, kDimGreen);
            if (line.unread) {
                print_tinted(screen, *fonts_, kFont, kTextX0 + text_width(*fonts_, kFont, line.text) + 14, y,
                             tr("NEW"), kRed);
            }
            break;
        case Line::Kind::kTitle: print_tinted(screen, *fonts_, kFont, kTextX0, y, line.text, kGreen); break;
        case Line::Kind::kBody: print_tinted(screen, *fonts_, kFont, kTextX0, y, line.text, kDimGreen); break;
        case Line::Kind::kGap: break;
        }
    }
    if (lines_.empty()) {
        const std::string none = tr("NO NEWS");
        print_tinted(screen, *fonts_, kFont, (screen.width - text_width(*fonts_, kFont, none)) / 2,
                     kTextY0 + kTextDrop, none, kDimGreen);
    }
    // The arrows, when there is more above or below.
    for (const bool up : {true, false}) {
        if (up ? first_ == 0 : first_ + shown >= static_cast<int>(lines_.size())) continue;
        const Rect r = arrow(up);
        for (int row = 0; row < kArrowSize / 2; ++row) {
            const int y = up ? r.y0 + 2 + row : r.y1 - 2 - row;
            for (int x = r.x0 + kArrowSize / 2 - 1 - row; x <= r.x0 + kArrowSize / 2 + row; ++x) {
                if (x >= 0 && y >= 0 && x < screen.width && y < screen.height) screen.row(y)[x] = kGreen;
            }
        }
    }
    const Rect back = back_button();
    draw_button(screen, *fonts_, kFont, back.x0, back.y0, back.x1, back.y1, tr("BACK"), true);
}

// @orig none: port's online news
void NewsScreen::key(std::uint16_t code) {
    switch (code) {
    case gump_key::kUp:
    case '8': scroll(-1); break;
    case gump_key::kDown:
    case '2': scroll(1); break;
    case gump_key::kLeft:
    case '4':
    case kPageUp: scroll(-visible_lines()); break;
    case gump_key::kRight:
    case '6':
    case kPageDown: scroll(visible_lines()); break;
    case gump_key::kEnter:
    case gump_key::kEscape: closed_ = true; break;
    default: break;
    }
}

// @orig none: port's online news
void NewsScreen::mouse_down(int x, int y) {
    if (back_button().contains(x, y)) closed_ = true;
    else if (arrow(true).contains(x, y)) scroll(-visible_lines());
    else if (arrow(false).contains(x, y)) scroll(visible_lines());
}

}  // namespace crusader
