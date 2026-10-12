#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <cstring>

#include "game_icon.h"
#include "installer_window.h"

int main(int argc, char** argv) {
  // --make-icon <game dir> <out dir>: only the game's icon, made from an
  // extracted game as an installation makes it, to look at. Not in --help,
  // and before QApplication so it needs no display.
  if (argc == 4 && std::strcmp(argv[1], "--make-icon") == 0) {
    QCoreApplication core(argc, argv);
    return MakeIconCommand(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
  }
  QApplication app(argc, argv);
  QApplication::setApplicationName("Xerenge installer");

  QCommandLineParser parser;
  parser.setApplicationDescription("Installs Burnout Revenge (Xerenge) from a disc image.");
  parser.addHelpOption();
  QCommandLineOption iso("iso", "Disc image to install from.", "image");
  QCommandLineOption path("path", "Directory to install into.", "directory");
  QCommandLineOption bloom("bloom", "Turn bloom on.");
  QCommandLineOption blur("blur", "Turn motion blur on.");
  QCommandLineOption xenia("xenia", "Draw with the Xenos backend.");
  QCommandLineOption no_shortcuts("no-shortcuts", "Create no desktop shortcut or menu entry.");
  parser.addOptions({iso, path, bloom, blur, xenia, no_shortcuts});
  parser.process(app);

  InstallerWindow window;
  // With --path it installs at once, without showing the window.
  if (parser.isSet(path)) {
    QTimer::singleShot(0, &window, [&] {
      window.InstallUnattended(parser.value(iso), parser.value(path), parser.isSet(bloom),
                               parser.isSet(blur), parser.isSet(xenia),
                               !parser.isSet(no_shortcuts));
    });
    return app.exec();
  }
  window.show();
  return app.exec();
}
