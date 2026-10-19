// aptdump - the menus' Apt movies in a Burnout Revenge package (fe/FeMain.bin
// and the like), as text: each movie's characters, frames, placements and
// ActionScript, with the constants resolved.
//
//   aptdump <package> [name filter]
//
// The package holds the movies as pairs: an "Apt Data:7" blob (the movie) and
// an "Apt constant file" (its constants, and where the movie's root character
// lies in the data). Both big-endian, every offset relative to its own blob.
// The layout is EA's Apt as OpenSAGE reads it for C&C 3 (little-endian there):
//   character   type, 0x09876543, then by type (Movie 9, Sprite 5, Button 4,
//               Text 2, Shape 1, Image 7, Font 3)
//   list        count, offset (of the items, or of pointers to them)
//   frame item  type: 1 action, 2 label, 3 place, 4 remove, 5 background, 8 init action
//   actions     SWF's ActionScript 2 bytecode plus EA's opcodes; the operands of
//               some are aligned to 4 and point elsewhere in the blob.
// The movie's name is not in it; the filter matches any string the movie
// holds (e.g. DriverDetailsSettings).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Blob {
  const uint8_t* data = nullptr;
  size_t size = 0;

  bool Has(size_t offset, size_t length) const { return offset <= size && length <= size - offset; }
  uint8_t U8(size_t offset) const { return Has(offset, 1) ? data[offset] : 0; }
  uint16_t U16(size_t offset) const {
    return Has(offset, 2) ? uint16_t(data[offset] << 8 | data[offset + 1]) : 0;
  }
  uint32_t U32(size_t offset) const {
    return Has(offset, 4) ? uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 |
                                uint32_t(data[offset + 2]) << 8 | data[offset + 3]
                          : 0;
  }
  float F32(size_t offset) const {
    const uint32_t bits = U32(offset);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
  std::string Str(size_t offset) const {
    std::string text;
    while (offset < size && data[offset] && text.size() < 256) {
      text += char(data[offset++]);
    }
    return text;
  }
};

struct Constant {
  uint32_t type = 0;
  uint32_t value = 0;
  std::string text;
};

struct Movie {
  Blob apt;
  Blob constants;
  size_t apt_offset = 0;  // in the package
  size_t constants_offset = 0;
  uint32_t root = 0;
  std::vector<Constant> entries;
};

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

std::string ConstantText(const Movie& movie, uint32_t index, const std::vector<uint32_t>* pool = nullptr) {
  if (index >= movie.entries.size()) {
    return "const#" + std::to_string(index) + "?";
  }
  const Constant& c = movie.entries[index];
  switch (c.type) {
    case 1: return Quote(c.text);
    case 2: return "property " + std::to_string(c.value);
    case 3: return "none";
    case 4: return "r" + std::to_string(c.value);
    case 5: return c.value ? "true" : "false";
    case 6: {
      float f;
      std::memcpy(&f, &c.value, 4);
      return std::to_string(f);
    }
    case 7: return std::to_string(int32_t(c.value));
    case 8:  // an entry of the ConstantPool in effect
      if (pool && c.value < pool->size() && (*pool)[c.value] != index) {
        return ConstantText(movie, (*pool)[c.value]);
      }
      return "lookup " + std::to_string(c.value);
    default: return "type" + std::to_string(c.type) + ":" + std::to_string(c.value);
  }
}

const char* OpName(uint8_t op) {
  static const std::map<uint8_t, const char*> names = {
      {0x00, "End"}, {0x04, "NextFrame"}, {0x05, "PrevFrame"}, {0x06, "Play"}, {0x07, "Stop"},
      {0x0A, "Add"}, {0x0B, "Subtract"}, {0x0C, "Multiply"}, {0x0D, "Divide"}, {0x0E, "Equals"},
      {0x0F, "LessThan"}, {0x10, "And"}, {0x11, "Or"}, {0x12, "Not"}, {0x13, "StringEquals"},
      {0x14, "StringLength"}, {0x15, "SubString"}, {0x17, "Pop"}, {0x18, "ToInteger"},
      {0x1C, "GetVariable"}, {0x1D, "SetVariable"}, {0x20, "SetTarget2"}, {0x21, "StringConcat"},
      {0x22, "GetProperty"}, {0x23, "SetProperty"}, {0x24, "CloneSprite"}, {0x25, "RemoveSprite"},
      {0x26, "Trace"}, {0x29, "StringCompare"}, {0x2A, "Throw"}, {0x2B, "CastOp"},
      {0x2C, "ImplementsOp"}, {0x30, "Random"}, {0x34, "GetTimer"}, {0x3A, "Delete"},
      {0x3B, "Delete2"}, {0x3C, "DefineLocal"}, {0x3D, "CallFunction"}, {0x3E, "Return"},
      {0x3F, "Modulo"}, {0x40, "NewObject"}, {0x41, "Var"}, {0x42, "InitArray"},
      {0x43, "InitObject"}, {0x44, "TypeOf"}, {0x45, "TargetPath"}, {0x46, "Enumerate"},
      {0x47, "Add2"}, {0x48, "LessThan2"}, {0x49, "Equals2"}, {0x4A, "ToNumber"},
      {0x4B, "ToString"}, {0x4C, "PushDuplicate"}, {0x4D, "StackSwap"}, {0x4E, "GetMember"},
      {0x4F, "SetMember"}, {0x50, "Increment"}, {0x51, "Decrement"}, {0x52, "CallMethod"},
      {0x53, "NewMethod"}, {0x54, "InstanceOf"}, {0x55, "Enumerate2"}, {0x56, "PushThis"},
      {0x58, "PushGlobal"}, {0x59, "PushZero"}, {0x5A, "PushOne"}, {0x5B, "CallFuncPop"},
      {0x5C, "CallFunc"}, {0x5D, "CallMethodPop"}, {0x5E, "CallMethod(EA)"}, {0x60, "BitwiseAnd"},
      {0x61, "BitwiseOr"}, {0x62, "BitwiseXOr"}, {0x63, "ShiftLeft"}, {0x64, "ShiftRight"},
      {0x65, "ShiftRight2"}, {0x66, "StrictEqual"}, {0x67, "Greater"}, {0x68, "StringGreater"},
      {0x69, "Extends"}, {0x70, "PushThisVar"}, {0x71, "PushGlobalVar"}, {0x72, "ZeroVar"},
      {0x73, "PushTrue"}, {0x74, "PushFalse"}, {0x75, "PushNull"}, {0x76, "PushUndefined"},
      {0x77, "TraceStart"}, {0x81, "GotoFrame"}, {0x83, "GetURL"}, {0x87, "SetRegister"},
      {0x88, "ConstantPool"}, {0x8A, "WaitForFrame"}, {0x8B, "SetTarget"}, {0x8C, "GotoLabel"},
      {0x8D, "WaitForFrameExpr"}, {0x8E, "DefineFunction2"}, {0x8F, "Try"}, {0x94, "With"},
      {0x96, "PushData"}, {0x99, "BranchAlways"}, {0x9A, "GetURL2"}, {0x9B, "DefineFunction"},
      {0x9D, "BranchIfTrue"}, {0x9E, "CallFrame"}, {0x9F, "GotoFrame2"}, {0xA1, "PushString"},
      {0xA2, "PushConstantByte"}, {0xA3, "PushConstantWord"}, {0xA4, "GetStringVar"},
      {0xA5, "GetStringMember"}, {0xA6, "SetStringVar"}, {0xA7, "SetStringMember"},
      {0xAE, "PushValueOfVar"}, {0xAF, "GetNamedMember"}, {0xB0, "CallNamedFuncPop"},
      {0xB1, "CallNamedFunc"}, {0xB2, "CallNamedMethodPop"}, {0xB3, "CallNamedMethod"},
      {0xB4, "PushFloat"}, {0xB5, "PushByte"}, {0xB6, "PushShort"}, {0xB7, "PushLong"},
      {0xB8, "BranchIfFalse"}, {0xB9, "PushRegister"}, {0xFF, "Padding"}};
  const auto found = names.find(op);
  return found == names.end() ? nullptr : found->second;
}

bool Aligned(uint8_t op) {
  switch (op) {
    case 0x9B: case 0x8E: case 0x88: case 0x9D: case 0x99: case 0x96: case 0x83: case 0x8C:
    case 0x87: case 0x8B: case 0x81: case 0x9F: case 0x94: case 0xA1: case 0xB8: case 0xA4:
    case 0xA6: case 0xA5: case 0xA7:
      return true;
  }
  return false;
}

// One block of bytecode, from start to its End (or past the furthest branch).
void Disassemble(const Movie& movie, uint32_t start, const std::string& indent, std::set<uint32_t>& seen) {
  if (!seen.insert(start).second) {
    std::printf("%s(code at %06X, shown above)\n", indent.c_str(), start);
    return;
  }
  const Blob& b = movie.apt;
  std::vector<uint32_t> pool;
  size_t pc = start;
  size_t furthest = start;
  int guard = 0;
  while (pc < b.size && ++guard < 100000) {
    const size_t at = pc;
    const uint8_t op = b.U8(pc++);
    const char* name = OpName(op);
    std::string text = name ? name : "op" + std::to_string(op) + "?";
    if (name && Aligned(op)) {
      pc = (pc + 3) & ~size_t(3);
    }
    std::string args;
    switch (op) {
      case 0x88: {  // ConstantPool: indices into the constant file
        const uint32_t count = b.U32(pc), list = b.U32(pc + 4);
        pc += 8;
        pool.clear();
        for (uint32_t i = 0; i < count; ++i) {
          pool.push_back(b.U32(list + i * 4));
        }
        args = std::to_string(count) + " constant(s) from #" + (count ? std::to_string(pool[0]) : "-");
        break;
      }
      case 0x96: {  // PushData: constant indices
        const uint32_t count = b.U32(pc), list = b.U32(pc + 4);
        pc += 8;
        for (uint32_t i = 0; i < count; ++i) {
          args += (i ? ", " : "") + ConstantText(movie, b.U32(list + i * 4), &pool);
        }
        break;
      }
      case 0xA2: case 0xAE: case 0xAF: case 0xB0: case 0xB1: case 0xB2: case 0xB3: {
        const uint8_t index = b.U8(pc++);
        args = index < pool.size() ? ConstantText(movie, pool[index]) : "pool#" + std::to_string(index) + "?";
        break;
      }
      case 0xA3: {
        const uint16_t index = b.U16(pc);
        pc += 2;
        args = index < pool.size() ? ConstantText(movie, pool[index]) : "pool#" + std::to_string(index) + "?";
        break;
      }
      case 0xA1: case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0x8C:
        args = Quote(b.Str(b.U32(pc)));
        pc += 4;
        break;
      case 0x83:
        args = Quote(b.Str(b.U32(pc))) + ", " + Quote(b.Str(b.U32(pc + 4)));
        pc += 8;
        break;
      case 0x81: case 0x87: case 0x9F: case 0x8B: case 0x94:
        args = std::to_string(int32_t(b.U32(pc)));
        pc += 4;
        break;
      case 0x99: case 0x9D: case 0xB8: {
        const int32_t offset = int32_t(b.U32(pc));
        pc += 4;
        const size_t target = pc + offset;
        furthest = std::max(furthest, target);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "-> %06zX", target);
        args = buf;
        break;
      }
      case 0xB4: args = std::to_string(b.F32(pc)); pc += 4; break;
      case 0xB5: case 0xB9: args = std::to_string(b.U8(pc)); pc += 1; break;
      case 0xB6: args = std::to_string(b.U16(pc)); pc += 2; break;
      case 0xB7: args = std::to_string(int32_t(b.U32(pc))); pc += 4; break;
      case 0x9B: case 0x8E: {  // DefineFunction(2): name, parameters, body size
        const std::string fname = b.Str(b.U32(pc));
        std::string params;
        uint32_t body;
        if (op == 0x9B) {
          const uint32_t count = b.U32(pc + 4), list = b.U32(pc + 8);
          for (uint32_t i = 0; i < count; ++i) {
            params += (i ? ", " : "") + b.Str(b.U32(list + i * 4));
          }
          body = b.U32(pc + 12);
          pc += 16 + 8;
        } else {
          const uint32_t count = b.U32(pc + 4);
          const uint32_t list = b.U32(pc + 12);
          for (uint32_t i = 0; i < count; ++i) {
            params += (i ? ", " : "") + std::string("r") + std::to_string(b.U32(list + i * 8)) + "=" +
                      b.Str(b.U32(list + i * 8 + 4));
          }
          body = b.U32(pc + 16);
          pc += 20 + 8;
        }
        args = Quote(fname) + " (" + params + ") body " + std::to_string(body) + " byte(s)";
        break;
      }
      default:
        break;
    }
    std::printf("%s%06zX  %-18s %s\n", indent.c_str(), at, text.c_str(), args.c_str());
    if (!name) {
      std::printf("%s(unknown opcode - stopped)\n", indent.c_str());
      return;
    }
    if (op == 0x00 && pc > furthest) {
      return;
    }
  }
}

void DumpFrames(const Movie& movie, uint32_t count, uint32_t list, const std::string& indent,
                std::set<uint32_t>& seen) {
  const Blob& b = movie.apt;
  for (uint32_t f = 0; f < count; ++f) {
    // A frame: its items, a list of pointers.
    const uint32_t frame = list + f * 8;
    const uint32_t items = b.U32(frame), item_list = b.U32(frame + 4);
    std::printf("%sframe %u (%u item(s))\n", indent.c_str(), f, items);
    for (uint32_t i = 0; i < items; ++i) {
      const uint32_t item = b.U32(item_list + i * 4);
      const uint32_t type = b.U32(item);
      const std::string in = indent + "  ";
      switch (type) {
        case 1:
          std::printf("%saction:\n", in.c_str());
          Disassemble(movie, b.U32(item + 4), in + "  ", seen);
          break;
        case 2:
          std::printf("%slabel %s\n", in.c_str(), Quote(b.Str(b.U32(item + 4))).c_str());
          break;
        case 3: {
          const uint32_t flags = b.U32(item + 4) & 0xFF;
          const int32_t depth = int32_t(b.U32(item + 8));
          const int32_t character = int32_t(b.U32(item + 12));
          const float a = b.F32(item + 16), bb = b.F32(item + 20), c = b.F32(item + 24), d = b.F32(item + 28);
          const float x = b.F32(item + 32), y = b.F32(item + 36);
          const std::string name = (flags & 0x20) ? b.Str(b.U32(item + 52)) : "";
          std::printf("%splace depth %d character %d%s%s flags %02X matrix [%g %g %g %g] at (%g, %g)\n",
                      in.c_str(), depth, (flags & 2) ? character : -1, name.empty() ? "" : " name ",
                      name.empty() ? "" : Quote(name).c_str(), flags, a, bb, c, d, x, y);
          if (flags & 0x80) {
            const uint32_t events = b.U32(item + 60);
            if (events) {
              const uint32_t n = b.U32(events), elist = b.U32(events + 4);
              for (uint32_t e = 0; e < n; ++e) {
                const uint32_t ev = elist + e * 12;
                std::printf("%s  clip event %08X:\n", in.c_str(), b.U32(ev));
                Disassemble(movie, b.U32(ev + 8), in + "    ", seen);
              }
            }
          }
          break;
        }
        case 4:
          std::printf("%sremove depth %d\n", in.c_str(), int32_t(b.U32(item + 4)));
          break;
        case 5:
          std::printf("%sbackground %08X\n", in.c_str(), b.U32(item + 4));
          break;
        case 8:
          std::printf("%sinit action for sprite %u:\n", in.c_str(), b.U32(item + 4));
          Disassemble(movie, b.U32(item + 8), in + "  ", seen);
          break;
        default:
          std::printf("%sitem type %u at %06X\n", in.c_str(), type, item);
      }
    }
  }
}

void DumpMovie(const Movie& movie) {
  const Blob& b = movie.apt;
  const uint32_t root = movie.root;
  std::printf("== movie: data at %zX, constants at %zX (%zu entries), root at %X\n", movie.apt_offset,
              movie.constants_offset, movie.entries.size(), root);
  if (b.U32(root) != 9 || b.U32(root + 4) != 0x09876543) {
    std::printf("   root is not a movie character (%08X %08X)\n", b.U32(root), b.U32(root + 4));
    return;
  }
  const uint32_t frame_count = b.U32(root + 8), frame_list = b.U32(root + 12);
  const uint32_t char_count = b.U32(root + 20), char_list = b.U32(root + 24);
  std::printf("   %ux%u, %u ms a frame, %u frame(s), %u character(s)\n", b.U32(root + 28), b.U32(root + 32),
              b.U32(root + 36), frame_count, char_count);
  const uint32_t imports = b.U32(root + 40), import_list = b.U32(root + 44);
  for (uint32_t i = 0; i < imports; ++i) {
    const uint32_t im = import_list + i * 16;
    std::printf("   import %s from %s as character %u\n", Quote(b.Str(b.U32(im + 4))).c_str(),
                Quote(b.Str(b.U32(im))).c_str(), b.U32(im + 8));
  }
  const uint32_t exports = b.U32(root + 48), export_list = b.U32(root + 52);
  for (uint32_t i = 0; i < exports; ++i) {
    const uint32_t ex = export_list + i * 8;
    std::printf("   export %s = character %u\n", Quote(b.Str(b.U32(ex))).c_str(), b.U32(ex + 4));
  }
  std::set<uint32_t> seen;
  for (uint32_t i = 1; i < char_count; ++i) {
    const uint32_t ch = b.U32(char_list + i * 4);
    if (!ch) {
      continue;
    }
    const uint32_t type = b.U32(ch);
    switch (type) {
      case 5:
        std::printf("   character %u: sprite, %u frame(s)\n", i, b.U32(ch + 8));
        DumpFrames(movie, b.U32(ch + 8), b.U32(ch + 12), "     ", seen);
        break;
      case 2:
        std::printf("   character %u: text %s value %s\n", i, Quote(b.Str(b.U32(ch + 8 + 52))).c_str(),
                    Quote(b.Str(b.U32(ch + 8 + 56))).c_str());
        break;
      case 4:
        std::printf("   character %u: button\n", i);
        break;
      default:
        std::printf("   character %u: type %u\n", i, type);
    }
  }
  std::printf("   the movie's own frames:\n");
  DumpFrames(movie, frame_count, frame_list, "     ", seen);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: aptdump <package> [string the movie holds]\n");
    return 2;
  }
  std::ifstream file(argv[1], std::ios::binary);
  const std::vector<uint8_t> package((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (package.empty()) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const std::string filter = argc > 2 ? argv[2] : "";
  const auto find = [&](const char* magic, size_t from) {
    const size_t length = std::strlen(magic);
    for (size_t i = from; i + length <= package.size(); ++i) {
      if (std::memcmp(&package[i], magic, length) == 0) {
        return i;
      }
    }
    return package.size();
  };
  size_t at = 0;
  int count = 0;
  while ((at = find("Apt Data:7", at)) < package.size()) {
    const size_t constants = find("Apt constant file", at);
    if (constants >= package.size()) {
      break;
    }
    const size_t next = find("Apt Data:7", at + 1);
    Movie movie;
    movie.apt_offset = at;
    movie.constants_offset = constants;
    movie.apt = {&package[at], constants - at};
    movie.constants = {&package[constants], std::min(next, package.size()) - constants};
    const Blob& c = movie.constants;
    movie.root = c.U32(20);
    const uint32_t entries = c.U32(24);
    for (uint32_t i = 0; i < entries; ++i) {
      Constant k{c.U32(32 + i * 8), c.U32(32 + i * 8 + 4), {}};
      if (k.type == 1) {
        k.text = c.Str(k.value);
      }
      movie.entries.push_back(std::move(k));
    }
    bool match = filter.empty();
    for (const auto& k : movie.entries) {
      match = match || k.text == filter;
    }
    if (match) {
      DumpMovie(movie);
      ++count;
    }
    at = constants;
  }
  std::fprintf(stderr, "%d movie(s)\n", count);
  return 0;
}
