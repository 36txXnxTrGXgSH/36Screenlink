// Lecteur à faible latence pour la télévision : reçoit de JavaScript des images H.264 (Annex-B)
// déjà déchiffrées, et les donne directement au décodeur matériel via Elementary Media Stream
// Source (mode faible latence). JavaScript garde l'appairage, le chiffrement et le réseau.
//
// AppendPacket classique est interdit sur le fil principal ; on utilise AppendPacketAsync, qui
// lui est autorisé sur le fil principal. Pas de thread : le moteur WebAssembly de ce téléviseur
// (Chromium 69) refuse les modules multi-thread.
#include <emscripten.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <samsung/html/html_media_element.h>
#include <samsung/html/html_media_element_listener.h>
#include <samsung/wasm/elementary_media_packet.h>
#include <samsung/wasm/elementary_media_track_config.h>
#include <samsung/wasm/elementary_video_track_config.h>
#include <samsung/wasm/elementary_media_stream_source.h>
#include <samsung/wasm/elementary_media_stream_source_listener.h>
#include <samsung/wasm/elementary_media_track.h>
#include <samsung/wasm/elementary_media_track_listener.h>

using samsung::wasm::ElementaryMediaPacket;
using samsung::wasm::ElementaryMediaStreamSource;
using samsung::wasm::ElementaryMediaStreamSourceListener;
using samsung::wasm::ElementaryMediaTrack;
using samsung::wasm::ElementaryMediaTrackListener;
using samsung::wasm::ElementaryVideoTrackConfig;
using samsung::wasm::EmssLatencyMode;
using samsung::wasm::EmssRenderingMode;
using samsung::wasm::OperationResult;
using samsung::wasm::SessionId;
using samsung::wasm::Seconds;
using samsung::html::HTMLMediaElement;
using samsung::html::HTMLMediaElementListener;

EM_JS(void, js_log, (const char* s), {
  var msg = '[wasm] ' + UTF8ToString(s);
  try { if (window.__tvLog) { window.__tvLog(msg); } else { console.log(msg); } } catch (e) {}
});

namespace {

void Log(const std::string& msg) { js_log(msg.c_str()); }

class Player : public ElementaryMediaStreamSourceListener,
               public ElementaryMediaTrackListener,
               public HTMLMediaElementListener {
 public:
  std::string mime;
  std::vector<uint8_t> extradata;
  uint32_t width = 1920, height = 1080, fps_num = 60, fps_den = 1;
  bool config_ready = false;

  // AppendPacketAsync ne copie pas les octets : il transmet au téléviseur une vue sur cette
  // mémoire, lue plus tard. Réutiliser un même tampon mélangerait deux images et corromprait les
  // images de référence. Chaque image est donc écrite dans un anneau de 32 Mo et n'y est réécrite
  // qu'après environ 4 s de flux, bien après que le téléviseur l'a lue.
  static constexpr size_t kArena = 32u << 20;
  uint8_t* arena = nullptr;
  size_t head = 0;
  uint8_t* input = nullptr;

  uint8_t* Buffer(size_t size) {
    if (!arena) arena = static_cast<uint8_t*>(malloc(kArena));
    if (!arena || size == 0 || size > kArena / 4) return input = nullptr;
    if (head + size > kArena) head = 0;
    input = arena + head;
    head = (head + size + 15) & ~static_cast<size_t>(15);
    return input;
  }

  void Setup() {
    media_element_.reset(new HTMLMediaElement("video-element"));
    media_element_->SetListener(this);
    source_.reset(new ElementaryMediaStreamSource(
        EmssLatencyMode::kLow, EmssRenderingMode::kMediaElement));
    source_->SetListener(this);
    media_element_->SetSrc(source_.get());
    Log("installation faite");
  }

  void OnSourceClosed() override {
    Log("source fermee (configurable)");
    if (!config_ready) { Log("ERREUR : pas de configuration"); return; }
    // Durée fixée avant l'ouverture, comme dans l'exemple. En direct, elle est factice mais le
    // pipeline semble l'exiger pour passer de « en attente » à « ouverte ».
    auto dur = source_->SetDuration(Seconds{86400.0});
    if (!dur) Log("SetDuration -> " + std::to_string((int)dur.operation_result));
    ElementaryVideoTrackConfig vcfg;
    vcfg.mime_type = mime;
    vcfg.extradata = extradata;
    vcfg.width = width;
    vcfg.height = height;
    vcfg.framerate_num = fps_num;
    vcfg.framerate_den = fps_den;
    vcfg.decoding_mode = samsung::wasm::DecodingMode::kHardware;
    auto added = source_->AddTrack(vcfg);
    if (!added) {
      Log("AddTrack a echoue : " + std::to_string((int)added.operation_result));
      return;
    }
    track_ = std::move(added.value);
    track_.SetListener(this);
    track_valid_ = true;
    Log("piste ajoutee : " + mime + " " + std::to_string(width) + "x" + std::to_string(height));
    // En faible latence, la source ne passe de « en attente » à « ouverte » qu'au lancement de la
    // lecture : on appelle Play() dès l'ouverture, sans attendre OnCanPlay (qui ne viendra pas).
    source_->Open([this](OperationResult r) {
      if (r != OperationResult::kSuccess) { Log("ouverture refusee"); return; }
      Log("source ouverte, lancement lecture");
      media_element_->Play([](OperationResult pr) {
        Log(pr == OperationResult::kSuccess ? "lecture demarree" : "lecture refusee");
      });
    });
  }
  void OnSourceOpen() override { Log("source OUVERTE"); }
  void OnSourceOpenPending() override { Log("source en attente d'ouverture"); }
  void OnSourceDetached() override { track_valid_ = false; Log("source detachee"); }
  void OnSourceEnded() override { Log("source terminee"); }

  void OnTrackOpen() override {
    track_open_ = true;
    auto sid = track_.GetSessionId();
    if (sid) session_id_ = sid.value;
    Log("piste OUVERTE, session " + std::to_string(session_id_));
  }
  void OnTrackClosed(ElementaryMediaTrack::CloseReason reason) override {
    track_open_ = false;
    Log("piste fermee : " + std::to_string((int)reason));
  }
  void OnSessionIdChanged(SessionId sid) override { session_id_ = sid; }
  void OnAppendError(OperationResult r) override {
    Log("erreur d'ajout : " + std::to_string((int)r));
  }

  void OnCanPlay() override {
    if (!media_element_) return;
    media_element_->Play([](OperationResult r) {
      Log(r == OperationResult::kSuccess ? "lecture demarree" : "lecture refusee");
    });
  }
  void OnPlaying() override { Log("en lecture"); }
  void OnError(samsung::html::MediaError, const char* msg) override {
    Log(std::string("erreur video : ") + (msg ? msg : ""));
  }

  // Appelé par JavaScript pour chaque image, sur le fil principal. AppendPacketAsync y est autorisé.
  void Push(size_t size, double pts, bool key) {
    if (!input || !track_valid_) return;
    const uint8_t* data = input;
    input = nullptr;  // une seule image par emplacement
    ElementaryMediaPacket pkt{};
    pkt.pts = Seconds{pts};
    pkt.dts = Seconds{pts};
    pkt.duration = Seconds{1.0 * fps_den / fps_num};
    pkt.is_key_frame = key;
    pkt.data_size = size;
    pkt.data = data;
    pkt.width = width;
    pkt.height = height;
    pkt.framerate_num = fps_num;
    pkt.framerate_den = fps_den;
    pkt.session_id = session_id_;
    auto r = track_.AppendPacketAsync(pkt);
    if (!r && pushed_logged_ < 8) {
      Log("AppendPacketAsync cle=" + std::to_string(key ? 1 : 0) + " -> " +
          std::to_string((int)r.operation_result));
      ++pushed_logged_;
    }
  }

  int State() {
    if (!source_) return -1;
    auto rs = source_->GetReadyState();
    int s = rs ? (int)rs.value : -2;
    return s * 10 + (track_open_ ? 1 : 0);
  }

 private:
  std::unique_ptr<HTMLMediaElement> media_element_;
  std::unique_ptr<ElementaryMediaStreamSource> source_;
  ElementaryMediaTrack track_;
  bool track_valid_ = false;
  bool track_open_ = false;
  int pushed_logged_ = 0;
  SessionId session_id_ = 0;
};

Player g_player;

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void emss_setup() { g_player.Setup(); }

EMSCRIPTEN_KEEPALIVE void emss_configure(int w, int h, int num, int den,
                                         const char* mime, const uint8_t* extra,
                                         int extra_len) {
  g_player.width = w;
  g_player.height = h;
  g_player.fps_num = num;
  g_player.fps_den = den;
  g_player.mime = mime ? mime : "";
  g_player.extradata.assign(extra, extra + extra_len);
  g_player.config_ready = true;
}

EMSCRIPTEN_KEEPALIVE uint8_t* emss_buffer(int size) {
  return g_player.Buffer((size_t)size);
}

EMSCRIPTEN_KEEPALIVE void emss_push(int size, double pts, int key) {
  g_player.Push((size_t)size, pts, key != 0);
}

EMSCRIPTEN_KEEPALIVE int emss_state() { return g_player.State(); }

}  // extern "C"

int main() {
  Log("module charge");
  return 0;
}
