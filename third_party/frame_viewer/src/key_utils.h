// key_utils.h — small helpers to make key handling predictable.
#pragma once

#include "fv/viewer.h"

#include <QString>

#include <string_view>

namespace fv::detail {

// Makes a key event match how people write bindings:
// - drops the keypad flag, so numpad '+' == main-keyboard '+'
// - drops Shift on symbol keys, so '+' (typed as Shift+'=' on US layouts) matches Qt::Key_Plus
// Letters keep Shift, so "Shift+A" can still be bound separately from "A".
Key normalize_key(Key k);

// "Ctrl+S" -> Ctrl+S. Throws std::invalid_argument if the text isn't a single key combination.
Key parse_key(std::string_view text);

// Human-readable name, e.g. "Ctrl+S".
QString key_name(Key k);

bool is_modifier_only(Qt::Key k);

} // namespace fv::detail
