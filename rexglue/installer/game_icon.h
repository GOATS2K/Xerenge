#pragma once

#include <QImage>
#include <QString>

// The game's icon, made at install time from the player's own game files: the
// title screen's "Burnout Revenge" logo is read out of fe/FeMain.bin and drawn
// onto a dark plate. Nothing of the game's artwork ships with the installer.

// The logo as the title screen has it (1024x256, with its alpha), or a null
// image and `reason` when these game files do not hold it where the retail
// disc does.
QImage ReadGameLogo(const QString& game_dir, QString* reason);

// A square icon of `size` pixels. `mac` gives it the margins, the rounded
// corners and the shadow of a macOS app icon; without it the plate is the
// whole square.
QImage DrawGameIcon(const QImage& logo, int size, bool mac);

// An .icns with every size up to 512x512@2x, through macOS's own iconutil.
// Returns an error, or nothing.
QString WriteGameIcns(const QImage& logo, const QString& icns_path);

// One 256x256 PNG, what a Linux desktop entry names with Icon=.
QString WriteGameIconPng(const QImage& logo, const QString& png_path);

// `--make-icon <game dir> <out dir>`: only the icon step, for looking at what
// it makes - the logo as read, the icon in each size and, on a Mac, the .icns.
int MakeIconCommand(const QString& game_dir, const QString& out_dir);
