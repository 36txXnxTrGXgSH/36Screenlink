#include "input.h"

namespace {

constexpr UINT32 kMaxContacts = 10;

POINTER_TYPE_INFO makeContact(uint8_t id, POINT p, POINTER_FLAGS flags) {
    POINTER_TYPE_INFO info{};
    info.type = PT_TOUCH;
    auto& t = info.touchInfo;
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.pointerId = id;
    t.pointerInfo.ptPixelLocation = p;
    t.pointerInfo.pointerFlags = flags;
    t.touchFlags = TOUCH_FLAG_NONE;
    t.touchMask = TOUCH_MASK_CONTACTAREA;
    t.rcContact = {p.x - 2, p.y - 2, p.x + 2, p.y + 2};
    return info;
}

} // namespace

TouchInjector::~TouchInjector() {
    releaseAll();
    if (device_) DestroySyntheticPointerDevice(device_);
}

bool TouchInjector::ensureDevice() {
    if (!device_) device_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxContacts, POINTER_FEEDBACK_DEFAULT);
    return device_ != nullptr;
}

void TouchInjector::apply(const std::vector<Contact>& contacts) {
    ++received_;
    if (!enabled_ || !ensureDevice()) return;

    const LONG w = target_.right - target_.left, h = target_.bottom - target_.top;
    std::vector<POINTER_TYPE_INFO> batch;
    std::map<uint8_t, POINT> next;

    for (const Contact& c : contacts) {
        const POINT p{target_.left + MulDiv(c.x, w - 1, 65535), target_.top + MulDiv(c.y, h - 1, 65535)};
        const bool known = active_.count(c.id) != 0;
        if (c.down) {
            const POINTER_FLAGS phase = known ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN;
            batch.push_back(makeContact(c.id, p, phase | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT));
            next[c.id] = p;
        } else if (known) {
            batch.push_back(makeContact(c.id, p, POINTER_FLAG_UP));
        }
    }
    // Un contact absent du message est un contact relâché, par exemple après une perte de focus.
    for (const auto& [id, p] : active_) {
        bool listed = false;
        for (const Contact& c : contacts) listed = listed || c.id == id;
        if (!listed) batch.push_back(makeContact(id, p, POINTER_FLAG_UP));
    }
    active_ = std::move(next);
    if (!batch.empty()) InjectSyntheticPointerInput(device_, batch.data(), static_cast<UINT32>(batch.size()));
}

void TouchInjector::releaseAll() {
    if (active_.empty()) return;
    apply({});
}

PenInjector::~PenInjector() {
    // Stylet posé au moment de la coupure : relâché, comme une touche.
    if (contact_ || inRange_) apply({0, 0, 0, 0, 0, 0, 0});
    if (device_) DestroySyntheticPointerDevice(device_);
}

void PenInjector::apply(const PenSample& s) {
    ++received_;
    if (!enabled_) return;
    if (!device_) device_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    if (!device_) return;

    const bool inRange = s.state & 0x01, contact = s.state & 0x02;
    if (!inRange && !inRange_) return;

    const LONG w = target_.right - target_.left, h = target_.bottom - target_.top;
    // Sortie de portée : pas de position fiable, on garde la dernière.
    const POINT p = inRange ? POINT{target_.left + MulDiv(s.x, w - 1, 65535), target_.top + MulDiv(s.y, h - 1, 65535)} : last_;

    POINTER_FLAGS flags;
    if (contact) flags = (contact_ ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN) | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    else if (contact_) flags = POINTER_FLAG_UP | POINTER_FLAG_INRANGE;
    else flags = POINTER_FLAG_UPDATE | (inRange ? POINTER_FLAG_INRANGE : POINTER_FLAG_NONE);

    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    auto& pen = info.penInfo;
    pen.pointerInfo.pointerType = PT_PEN;
    pen.pointerInfo.pointerId = 1;
    pen.pointerInfo.ptPixelLocation = p;
    pen.pointerInfo.pointerFlags = flags;
    pen.penMask = PEN_MASK_PRESSURE | PEN_MASK_ROTATION | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    pen.penFlags = ((s.state & 0x04) ? PEN_FLAG_BARREL : 0) | ((s.state & 0x08) ? PEN_FLAG_ERASER : 0);
    pen.pressure = contact ? s.pressure : 0;
    pen.rotation = s.rotation;
    pen.tiltX = s.tiltX;
    pen.tiltY = s.tiltY;
    InjectSyntheticPointerInput(device_, &info, 1);

    inRange_ = inRange;
    contact_ = contact;
    last_ = p;
}

KeyInjector::~KeyInjector() {
    // Lien coupé touche enfoncée : sans ce relâchement, Ctrl resterait bloqué sous Windows.
    for (const uint16_t code : held_) send(code, false);
}

void KeyInjector::send(uint16_t code, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(code & 0xFF);
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (code & 0x100) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

void KeyInjector::key(bool down, uint16_t code) {
    ++received_;
    if (!enabled_) return;
    if (down) held_.insert(code);
    else held_.erase(code);
    send(code, down);
}

void KeyInjector::text(const std::string& utf8) {
    ++received_;
    if (!enabled_ || utf8.empty()) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n);

    std::vector<INPUT> inputs;
    for (const wchar_t c : wide) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = c;
        in.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(in);
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(in);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}
