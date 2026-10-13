#include "game_icon.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>

#include <algorithm>
#include <cstdint>

namespace {

// fe/FeMain.bin, the front end's screens in one file:
//
//   +0x08  number of screens            +0x0C  offset of the screen table
//   screen table: per screen the offset of a pair { name offset, data offset }
//
// A screen ("title", "mainmenu", ...) is its Apt movie followed by its
// textures:
//
//   +0x04  number of textures           +0x08  offset of the texture table
//   texture table: per texture its offset from the screen's start
//
// and a texture is a page of header and then the pixels:
//
//   +0x2C  size with the header         +0x30  offset of the pixels
//   +0x44  the six words of the Xenos texture fetch constant
//
// Everything is big-endian.
constexpr qint64 kScreenCountOffset = 0x08;
constexpr qint64 kTextureCountOffset = 0x04;
constexpr qint64 kTextureSizeOffset = 0x2C;
constexpr qint64 kTextureFetchOffset = 0x44;
// More than any front end file has; a file that claims more is not one.
constexpr uint32_t kMostEntries = 4096;

// The screens that hold the logo as one texture: the title screen's, and the
// cleaner one the menus put in their corner should the first not be there.
constexpr const char* kLogoScreens[] = {"title", "b4title"};

bool ReadAt(QFile& file, qint64 offset, void* out, qint64 size) {
  return offset >= 0 && file.seek(offset) && file.read(static_cast<char*>(out), size) == size;
}

bool ReadWords(QFile& file, qint64 offset, uint32_t* out, int count) {
  if (!ReadAt(file, offset, out, qint64(count) * 4)) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    out[i] = qFromBigEndian(out[i]);
  }
  return true;
}

// Where a screen's data starts, or 0.
qint64 FindScreen(QFile& file, const QByteArray& wanted) {
  uint32_t header[2];  // count, table
  if (!ReadWords(file, kScreenCountOffset, header, 2) || header[0] == 0 ||
      header[0] > kMostEntries) {
    return 0;
  }
  for (uint32_t i = 0; i < header[0]; ++i) {
    uint32_t pair_offset;
    uint32_t pair[2];  // name, data
    if (!ReadWords(file, qint64(header[1]) + 4 * i, &pair_offset, 1) ||
        !ReadWords(file, pair_offset, pair, 2)) {
      return 0;
    }
    char name[32] = {};
    if (!file.seek(pair[0]) || file.read(name, sizeof(name) - 1) <= 0) {
      return 0;
    }
    if (wanted == name) {
      return pair[1];
    }
  }
  return 0;
}

// Where the pixel (x, y) of a tiled Xenos texture lies, in bytes: the 360
// stores textures in 32x32 tiles with the pixels inside interleaved. As
// GetTiledOffset2D of the SDK (graphics/pipeline/texture/util.cpp) for four
// bytes a pixel.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9;
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << 2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// The texture at `offset` when it can be the logo - 32 bits a pixel, four
// times as wide as high and no smaller than 512x128 - else a null image.
QImage ReadLogoTexture(QFile& file, qint64 offset) {
  uint32_t place[2];  // size with the header, offset of the pixels
  uint32_t fetch[6];
  if (!ReadWords(file, offset + kTextureSizeOffset, place, 2) ||
      !ReadWords(file, offset + kTextureFetchOffset, fetch, 6)) {
    return {};
  }
  // The fetch constant as the GPU reads it (xenos.h, xe_gpu_texture_fetch_t).
  const bool tiled = fetch[0] >> 31;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F;
  const uint32_t endianness = (fetch[1] >> 6) & 3;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t dimension = (fetch[5] >> 9) & 3;
  // k_8_8_8_8, its words byte-swapped (8-in-32), a 2D texture.
  if (format != 6 || endianness != 2 || dimension != 1) {
    return {};
  }
  if (width != height * 4 || width < 512 || width > 4096 || pitch < width) {
    return {};
  }
  // What the layout takes: whole tiles when tiled.
  const uint32_t rows = tiled ? (height + 31) & ~31u : height;
  const qint64 needed = qint64(pitch) * rows * 4;
  if (place[1] >= place[0] || qint64(place[0]) - place[1] < needed) {
    return {};
  }
  QByteArray data(needed, Qt::Uninitialized);
  if (!ReadAt(file, offset + place[1], data.data(), needed)) {
    return {};
  }
  QImage image(int(width), int(height), QImage::Format_ARGB32);
  const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
  for (uint32_t y = 0; y < height; ++y) {
    auto* line = reinterpret_cast<QRgb*>(image.scanLine(int(y)));
    for (uint32_t x = 0; x < width; ++x) {
      // Alpha, red, green, blue as the bytes lie in the file.
      const uchar* p = bytes + (tiled ? TiledOffset(x, y, pitch) : (y * pitch + x) * 4);
      line[x] = qRgba(p[1], p[2], p[3], p[0]);
    }
  }
  return image;
}

// A logo is letters on nothing: a good part of it see-through, a good part
// solid. A full-screen picture or noise read from the wrong place is neither.
bool LooksLikeLogo(const QImage& image) {
  qint64 clear = 0;
  qint64 solid = 0;
  for (int y = 0; y < image.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      clear += qAlpha(line[x]) == 0;
      solid += qAlpha(line[x]) == 255;
    }
  }
  const double pixels = double(image.width()) * image.height();
  return clear > 0.2 * pixels && clear < 0.8 * pixels && solid > 0.15 * pixels;
}

// The rectangle the mark itself takes, without the empty texture around it
// and without the faint end of its shadow.
QRect MarkBounds(const QImage& logo) {
  int left = logo.width(), top = logo.height(), right = -1, bottom = -1;
  for (int y = 0; y < logo.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(logo.constScanLine(y));
    for (int x = 0; x < logo.width(); ++x) {
      if (qAlpha(line[x]) >= 128) {
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
  }
  if (right < left) {
    return logo.rect();
  }
  return QRect(QPoint(left, top), QPoint(right, bottom));
}

// The icon at 1024 pixels, which every size is scaled from. `close` is the
// one for the small sizes: there the whole word is a red line a few pixels
// high, so it shows the start of the mark as large as the plate lets it.
QImage DrawMaster(const QImage& logo, bool mac, bool close) {
  constexpr int kSize = 1024;
  QImage icon(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
  icon.fill(Qt::transparent);
  QPainter painter(&icon);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

  // Apple's icon grid: an 824 pixel plate in the 1024 canvas, its corners
  // rounded by 185, a soft shadow below.
  const QRectF plate = mac ? QRectF(100, 100, 824, 824) : QRectF(0, 0, kSize, kSize);
  const qreal radius = mac ? 185.4 : 0;
  QPainterPath outline;
  outline.addRoundedRect(plate, radius, radius);
  if (mac) {
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 12; ++i) {
      const qreal grow = 14 - i * 1.5;
      painter.setBrush(QColor(0, 0, 0, 9));
      painter.drawRoundedRect(plate.adjusted(-grow, -grow + 10, grow, grow + 10), radius + grow,
                              radius + grow);
    }
  }

  // The plate: nearly black, lighter at the top, with a red glow behind where
  // the logo goes.
  QLinearGradient shade(plate.topLeft(), plate.bottomLeft());
  shade.setColorAt(0, QColor(0x2c, 0x2c, 0x31));
  shade.setColorAt(1, QColor(0x08, 0x08, 0x0a));
  painter.fillPath(outline, shade);
  painter.setClipPath(outline);
  QRadialGradient glow(plate.center(), plate.width() * 0.62);
  glow.setColorAt(0, QColor(0xc8, 0x1c, 0x14, 120));
  glow.setColorAt(1, QColor(0xc8, 0x1c, 0x14, 0));
  painter.fillRect(plate, glow);

  const QRect mark = MarkBounds(logo);
  if (close) {
    // A square from the mark's left end, as high as the mark: its first
    // letters, fading out to the right where the cut goes through one.
    const QRect first(mark.left(), mark.top(), std::min(mark.height(), mark.width()),
                      mark.height());
    QImage letters = logo.copy(first).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
      QPainter fade(&letters);
      QLinearGradient across(0, 0, letters.width(), 0);
      across.setColorAt(0.78, QColor(0, 0, 0, 255));
      across.setColorAt(1, QColor(0, 0, 0, 0));
      fade.setCompositionMode(QPainter::CompositionMode_DestinationIn);
      fade.fillRect(letters.rect(), across);
    }
    const qreal side = plate.width() * 0.88;
    painter.drawImage(QRectF(plate.center().x() - side / 2, plate.center().y() - side / 2, side,
                             side),
                      letters);
  } else {
    // The whole mark across the plate, centred.
    const qreal width = plate.width() * 0.9;
    const qreal height = width * mark.height() / mark.width();
    painter.drawImage(QRectF(plate.center().x() - width / 2, plate.center().y() - height / 2,
                             width, height),
                      logo, mark);
  }
  painter.setClipping(false);

  // A thin lighter edge, so the plate does not sink into a dark background.
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QColor(255, 255, 255, 28), 4));
  painter.drawRoundedRect(plate.adjusted(2, 2, -2, -2), std::max(radius - 2, 0.0),
                          std::max(radius - 2, 0.0));
  return icon;
}

}  // namespace

// A path under the game, each part matched without regard to case: the disc's
// names come out of an extraction as the image has them (fe/femain.bin here),
// and only Windows and macOS would find them by another spelling.
QString FindIgnoringCase(const QString& game_dir, const QStringList& parts) {
  QDir dir(game_dir);
  for (int i = 0; i < parts.size(); ++i) {
    const bool last = i + 1 == parts.size();
    const QStringList names =
        dir.entryList(last ? QDir::Files : QDir::Dirs | QDir::NoDotAndDotDot);
    QString found;
    for (const QString& name : names) {
      if (name.compare(parts[i], Qt::CaseInsensitive) == 0) {
        found = name;
        break;
      }
    }
    if (found.isEmpty()) {
      return dir.filePath(parts.mid(i).join('/'));
    }
    if (last) {
      return dir.filePath(found);
    }
    dir.cd(found);
  }
  return dir.path();
}

QImage ReadGameLogo(const QString& game_dir, QString* reason) {
  QFile file(FindIgnoringCase(game_dir, {"fe", "FeMain.bin"}));
  if (!file.open(QIODevice::ReadOnly)) {
    *reason = QString("cannot open %1").arg(file.fileName());
    return {};
  }
  for (const char* screen_name : kLogoScreens) {
    const qint64 screen = FindScreen(file, screen_name);
    uint32_t textures[2];  // count, table
    if (screen == 0 || !ReadWords(file, screen + kTextureCountOffset, textures, 2) ||
        textures[0] > kMostEntries) {
      continue;
    }
    for (uint32_t i = 0; i < textures[0]; ++i) {
      uint32_t offset;
      if (!ReadWords(file, screen + textures[1] + 4 * i, &offset, 1)) {
        break;
      }
      const QImage logo = ReadLogoTexture(file, screen + offset);
      if (!logo.isNull() && LooksLikeLogo(logo)) {
        return logo;
      }
    }
  }
  *reason = QString("%1 has no title logo where the retail disc has it").arg(file.fileName());
  return {};
}

QImage DrawGameIcon(const QImage& logo, int size, bool mac) {
  // Up to 32 pixels the start of the mark; from there the whole of it.
  return DrawMaster(logo, mac, size <= 32)
      .scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QString WriteGameIcns(const QImage& logo, const QString& icns_path) {
  // iconutil turns a folder of PNGs with these names into the .icns; it is
  // part of macOS itself.
  QTemporaryDir work;
  if (!work.isValid()) {
    return "no temporary directory for the icon";
  }
  const QDir iconset(work.filePath("xerenge.iconset"));
  if (!QDir().mkpath(iconset.absolutePath())) {
    return "no temporary directory for the icon";
  }
  for (const int points : {16, 32, 128, 256, 512}) {
    for (const int scale : {1, 2}) {
      const QString name = scale == 1 ? QString("icon_%1x%1.png").arg(points)
                                      : QString("icon_%1x%1@2x.png").arg(points);
      if (!DrawGameIcon(logo, points * scale, true).save(iconset.filePath(name), "PNG")) {
        return QString("cannot write %1").arg(iconset.filePath(name));
      }
    }
  }
  QProcess iconutil;
  iconutil.setProcessChannelMode(QProcess::MergedChannels);
  iconutil.start("/usr/bin/iconutil", {"-c", "icns", iconset.absolutePath(), "-o", icns_path});
  if (!iconutil.waitForFinished(30000) || iconutil.exitStatus() != QProcess::NormalExit ||
      iconutil.exitCode() != 0 || !QFileInfo::exists(icns_path)) {
    iconutil.kill();
    iconutil.waitForFinished(1000);
    QFile::remove(icns_path);
    return QString("iconutil failed: %1")
        .arg(QString::fromUtf8(iconutil.readAll()).trimmed().left(200));
  }
  return {};
}

QString WriteGameIconPng(const QImage& logo, const QString& png_path) {
  if (!DrawGameIcon(logo, 256, false).save(png_path, "PNG")) {
    return QString("cannot write %1").arg(png_path);
  }
  return {};
}

int MakeIconCommand(const QString& game_dir, const QString& out_dir) {
  QTextStream out(stdout);
  QString reason;
  const QImage logo = ReadGameLogo(game_dir, &reason);
  if (logo.isNull()) {
    out << "no icon: " << reason << "\n";
    return 1;
  }
  const QDir dir(out_dir);
  if (!QDir().mkpath(dir.absolutePath()) || !logo.save(dir.filePath("logo.png"), "PNG")) {
    out << "cannot write into " << dir.absolutePath() << "\n";
    return 1;
  }
  out << "logo: " << logo.width() << "x" << logo.height() << "\n";
  for (const bool mac : {true, false}) {
    for (const int size : {16, 32, 64, 128, 256, 512, 1024}) {
      DrawGameIcon(logo, size, mac)
          .save(dir.filePath(QString("icon-%1-%2.png").arg(mac ? "macos" : "linux").arg(size)),
                "PNG");
    }
  }
  if (const QString error = WriteGameIconPng(logo, dir.filePath("burnout-revenge.png"));
      !error.isEmpty()) {
    out << error << "\n";
    return 1;
  }
#ifdef Q_OS_MACOS
  if (const QString error = WriteGameIcns(logo, dir.filePath("xerenge.icns")); !error.isEmpty()) {
    out << error << "\n";
    return 1;
  }
#endif
  out << "written: " << dir.absolutePath() << "\n";
  return 0;
}
