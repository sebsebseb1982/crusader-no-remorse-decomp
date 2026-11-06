#pragma once
// Weasel's shop between two missions (WEZZY.C, segment 13E0; WeaselGump in
// ScummVM), opened by the intrinsic WeaselGump::I_showWeaselGump(level)
// (0x12D, 13E0:0000): the gump (13E0:005C) run by Gump_run_modal, then the
// camera's method +0x2C. The WEA_BOOT class (equip event) passes global 0x1F
// when it is 1..14, else 0. The world is in the menu mode while the gump is
// open (WORLD_13e8_0e7d, left by the dtor 13E0:0435 with WORLD_13e8_0ef9).
//
// Layout (13E0:005C): a 211 x 438 gump centred on the screen, 15 pixels
// higher (214, 6 on a 640 x 480 screen). Its paint (13E0:07BB) draws
// GUMPS.FLX shapes 0x16..0x19, frame 0, at (3, 0), (0, 0x47), (5, 0xCC) and
// (9, 0x16A), then texts in font 6, depending on the state (+0x142):
//   - 2 (confirm): "Etes-vous sûr de" / "vouloir acheter" / "ceci ?" at
//     (0x19, 0x5A), (0x19, 0x62), (0x19, 0x6A); the US version has two
//     lines, "Are you sure you" / "want to buy this?";
//   - 6 (browsing), for the current item: its icon (13E0:19EF: shape 3 for
//     the weapons and the credits 0x4ED, 4 for family 12, 5 for the ammunition,
//     family 13 and the keycard 0x111; frame DTABLE_1120_05dc) at (0x6C, 0x83);
//     its name (DTABLE_1120_056a) at (0x19, 0xA3); "Prix :%u" (0xAD),
//     "Acheté(s) :%02u" (0xB7, the number in the cart) and "Vous en avez :%02u"
//     (0xC1, 13E0:1610: 1 for a weapon carried, else the quality of the one
//     carried) at x 0x19; "Crédits :%ld" at (0x1D, 0x38);
//   - 8 (anything else?): "Voulez-vous" / "autre chose ?" at (0x19, 0x5A),
//     (0x19, 0x62).
// Nine buttons (ShapeButton, 1378:0000: frame 1 while pressed; a point is in
// it when it is in its rectangle and on a pixel of the frame), children of
// the gump, by number: 0 no (shape 0xF at 0x72, 0xED), 1 yes (0x10 at 0x11,
// 0xED), 2 buy (0x1C at 0x14, 0x118), 3 exit (0x1D at 0x2D, 0x16F), 4 previous
// (0xD at 0xF, 0xD4), 5 next (0xE at 0x89, 0xD4), 6 blank (0x1A at 0x4C,
// 0xD4, does nothing), 7 ammunition (0x1B at 0x13, 0x140) and 8 weapons
// (0x53, same place). A hidden gump draws nothing and takes no event.
//
// The items (13E0:0B39): STATIC/STUFF.DAT, blocks of 0x142 bytes, one per
// level: a count, then 20 records of 16 bytes (+0 a name, "W01"..., +8 the
// shape, +0xA the price, +0xC and +0xE unused here). Level 0 (all free) when
// cheating (DS:08F0, Game::show_weasel), 1 for the ammunition. In the ammunition, 0x341 is
// left out before level 5 and 0x340 before level 6 (the level kept at +0x36,
// -1 for the weapons). The current item (+0x144) is the first one with a
// shape; previous and next (13E0:19A8, 1956) go round, skipping those without.
//
// Credits (+0x3A, 32 bits): those of the player (Player_credits) when it
// opens; buying takes them from there, leaving takes them back.
//
// The steps (13E0:0EB7, an event the gump sends itself, 13E0:0E78), each
// with a movie of Weasel (13E0:0CD8, one at random from a list, played as a
// cutscene; none with "noweasel=true" in CRUSADER.CFG, DS:0178):
//   0 at the opening: 17A the first time (DS:5ECF), then 18A/18B/18C; the gump
//     shown, browsing (state 6);
//   1 left without buying: 19C/19D, then the question (state 8);
//   2 left with a cart: 21A/21B, then the confirmation (state 2);
//   3 cart refused: 21E/21F, then the question (state 8);
//   4 cart bought: 21C/21D, then the question (state 8);
//   5 (never reached): 20C/20D, browsing;
//   7: the gump closes.
// The browsing mode (13E0:0608) shows buttons 2..7 and hides 0, 1 (and
// empties +0x148, "something in the cart"); the question mode (13E0:0486)
// shows 0, 1 and hides 2..8. Button 8 starts hidden.
//
// The buttons (13E0:10B5):
//   - 0 (no): with the question (8), the gump closes, through step 7 when
//     something was bought (+0x149); with the confirmation (2), the cart is
//     emptied and the credits back, step 3;
//   - 1 (yes): with the question, browsing again; with the confirmation, the
//     cart bought (13E0:1895), step 4;
//   - 2 (buy, 13E0:1767): sound 0xB0 when the credits are short, when the
//     player could not take the item (WORLD_13e8_00c8, may_add_item) or when
//     he would have too many (13E0:168A: the cart and those carried, below 1
//     for a weapon, 20 for an ammunition or 0x560, else 10); else the item is
//     in the cart and its price taken from the credits;
//   - 3 (exit): back to the weapons, then step 2 with a cart, else the credits
//     back and step 1;
//   - 4, 5: previous, next item;
//   - 7 (ammunition): unless already there, button 8 shown, 7 hidden, the
//     level kept and the ammunition (level 1) read;
//   - 8 (weapons): the other way round.
// Buying the cart (13E0:1895): each item made (Item_I_create, popToCoords at
// 0, 0, 0) and taken by the player (MainActor::I_addItemCru, not shown); the
// credits 0x4ED carried get the credits left (Item::I_setQuality, low 16
// bits); the cart emptied.
//
// Keys (13E0:13D3): Escape exits while browsing, says no to the question and
// the confirmation, else closes the gump; Y, O, J (either case) say yes and N
// says no, in states 2, 3, 4 and 8; left and right arrows, Enter and Space
// (buy), Tab (weapons at level 1, else ammunition) while browsing.
//
// Not reproduced:
//   - DS:5E82, which picks 18D/18E/18F instead of 18A/18B/18C: it is never set
//     (13E0:0000 writes back the value it reads);
//   - buying makes a temporary item (Item_I_create, popToCoords, then
//     Item_I_destroy) to check it could be taken; the port checks the shape;
//   - STUFF.DAT is read whole once, rather than at each change of level; a
//     level with no item gives an empty gump instead of an endless loop;
//   - the screen repaints of NEWGUMP (the port draws everything each frame).

#include "core/font.hpp"
#include "core/surface.hpp"
#include "engine/ui/modal_gump.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace crusader {

class WeaselGump : public ModalGump {
public:
    static constexpr int kWidth = 211;
    static constexpr int kHeight = 438;
    static constexpr int kFont = 6;
    static constexpr int kCantBuySound = 0xB0;
    static constexpr int kBlockSize = 0x142;
    static constexpr int kMaxItems = 20;
    static constexpr int kCartSize = 128;
    static constexpr int kAmmoLevel = 1;
    static constexpr std::uint16_t kCredits = 0x4ED;

    enum State {
        kOpening = 0,
        kLeftEmpty = 1,
        kConfirm = 2,
        kRefused = 3,
        kBought = 4,
        kShort = 5,
        kBrowsing = 6,
        kClosing = 7,
        kQuestion = 8,
    };
    enum Button {
        kNo = 0,
        kYes = 1,
        kBuy = 2,
        kExit = 3,
        kPrevious = 4,
        kNext = 5,
        kBlank = 6,
        kAmmo = 7,
        kWeapons = 8,
        kButtons = 9,
    };

    struct Item {
        std::uint16_t shape = 0;
        std::uint16_t price = 0;
    };

    struct Hooks {
        std::function<void(int)> play_sfx;
        // A movie of Weasel (FLICS/<name>), played as a cutscene.
        std::function<void(const std::string&)> play_movie;
        // 0 .. n - 1 (random_range).
        std::function<int(int)> random;
    };

    // `stuff_dat`: STATIC/STUFF.DAT; `intro_played`: DS:5ECF, kept by the
    // game; `movies`: false with "noweasel=true".
    WeaselGump(World& world, ShapeCache& gumps, const FontSet& fonts, std::vector<std::uint8_t> stuff_dat, int level,
               bool& intro_played, bool movies, Hooks hooks, int screen_width = 640, int screen_height = 480);

    void draw(Surface8& screen) const override;
    void key(std::uint16_t code) override;
    void mouse_down(int x, int y) override;
    void mouse_move(int x, int y) override;
    void mouse_up(int x, int y) override;
    void update(std::uint32_t ticks) override;
    bool closed() const override { return closed_; }
    bool over_view() const override { return true; }

    int x() const { return x_; }
    int y() const { return y_; }
    int state() const { return state_; }
    int level() const { return level_; }
    std::int32_t credits() const { return credits_; }
    int current() const { return current_; }
    const std::vector<Item>& items() const { return items_; }
    // The cart, in the order bought.
    std::vector<std::uint16_t> cart() const;
    bool shown() const { return !hidden_; }
    bool button_shown(int button) const { return !buttons_[static_cast<std::size_t>(button)].hidden; }
    // The button's rectangle on the screen (inclusive).
    struct Rect {
        int x0, y0, x1, y1;
    };
    Rect button_rect(int button) const;

private:
    struct ButtonState {
        std::uint16_t shape = 0;
        int x = 0;  // in the gump
        int y = 0;
        bool hidden = false;
        bool pressed = false;
    };

    void load_items(int level);                   // 13E0:0B39
    void step();                                  // 13E0:0EB7
    void play_movie(const char* const* names, int count);  // 13E0:0CD8
    void browsing_mode();                         // 13E0:0608
    void question_mode();                         // 13E0:0486
    void clamp_current();                         // 13E0:0C97
    void button(int index);                       // 13E0:10B5
    void buy();                                   // 13E0:1767
    void buy_cart();                              // 13E0:1895
    void previous();                              // 13E0:19A8
    void next();                                  // 13E0:1956
    int in_cart(std::uint16_t shape) const;       // 13E0:15D4
    int owned(std::uint16_t shape) const;         // 13E0:1610
    bool may_buy_more(std::uint16_t shape) const; // 13E0:168A
    void reset_credits();
    void empty_cart();
    void close() { closed_ = true; }              // vtable +0x3C, Gump_close
    bool on_button(int index, int x, int y) const;  // 1378:0105
    int button_at(int x, int y) const;
    const ShapeFrame* frame(std::uint16_t shape, int frame) const;

    World* world_;
    ShapeCache* gumps_;
    const FontSet* fonts_;
    std::vector<std::uint8_t> stuff_dat_;
    bool* intro_played_;
    bool movies_;
    Hooks hooks_;
    int x_ = 0;
    int y_ = 0;
    int level_ = 0;            // +0x34
    int kept_level_ = -1;      // +0x36, the weapons' level while in the ammunition
    std::int32_t credits_ = 0; // +0x3A
    std::array<std::uint16_t, kCartSize> cart_{};  // +0x3E
    int cart_count_ = 0;       // +0x14A
    int state_ = kOpening;     // +0x142
    int current_ = 0;          // +0x144
    std::vector<Item> items_;  // +0x146 count, +0x14B records
    bool in_cart_ = false;     // +0x148
    bool bought_ = false;      // +0x149
    bool hidden_ = false;
    bool step_pending_ = false;
    std::array<ButtonState, kButtons> buttons_{};
    int captured_ = -1;
    bool closed_ = false;
};

}  // namespace crusader
