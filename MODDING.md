# Making mods

Xerenge looks for mods in a `mods` folder inside the game's files - the folder
that holds `default.xex`, `fe`, `language` and the rest (`game` in an installed
copy, `game_eu`/`game_us` in a checkout). Each mod is a folder of its own with a
`mod.cfg` in it:

```
game/
  default.xex
  language/
  mods/
    locale_rus/
      mod.cfg
      locale_rus.txt
      Big.fnt
      Big.png
      ...
```

`mod.cfg` is a list of `Key = value` lines; everything after a `#` is a
comment. `Type` says what the mod is; the rest depends on the type.

So far there is one type: **localisation**.

## Localisation mods

A localisation mod puts its own text in place of the game's, and its own fonts
in place of the game's fonts - for a language the game was never released in.

```
Version = 0.1
Type = Localization
Name = Russian

Localization = locale_rus.txt

FontBig = Big.fnt
FontBigDistressed = BigDistressed.fnt
FontSmall = Small.fnt
# FontDigital = Digital.fnt
```

| Key | What it is |
|---|---|
| `Version` | The mod's own version, shown in the log. |
| `Type` | `Localization`. |
| `Name` | The language's name as it appears in **Settings > LANGUAGE**. |
| `Localization` | The text file (below). |
| `FontBig`, `FontBigDistressed`, `FontSmall`, `FontDigital` | Fonts in place of the game's (below). A font left out stays the game's. |

### Turning it on

Every localisation mod found appears in **Settings > LANGUAGE** after the
game's own languages, under its `Name`; choosing it takes effect after the
restart the game offers. It is written to `xerenge.conf` as
`locale_mod = <the mod's folder>`. Without a `locale_mod` line at all, the first
mod found (by folder name) is used; `locale_mod =` with nothing after it turns
mods off.

The game's language stays whatever was chosen before: the mod's text replaces
those strings, and any string the mod does not have keeps that language's.

### The text file

UTF-8, one string a line:

```
# A comment.
FESONGMANAGER = МЕНЕДЖЕР ПЕСЕН
FEPREVIEWTRACK = Прослушать
crashTip19 = Playing Crash\n\nSmashing up street furniture earns\nsmall amounts of $.
0x80303571 = ЛУЧШИЙ СЧЁТ
```

- The key is the string's name, as the game's interface asks for it (in the
  menus it is written `$FESONGMANAGER`; the `$` may be left out).
- `\n` is a new line, `\\` a backslash.
- The game keeps its strings by a hash of the name, not by the name, and not
  every name could be found again. A string whose name is not known is written
  with its hash, `0x80303571`; such a key works the same.
- Keep the game's placeholders (`%1`, `%s` and the like) where they are in the
  English text.

**Start from the game's own text.** `localeexport` writes every string of a
language file into this format, with its name where it is known and its hash
where it is not:

```bash
tools/localeexport/localeexport game/language/strings/MainUK.bin strings_template.txt game/fe/*.bin
```

(`MainUK.bin` on the European disc, `MainUS.bin` on the American one; the
`fe/*.bin` files are where names are looked for. Adding the game's decrypted
executable finds a few more.) Translate the lines you want and keep them in
your text file; the template need not be shipped.

### Fonts

The game draws all of its text with four fonts:

| Key | The game's file | Where it is used |
|---|---|---|
| `FontBig` | `language/fonts/Big.bin` | Most of the menus: lists, buttons, hints (Univers 59 Ultra Condensed) |
| `FontBigDistressed` | `language/fonts/BigDistressed.bin` | Titles: SONG MANAGER, [EA TRAX]... - the same letters, worn |
| `FontSmall` | `language/fonts/small.bin` | Smaller text (Univers 47 Condensed Light) |
| `FontDigital` | `language/fonts/digital.bin` | The LCD-style numbers - digits only |

A font may be given in either of two forms; the extension says which:

- **A BMFont** - `.fnt` (the text format) and its `.png` page(s). Pictures of
  each letter, drawn as you like: this is how to make a font look like the
  game's, wear and all.
- **A TrueType or OpenType font** - `.ttf`, `.otf`. Simplest: it is drawn at the
  start, sized so its capitals are as tall as the game's. Clean letters only -
  no wear for `FontBigDistressed`.

**Start from the game's own fonts.** `fontgen export` writes a game font as a
BMFont, with Cyrillic already put together out of its own Latin letters (Я from
a mirrored R, Б from E over B, И from a mirrored N, and so on) - a draft to
redraw letter by letter in any image editor:

```bash
tools/fontgen/fontgen export game/language/fonts/Big.bin Big mods/locale_rus
tools/fontgen/fontgen export game/language/fonts/BigDistressed.bin BigDistressed mods/locale_rus
tools/fontgen/fontgen export game/language/fonts/small.bin Small mods/locale_rus
```

`fontgen fnt <game font> <your .fnt> <out.bin>` (or `fontgen ttf ...`) makes the
game's font of yours the way the game will, and writes `<out.bin>.pam`: a line
of text drawn with it, to look at before starting the game.

What to keep in mind when drawing:

- **The atlas keeps the game's size**: 512x512 for Big and BigDistressed,
  256x256 for Small. The game sizes a letter on the screen by its share of
  that atlas, so a bigger one would draw everything smaller. Your page may be
  any size - its letters are packed anew - but they must fit the game's.
  When they do not, the letters that matter least are left out first: the
  accented Latin ones (À-ÿ), then the rarer signs. ASCII, the quotes, ©, ® and
  ™, and Cyrillic are always kept.
- **Keep the game's proportions.** A letter's `yoffset` is measured from the
  top of the line and `base` is the baseline; the exported files have the
  game's own values - new letters should sit on the same baseline and be as
  tall as their Latin neighbours.
- **White letters** on a transparent background: the game colours them.
- Every character your text uses needs a glyph. A character the font does not
  have is drawn as nothing.

### What happens at the start

Before the game's files are opened, Xerenge makes each of the mod's fonts into
the game's own format and keeps the result in the mod's `.cache` folder. It is
made again only when the font, its pages, `mod.cfg` or the text file are newer
than it - so editing a `.png` and starting the game is enough. Delete `.cache`
to have everything made again.

`burnout.log` says what was found and done - lines starting with `mods:`:

```
mods: locale_rus (Russian, version 0.1)
mods: 412 strings from .../mods/locale_rus/locale_rus.txt
mods: locale_rus: Big made of Big.fnt (1060864 bytes)
mods: the game's Language/Fonts/Big.bin is mods/locale_rus/.cache/Big.bin ...
```

and why a font was not used, if it was not.

### Your own music

Not a mod, but close: the songs of a `music` folder (beside the game, or
`music_dir = <folder>` in `xerenge.conf`) can take EA Trax's place - **Driver
Details > Song Manager**, LT. MP3 and WMA; the names come from the files' tags
(or from `Artist - Title.mp3`). With a localisation mod whose fonts have the
letters, the names are shown as they are; without one, Cyrillic in them is
written in Latin letters.

### Sharing a mod

A mod is shared as its folder, `.cache` left out. Do not put the game's own
files in it: a font exported from the game and only touched up is still the
game's font, and the game's text is the game's. Fonts of your own drawing, or
fonts whose licence allows it (SIL Open Font License and the like), and your
own translation, are fine.
