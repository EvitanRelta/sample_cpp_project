/** Small helpers to make key handling predictable. */
#pragma once

#include <QString>
#include <string_view>
#include "fv/viewer.h"

namespace fv::detail {

/**
 * Make a key event match how people write bindings:
 * - drop the keypad flag, so numpad '+' == main-keyboard '+'
 * - drop Shift on symbol keys, so '+' (typed as Shift+'=' on US layouts) matches Qt::Key_Plus
 *
 * Letters keep Shift, so "Shift+A" can still be bound separately from "A".
 */
Key normalize_key(Key k);

/**
 * Parse "Ctrl+S" into Ctrl+S. Throws std::invalid_argument if the text isn't a single key
 * combination.
 */
Key parse_key(std::string_view text);

/** Return a human-readable name, e.g. "Ctrl+S". */
QString key_name(Key k);

bool is_modifier_only(Qt::Key k);

}  // namespace fv::detail
