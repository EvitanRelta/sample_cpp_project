#include "key_utils.h"
#include <QKeySequence>
#include <mutex>
#include <stdexcept>
#include <string>

namespace fv::detail {

Key normalize_key(Key k) {
    Qt::KeyboardModifiers mods = k.modifiers();
    mods &= ~Qt::KeypadModifier;
    mods &= ~Qt::GroupSwitchModifier;

    const int code = k.code();
    const bool is_letter = code >= Qt::Key_A && code <= Qt::Key_Z;
    const bool is_printable = code >= 0x20 && code <= 0x7e;
    if (is_printable && !is_letter) mods &= ~Qt::ShiftModifier;

    return Key(mods, k.code());
}

Key parse_key(std::string_view text) {
    // Qt initializes PortableText's shared modifier list lazily, without synchronization.
    static std::mutex parse_mutex;
    std::lock_guard<std::mutex> lock(parse_mutex);
    const QKeySequence seq = QKeySequence::fromString(
        QString::fromUtf8(text.data(), static_cast<int>(text.size())), QKeySequence::PortableText);
    if (seq.count() != 1 || Key(seq[0]).code() == Qt::Key_unknown)
        throw std::invalid_argument("fv::Viewer::bind: can't parse key \"" + std::string(text) +
                                    "\"");
    return normalize_key(Key(seq[0]));
}

QString key_name(Key k) {
    return QKeySequence(k.combination()).toString(QKeySequence::NativeText);
}

bool is_modifier_only(Qt::Key k) {
    switch (k) {
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_Super_L:
        case Qt::Key_Super_R:
        case Qt::Key_Hyper_L:
        case Qt::Key_Hyper_R:
        case Qt::Key_CapsLock:
        case Qt::Key_NumLock:
        case Qt::Key_ScrollLock:
            return true;
        default:
            return false;
    }
}

}  // namespace fv::detail
