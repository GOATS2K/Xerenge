#pragma once

#include <cstdint>

namespace xerenge::music {

// The music folder's songs chosen in Song Manager, in place of EA Trax.
bool Custom();

// The folder looked at again (Song Manager opening): songs added or taken away
// since are in the list.
void Rescan(uint8_t* base);

}  // namespace xerenge::music
