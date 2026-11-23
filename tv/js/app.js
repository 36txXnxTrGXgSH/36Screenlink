// 36Screenlink pour téléviseur Samsung (Tizen) : la télévision devient un écran du PC.
// Les images H.264 vont directement au décodeur matériel via le lecteur WebAssembly (Elementary
// Media Stream Source, mode faible latence), sans le tampon d'une demi-seconde du lecteur web :
// environ 80 ms de bout en bout, contre 350 à 560 ms auparavant. Appairage, chiffrement et réseau
// restent en JavaScript. Réseau local seulement, flux chiffré, aucun compte ni serveur.
'use strict';

(function () {
    var PORT = 7240;
    var KIND_TV = 2;
    var MSG = { HELLO: 1, STREAM_CONFIG: 2, VIDEO: 3, PING: 5, PONG: 6, STATS: 7, REQUEST_IDR: 8, CURSOR_POS: 9, CURSOR_SHAPE: 10, CLOCK: 11, LOG: 28 };
    var FLAG_KEY = 2;
    var KEY = { ENTER: 13, BACK: 10009, RED: 403, INFO: 457 };

    var video = document.getElementById('video-element');
    var panel = document.getElementById('panel');
    var statusEl = document.getElementById('status');
    var codeEl = document.getElementById('code');
    var hintEl = document.getElementById('hint');
    var statsEl = document.getElementById('stats');
    var cursorEl = document.getElementById('cursor');

    // Fonctions du module WebAssembly, préparées une fois le moteur initialisé.
    var wasm = {
        setup: Module.cwrap('emss_setup', null, []),
        configure: Module.cwrap('emss_configure', null, ['number', 'number', 'number', 'number', 'string', 'number', 'number']),
        buffer: Module.cwrap('emss_buffer', 'number', ['number']),
        push: Module.cwrap('emss_push', null, ['number', 'number', 'number']),
        state: Module.cwrap('emss_state', 'number', [])
    };

    var pairingArmed = false;
    var tvLog = function (text) { console.log(text); };
    var pendingConfirm = null;
    var streaming = false;

    function show(text, hint) {
        panel.style.display = 'flex';
        statusEl.textContent = text;
        hintEl.textContent = hint || '';
        codeEl.style.display = 'none';
    }
    function nowUs() { return performance.now() * 1000; }
    function keepScreenOn() {
        try { webapis.appcommon.setScreenSaver(webapis.appcommon.AppCommonScreenSaverState.SCREEN_SAVER_OFF); } catch (e) {}
    }

    // ---------------------------------------------------------------- télécommande
    try { tizen.tvinputdevice.registerKey('ColorF0Red'); } catch (e) {}
    try { tizen.tvinputdevice.registerKey('Info'); } catch (e) {}
    document.addEventListener('keydown', function (e) {
        if (pendingConfirm) {
            if (e.keyCode === KEY.ENTER) { pendingConfirm(true); pendingConfirm = null; }
            else if (e.keyCode === KEY.BACK) { pendingConfirm(false); pendingConfirm = null; }
            e.preventDefault();
            return;
        }
        if (e.keyCode === KEY.RED) {
            pairingArmed = true;
            if (!streaming) show('Appairage autorisé', 'Lancez 36Screenlink sur le PC : un code va s\'afficher sur les deux écrans.');
        } else if (e.keyCode === KEY.INFO || (streaming && e.keyCode === KEY.ENTER)) {
            statsEl.style.display = statsEl.style.display === 'block' ? 'none' : 'block';
        } else if (e.keyCode === KEY.BACK) {
            try { tizen.application.getCurrentApplication().exit(); } catch (err) {}
        }
    });

    function allowPairing() { return !Secure.loadPsk() || pairingArmed; }
    function confirmCode(code) {
        return new Promise(function (resolve) {
            var text = String(code);
            while (text.length < 6) text = '0' + text;
            panel.style.display = 'flex';
            statusEl.textContent = 'Appairer ce PC ?';
            codeEl.textContent = text.substr(0, 3) + ' ' + text.substr(3);
            codeEl.style.display = 'block';
            hintEl.textContent = 'Appuyez sur OK seulement si le PC affiche le même code. Retour pour refuser.';
            var timer = setTimeout(function () { if (pendingConfirm) { pendingConfirm = null; resolve(false); } }, 110000);
            pendingConfirm = function (ok) { clearTimeout(timer); resolve(ok); };
        });
    }

    // ---------------------------------------------------------------- recherche du PC
    function ping(ip) {
        return new Promise(function (resolve) {
            var x = new XMLHttpRequest();
            x.open('GET', 'http://' + ip + ':' + PORT + '/36screenlink/ping');
            x.timeout = 1500;
            x.onload = function () { resolve(x.responseText === '36Screenlink' ? ip : null); };
            x.onerror = x.ontimeout = function () { resolve(null); };
            x.send();
        });
    }
    async function findPc() {
        var saved = null;
        try { saved = localStorage.getItem('36screenlink.pc'); } catch (e) {}
        if (saved && await ping(saved)) return saved;
        var own = '';
        try { own = webapis.network.getIp(); } catch (e) {}
        if (!/^\d+\.\d+\.\d+\.\d+$/.test(own)) return null;
        var prefix = own.split('.').slice(0, 3).join('.');
        show('Recherche du PC sur le réseau…', 'Le PC doit avoir 36Screenlink en marche, sur le même réseau.');
        for (var start = 1; start < 255; start += 32) {
            var batch = [];
            for (var i = start; i < start + 32 && i < 255; i++) {
                var ip = prefix + '.' + i;
                if (ip !== own) batch.push(ping(ip));
            }
            var found = (await Promise.all(batch)).filter(function (x) { return x; });
            if (found.length) {
                try { localStorage.setItem('36screenlink.pc', found[0]); } catch (e) {}
                return found[0];
            }
        }
        return null;
    }

    // ---------------------------------------------------------------- unités NAL (pour lire le SPS)
    function firstNalOfType(data, type) {
        var i = 0, n = data.length, start = -1, t = -1;
        while (i + 2 < n) {
            if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) {
                if (start >= 0 && t === type) return data.subarray(start, i > 0 && data[i - 1] === 0 ? i - 1 : i);
                i += 3;
                start = i;
                t = data[i] & 0x1F;
            } else i++;
        }
        if (start >= 0 && t === type) return data.subarray(start);
        return null;
    }
    // Lecture du SPS H.264 (diagnostic) : ce que le flux déclare sur la réserve d'images du décodeur.
    // Sans « bitstream_restriction », le décodeur suppose un réordonnancement possible et retient
    // plusieurs images avant d'afficher : c'est de la latence pure pour un flux sans images B.
    function describeSps(nal) {
        var rbsp = [], z = 0, i;
        for (i = 1; i < nal.length; i++) {            // retire les octets d'émulation 00 00 03
            if (z >= 2 && nal[i] === 3) { z = 0; continue; }
            rbsp.push(nal[i]); z = nal[i] === 0 ? z + 1 : 0;
        }
        var pos = 0;
        function u(n) { var v = 0; while (n--) { v = v * 2 + ((rbsp[pos >> 3] >> (7 - (pos & 7))) & 1); pos++; } return v; }
        function ue() { var zeros = 0; while (u(1) === 0 && zeros < 32) zeros++; return (1 << zeros) - 1 + u(zeros); }
        function se() { var v = ue(); return v & 1 ? (v + 1) / 2 : -v / 2; }
        function hrd() { var n = ue(); u(8); for (var k = 0; k <= n; k++) { ue(); ue(); u(1); } u(20); }
        try {
            var profile = u(8); u(8); var level = u(8); ue();
            if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].indexOf(profile) >= 0) {
                if (ue() === 3) u(1);
                ue(); ue(); u(1);
                if (u(1)) for (var l = 0; l < 8; l++) if (u(1)) { var last = 8, next = 8; for (var j = 0; j < (l < 6 ? 16 : 64); j++) { if (next) next = (last + se() + 256) % 256; last = next || last; } }
            }
            ue(); var poc = ue();
            if (poc === 0) ue(); else if (poc === 1) { u(1); se(); se(); var n = ue(); while (n--) se(); }
            var refs = ue(); u(1); ue(); ue();
            if (!u(1)) u(1);
            u(1); if (u(1)) { ue(); ue(); ue(); ue(); }
            var out = 'SPS profil ' + profile + ' niveau ' + level + ', références ' + refs;
            if (!u(1)) return out + ', sans VUI';
            if (u(1)) { if (u(8) === 255) u(32); }
            if (u(1)) u(1);
            if (u(1)) { u(4); if (u(1)) u(24); }
            if (u(1)) { ue(); ue(); }
            if (u(1)) u(65);
            var nal_ = u(1); if (nal_) hrd();
            var vcl = u(1); if (vcl) hrd();
            if (nal_ || vcl) u(1);
            u(1);
            if (!u(1)) return out + ', sans restriction de flux (le décodeur garde une réserve)';
            u(1); ue(); ue(); ue(); ue();
            return out + ', réordonnancement ' + ue() + ', réserve décodeur ' + ue();
        } catch (e) { return 'SPS illisible'; }
    }
    function codecString(sps) {
        function h(v) { return (v < 16 ? '0' : '') + v.toString(16); }
        return 'avc1.' + h(sps[1]) + h(sps[2]) + h(sps[3]);
    }

    // ---------------------------------------------------------------- lecteur WebAssembly
    var player = null;
    // Le module WASM ne s'initialise qu'une fois, même si la session réseau se reconnecte.
    var wasmStarted = false;
    var firstSec = -1;
    // Horodatages envoyés au lecteur (secondes, croissants même après une reconnexion) et heure de
    // capture correspondante. LEAD : avance maximale sur l'horloge du lecteur, environ deux images.
    var lastSent = -1, lastCapture = 0;
    var LEAD = 0.035;

    function WasmPlayer() {
        this.width = 1920;
        this.height = 1080;
        this.fps = 60;
        this.setupDone = false;
        this.waitingKey = true;
        this.firstSec = -1;
        this.timeline = [];
        this.latencies = [];
        this.appended = 0;
        this.drops = 0;
        this.arrSum = 0; this.arrN = 0; this.arrMax = 0;
        this.rxBytes = 0;
        this.lastCatchUp = 0;
        this.decSum = 0; this.decN = 0;
        this.lastTotal = 0; this.lastDropped = 0;
        this.clockOffset = null;
        this.lastIdrRequest = 0;
        this.requestIdr = function () {};
    }
    WasmPlayer.prototype.configure = function (w, h, fps) {
        this.width = w; this.height = h; this.fps = fps || 60;
        placeCursor();
    };
    WasmPlayer.prototype.askKey = function () {
        var now = performance.now();
        if (now - this.lastIdrRequest < 500) return;
        this.lastIdrRequest = now;
        this.requestIdr();
    };
    WasmPlayer.prototype.frame = function (data, pts, key) {
        // Première fois : lire le SPS pour le codec, configurer et démarrer le lecteur WASM (une seule fois).
        if (!wasmStarted) {
            var sps = firstNalOfType(data, 7);
            if (!sps) { if (key) this.askKey(); return; }  // attendre une image clé avec en-tête
            var mime = 'video/mp4; codecs="' + codecString(sps) + '"';
            wasm.configure(this.width, this.height, this.fps, 1, mime, 0, 0);
            wasm.setup();
            wasmStarted = true;
            this.waitingKey = true;
            tvLog('lecteur WASM : ' + mime + ' ' + this.width + 'x' + this.height + ' ; ' + describeSps(sps));
        }
        // En faible latence, la source s'ouvre une fois les premières images reçues : on envoie
        // dès la première image clé, sans attendre que la piste signale son ouverture.
        if (this.waitingKey) {
            if (!key) { this.askKey(); return; }
            this.waitingKey = false;
            if (firstSec < 0) firstSec = pts / 1e6;   // l'horloge du lecteur part de cette image clé
        }
        // Horodatage envoyé au lecteur. Son horloge s'arrête dès qu'aucune image n'arrive (micro-trou
        // du réseau) : des horodatages calqués sur la capture du PC prendraient alors de l'avance sur
        // elle, et ce retard s'additionnerait sans fin. On suit donc la progression réelle, mais jamais
        // plus de LEAD devant l'horloge du lecteur, et jamais en arrière.
        var rel;
        if (lastSent < 0) rel = 0;
        else {
            var step = Math.max(0, (pts - lastCapture) / 1e6);
            rel = Math.min(lastSent + step, video.currentTime + LEAD);
            if (rel <= lastSent) rel = lastSent + 0.0005;
        }
        lastSent = rel;
        lastCapture = pts;
        // Chaque image a son propre emplacement dans l'anneau du module (voir player.cc).
        var ptr = wasm.buffer(data.length);
        if (!ptr) { this.recover('image trop grande (' + data.length + ' octets)'); return; }
        HEAPU8.set(data, ptr);
        wasm.push(data.length, rel, key ? 1 : 0);
        this.timeline.push({ t: rel, pts: pts });
        if (this.timeline.length > 600) this.timeline.splice(0, 300);
        this.appended++;
        // Pas d'image clé périodique : le transport est fiable (TCP). Une image clé n'est demandée
        // qu'au démarrage et quand le téléviseur refuse une image (voir recover).
    };
    WasmPlayer.prototype.sample = function () {
        if (this.clockOffset === null || video.paused || !this.timeline.length) return;
        var t = video.currentTime, shown = null;
        for (var i = this.timeline.length - 1; i >= 0; i--) {
            if (this.timeline[i].t <= t + 0.001) { shown = this.timeline[i]; break; }
        }
        if (!shown) return;
        var latency = nowUs() - this.clockOffset - shown.pts;
        if (latency > 0 && latency < 5e6) this.latencies.push(latency);
    };
    WasmPlayer.prototype.stats = function () {
        var l = this.latencies.slice().sort(function (a, b) { return a - b; });
        this.latencies = [];
        var avg = l.length ? l.reduce(function (a, b) { return a + b; }, 0) / l.length : 0;
        var p95 = l.length ? l[Math.min(l.length - 1, Math.floor(l.length * 0.95))] : 0;
        var frames = this.appended; this.appended = 0;
        return { frames: frames, avg: avg, p95: p95, drops: this.drops };
    };
    // Une image perdue casse toutes celles qui en dépendent : on n'envoie plus rien jusqu'à la
    // prochaine image clé, demandée aussitôt. L'image reste figée un instant au lieu de se dégrader.
    WasmPlayer.prototype.recover = function (why) {
        this.drops++;
        if (!this.waitingKey) tvLog('vidéo : ' + why + ', reprise sur image clé');
        this.waitingKey = true;
        this.askKey();
    };

    // Les envois asynchrones refusés par le téléviseur ne remontent que sous forme de promesse
    // rejetée, sans gestionnaire : on les intercepte ici pour déclencher la reprise.
    window.addEventListener('unhandledrejection', function (e) {
        var msg = String(e.reason && (e.reason.message || e.reason));
        if (!/Append failed/i.test(msg)) return;
        e.preventDefault();
        if (player && streaming) player.recover('image refusée (' + msg.replace(/^.*Append failed:?\s*/i, '') + ')');
    });

    (function tick() {
        if (player && streaming) player.sample();
        requestAnimationFrame(tick);
    })();

    // ---------------------------------------------------------------- curseur
    var cursor = { x: 0, y: 0, hotX: 0, hotY: 0, visible: false };
    var cursorCtx = cursorEl.getContext('2d');
    function placeCursor() {
        if (!player || !player.width) return;
        var scale = Math.min(1920 / player.width, 1080 / player.height);
        var left = (1920 - player.width * scale) / 2, top = (1080 - player.height * scale) / 2;
        cursorEl.style.width = (cursorEl.width * scale) + 'px';
        cursorEl.style.height = (cursorEl.height * scale) + 'px';
        cursorEl.style.transform = 'translate(' + (left + (cursor.x - cursor.hotX) * scale) + 'px,' + (top + (cursor.y - cursor.hotY) * scale) + 'px)';
        cursorEl.style.display = cursor.visible && streaming ? 'block' : 'none';
    }
    function cursorShape(r) {
        var w = r.u16(), h = r.u16();
        cursor.hotX = r.u16(); cursor.hotY = r.u16();
        if (w < 1 || h < 1 || w > 256 || h > 256 || r.remaining() < w * h * 4) return;
        var bgra = r.take(w * h * 4), image = cursorCtx.createImageData(w, h);
        for (var i = 0; i < bgra.length; i += 4) {
            image.data[i] = bgra[i + 2]; image.data[i + 1] = bgra[i + 1];
            image.data[i + 2] = bgra[i]; image.data[i + 3] = bgra[i + 3];
        }
        cursorEl.width = w; cursorEl.height = h;
        cursorCtx.putImageData(image, 0, 0);
        placeCursor();
    }

    // ---------------------------------------------------------------- session
    async function session(pc) {
        var keyId = '';
        var psk = Secure.loadPsk();
        if (psk) keyId = await Secure.keyId(psk);
        var ws = new WebSocket('ws://' + pc + ':' + PORT + '/36screenlink/ws?id=' + keyId);
        ws.binaryType = 'arraybuffer';
        var stream = new ByteStream();
        ws.onmessage = function (m) { stream.push(new Uint8Array(m.data)); };
        ws.onclose = ws.onerror = function () { stream.close(); };
        await new Promise(function (resolve, reject) {
            ws.onopen = resolve;
            setTimeout(function () { reject(new Error('le PC ne répond pas')); }, 5000);
        });
        var sendRaw = function (bytes) { if (ws.readyState === 1) ws.send(bytes); };
        show('Connexion au PC…');

        var keys = await Secure.accept(stream, sendRaw, { allowPairing: allowPairing, confirm: confirmCode });
        var encoder = new TextEncoder();
        if (keys === 'paired') {
            pairingArmed = false;
            show('Télévision appairée', 'Connexion au PC…');
            ws.close();
            return;
        }
        var sending = Promise.resolve();
        var send = function (plain) {
            sending = sending.then(async function () {
                var sealed = await keys.tx.seal(plain);
                var n = sealed.length;
                sendRaw(Bytes.concat(new Uint8Array([n >>> 24, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]), sealed));
            }).catch(function () {});
        };
        tvLog = function (text) {
            console.log(text);
            send(Bytes.concat(new Uint8Array([MSG.LOG]), encoder.encode(String(text).substr(0, 280))));
        };
        // Le module WASM journalise aussi (Module.print) ; relayé au PC.
        window.__tvLog = tvLog;
        // Réglage à chaud de l'avance (mesures depuis le débogueur).
        window.__setLead = function (v) { LEAD = v; tvLog('avance max : ' + (v * 1000).toFixed(0) + ' ms'); };

        send(new Writer(MSG.HELLO).u16(1920).u16(1080).u16(60).u8(1).u8(KIND_TV).bytes());
        tvLog('lecteur WASM prêt, flux demandé 1920x1080');

        player = new WasmPlayer();
        player.requestIdr = function () { send(new Uint8Array([MSG.REQUEST_IDR])); };
        var statsTimer = setInterval(function () {
            var s = player.stats();
            statsEl.textContent = s.frames + ' images/s · ' + (s.avg / 1000).toFixed(0) + ' ms (p95 ' + (s.p95 / 1000).toFixed(0) + ' ms)';
            send(new Writer(MSG.STATS).u32(s.frames).u32(Math.round(s.avg)).u32(Math.round(s.p95)).u32(0).u32(s.drops).bytes());
            var arrAvg = player.arrN ? player.arrSum / player.arrN / 1000 : 0, arrMax = player.arrMax / 1000;
            var mbps = player.rxBytes * 8 / 1e6;
            player.arrSum = 0; player.arrN = 0; player.arrMax = 0; player.rxBytes = 0;
            // Rattrapage : si le téléviseur a pris du retard (décodeur saturé), on cesse d'envoyer
            // jusqu'à la prochaine image clé. Le décodeur vide sa file, puis repart à jour. Le PC,
            // qui reçoit aussi cette latence, baisse son débit pour que le retard ne revienne pas.
            if (s.avg > 300000 && performance.now() - player.lastCatchUp > 4000) {
                player.lastCatchUp = performance.now();
                player.recover('retard de ' + (s.avg / 1000).toFixed(0) + ' ms');
            }
            var ahead = lastSent >= 0 ? (lastSent - video.currentTime) * 1000 : 0;
            var dec = player.decN ? player.decSum / player.decN : 0;
            player.decSum = 0; player.decN = 0;
            // Images perdues par le lecteur (affichées trop tard), si le téléviseur les compte.
            var q = null, lost = '';
            try { q = video.getVideoPlaybackQuality ? video.getVideoPlaybackQuality() : null; } catch (e) {}
            var dropped = q ? q.droppedVideoFrames : (video.webkitDroppedFrameCount || 0);
            var total = q ? q.totalVideoFrames : (video.webkitDecodedFrameCount || 0);
            lost = ', lecteur ' + (total - player.lastTotal) + ' img, perdues ' + (dropped - player.lastDropped);
            player.lastTotal = total; player.lastDropped = dropped;
            tvLog(s.frames + ' img/s, ' + mbps.toFixed(1) + ' Mb/s, avance ' + ahead.toFixed(0) + ' ms, latence ' + (s.avg / 1000).toFixed(0) + ' ms (p95 ' + (s.p95 / 1000).toFixed(0) +
                  '), reception ' + arrAvg.toFixed(0) + ' ms (max ' + arrMax.toFixed(0) + '), dechiffrement ' + dec.toFixed(1) + ' ms' + lost + ', pertes ' + s.drops);
        }, 1000);

        try {
            for (;;) {
                var head = await stream.read(4);
                var length = ((head[0] << 24) >>> 0) + ((head[1] << 16) | (head[2] << 8) | head[3]);
                if (length < 16 || length > 8 * 1024 * 1024) throw new Error('enregistrement invalide');
                var sealedRec = await stream.read(length);
                var t0 = performance.now();
                var plain = await keys.rx.open(sealedRec);
                player.decSum += performance.now() - t0; player.decN++;
                var r = new Reader(plain, 1);
                switch (plain[0]) {
                case MSG.STREAM_CONFIG:
                    r.u8();
                    var w = r.u16(), h = r.u16(), fps = r.u16();
                    player.configure(w, h, fps);
                    streaming = true;
                    panel.style.display = 'none';
                    keepScreenOn();
                    break;
                case MSG.VIDEO:
                    var flags = r.u8(), pts = r.u64();
                    // Retard à la réception (réseau + déchiffrement), avant le décodeur.
                    if (player.clockOffset !== null) {
                        var arrival = nowUs() - player.clockOffset - pts;
                        player.arrSum += arrival; player.arrN++;
                        if (arrival > player.arrMax) player.arrMax = arrival;
                    }
                    player.rxBytes += plain.length - r.p;
                    player.frame(plain.subarray(r.p), pts, (flags & FLAG_KEY) !== 0);
                    break;
                case MSG.PING:
                    send(new Writer(MSG.PONG).u64(r.u64()).u64(Math.round(nowUs())).bytes());
                    break;
                case MSG.CLOCK:
                    player.clockOffset = r.i64();
                    break;
                case MSG.CURSOR_POS:
                    cursor.visible = r.u8() !== 0;
                    cursor.x = r.i16(); cursor.y = r.i16();
                    placeCursor();
                    break;
                case MSG.CURSOR_SHAPE:
                    cursorShape(r);
                    break;
                default: break;
                }
            }
        } finally {
            clearInterval(statsTimer);
            tvLog = function (text) { console.log(text); };
            window.__tvLog = null;
            streaming = false;
            cursorEl.style.display = 'none';
            try { ws.close(); } catch (e) {}
        }
    }

    async function main() {
        keepScreenOn();
        for (;;) {
            var pc = await findPc();
            if (!pc) {
                show('PC introuvable', 'Démarrez 36Screenlink sur le PC. Nouvel essai dans quelques secondes.');
                await new Promise(function (r) { setTimeout(r, 5000); });
                continue;
            }
            try { await session(pc); }
            catch (e) {
                var paired = !!Secure.loadPsk();
                show(paired ? 'En attente du PC' : 'Télévision non appairée',
                     (paired ? '' : 'Appuyez sur le bouton rouge pour autoriser l\'appairage, puis lancez 36Screenlink sur le PC. ') + '(' + e.message + ')');
            }
            await new Promise(function (r) { setTimeout(r, 1500); });
        }
    }

    show('Démarrage…', 'Bouton rouge : nouvel appairage · Info : mesures · Retour : quitter');
    main();
})();
