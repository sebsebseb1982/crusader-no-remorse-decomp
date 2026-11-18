#include "engine/ui/shape_dialog.hpp"

#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"

namespace crusader {

namespace {

constexpr std::uint16_t kYesShape = 0x13, kNoShape = 0x14, kOkShape = 0x50;
constexpr int kOkX = 91, kButtonY = 47;

struct YesNo {
    int yes_x, no_x;
};

// World_on_key gives the places: in the French executable the quit question
// has its own (13E8:159A), the quick load and save ones share theirs
// (13E8:1ACD, 1BAB); the US one uses the same for the three. The German one
// keeps the French quit question and moves the two others for its shapes.
// @orig 13E8:159A 13E8:1ACD 13E8:1BAB
YesNo yes_no_places(std::uint16_t shape) {
    if (us_version()) return {55, 135};
    if (shape == ShapeDialog::kQuitQuestion) return {57, 125};
    if (german_version()) return {52, 116};
    return {49, 121};
}

const ShapeFrame* first_frame(ShapeCache& gumps, std::uint16_t shape, std::size_t frame = 0) {
    const Shape* s = gumps.get(shape);
    return s && frame < s->frames().size() ? &s->frames()[frame] : nullptr;
}

}  // namespace

// @orig 1378:0000
ShapeButton::ShapeButton(ShapeCache& g, std::uint16_t s, int i, int x, int y) : gumps(&g), shape(s), id(i) {
    const ShapeFrame* f = first_frame(g, s);
    const int w = f ? f->width : 1;
    const int h = f ? f->height : 1;
    rect = {x, y, x + w - 1, y + h - 1};
}

bool ShapeButton::contains(int x, int y) const {
    if (!rect.contains(x, y)) return false;
    const ShapeFrame* f = first_frame(*gumps, shape, selected ? 1 : 0);
    return f && f->pixel(x - rect.x0, y - rect.y0) >= 0;
}

void ShapeButton::draw(Surface8& screen) const {
    if (const ShapeFrame* f = first_frame(*gumps, shape, selected ? 1 : 0)) f->draw(screen, rect.x0, rect.y0);
}

// @orig 1310:001D
void ShapeButtons::mouse_down(int x, int y) {
    if (captured_ >= 0) return;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (buttons[i].contains(x, y)) {
            captured_ = static_cast<int>(i);
            buttons[i].selected = true;
            return;
        }
    }
}

void ShapeButtons::mouse_move(int x, int y) {
    if (captured_ < 0) return;
    ShapeButton& b = buttons[captured_];
    b.selected = b.contains(x, y);
}

// @orig 1310:001D
int ShapeButtons::mouse_up(int x, int y) {
    // 1310:001D: the capture ends; the button is not deselected.
    if (captured_ < 0) return -1;
    const ShapeButton& b = buttons[captured_];
    captured_ = -1;
    return b.contains(x, y) ? b.id : -1;
}

void ShapeButtons::draw(Surface8& screen) const {
    for (const ShapeButton& b : buttons) b.draw(screen);
}

// @orig 13F8:0000 13F8:0383
ShapeDialog::ShapeDialog(ShapeCache& gumps, std::uint16_t shape, bool question)
    : gumps_(&gumps), shape_(shape), question_(question) {
    const ShapeFrame* f = first_frame(gumps, shape);
    const int w = f ? f->width : 1;
    const int h = f ? f->height : 1;
    const int x = (640 - w) >> 1;
    const int y = (479 - h) >> 1;
    rect_ = {x, y, x + w - 1, y + h - 1};
    if (question) {
        const YesNo p = yes_no_places(shape);
        buttons_.buttons.emplace_back(gumps, kYesShape, 1, x + p.yes_x, y + kButtonY);
        buttons_.buttons.emplace_back(gumps, kNoShape, 0, x + p.no_x, y + kButtonY);
    } else {
        buttons_.buttons.emplace_back(gumps, kOkShape, 1, x + kOkX, y + kButtonY);
    }
}

ShapeDialog ShapeDialog::question(ShapeCache& gumps, std::uint16_t shape) { return {gumps, shape, true}; }

ShapeDialog ShapeDialog::notice(ShapeCache& gumps, std::uint16_t shape) { return {gumps, shape, false}; }

void ShapeDialog::draw(Surface8& screen) const {
    if (const ShapeFrame* f = first_frame(*gumps_, shape_)) f->draw(screen, rect_.x0, rect_.y0);
    buttons_.draw(screen);
}

// @orig 13F8:0299 13F8:0575
void ShapeDialog::key(std::uint16_t code) {
    if (closed_) return;
    if (question_) {
        switch (code) {
        case 'y': case 'Y': case 'j': case 'J': case 'o': case 'O':
            close(1);
            break;
        case 'n': case 'N': case 's': case 'S': case gump_key::kEscape:
            close(0);
            break;
        default:
            break;  // not given to the dialog
        }
    } else if (code == gump_key::kEnter || code == gump_key::kEscape) {
        close(0);
    }
}

void ShapeDialog::mouse_up(int x, int y) {
    if (closed_) return;
    const int id = buttons_.mouse_up(x, y);
    if (id >= 0) close(id);
}

void ShapeDialog::close(int result) {
    closed_ = true;
    result_ = result;
}

}  // namespace crusader
