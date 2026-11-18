# Translating the game

This guide explains where the game's texts come from, how the port organises languages, and what to do to add one.

## Where the texts are

The texts on screen come from three places:

| Source | Examples | Files |
|---|---|---|
| Game data | Movie subtitles, computer terminals and in-game messages, conversations | `FLICS/*.IFF` (No Regret) or `FLICS/*.TXT` (No Remorse), `USECODE/EUSECODE.FLX` |
| Pictures of the game data | Main menu buttons, options screen, the quit question, the status bar, help and credits screens | `STATIC/GUMPS.FLX`, `STATIC/*.BMP`, `STATIC/*.DAT`; the letters are in `STATIC/FONTS.FLX` |
| Texts of the executable and of the port | Item names, Weasel's shop, a few messages ("TARGETING RETICLE ACTIVE."), the version lines of the main menu, the Enhancements and controller screens | `port.txt` of each language folder (below) |

A language folder can hold all three: the port's texts, and translated copies of the game data files.

## Language folders

The `languages` folder sits next to the executable. It holds one folder per language, named after the language's id in lower case, with no spaces: `en`, `fr`, `pt-br`...

```
languages/
  en/port.txt              the port's texts in English (the reference)
  fr/port.txt              the French texts
  de/port.txt              the German texts
  pt-br/
    port.txt               the port's texts in Brazilian Portuguese
    STATIC/GUMPS.FLX       translated pictures
    STATIC/FONTS.FLX       fonts with the accented letters
    USECODE/EUSECODE.FLX   translated terminals and messages
    FLICS/01A.IFF          translated subtitles
```

- `port.txt` holds the texts the port prints itself (format below).
- Any other file replaces the data folder's file with the same path. Files that are not there come from the data folder, so a translation only ships the files it changes. Use the same names, in the same case, as in the data folder.
- `en/port.txt` is generated from the sources and must not be edited. `fr/port.txt` holds the texts of the French `CRUSADER.EXE` 1.08, which the French data needs, and the French texts of the port's screens; `de/port.txt` those of the German `CRUSADER.EXE` 1.07 and the German texts of the port's screens. The repository only ships `port.txt` files: the game data files belong to their publisher and are not distributed with the port.

In the game, **Options**, **+** (Enhancements), **Game**, **Language** cycles through **AUTO** and every language folder, showing each one's name. The choice is saved in `CRUSADER.CFG` (`language=pt-br`).

- **AUTO** (the default) uses the texts of the data's language (`fr` with the French data, `en` with the English data, `de` with the German data) and no replacement data files.
- The texts of `port.txt` change at once.
- The data files of a language folder are read when the game starts. After picking a language that has data files, or leaving one, the window shows "RESTART TO LOAD ITS GAME FILES", and the game files change at the next start.

### Things to know about data files

- **Same version.** The files of a language folder must be translations of the files of the data in use: the same game and version (No Regret 1.06, No Remorse French 1.08, US 1.23, German 1.07...). The port works out the version from the data folder, and the scripts of another version call the wrong routines.
- **Saved games.** A translated `EUSECODE.FLX` whose texts have a different length can move the scripts. A game saved with one language's scripts may not load with another's. Players should keep the same language for a whole game.

## `port.txt`

The file is UTF-8 text:

```
# A comment.
@name = PORTUGUES (BRASIL)

# --- enhancements window (src/engine/ui/enhancements_screen.cpp)
ENHANCEMENTS = MELHORIAS
BACK = VOLTAR
Do you want\nanything else? = Quer\nmais alguma coisa?
Cost:%u = Preco:%u
```

- `@name` is the name the game shows for the language.
- Each other line is an English text exactly as in `en/port.txt`, then `=`, then the translation. Spaces around both are ignored.
- `\n` is a line break and `\\` a backslash. Line breaks only work where the English text has them (the questions of Weasel's shop).
- An empty translation (`BACK =`), or a missing line, shows the English text.
- Keep the `%` codes of the English text (`%u`, `%02u`, `%ld`), in the same order. The game ignores a translation that changes them, and says so on the error output.
- Lines starting with `#` are comments. The `# ---` lines tell where the texts are used.

### Letters and fonts

The game's fonts use code page 850. The game converts `port.txt` to it, so most accented Latin letters (á, ã, ç, é, ê, õ, ü...) can be written as they are. A character outside code page 850 becomes `?`.

A letter is only drawn if the font has a picture for it. The fonts of the original data have no accented capitals, which is why the French texts of the port's menus have none. To keep the accents, put fonts with those letters in `STATIC/FONTS.FLX` of the language folder.

The screens have a fixed layout: an option's name and its value share a row. Prefer short texts, and check them in the game.

## Adding a language

1. Create `languages/<id>/` next to the executable, for example `languages/pt-br/`.
2. Copy `languages/en/port.txt` into it.
3. Set `@name` to the language's name as players should see it, in capitals for the menus: `@name = PORTUGUES (BRASIL)`.
4. Replace the right-hand side of each line with its translation. Keep the left-hand side as it is. A text not translated yet can stay in English.
5. Optionally, put translated data files in the folder, at their path in the data folder (`STATIC/GUMPS.FLX`...).
6. Test in the game: pick the language in **Options**, **+**, **Game**, **Language**, then restart the game if the folder has data files. `port.txt` is read again every time a language is picked, so after editing it, pick another language and then yours again. Translations the game rejects are reported on the error output (`stderr`).
7. To ship the port's texts with the port, open a pull request that adds `languages/<id>/port.txt`. From the repository, `python scripts/update_languages.py` replaces the header copied from `en/port.txt`, puts the lines in the reference order and adds an empty line for every missing text. The data files are distributed by the translator, as a folder to put in `languages/`.

## When the port's texts change (developers)

The game's code writes its texts in English with `tr("...")`: `tr("BACK")` returns the translation of `BACK` in the language in use. The level editor's own texts are not in the language folders: they keep `tr("french", "english")`, a form that only the editor may use.

After adding, changing or removing a text in the code (a `tr("...")` call, an item name in `src/core/dtable.cpp`, a controller label in `src/core/pad_config.hpp`), run:

```sh
python scripts/update_languages.py
```

It rewrites `en/port.txt` and brings every other `port.txt` up to date. Their comments at the top, their `@name` and their translations are kept. New texts are added with an empty translation, and texts the game no longer uses are moved to a commented section at the end. Then complete `fr/port.txt`.

The `language_files` test (`ctest`) runs the script with `--check`. It fails when a file is out of date, when `fr/port.txt` misses a translation, or when game code calls `tr()` with two texts.

Two different texts must not share the same English wording, because a language has one translation per English text. For example, the controller action that selects objects is "SELECT OBJECT", so that it does not collide with the "SELECT" button.

## Reference

- `src/core/translation.hpp`: the `port.txt` format and the code page conversion.
- `src/core/language.hpp`: `tr()`, the language choice, the language folders.
- `src/core/data_files.hpp`: how a data file is looked up (language folder, mod folder, data folder).
- `scripts/update_languages.py`: generation and checks.
- [enhancements.md](../enhancements.md), section 8b: the Language option.
