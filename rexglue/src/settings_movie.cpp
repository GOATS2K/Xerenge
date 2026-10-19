// Driver Details > Settings with Xerenge's own buttons: GRAPHICS, HACKS and
// DEBUG under the game's sound rows, each opening a page of settings.
//
// The screen is an Apt movie (tools/aptdump shows it). Apt is handed a movie
// by CB4AptManager::AptCBLoadAnimation (0x821FF408) through sub_824277A0
// (file, data, constants, entry): the movie's "Apt Data" blob and its
// "Apt constant file", raw, as the front-end package holds them. For the
// settings movie the two are rebuilt here into new guest memory and handed
// over instead - the original data at the same offsets, with what is added
// after it, and the constants with entries of our own - so the game's own
// rows, code and offsets stay as they were.
//
// Added to the movie, as xerenge_settings.cpp lays the pages out:
//   - an import of b4Components/B4LongMenuOption (the Xbox Live options' EA
//     TRAX button, "B4LongMenuOption") as a new character;
//   - on the main page, under Music Volume, a Language row and the buttons
//     GRAPHICS, HACKS and DEBUG; on ours, our rows in place of the game's
//     and the page's name in place of [SETTINGS];
//   - each row's fields set when it loads, as the game's rows set theirs:
//     a choice row (MenuChoice, like Takedown Camera) on the natives
//     XerengeGet/XerengeSet; a button with an A press handler that calls
//     MenuScreenManager.SelectedOption(its id), which the menu service hook
//     (xerenge_menu.cpp) takes.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "xerenge_settings.h"

extern "C" void __imp__sub_824277A0(PPCContext& __restrict, uint8_t*);

// The ids the buttons hand to SelectedOption (xerenge_menu.cpp).
constexpr uint32_t kXerengeOptionBase = 0x58000;

namespace {

// --- blobs ---------------------------------------------------------------

struct Blob {
  std::vector<uint8_t> bytes;

  uint32_t U32(uint32_t at) const {
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 |
           bytes[at + 3];
  }
  void Set32(uint32_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
  }
  void Align(uint32_t to) {
    while (bytes.size() % to) {
      bytes.push_back(0);
    }
  }
  uint32_t Here() const { return uint32_t(bytes.size()); }
  void Put8(uint8_t value) { bytes.push_back(value); }
  void Put32(uint32_t value) {
    bytes.resize(bytes.size() + 4);
    Set32(Here() - 4, value);
  }
  uint32_t PutString(const std::string& text) {
    Align(4);
    const uint32_t at = Here();
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0);
    return at;
  }
  std::string Str(uint32_t at) const {
    std::string text;
    while (at < bytes.size() && bytes[at]) {
      text += char(bytes[at++]);
    }
    return text;
  }
};

// The constant file: its entries, rebuilt with the strings laid out anew.
struct Constants {
  uint8_t header[20] = {};
  uint32_t root = 0;
  struct Entry {
    uint32_t type, value;
    std::string text;
  };
  std::vector<Entry> entries;

  bool Read(const uint8_t* raw, size_t size) {
    if (size < 32 || std::memcmp(raw, "Apt constant file", 17) != 0) {
      return false;
    }
    Blob b;
    b.bytes.assign(raw, raw + size);
    std::memcpy(header, raw, 20);
    root = b.U32(20);
    const uint32_t count = b.U32(24);
    if (32 + count * 8 > size) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      Entry e{b.U32(32 + i * 8), b.U32(32 + i * 8 + 4), {}};
      if (e.type == 1) {
        e.text = b.Str(e.value);
      }
      entries.push_back(std::move(e));
    }
    return true;
  }

  uint32_t String(const std::string& text) {
    for (uint32_t i = 0; i < entries.size(); ++i) {
      if (entries[i].type == 1 && entries[i].text == text) {
        return i;
      }
    }
    entries.push_back({1, 0, text});
    return uint32_t(entries.size() - 1);
  }
  uint32_t Value(uint32_t type, uint32_t value) {
    for (uint32_t i = 0; i < entries.size(); ++i) {
      if (entries[i].type == type && entries[i].value == value) {
        return i;
      }
    }
    entries.push_back({type, value, {}});
    return uint32_t(entries.size() - 1);
  }
  uint32_t Int(int32_t value) { return Value(7, uint32_t(value)); }
  uint32_t Bool(bool value) { return Value(5, value ? 1 : 0); }

  Blob Write() const {
    Blob b;
    b.bytes.assign(header, header + 20);
    b.Put32(root);
    b.Put32(uint32_t(entries.size()));
    b.Put32(32);
    const uint32_t table = b.Here();
    b.bytes.resize(table + entries.size() * 8);
    for (uint32_t i = 0; i < entries.size(); ++i) {
      uint32_t value = entries[i].value;
      if (entries[i].type == 1) {
        value = b.PutString(entries[i].text);
      }
      b.Set32(table + i * 8, entries[i].type);
      b.Set32(table + i * 8 + 4, value);
    }
    b.Align(16);
    return b;
  }
};

// --- bytecode --------------------------------------------------------------

// ActionScript into the data blob. PushData's lists go after the code, and
// their offsets are filled in at the end (Finish).
class Code {
 public:
  explicit Code(Blob& blob) : b_(blob) {}

  uint32_t Start() const { return b_.Here(); }

  void Op(uint8_t op) { b_.Put8(op); }
  void Aligned(uint8_t op) {
    b_.Put8(op);
    b_.Align(4);
  }
  void Push(std::vector<uint32_t> constants) {
    Aligned(0x96);
    b_.Put32(uint32_t(constants.size()));
    lists_.push_back({b_.Here(), std::move(constants)});
    b_.Put32(0);
  }
  void PushRegister(uint8_t reg) {
    Op(0xB9);
    Op(reg);
  }
  // DefineFunction2 with one parameter, by name, and the header the game's own
  // handlers have (two registers, _global in r1). Returns where the body size goes.
  uint32_t BeginFunction(uint32_t param_name) {
    Aligned(0x8E);
    b_.Put32(empty_name_);
    b_.Put32(1);
    b_.Put32(0x0002012A);
    params_.push_back(b_.Here());
    b_.Put32(param_name);  // fixed up to the parameter list in Finish
    const uint32_t size_at = b_.Here();
    b_.Put32(0);
    b_.Put32(0x98765432);
    b_.Put32(0x12345678);
    return size_at;
  }
  void EndFunction(uint32_t size_at) { b_.Set32(size_at, b_.Here() - (size_at + 12)); }

  void SetEmptyName(uint32_t at) { empty_name_ = at; }

  void Finish() {
    for (auto& list : lists_) {
      b_.Align(4);
      const uint32_t at = b_.Here();
      for (uint32_t c : list.second) {
        b_.Put32(c);
      }
      b_.Set32(list.first, at);
    }
    for (uint32_t at : params_) {
      const uint32_t name = b_.U32(at);
      b_.Align(4);
      const uint32_t list = b_.Here();
      b_.Put32(0);  // not in a register
      b_.Put32(name);
      b_.Set32(at, list);
    }
    lists_.clear();
    params_.clear();
  }

 private:
  Blob& b_;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> lists_;
  std::vector<uint32_t> params_;
  uint32_t empty_name_ = 0;
};

// --- the settings movie ----------------------------------------------------
// The buttons on the main page, each opening one of ours.
struct Button {
  const char* name;
  const char* label;
  xerenge::settings::Page page;
};
constexpr Button kButtons[] = {
    {"xerenge_graphics_btn", "GRAPHICS", xerenge::settings::kGraphics},
    {"xerenge_hacks_btn", "HACKS", xerenge::settings::kHacks},
    {"xerenge_debug_btn", "DEBUG", xerenge::settings::kDebug},
};
// Where rows go: the game's rows are 40 apart, from Takedown Camera's 148; the
// main page's additions follow Music Volume, from Speaker Config's 267 (the
// row the game hides on an Xbox). Our pages hold up to 8 rows, a little closer.
constexpr float kChoiceX = 22.25f;
constexpr float kButtonX = 10.6f;  // as the Xbox Live options place EA TRAX
constexpr float kMainFirstY = 267.0f;
constexpr float kMainStep = 40.0f;
constexpr float kPageFirstY = 148.0f;
constexpr float kPageStep = 34.0f;
// The game's rows, left out of our pages.
constexpr const char* kGameRows[] = {"takedown_btn", "effectsvol_btn", "musicvol_btn", "speaker_btn"};

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

bool IsSettingsMovie(const uint8_t* data, size_t size) {
  static const char kName[] = "__Packages.DriverDetailsSettingsClass";
  return std::search(data, data + size, kName, kName + sizeof(kName) - 1) != data + size;
}

// The rebuilt blobs, or false to leave the movie as it is.
bool Rebuild(const uint8_t* data, size_t data_size, const uint8_t* constants, size_t constants_size,
             Blob& out_data, Blob& out_constants) {
  Constants c;
  if (!c.Read(constants, constants_size)) {
    REXLOG_WARN("menu: settings movie: its constants do not read");
    return false;
  }
  Blob& b = out_data;
  b.bytes.assign(data, data + data_size);
  const uint32_t root = c.root;
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    REXLOG_WARN("menu: settings movie: no movie at {:X}", root);
    return false;
  }
  b.Align(16);

  // The import: a new character.
  const uint32_t imports = b.U32(root + 40), import_list = b.U32(root + 44);
  const uint32_t characters = b.U32(root + 20), character_list = b.U32(root + 24);
  const uint32_t button_character = characters;
  const uint32_t movie_name = b.PutString("b4Components/B4LongMenuOption");
  const uint32_t export_name = b.PutString("B4LongMenuOption");
  b.Align(4);
  const uint32_t new_imports = b.Here();
  b.bytes.insert(b.bytes.end(), data + import_list, data + import_list + imports * 16);
  b.Put32(movie_name);
  b.Put32(export_name);
  b.Put32(button_character);
  b.Put32(0);
  b.Set32(root + 40, imports + 1);
  b.Set32(root + 44, new_imports);
  const uint32_t new_characters = b.Here();
  b.bytes.insert(b.bytes.end(), data + character_list, data + character_list + characters * 4);
  b.Put32(0);
  b.Set32(root + 20, characters + 1);
  b.Set32(root + 24, new_characters);

  // The root's first frame: its items, and the template the buttons copy - the
  // hidden Speaker Config row's placement (flags, colour, clip event header).
  const uint32_t frame = b.U32(root + 12);
  const uint32_t item_count = b.U32(frame), item_list = b.U32(frame + 4);
  uint32_t speaker = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) == 3 && (b.U32(item + 4) & 0x20) && b.Str(b.U32(item + 52)) == "speaker_btn") {
      speaker = item;
    }
  }
  if (!speaker) {
    REXLOG_WARN("menu: settings movie: no speaker_btn row to copy");
    return false;
  }
  const uint32_t speaker_events = b.U32(speaker + 60);
  const uint32_t event_flags = b.U32(b.U32(speaker_events + 4));

  Code code(b);
  code.SetEmptyName(b.PutString(""));
  const uint32_t param_name = b.PutString("m");  // before any code: nothing may go in between
  // The A press handler: MenuScreenManager.SelectedOption(m.xid), m the button.
  const auto define_handler = [&] {
    const uint32_t body = code.BeginFunction(param_name);
    code.Push({c.String("m")});
    code.Op(0x1C);  // GetVariable
    code.Push({c.String("xid")});
    code.Op(0x4E);  // GetMember
    code.Push({c.Int(1), c.String("MenuScreenManager")});
    code.Op(0x1C);
    code.Push({c.String("SelectedOption")});
    code.Op(0x52);  // CallMethod
    code.Op(0x17);  // Pop
    code.EndFunction(body);
  };
  const auto set = [&](const std::string& name, uint32_t constant) {
    code.Push({c.String(name), constant});
    code.Op(0x1D);  // SetVariable
  };
  // name = []; name[i] = values[i]
  const auto set_array = [&](const std::string& name, const std::vector<uint32_t>& values) {
    code.Push({c.String(name), c.Int(0)});
    code.Op(0x42);  // InitArray
    code.Op(0x1D);
    for (size_t i = 0; i < values.size(); ++i) {
      code.Push({c.String(name)});
      code.Op(0x1C);
      code.Push({c.Int(int32_t(i)), values[i]});
      code.Op(0x4F);  // SetMember
    }
  };
  const auto common = [&](int tab, bool first) {
    set("m_bEnabled", c.Bool(true));
    set("m_refFM", c.String("_root.gFM"));
    set("m_initiallySelected", c.Bool(first));
    set("m_tabIndex", c.Int(tab));
    set("m_bVisible", c.Bool(true));
  };
  // A choice row's fields, as the game's Takedown Camera row sets them, with
  // our natives behind it.
  const auto choice_row = [&](const xerenge::settings::Row& row, int tab, bool first) {
    const uint32_t start = code.Start();
    set("m_focusDirs", c.String("Up/Down"));
    set("m_choiceFunc", c.String("XerengeGet"));
    set("m_choiceFuncParam", c.Int(row.id));
    set("m_label", c.String(row.label));
    std::vector<uint32_t> options, funcs, params;
    for (size_t i = 0; i < row.options.size(); ++i) {
      options.push_back(c.String(row.options[i]));
      funcs.push_back(c.String("XerengeSet"));
      params.push_back(c.Int(row.id * 16 + int32_t(i)));
    }
    set_array("m_options", options);
    set_array("m_serviceFuncs", funcs);
    set_array("m_serviceFuncParams", params);
    set("m_serviceName", c.String("DriverDetailsSettings"));
    set("m_showIcon", c.Bool(false));
    common(tab, first);
    code.Op(0x00);
    code.Finish();
    return start;
  };
  // A button's fields, as the Driver Details menu's set them; with an id, A on
  // it hands the id to SelectedOption.
  const auto button_row = [&](const std::string& label, uint32_t id, int tab, bool first) {
    const uint32_t start = code.Start();
    set("m_focusDirs", c.String("Up/Down"));
    set("m_label", c.String(label));
    common(tab, first);
    if (id) {
      set("xid", c.Int(int32_t(id)));
      code.Push({c.String("xerengeSelect")});
      define_handler();
      code.Op(0x1D);
      for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
        // this.setOnInput(xerengeSelect, INPUTCODE.<input>)
        code.Push({c.String("xerengeSelect")});
        code.Op(0x1C);
        code.Push({c.String("INPUTCODE")});
        code.Op(0x1C);
        code.Push({c.String(input)});
        code.Op(0x4E);
        code.Push({c.Int(2)});
        code.Op(0x56);  // PushThis
        code.Push({c.String("setOnInput")});
        code.Op(0x52);
        code.Op(0x17);
      }
    }
    code.Op(0x00);
    code.Finish();
    return start;
  };
  std::vector<uint32_t> new_items;
  std::vector<std::pair<std::string, uint32_t>> buttons;  // placed buttons and their ids (0: none)
  uint32_t depth = 80;
  // A placement of ours: the template's, at its own depth, place and name, and
  // the given code run when it loads.
  const auto place = [&](uint32_t character, const std::string& name, float x, float y, uint32_t on_load) {
    b.Align(4);
    const uint32_t events = b.Here();
    b.Put32(1);
    b.Put32(events + 8);
    b.Put32(event_flags);
    b.Put32(0);
    b.Put32(on_load);
    b.Align(4);
    const uint32_t at = b.Here();
    const std::vector<uint8_t> templ(b.bytes.begin() + speaker, b.bytes.begin() + speaker + 64);
    b.bytes.insert(b.bytes.end(), templ.begin(), templ.end());
    b.Set32(at + 8, depth);
    depth += 10;
    b.Set32(at + 12, character);
    b.Set32(at + 16, FloatBits(1.0f));
    b.Set32(at + 20, 0);
    b.Set32(at + 24, 0);
    b.Set32(at + 28, FloatBits(1.0f));
    b.Set32(at + 32, FloatBits(x));
    b.Set32(at + 36, FloatBits(y));
    b.Set32(at + 52, b.PutString(name));
    b.Set32(at + 60, events);
    new_items.push_back(at);
  };
  const uint32_t choice_character = b.U32(speaker + 12);  // MenuChoice

  const auto page = xerenge::settings::CurrentPage();
  const auto rows = xerenge::settings::Rows(page);
  std::vector<std::string> left_out;
  if (page == xerenge::settings::kMain) {
    // The game's rows (tabs 0-2), then ours: Language, and the buttons. Speaker
    // Config, which the game hides on an Xbox, goes: Language takes its place.
    left_out.push_back("speaker_btn");
    float y = kMainFirstY;
    int tab = 3;  // Speaker Config's, then on: the focus manager wants them without gaps
    for (const auto& row : rows) {
      place(choice_character, "xerenge_row" + std::to_string(row.id), kChoiceX, y,
            choice_row(row, tab++, false));
      y += kMainStep;
    }
    for (const Button& button : kButtons) {
      buttons.push_back({button.name, kXerengeOptionBase + button.page});
      place(button_character, button.name, kButtonX, y,
            button_row(button.label, kXerengeOptionBase + button.page, tab++, false));
      y += kMainStep;
    }
  } else {
    // The page's rows in place of the game's.
    left_out.assign(std::begin(kGameRows), std::end(kGameRows));
    float y = kPageFirstY;
    int tab = 0;
    for (const auto& row : rows) {
      const std::string name = "xerenge_row" + std::to_string(tab);
      if (row.options.empty()) {
        buttons.push_back({name, 0});
        place(button_character, name, kButtonX, y, button_row(row.label, 0, tab, tab == 0));
      } else {
        place(choice_character, name, kChoiceX, y, choice_row(row, tab, tab == 0));
      }
      ++tab;
      y += kPageStep;
    }
    // The page's name under DRIVER DETAILS, in place of [SETTINGS].
    for (auto& entry : c.entries) {
      if (entry.type == 1 && entry.text == "$FEDRIVERDETSETTING") {
        entry.text = xerenge::settings::PageTitle(page);
      }
    }
  }

  // An action after the placements, once the buttons have set themselves up
  // (a control clears its inputs when it does, so what its onLoad bound is
  // gone): A on each button with an id to the handler; and, as a button gets
  // the focus, LEFT and RIGHT taken back from the focus manager - a choice
  // row binds them there for itself (MenuChoice's setInputs) and leaves them
  // bound when the focus moves on.
  if (!buttons.empty()) {
    const uint32_t action_code = code.Start();
    code.Push({c.String("xerengeSelect")});
    define_handler();
    code.Op(0x1D);
    code.Push({c.String("xerengeNoArrows")});
    const uint32_t body = code.BeginFunction(param_name);
    for (const char* input : {"LEFT", "RIGHT"}) {
      // _root.gFM.setOnInput(INPUTCODE.<input>, undefined, undefined)
      code.Push({c.Value(3, 0), c.Value(3, 0)});  // undefined twice, as the game's code pushes it
      code.Push({c.String("INPUTCODE")});
      code.Op(0x1C);
      code.Push({c.String(input)});
      code.Op(0x4E);
      code.Push({c.Int(3), c.String("_root")});
      code.Op(0x1C);
      code.Push({c.String("gFM")});
      code.Op(0x4E);
      code.Push({c.String("setOnInput")});
      code.Op(0x52);
      code.Op(0x17);
    }
    code.EndFunction(body);
    code.Op(0x1D);
    for (const auto& [name, id] : buttons) {
      if (id) {
        for (const char* input : {"MENUPAGEFORWARD", "MENUPAGEFORWARD2"}) {
          // name.setOnInput(INPUTCODE.<input>, xerengeSelect)
          code.Push({c.String("xerengeSelect")});
          code.Op(0x1C);
          code.Push({c.String("INPUTCODE")});
          code.Op(0x1C);
          code.Push({c.String(input)});
          code.Op(0x4E);
          code.Push({c.Int(2), c.String(name)});
          code.Op(0x1C);
          code.Push({c.String("setOnInput")});
          code.Op(0x52);
          code.Op(0x17);
        }
      }
      // name.setOnFocus(xerengeNoArrows)
      code.Push({c.String("xerengeNoArrows")});
      code.Op(0x1C);
      code.Push({c.Int(1), c.String(name)});
      code.Op(0x1C);
      code.Push({c.String("setOnFocus")});
      code.Op(0x52);
      code.Op(0x17);
    }
    code.Op(0x00);
    code.Finish();
    b.Align(4);
    const uint32_t action = b.Here();
    b.Put32(1);
    b.Put32(action_code);
    new_items.push_back(action);
  }

  // The frame's item list: the game's (less its rows on our pages), then ours.
  b.Align(4);
  const uint32_t new_item_list = b.Here();
  uint32_t kept = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    const uint32_t item = b.U32(item_list + i * 4);
    if (b.U32(item) == 3 && (b.U32(item + 4) & 0x20)) {
      const std::string name = b.Str(b.U32(item + 52));
      if (std::find(left_out.begin(), left_out.end(), name) != left_out.end()) {
        continue;
      }
    }
    b.Put32(item);
    ++kept;
  }
  for (uint32_t item : new_items) {
    b.Put32(item);
  }
  b.Set32(frame, kept + uint32_t(new_items.size()));
  b.Set32(frame + 4, new_item_list);
  b.Align(16);

  out_constants = c.Write();
  return true;
}

// The constant file's size: up to the end of its last string.
size_t ConstantsSize(const uint8_t* raw) {
  Blob header;
  header.bytes.assign(raw, raw + 32);
  const uint32_t count = header.U32(24);
  size_t end = 32 + size_t(count) * 8;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = raw + 32 + i * 8;
    const uint32_t type = uint32_t(entry[0]) << 24 | uint32_t(entry[1]) << 16 | uint32_t(entry[2]) << 8 | entry[3];
    if (type == 1) {
      const uint32_t at = uint32_t(entry[4]) << 24 | uint32_t(entry[5]) << 16 | uint32_t(entry[6]) << 8 | entry[7];
      end = std::max(end, at + std::strlen(reinterpret_cast<const char*>(raw + at)) + 1);
    }
  }
  return end;
}

uint32_t ToGuest(uint8_t* base, const Blob& blob) {
  const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(blob.bytes.size()), 0x10);
  std::memcpy(xerenge::GuestPointer(base, at), blob.bytes.data(), blob.bytes.size());
  return at;
}

}  // namespace

REX_HOOK_RAW(sub_824277A0) {
  const uint32_t data = ctx.r4.u32;
  const uint32_t constants = ctx.r5.u32;
  if (data && constants > data && constants - data <= 0x40000) {
    const uint8_t* raw_data = xerenge::GuestPointer(base, data);
    const size_t data_size = constants - data;
    if (IsSettingsMovie(raw_data, data_size)) {
      const uint8_t* raw_constants = xerenge::GuestPointer(base, constants);
      Blob new_data, new_constants;
      if (Rebuild(raw_data, data_size, raw_constants, ConstantsSize(raw_constants), new_data, new_constants)) {
        // Kept for as long as the movie may run: a screen's worth, not freed.
        ctx.r4.u64 = ToGuest(base, new_data);
        ctx.r5.u64 = ToGuest(base, new_constants);
        REXLOG_INFO("menu: settings movie rebuilt: data {:X} -> {:X} bytes, constants {:X} bytes, at {:08X}/{:08X}",
                    data_size, new_data.bytes.size(), new_constants.bytes.size(), ctx.r4.u32, ctx.r5.u32);
        // XERENGE_DUMP_MENU_MOVIE=<file>: the rebuilt blobs, data then constants, for tools/aptdump.
        if (const char* path = std::getenv("XERENGE_DUMP_MENU_MOVIE")) {
          if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(new_data.bytes.data(), 1, new_data.bytes.size(), file);
            std::fwrite(new_constants.bytes.data(), 1, new_constants.bytes.size(), file);
            std::fclose(file);
          }
        }
      }
    }
  }
  __imp__sub_824277A0(ctx, base);
}
