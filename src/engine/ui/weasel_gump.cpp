#include "engine/ui/weasel_gump.hpp"

#include "core/dtable.hpp"
#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"
#include "engine/world/inventory.hpp"

#include <cstdio>
#include <string_view>

namespace crusader {

namespace {

constexpr std::uint8_t kFamilyWeapon = 10;
constexpr std::uint8_t kFamilyAmmo = 11;

// The movies of each step (DS:5E83..5ECB).
constexpr const char* kFirstIntro[] = {"17a"};
constexpr const char* kIntro[] = {"18a", "18b", "18c"};
constexpr const char* kLeftEmptyMovies[] = {"19c", "19d"};
constexpr const char* kConfirmMovies[] = {"21a", "21b"};
constexpr const char* kRefusedMovies[] = {"21e", "21f"};
constexpr const char* kBoughtMovies[] = {"21c", "21d"};
constexpr const char* kShortMovies[] = {"20c", "20d"};

// The buttons (13E0:005C): shape and place in the gump, by number.
struct ButtonPlace {
    std::uint16_t shape;
    int x, y;
};
constexpr ButtonPlace kButtonPlaces[WeaselGump::kButtons] = {
    {0x0F, 0x72, 0xED},  {0x10, 0x11, 0xED},  {0x1C, 0x14, 0x118}, {0x1D, 0x2D, 0x16F}, {0x0D, 0x0F, 0xD4},
    {0x0E, 0x89, 0xD4},  {0x1A, 0x4C, 0xD4},  {0x1B, 0x13, 0x140}, {0x53, 0x13, 0x140},
};

// The gump's background (13E0:07BB): GUMPS.FLX shapes 0x16..0x19.
constexpr ButtonPlace kBackground[] = {{0x16, 3, 0}, {0x17, 0, 0x47}, {0x18, 5, 0xCC}, {0x19, 9, 0x16A}};

std::uint16_t word_at(const std::vector<std::uint8_t>& d, std::size_t at) {
    return at + 2 <= d.size() ? static_cast<std::uint16_t>(d[at] | d[at + 1] << 8) : 0;
}

// DTABLE_1120_056a: the name of an inventory item, "INVALID" for none.
// @orig 1120:056A
const char* item_name(std::uint16_t shape) {
    if (dtable::weapon_index(shape)) return dtable::weapon_name(shape);
    if (dtable::inventory_item_index(shape)) return dtable::item_name(shape);
    if (dtable::family12_index(shape)) return dtable::family12_name(shape);
    return dtable::ammo_name(shape);  // record 0: "INVALID"
}

// DTABLE_1120_05dc: the icon frame of an inventory item.
// @orig 1120:05DC
std::uint16_t icon_frame(std::uint16_t shape) {
    if (const std::uint16_t f = dtable::weapon_icon(shape)) return f;
    if (const std::uint16_t f = dtable::item_icon(shape)) return f;
    if (const std::uint16_t f = dtable::item_icon_family12(shape)) return f;
    return dtable::ammo_icon(shape);
}

// The icon's shape in GUMPS.FLX, 0 for none.
// @orig 13E0:19EF
std::uint16_t icon_shape(const World& world, std::uint16_t shape) {
    if (shape == 0x111) return 5;
    if (shape == WeaselGump::kCredits) return 3;
    switch (world.family_of(shape)) {
    case kFamilyWeapon: return 3;
    case kFamilyAmmo: return 5;
    case 12: return 4;
    case 13: return 5;
    default: return 0;
    }
}

template <typename... Args>
std::string format(const char* f, Args... args) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, f, args...);
    return buffer;
}

}  // namespace

// @orig 13E0:005C 1378:0000
WeaselGump::WeaselGump(World& world, ShapeCache& gumps, const FontSet& fonts, std::vector<std::uint8_t> stuff_dat,
                       int level, bool& intro_played, bool movies, Hooks hooks, int screen_width, int screen_height)
    : world_(&world),
      gumps_(&gumps),
      fonts_(&fonts),
      stuff_dat_(std::move(stuff_dat)),
      intro_played_(&intro_played),
      movies_(movies),
      hooks_(std::move(hooks)) {
    // Centred, 15 pixels higher.
    x_ = (screen_width - kWidth) >> 1;
    y_ = ((screen_height - kHeight) >> 1) - 15;
    reset_credits();
    for (int i = 0; i < kButtons; ++i) {
        const ButtonPlace& p = kButtonPlaces[i];
        buttons_[static_cast<std::size_t>(i)] = ButtonState{p.shape, p.x, p.y};
    }
    buttons_[kWeapons].hidden = true;
    load_items(level);
    question_mode();
    // The first step, sent to itself; the gump stays hidden until then.
    step_pending_ = true;
    hidden_ = true;
}

std::vector<std::uint16_t> WeaselGump::cart() const {
    return {cart_.begin(), cart_.begin() + cart_count_};
}

// @orig none: a frame of GUMPS.FLX, null when missing
const ShapeFrame* WeaselGump::frame(std::uint16_t shape, int frame) const {
    const Shape* s = gumps_->get(shape);
    return s && frame >= 0 && static_cast<std::size_t>(frame) < s->frames().size()
               ? &s->frames()[static_cast<std::size_t>(frame)]
               : nullptr;
}

// GumpShape_rect of frame 0, at the button's place.
// @orig 1378:0000
WeaselGump::Rect WeaselGump::button_rect(int button) const {
    const ButtonState& b = buttons_[static_cast<std::size_t>(button)];
    const ShapeFrame* f = frame(b.shape, 0);
    const int x0 = x_ + b.x;
    const int y0 = y_ + b.y;
    return {x0, y0, x0 + (f ? f->width : 1) - 1, y0 + (f ? f->height : 1) - 1};
}

// @orig 13E0:0B39
void WeaselGump::load_items(int level) {
    level_ = level;
    items_.clear();
    const std::size_t block = static_cast<std::size_t>(level) * kBlockSize;
    const int count = std::min<int>(word_at(stuff_dat_, block), kMaxItems);
    for (int i = 0; i < count; ++i) {
        const std::size_t record = block + 2 + static_cast<std::size_t>(i) * 16;
        items_.push_back(Item{word_at(stuff_dat_, record + 8), word_at(stuff_dat_, record + 10)});
    }
    // Ammunition the weapons of the level do not use yet.
    for (Item& item : items_) {
        if (kept_level_ < 5 && item.shape == 0x341) item.shape = 0;
        if (kept_level_ < 6 && item.shape == 0x340) item.shape = 0;
    }
    current_ = 0;
    while (current_ < static_cast<int>(items_.size()) && items_[static_cast<std::size_t>(current_)].shape == 0) {
        ++current_;
    }
}

// @orig 10F0:3D74
void WeaselGump::reset_credits() {
    const ObjId c = carried(*world_, kCredits);
    credits_ = c ? world_->get(c)->quality : 0;
}

void WeaselGump::empty_cart() {
    cart_.fill(0);
    cart_count_ = 0;
}

// @orig 13E0:07BB 1378:0093 1368:05D4
void WeaselGump::draw(Surface8& screen) const {
    if (hidden_) return;
    for (const ButtonPlace& p : kBackground) {
        if (const ShapeFrame* f = frame(p.shape, 0)) f->draw(screen, x_ + p.x, y_ + p.y);
    }
    TextWriter text(*fonts_, kFont);
    const auto print = [&](int x, int y, const std::string& s) { text.print(screen, x_ + x, y_ + y, s); };
    // A question, a line every 8 pixels.
    const auto print_lines = [&](int x, int y, std::string_view s) {
        for (std::size_t end; (end = s.find('\n')) != std::string_view::npos; s.remove_prefix(end + 1), y += 8)
            print(x, y, std::string(s.substr(0, end)));
        print(x, y, std::string(s));
    };
    if (state_ == kConfirm) {
        print_lines(0x19, 0x5A, tr("Are you sure you\nwant to buy this?"));
    } else if (state_ == kBrowsing && current_ < static_cast<int>(items_.size())) {
        const Item& item = items_[static_cast<std::size_t>(current_)];
        // The original stops with a fatal assertion without an icon.
        if (const std::uint16_t shape = icon_shape(*world_, item.shape)) {
            if (const ShapeFrame* f = frame(shape, icon_frame(item.shape))) f->draw(screen, x_ + 0x6C, y_ + 0x83);
            print(0x19, 0xA3, item_name(item.shape));
            print(0x19, 0xAD, format(tr("Cost:%u"), unsigned{item.price}));
            print(0x19, 0xB7, format(tr("Purchased:%02u"), static_cast<unsigned>(in_cart(item.shape))));
            print(0x19, 0xC1, format(tr("Owned:%02u"), static_cast<unsigned>(owned(item.shape))));
        }
        print(0x1D, 0x38, format(tr("Credits:%ld"), static_cast<long>(credits_)));
    } else if (state_ == kQuestion) {
        print_lines(0x19, 0x5A, tr("Do you want\nanything else?"));
    }
    // The children, after the gump.
    for (const ButtonState& b : buttons_) {
        if (b.hidden) continue;
        if (const ShapeFrame* f = frame(b.shape, b.pressed ? 1 : 0)) f->draw(screen, x_ + b.x, y_ + b.y);
    }
}

// The step sent at the opening and after each change (13E0:0E78) comes back
// at the next pass of the modal loop.
// @orig 1328:1588
void WeaselGump::update(std::uint32_t) {
    if (closed_ || !step_pending_) return;
    step_pending_ = false;
    step();
}

// One of `count` movies at random; nothing with "noweasel=true".
// @orig 13E0:0CD8
void WeaselGump::play_movie(const char* const* names, int count) {
    if (!movies_) return;
    const int i = hooks_.random ? hooks_.random(count) : 0;
    if (hooks_.play_movie) hooks_.play_movie(names[i]);
}

// @orig 13E0:0EB7
void WeaselGump::step() {
    switch (state_) {
    case kOpening:
        if (!*intro_played_) {
            play_movie(kFirstIntro, 1);
            *intro_played_ = true;
        } else {
            play_movie(kIntro, 3);
        }
        state_ = kBrowsing;
        hidden_ = false;
        browsing_mode();
        clamp_current();
        break;
    case kLeftEmpty:
        play_movie(kLeftEmptyMovies, 2);
        state_ = kQuestion;
        question_mode();
        break;
    case kConfirm:
        play_movie(kConfirmMovies, 2);
        question_mode();
        break;
    case kRefused:
        play_movie(kRefusedMovies, 2);
        state_ = kQuestion;
        question_mode();
        break;
    case kBought:
        play_movie(kBoughtMovies, 2);
        state_ = kQuestion;
        question_mode();
        break;
    case kShort:
        play_movie(kShortMovies, 2);
        state_ = kBrowsing;
        clamp_current();
        break;
    case kClosing:
        close();
        break;
    default:
        break;
    }
}

// @orig 13E0:0608
void WeaselGump::browsing_mode() {
    buttons_[kYes].hidden = true;
    buttons_[kNo].hidden = true;
    for (int i = kBuy; i <= kAmmo; ++i) buttons_[static_cast<std::size_t>(i)].hidden = false;
    in_cart_ = false;
}

// @orig 13E0:0486
void WeaselGump::question_mode() {
    for (int i = kBuy; i <= kWeapons; ++i) buttons_[static_cast<std::size_t>(i)].hidden = true;
    buttons_[kYes].hidden = false;
    buttons_[kNo].hidden = false;
}

// @orig 13E0:0C97
void WeaselGump::clamp_current() {
    if (current_ >= static_cast<int>(items_.size())) current_ = static_cast<int>(items_.size()) - 1;
    if (current_ < 0) current_ = 0;
}

// @orig 13E0:10B5
void WeaselGump::button(int index) {
    switch (index) {
    case kNo:
        if (state_ == kQuestion) {
            if (bought_) {
                state_ = kClosing;
                step_pending_ = true;
            } else {
                close();
            }
        } else if (state_ == kConfirm) {
            reset_credits();
            empty_cart();
            state_ = kRefused;
            step_pending_ = true;
        }
        break;
    case kYes:
        if (state_ == kQuestion) {
            state_ = kBrowsing;
            browsing_mode();
            clamp_current();
        } else if (state_ == kConfirm) {
            buy_cart();
            bought_ = true;
            state_ = kBought;
            step_pending_ = true;
        }
        break;
    case kBuy:
        buy();
        break;
    case kExit:
        if (kept_level_ >= 0) {
            const int level = kept_level_;
            load_items(level);
            kept_level_ = -1;
        }
        if (in_cart_) {
            state_ = kConfirm;
        } else {
            reset_credits();
            state_ = kLeftEmpty;
        }
        step_pending_ = true;
        break;
    case kPrevious:
        previous();
        break;
    case kNext:
        next();
        break;
    case kAmmo:
        if (kept_level_ < 0) {
            buttons_[kWeapons].hidden = false;
            buttons_[kAmmo].hidden = true;
            kept_level_ = level_;
            load_items(kAmmoLevel);
        }
        break;
    case kWeapons:
        if (kept_level_ >= 0) {
            buttons_[kAmmo].hidden = false;
            buttons_[kWeapons].hidden = true;
            const int level = kept_level_;
            load_items(level);
            kept_level_ = -1;
        }
        break;
    default:
        break;
    }
}

// @orig 13E0:1767
void WeaselGump::buy() {
    if (current_ >= static_cast<int>(items_.size())) return;
    const Item& item = items_[static_cast<std::size_t>(current_)];
    const std::int32_t price = static_cast<std::int16_t>(item.price);  // CWD
    const bool can = credits_ - price >= 0 && may_add_item(*world_, item.shape) && may_buy_more(item.shape);
    if (!can) {
        if (hooks_.play_sfx) hooks_.play_sfx(kCantBuySound);
        return;
    }
    in_cart_ = true;
    credits_ -= price;
    if (cart_count_ < kCartSize) cart_[static_cast<std::size_t>(cart_count_++)] = item.shape;
}

// @orig 13E0:1895
void WeaselGump::buy_cart() {
    for (const std::uint16_t shape : cart_) {
        if (shape == 0) continue;
        const ObjId id = world_->create(shape, 0);
        if (id == 0) continue;
        world_->pop_to_coords(0, 0, 0);
        add_item_cru(*world_, id, false);
    }
    // Item::I_setQuality, for family 1 only.
    if (const ObjId c = carried(*world_, kCredits)) {
        if (world_->family_of(kCredits) == 1) world_->get(c)->quality = static_cast<std::uint16_t>(credits_);
    }
    empty_cart();
}

// @orig 13E0:19A8
void WeaselGump::previous() {
    const int count = static_cast<int>(items_.size());
    for (int i = 0; i < count; ++i) {
        current_ = current_ == 0 ? count - 1 : current_ - 1;
        if (items_[static_cast<std::size_t>(current_)].shape != 0) break;
    }
    clamp_current();
}

// @orig 13E0:1956
void WeaselGump::next() {
    const int count = static_cast<int>(items_.size());
    for (int i = 0; i < count; ++i) {
        current_ = current_ < count - 1 ? current_ + 1 : 0;
        if (items_[static_cast<std::size_t>(current_)].shape != 0) break;
    }
    clamp_current();
}

// @orig 13E0:15D4
int WeaselGump::in_cart(std::uint16_t shape) const {
    int n = 0;
    for (int i = 0; i < cart_count_; ++i) n += cart_[static_cast<std::size_t>(i)] == shape;
    return n;
}

// @orig 13E0:1610
int WeaselGump::owned(std::uint16_t shape) const {
    const ObjId c = carried(*world_, shape);
    if (!c) return 0;
    return world_->family_of(shape) == kFamilyWeapon ? 1 : world_->get(c)->quality;
}

// @orig 13E0:168A
bool WeaselGump::may_buy_more(std::uint16_t shape) const {
    const std::uint8_t fam = world_->family_of(shape);
    int limit = 10;
    if (fam == kFamilyAmmo) limit = 20;
    else if (fam == kFamilyWeapon) limit = 1;
    if (shape == 0x560) limit = 20;
    return in_cart(shape) + owned(shape) < limit;
}

// @orig 13E0:13D3
void WeaselGump::key(std::uint16_t code) {
    if (closed_) return;
    const bool answering = state_ == kQuestion || state_ == kConfirm || state_ == kRefused || state_ == kBought;
    switch (code) {
    case gump_key::kEscape:
        if (state_ == kBrowsing) button(kExit);
        else if (state_ == kConfirm || state_ == kQuestion) button(kNo);
        else close();
        break;
    case 'Y': case 'y': case 'O': case 'o': case 'J': case 'j':
        if (answering) button(kYes);
        break;
    case 'N': case 'n':
        if (answering) button(kNo);
        break;
    case gump_key::kLeft:
        if (state_ == kBrowsing) button(kPrevious);
        break;
    case gump_key::kRight:
        if (state_ == kBrowsing) button(kNext);
        break;
    case gump_key::kEnter:
    case ' ':
        if (state_ == kBrowsing) button(kBuy);
        break;
    case '\t':
        if (state_ == kBrowsing) button(level_ == kAmmoLevel ? kWeapons : kAmmo);
        break;
    default:
        break;
    }
}

// In its rectangle, then on a pixel of the frame.
// @orig 1378:0105
bool WeaselGump::on_button(int index, int x, int y) const {
    const Rect r = button_rect(index);
    if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) return false;
    const ButtonState& b = buttons_[static_cast<std::size_t>(index)];
    const ShapeFrame* f = frame(b.shape, b.pressed ? 1 : 0);
    return f && f->pixel(x - r.x0, y - r.y0) >= 0;
}

// The shown button under the point, -1 for none.
// @orig 1368:0955
int WeaselGump::button_at(int x, int y) const {
    if (hidden_) return -1;
    for (int i = 0; i < kButtons; ++i) {
        if (!buttons_[static_cast<std::size_t>(i)].hidden && on_button(i, x, y)) return i;
    }
    return -1;
}

// @orig 1310:001D 1378:00CD
void WeaselGump::mouse_down(int x, int y) {
    if (closed_ || captured_ >= 0) return;
    const int i = button_at(x, y);
    if (i < 0) return;
    captured_ = i;
    buttons_[static_cast<std::size_t>(i)].pressed = true;
}

// @orig 1310:0151
void WeaselGump::mouse_move(int x, int y) {
    if (captured_ < 0) return;
    buttons_[static_cast<std::size_t>(captured_)].pressed = on_button(captured_, x, y);
}

// @orig 1310:0254 1378:00E9
void WeaselGump::mouse_up(int x, int y) {
    if (captured_ < 0) return;
    const int index = captured_;
    captured_ = -1;
    const bool inside = on_button(index, x, y);
    buttons_[static_cast<std::size_t>(index)].pressed = false;
    if (inside) button(index);
}

}  // namespace crusader
