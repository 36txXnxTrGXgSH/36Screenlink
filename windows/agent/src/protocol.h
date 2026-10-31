// Protocole v1, voir docs/02-protocole.md. Toute modification se reporte dans
// android/app/src/main/java/com/screenlink36/app/Protocol.kt.
#pragma once
#include <cstdint>
#include <vector>

namespace proto {

inline constexpr uint16_t kPort = 7236;
inline constexpr uint8_t kVersion = 3;
inline constexpr char kMagic[4] = {'S', '3', '6', 'X'};
inline constexpr uint32_t kMaxRecord = 8 * 1024 * 1024;

enum HandshakeMode : uint8_t { kModeSession = 0, kModePair = 1 };
enum HandshakeStatus : uint8_t { kStatusOk = 0, kStatusNotPaired = 1, kStatusBadVersion = 2, kStatusPairRefused = 3 };

enum Msg : uint8_t {
    kHello = 1,        // T->A u16 w, u16 h, u16 maxFps, u8 codecs (bit0 avc, bit1 hevc)
    kStreamConfig = 2, // A->T u8 codec, u16 w, u16 h, u16 fps, u32 bitrate, u16 rotation (degrés)
    kVideo = 3,        // A->T u8 flags, u64 ptsUs, bytes
    kTouch = 4,        // T->A u8 n, n * (u8 id, u8 down, u16 x, u16 y) ; x,y sur 0..65535
    kPing = 5,         // A->T u64 t0
    kPong = 6,         // T->A u64 t0, u64 tabletUs
    kStats = 7,        // T->A u32 frames, avgLatUs, p95LatUs, avgDecUs, drops
    kRequestIdr = 8,   // T->A vide
    kCursorPos = 9,    // A->T u8 visible, i16 x, i16 y
    kCursorShape = 10, // A->T u16 w, h, hotX, hotY, BGRA
    kClock = 11,       // A->T i64 offsetUs (tablette - agent)
    kKey = 12,         // T->A u8 down, u16 code (scancode set 1, 0x100 = préfixe E0)
    kText = 13,        // T->A texte UTF-8 du clavier à l'écran
    kPen = 14,         // T->A u8 état, u16 x, u16 y, u16 pression 0..1024, i8 tiltX, i8 tiltY, u16 rotation
    kAudio = 15,       // A->T u32 fréquence, PCM stéréo 16 bits little-endian
    kAudioRoute = 16,  // T->A u8 sortie demandée (0 PC, 1 tablette)
    kAudioState = 17,  // A->T u8 sortie active (0 PC, 1 tablette)
};

enum PenState : uint8_t { kPenInRange = 1, kPenContact = 2, kPenBarrel = 4, kPenEraser = 8 };

enum Codec : uint8_t { kCodecAvc = 1, kCodecHevc = 2 };
enum VideoFlags : uint8_t { kFlagConfig = 1, kFlagKey = 2 };

// Sérialisation big-endian, identique à java.nio.ByteBuffer côté Android.
class Writer {
public:
    explicit Writer(uint8_t type) { buf_.push_back(type); }
    Writer& u8(uint8_t v) { buf_.push_back(v); return *this; }
    Writer& u16(uint16_t v) { return be(v, 2); }
    Writer& u32(uint32_t v) { return be(v, 4); }
    Writer& u64(uint64_t v) { return be(v, 8); }
    Writer& bytes(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
        return *this;
    }
    const std::vector<uint8_t>& data() const { return buf_; }

private:
    Writer& be(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) buf_.push_back(static_cast<uint8_t>(v >> (i * 8)));
        return *this;
    }
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool has(size_t n) const { return static_cast<size_t>(end_ - p_) >= n; }
    uint8_t u8() { return static_cast<uint8_t>(be(1)); }
    uint16_t u16() { return static_cast<uint16_t>(be(2)); }
    uint32_t u32() { return static_cast<uint32_t>(be(4)); }
    uint64_t u64() { return be(8); }

private:
    uint64_t be(int n) {
        uint64_t v = 0;
        for (int i = 0; i < n && p_ < end_; ++i) v = (v << 8) | *p_++;
        return v;
    }
    const uint8_t* p_;
    const uint8_t* end_;
};

} // namespace proto
