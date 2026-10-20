// The online menus' news (Xbox Live > News): the latest GitHub release's notes.
//
// The game downloads its news over HTTP from the URL its lobby login gives
// (the client points it at this server, http://<server>:31862/news, beside the
// Locker) and shows the text as it comes, UTF-8. The text here is the release
// of a repository the server follows: its tag and its notes, the notes'
// Markdown made plain for the game's fonts. Fetched with curl from GitHub's
// API at the start and every ten minutes, so new notes reach the players
// without a server or game update.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ealobby {

class News {
 public:
  // repository: owner/name on GitHub; empty: no news.
  News(std::string repository, std::function<void(const std::string&)> log);
  ~News();

  // Whether an HTTP request (a whole one, Locker::Complete) asks for the news.
  static bool Wanted(const std::vector<uint8_t>& request);
  // The whole HTTP response with the news.
  std::vector<uint8_t> Response() const;

  // The release notes as the game shows them, from GitHub's JSON for a release.
  static std::string FromRelease(const std::string& json);

 private:
  void Run();

  std::string repository_;
  std::function<void(const std::string&)> log_;
  mutable std::mutex mutex_;
  std::string text_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}  // namespace ealobby
