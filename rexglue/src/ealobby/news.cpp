#include "ealobby/news.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace ealobby {
namespace {

// A JSON string's value by its key, unescaped (\n, \", \\, \uXXXX as UTF-8).
// Enough for GitHub's flat release fields; not a JSON parser.
std::string JsonString(const std::string& json, const std::string& key) {
  const std::string quoted = "\"" + key + "\"";
  size_t at = json.find(quoted);
  if (at == std::string::npos) {
    return {};
  }
  at = json.find(':', at + quoted.size());
  if (at == std::string::npos) {
    return {};
  }
  at = json.find_first_not_of(" \t\r\n", at + 1);
  if (at == std::string::npos || json[at] != '"') {
    return {};
  }
  std::string out;
  for (size_t i = at + 1; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '"') {
      break;
    }
    if (c != '\\' || i + 1 >= json.size()) {
      out += c;
      continue;
    }
    const char e = json[++i];
    switch (e) {
      case 'n': out += '\n'; break;
      case 'r': break;
      case 't': out += ' '; break;
      case 'u': {
        if (i + 4 < json.size()) {
          const unsigned code = unsigned(std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
          i += 4;
          if (code < 0x80) {
            out += char(code);
          } else if (code < 0x800) {
            out += char(0xC0 | (code >> 6));
            out += char(0x80 | (code & 0x3F));
          } else {
            out += char(0xE0 | (code >> 12));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
          }
        }
        break;
      }
      default: out += e;
    }
  }
  return out;
}

void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
  for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) {
    text.replace(at, from.size(), to);
  }
}

// Markdown made plain, and the characters the game's fonts lack made ASCII.
std::string Plain(std::string text) {
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {"**", ""}, {"__", ""}, {"`", ""}, {"\xE2\x80\x94", "-"}, {"\xE2\x80\x93", "-"},
           {"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}, {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""},
           {"\xE2\x80\xA6", "..."}, {"\xE2\x86\x92", "->"}, {"\xC2\xA0", " "}}) {
    ReplaceAll(text, from, to);
  }
  std::string out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(start, end - start);
    // Headings without their #; links as their text.
    size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#') {
      ++hashes;
    }
    if (hashes && hashes < line.size() && line[hashes] == ' ') {
      line = line.substr(hashes + 1);
    }
    for (size_t open; (open = line.find('[')) != std::string::npos;) {
      const size_t close = line.find("](", open);
      const size_t paren = close == std::string::npos ? close : line.find(')', close);
      if (paren == std::string::npos) {
        break;
      }
      line = line.substr(0, open) + line.substr(open + 1, close - open - 1) + line.substr(paren + 1);
    }
    out += line + "\n";
    start = end + 1;
  }
  // What is left beyond ASCII would show as nothing.
  std::string ascii;
  for (const unsigned char c : out) {
    if (c < 0x80) {
      ascii += char(c);
    } else if ((c & 0xC0) == 0xC0) {
      ascii += '?';
    }
  }
  while (!ascii.empty() && (ascii.back() == '\n' || ascii.back() == ' ')) {
    ascii.pop_back();
  }
  return ascii;
}

}  // namespace

News::News(std::string repository, std::function<void(const std::string&)> log)
    : repository_(std::move(repository)), log_(std::move(log)) {
  if (!repository_.empty()) {
    thread_ = std::thread([this] { Run(); });
  }
}

News::~News() {
  stopping_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool News::Wanted(const std::vector<uint8_t>& request) {
  static const char kGet[] = "GET /news";
  return request.size() >= sizeof(kGet) - 1 && std::memcmp(request.data(), kGet, sizeof(kGet) - 1) == 0;
}

std::vector<uint8_t> News::Response() const {
  std::string text;
  {
    std::lock_guard lock(mutex_);
    text = text_;
  }
  if (text.empty()) {
    text = "No news yet.";
  }
  const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                               std::to_string(text.size()) + "\r\nConnection: close\r\n\r\n" + text;
  return {response.begin(), response.end()};
}

std::string News::FromRelease(const std::string& json) {
  const std::string tag = JsonString(json, "tag_name");
  const std::string body = JsonString(json, "body");
  if (tag.empty() && body.empty()) {
    return {};
  }
  // In the game, what is new: the notes from their "What's new" on, when they
  // have one (before it: how to install, which a player in the game has done).
  std::string notes = body;
  if (const size_t news = notes.find("What's new"); news != std::string::npos) {
    const size_t line = notes.rfind('\n', news);
    notes = notes.substr(line == std::string::npos ? 0 : line + 1);
  }
  // Code blocks' fences go.
  std::string kept;
  size_t start = 0;
  while (start < notes.size()) {
    size_t end = notes.find('\n', start);
    if (end == std::string::npos) {
      end = notes.size();
    }
    const std::string line = notes.substr(start, end - start);
    if (line.find_first_not_of(" ") == std::string::npos || line.compare(line.find_first_not_of(" "), 3, "```") != 0) {
      kept += line + "\n";
    }
    start = end + 1;
  }
  // Within what the game takes (its buffer holds 0x15400 bytes of UTF-16).
  std::string text = Plain("Xerenge " + tag + "\n\n" + kept);
  if (text.size() > 20000) {
    text.resize(20000);
  }
  return text;
}

void News::Run() {
  const std::string command = "curl -fsSL --max-time 30 -H \"Accept: application/vnd.github+json\" "
                              "https://api.github.com/repos/" + repository_ + "/releases/latest";
  while (!stopping_) {
    std::string json;
    if (FILE* pipe = popen(command.c_str(), "r")) {
      char buffer[4096];
      size_t read;
      while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        json.append(buffer, read);
      }
      pclose(pipe);
    }
    const std::string text = FromRelease(json);
    if (!text.empty()) {
      bool changed;
      {
        std::lock_guard lock(mutex_);
        changed = text != text_;
        text_ = text;
      }
      if (changed) {
        log_("news: " + text.substr(0, text.find('\n')) + " (" + std::to_string(text.size()) + " bytes)");
      }
    } else {
      log_("news: could not read the latest release of " + repository_ + " (curl missing, or GitHub unreachable)");
    }
    for (int i = 0; i < 600 && !stopping_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

}  // namespace ealobby
