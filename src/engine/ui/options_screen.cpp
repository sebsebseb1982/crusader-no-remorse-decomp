#include "engine/ui/options_screen.hpp"

#include "core/language.hpp"
#include "engine/audio/audio.hpp"
#include "engine/ui/gump_keys.hpp"

#include <string>
#include <utility>


namespace crusader {

namespace {

struct Placement {
    OptionsScreen::Widget id;
    std::uint16_t shape;
    int x, y;
};
using W = OptionsScreen::Widget;
// In creation order (IRGUMP_13d0_0ece), which is also the drawing order.
constexpr std::array<Placement, OptionsScreen::kWidgets> kPlacements = {{
    {W::kAnimationOn, 31, 35, 163},  {W::kAnimationOff, 32, 165, 163},
    {W::kFrameSkipOn, 33, 35, 203},  {W::kFrameSkipOff, 34, 165, 203},
    {W::kBlastsOn, 37, 35, 283},     {W::kBlastsOff, 38, 165, 283},
    {W::kFilterOn, 39, 35, 323},     {W::kFilterOff, 40, 165, 323},
    {W::kVideoBig, 35, 35, 244},     {W::kVideoSmall, 36, 137, 244},
    {W::kSoundDown, 49, 435, 146},   {W::kSoundUp, 50, 597, 146},
    {W::kMusicDown, 49, 435, 197},   {W::kMusicUp, 50, 597, 197},
    {W::kMouseDown, 49, 435, 249},   {W::kMouseUp, 50, 597, 249},
    {W::kQuit, 43, 517, 318},
}};
// The US executable places the "off" buttons and the video line for its
// English shapes (13D0:0ECE): x 95, y 243.
constexpr std::array<Placement, OptionsScreen::kWidgets> kPlacementsUs = {{
    {W::kAnimationOn, 31, 35, 163},  {W::kAnimationOff, 32, 95, 163},
    {W::kFrameSkipOn, 33, 35, 203},  {W::kFrameSkipOff, 34, 95, 203},
    {W::kBlastsOn, 37, 35, 283},     {W::kBlastsOff, 38, 95, 283},
    {W::kFilterOn, 39, 35, 323},     {W::kFilterOff, 40, 95, 323},
    {W::kVideoBig, 35, 35, 243},     {W::kVideoSmall, 36, 137, 243},
    {W::kSoundDown, 49, 435, 146},   {W::kSoundUp, 50, 597, 146},
    {W::kMusicDown, 49, 435, 197},   {W::kMusicUp, 50, 597, 197},
    {W::kMouseDown, 49, 435, 249},   {W::kMouseUp, 50, 597, 249},
    {W::kQuit, 43, 517, 318},
}};

struct Point {
    int x, y;
};
constexpr std::uint16_t kFirstBackground = 44;
constexpr std::array<Point, 4> kBackgroundCorners = {{{1, 120}, {320, 120}, {1, 240}, {320, 240}}};
constexpr std::uint16_t kMarker = 48;
constexpr int kMarkerX = 456, kMarkerStep = 6;
constexpr std::uint8_t kFocusColour = 0x0C;
constexpr std::uint32_t kRepeatTicks = 10;
// The controller button (port addition): colours of the quit button's frames.
constexpr std::uint8_t kButtonColour = 0x75, kButtonFill = 0x34;
constexpr int kButtonFont = 7;

// Focus outline of each line (13D0:1E62); the quit line has none.
constexpr std::array<OptionsScreen::Rect, OptionsScreen::kQuitLine> kLineRects = {{
    {22, 145, 258, 176}, {22, 185, 258, 216}, {22, 225, 227, 256}, {22, 265, 314, 296},
    {22, 305, 349, 336}, {432, 144, 614, 191}, {432, 194, 614, 242}, {432, 247, 614, 294},
}};
// US: the left lines end where their English labels do.
constexpr std::array<OptionsScreen::Rect, OptionsScreen::kQuitLine> kLineRectsUs = {{
    {22, 145, 198, 176}, {22, 185, 202, 216}, {22, 225, 225, 256}, {22, 265, 232, 296},
    {22, 305, 284, 336}, {432, 144, 614, 191}, {432, 194, 614, 242}, {432, 247, 614, 294},
}};
// Widgets acted on by left / '4' and right / '6', by line (tables at 13D0:234C and 233C).
constexpr std::array<W, OptionsScreen::kQuitLine> kLeftOf = {
    W::kAnimationOn, W::kFrameSkipOn, W::kVideoBig, W::kBlastsOn, W::kFilterOn, W::kSoundDown, W::kMusicDown, W::kMouseDown};
constexpr std::array<W, OptionsScreen::kQuitLine> kRightOf = {
    W::kAnimationOff, W::kFrameSkipOff, W::kVideoSmall, W::kBlastsOff, W::kFilterOff, W::kSoundUp, W::kMusicUp, W::kMouseUp};

// First step not below `volume`. The original keeps the volume itself as the
// step when it is above every step; the port takes the last one.
// @orig 13D0:0ECE
int step_for(const std::array<std::uint8_t, OptionsScreen::kSteps>& steps, int volume) {
    for (int i = 0; i < OptionsScreen::kSteps; ++i) {
        if (volume <= steps[i]) return i;
    }
    return OptionsScreen::kSteps - 1;
}

// One step up or down within the table; false at its end.
// @orig 13D0:236E
bool step(int& s, bool up) {
    const int next = s + (up ? 1 : -1);
    if (next < 0 || next >= OptionsScreen::kSteps) return false;
    s = next;
    return true;
}

}  // namespace

// @orig 13D0:0ECE
OptionsScreen::OptionsScreen(ShapeCache& gumps, GameConfig& config, Audio* audio, int& focus, const FontSet* fonts,
                             std::function<void()> vibration_preview)
    : gumps_(&gumps), config_(&config), audio_(audio), focus_(&focus), fonts_(fonts),
      vibration_preview_(std::move(vibration_preview)) {
    for (int i = 0; i < kWidgets; ++i) {
        const Placement& p = (us_version() ? kPlacementsUs : kPlacements)[i];
        const Shape* s = gumps_->get(p.shape);
        const int w = s && !s->frames().empty() ? s->frames()[0].width : 1;
        const int h = s && !s->frames().empty() ? s->frames()[0].height : 1;
        widgets_[i] = {p.id, p.shape, {p.x, p.y, p.x + w - 1, p.y + h - 1}};
    }
    sound_step_ = step_for(kSoundSteps, config.sound_volume);
    music_step_ = step_for(kMusicSteps, config.music_volume);
    quit_selected_ = *focus_ == kQuitLine;
}

// @orig none: index into the widget tables
int OptionsScreen::index(Widget w) {
    for (int i = 0; i < kWidgets; ++i) {
        if (kPlacements[i].id == w) return i;
    }
    return 0;
}

// @orig 13D0:1D81
int OptionsScreen::line_of(Widget w) {
    for (int i = 0; i < kQuitLine; ++i) {
        if (kLeftOf[i] == w || kRightOf[i] == w) return i;
    }
    return kQuitLine;
}

// @orig 13D0:0ECE
bool OptionsScreen::chosen(Widget w) const {
    const GameConfig& c = *config_;
    switch (w) {
    case kAnimationOn: return c.animation == 0;
    case kAnimationOff: return c.animation == 1;
    case kFrameSkipOn: return c.frameskip == 3;
    case kFrameSkipOff: return c.frameskip != 3;
    case kFilterOn: return c.filter;
    case kFilterOff: return !c.filter;
    case kVideoBig: return c.big_video;
    case kVideoSmall: return !c.big_video;
    case kBlastsOn: return c.limit_blasts;
    case kBlastsOff: return !c.limit_blasts;
    default: return false;
    }
}

bool OptionsScreen::pressed(Widget w) const {
    if (w == kQuit) return quit_selected_;
    return is_repeating(w) && captured_ == index(w) && inside_;
}

void OptionsScreen::draw_shape(Surface8& screen, std::uint16_t shape, int frame, int x, int y) const {
    const Shape* s = gumps_->get(shape);
    if (s && static_cast<std::size_t>(frame) < s->frames().size()) s->frames()[frame].draw(screen, x, y);
}

// @orig 13D0:17C5
void OptionsScreen::draw(Surface8& screen) const {
    if (enh_) {
        enh_->draw(screen);
        return;
    }
    screen.clear(0);
    for (int q = 0; q < 4; ++q) {
        draw_shape(screen, static_cast<std::uint16_t>(kFirstBackground + q), 0, kBackgroundCorners[q].x,
                   kBackgroundCorners[q].y);
    }
    draw_shape(screen, 41, 0, 468, 146);
    draw_shape(screen, 42, 0, us_version() ? 465 : 459, 196);  // 13D0:17C5: "music volume"
    if (*focus_ >= 0 && *focus_ < kQuitLine) {
        const Rect& r = (us_version() ? kLineRectsUs : kLineRects)[*focus_];
        outline(screen, r.x0, r.y0, r.x1, r.y1, kFocusColour);
    }
    // Mouse speed: 0x2164 per notch in 16.16, the last notch pulled back.
    int mouse = (config_->mouse_speed << 16) / 0x2164;
    if (config_->mouse_speed == 3) --mouse;
    draw_shape(screen, kMarker, 0, kMarkerX + kMarkerStep * sound_step_, 179);
    draw_shape(screen, kMarker, 0, kMarkerX + kMarkerStep * music_step_, 230);
    draw_shape(screen, kMarker, 0, kMarkerX + kMarkerStep * mouse, 282);
    for (const Item& it : widgets_) {
        const int frame = is_choice(it.id) ? (chosen(it.id) ? 0 : 1) : (pressed(it.id) ? 1 : 0);
        draw_shape(screen, it.shape, frame, it.rect.x0, it.rect.y0);
    }
    if (fonts_) draw_pad_button(screen);
}

// The "ameliorations" button: a "+" icon and a text button next to "quit", in
// its colours (frame, icon and text bright when focused or pressed).
// @orig none: port-only enhancements button
void OptionsScreen::draw_pad_button(Surface8& screen) const {
    const bool lit = *focus_ == kPadLine || pad_pressed_;
    const std::uint8_t colour = lit ? kFocusColour : kButtonColour;
    const Rect& r = pad_rect_;
    draw_rounded_box(screen, r.x0, r.y0, r.x1, r.y1, kButtonFill, colour);
    TextWriter writer(*fonts_, kButtonFont);
    const std::string text = tr("AMELIORATIONS", "ENHANCEMENTS");
    constexpr int kIcon = 11, kGap = 6;
    const int width = writer.measure(text).width;
    const int total = kIcon + kGap + width;
    const int left = r.x0 + (r.x1 - r.x0 + 1 - total) / 2;
    draw_plus_icon(screen, left, r.y0 + (r.y1 - r.y0 + 1 - kIcon) / 2, kIcon, colour);
    // Drawn on a scratch surface and copied in one colour, as the font has its own.
    constexpr int kPad = 4, kAbove = 32, kHeight = 48;
    Surface8 scratch(width + 2 * kPad, kHeight);
    writer.print(scratch, kPad, kAbove, text);
    const int x = left + kIcon + kGap, y = r.y0 + 7;  // font 7 hangs from its top
    for (int sy = 0; sy < kHeight; ++sy) {
        for (int sx = 0; sx < scratch.width; ++sx) {
            const int dx = x - kPad + sx, dy = y - kAbove + sy;
            if (scratch.row(sy)[sx] && dx >= 0 && dy >= 0 && dx < screen.width && dy < screen.height) {
                screen.row(dy)[dx] = colour;
            }
        }
    }
}

void OptionsScreen::open_enhancements() {
    if (!fonts_) return;
    enh_ = std::make_unique<EnhancementsScreen>(*fonts_, *config_, pad_focus_, vibration_preview_);
}

// @orig 13D0:20B7
void OptionsScreen::key(std::uint16_t code) {
    if (enh_) {
        enh_->key(code);
        if (enh_->closed()) enh_.reset();
        return;
    }
    if (closed_) return;
    int& focus = *focus_;
    if (focus == kPadLine && fonts_) {
        // The enhancements button, left of "quit".
        switch (code) {
        case gump_key::kEscape: closed_ = true; break;
        case gump_key::kEnter: open_enhancements(); break;
        case gump_key::kUp:
        case '8': focus = kQuitLine - 1; break;
        case gump_key::kRight:
        case '6': focus = kQuitLine; quit_selected_ = true; break;
        default: break;
        }
        return;
    }
    switch (code) {
    case gump_key::kEscape:
        closed_ = true;
        break;
    case gump_key::kEnter:
        if (focus == kQuitLine) closed_ = true;
        break;
    case gump_key::kUp:
    case '8':
        if (focus == 0) break;
        if (focus == kQuitLine) quit_selected_ = false;
        --focus;
        break;
    case gump_key::kDown:
    case '2':
        if (focus >= kQuitLine) break;
        if (++focus == kQuitLine) quit_selected_ = true;
        break;
    case gump_key::kLeft:
    case '4':
        if (focus >= 0 && focus < kQuitLine) {
            act(kLeftOf[focus]);
        } else if (focus == kQuitLine && fonts_) {
            quit_selected_ = false;
            focus = kPadLine;
        }
        break;
    case gump_key::kRight:
    case '6':
        if (focus >= 0 && focus < kQuitLine) act(kRightOf[focus]);
        break;
    default:
        break;
    }
}

// @orig 1378:0105
const OptionsScreen::Item* OptionsScreen::hit(int x, int y) const {
    // The last created is on top.
    for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it) {
        if (it->rect.contains(x, y)) return &*it;
    }
    return nullptr;
}

// @orig 13D0:0DC0 1310:0371
void OptionsScreen::mouse_down(int x, int y) {
    if (enh_) {
        enh_->mouse_down(x, y);
        if (enh_->closed()) enh_.reset();
        return;
    }
    if (closed_ || captured_ >= 0) return;
    if (fonts_ && pad_rect_.contains(x, y)) {
        pad_pressed_ = true;
        if (*focus_ == kQuitLine) quit_selected_ = false;
        *focus_ = kPadLine;
        return;
    }
    const Item* it = hit(x, y);
    if (!it) return;
    if (is_choice(it->id)) {
        // 13D0:0DC0: a press chooses the widget, unless it already is.
        if (!chosen(it->id)) widget_acted(it->id);
        return;
    }
    captured_ = index(it->id);
    inside_ = true;
    if (it->id == kQuit) {
        quit_selected_ = true;
        return;
    }
    // 1310:0371: acts at once, then repeats while held.
    next_repeat_ = now_ + kRepeatTicks;
    widget_acted(it->id);
}

// @orig 1310:0151 1310:04CD
void OptionsScreen::mouse_move(int x, int y) {
    if (enh_ || closed_) return;
    if (pad_pressed_) pad_pressed_ = pad_rect_.contains(x, y);
    if (captured_ < 0) return;
    const Item& it = widgets_[captured_];
    inside_ = it.rect.contains(x, y);
    if (it.id == kQuit) quit_selected_ = inside_;  // 1310:0151, select / deselect (1378:00CD / 00E9)
    update(now_);
}

// @orig 1310:001D 1310:0371
void OptionsScreen::mouse_up(int x, int y) {
    if (enh_) return;
    if (pad_pressed_) {  // acts at the release inside, like the quit button
        pad_pressed_ = false;
        if (pad_rect_.contains(x, y)) open_enhancements();
        return;
    }
    if (closed_ || captured_ < 0) return;
    const Item& it = widgets_[captured_];
    captured_ = -1;
    inside_ = false;
    if (it.id == kQuit && it.rect.contains(x, y)) widget_acted(kQuit);
}

// @orig 1310:04FD
void OptionsScreen::update(std::uint32_t ticks) {
    now_ = ticks;
    if (enh_) {
        enh_->update(ticks);
        return;
    }
    if (closed_ || captured_ < 0 || !inside_) return;
    const Widget w = widgets_[captured_].id;
    if (!is_repeating(w) || next_repeat_ >= now_) return;
    next_repeat_ = now_ + kRepeatTicks;
    widget_acted(w);
}

void OptionsScreen::widget_acted(Widget w) {
    *focus_ = line_of(w);
    if (w == kQuit) quit_selected_ = true;
    act(w);
}

// @orig 13D0:236E
void OptionsScreen::act(Widget w) {
    GameConfig& c = *config_;
    auto click = [this] {
        if (audio_) audio_->play_sfx(kClickSound);
    };
    switch (w) {
    case kAnimationOn: c.animation = 0; break;
    case kAnimationOff: c.animation = 1; break;
    case kFrameSkipOn: c.frameskip = 3; break;
    case kFrameSkipOff: c.frameskip = 0; break;
    case kFilterOn:
    case kFilterOff:
        c.filter = w == kFilterOn;
        if (audio_) audio_->mixer.set_filter(c.filter);
        break;
    case kVideoBig: c.big_video = true; break;
    case kVideoSmall: c.big_video = false; break;
    case kBlastsOn: c.limit_blasts = true; break;
    case kBlastsOff: c.limit_blasts = false; break;
    case kSoundDown:
    case kSoundUp:
        // The volume is set only when the step changes.
        if (step(sound_step_, w == kSoundUp)) {
            c.sound_volume = kSoundSteps[sound_step_];
            if (audio_) audio_->mixer.set_sound_volume(c.sound_volume);
        }
        click();
        break;
    case kMusicDown:
    case kMusicUp:
        if (step(music_step_, w == kMusicUp)) {
            c.music_volume = kMusicSteps[music_step_];
            if (audio_) audio_->mixer.set_music_volume(c.music_volume);
        }
        click();
        break;
    case kMouseDown:
        if (c.mouse_speed != 0) --c.mouse_speed;
        click();
        break;
    case kMouseUp:
        if (c.mouse_speed < 3) ++c.mouse_speed;
        click();
        break;
    case kQuit:
        closed_ = true;
        break;
    }
}

}  // namespace crusader
