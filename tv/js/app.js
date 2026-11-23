// 36Screenlink pour téléviseur Samsung (Tizen) : la télévision devient un écran du PC.
// Les images H.264 vont directement au décodeur matériel via le lecteur WebAssembly (Elementary
// Media Stream Source, mode faible latence), sans le tampon d'une demi-seconde du lecteur web :
// environ 80 ms de bout en bout, contre 350 à 560 ms auparavant. Appairage, chiffrement et réseau
// restent en JavaScript. Réseau local seulement, flux chiffré, aucun compte ni serveur.
'use strict';

(function () {
    var PORT = 7240;
    var KIND_TV = 2;
    var MSG = { HELLO: 1, STREAM_CONFIG: 2, VIDEO: 3, PING: 5, PONG: 6, STATS: 7, REQUEST_IDR: 8, CURSOR_POS: 9, CURSOR_SHAPE: 10, CLOCK: 11,
                KEY: 12, TEXT: 13, AUDIO: 15, AUDIO_ROUTE: 16, AUDIO_STATE: 17, CONTROL: 18, CONTROL_STATE: 19,
                SECURE_STATE: 20, MOUSE: 22, DEVICES: 27, LOG: 28 };
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
        push: Module.cwrap('emss_push', null, ['number', 'number', 'number', 'number']),
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
    // Codes reçus de la télécommande (Smart Remote Samsung 2020 et télécommandes classiques).
    var RC = {
        LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, OK: 13, BACK: 10009, EXIT: 10182,
        PLAYPAUSE: 10252, PLAY: 415, PAUSE: 19, VOLUP: 447, VOLDOWN: 448, MUTE: 449,
        CHUP: 427, CHDOWN: 428, GUIDE: 458, MENU: 10133, TOOLS: 10135, CHLIST: 10073
    };
    // Touches réservées seulement pendant l'affichage du PC : hors session, le volume et les chaînes
    // reviennent au téléviseur. Netflix, Prime Video, Rakuten et Accueil restent au système.
    var SESSION_KEYS = ['ChannelUp', 'ChannelDown', 'ChannelList', 'Menu', 'Tools', 'MediaPlayPause', 'MediaPlay', 'MediaPause', 'Exit'];
    // Volume : réglé sur le PC quand le son sort du PC ; laissé au téléviseur quand c'est lui qui le joue.
    var VOLUME_KEYS = ['VolumeUp', 'VolumeDown', 'VolumeMute'];
    // Touche qui ouvre le menu : l'appui sur le bouton Chaîne (Guide) de la Smart Remote, ou une
    // touche Menu / Outils / Liste des chaînes sur une autre télécommande.
    var MENU_KEYS = {}; MENU_KEYS[RC.GUIDE] = MENU_KEYS[RC.MENU] = MENU_KEYS[RC.TOOLS] = MENU_KEYS[RC.CHLIST] = true;
    // Codes de touches virtuelles Windows envoyés au PC.
    var VK = { BACK: 0x08, TAB: 0x09, ENTER: 0x0D, ALT: 0x12, ESC: 0x1B, PGUP: 0x21, PGDN: 0x22, LEFT: 0x25, UP: 0x26,
               RIGHT: 0x27, DOWN: 0x28, D: 0x44, LWIN: 0x5B, MUTE: 0xAD, VOLDOWN: 0xAE, VOLUP: 0xAF, PLAYPAUSE: 0xB3 };
    var ARROW_VK = {}; ARROW_VK[RC.LEFT] = VK.LEFT; ARROW_VK[RC.UP] = VK.UP; ARROW_VK[RC.RIGHT] = VK.RIGHT; ARROW_VK[RC.DOWN] = VK.DOWN;

    // Toujours réservées : rouge et Info (télécommandes classiques), et l'appui sur le bouton Chaîne
    // (Guide), seule touche libre de la Smart Remote : menu pendant l'affichage du PC, nouvel
    // appairage sinon. Le pavé de la touche 123 n'envoie rien aux applications.
    try { tizen.tvinputdevice.registerKey('ColorF0Red'); } catch (e) {}
    try { tizen.tvinputdevice.registerKey('Info'); } catch (e) {}
    try { tizen.tvinputdevice.registerKey('Guide'); } catch (e) {}
    function armPairing() {
        pairingArmed = true;
        if (!streaming) show('Appairage autorisé', 'Lancez 36Screenlink sur le PC : un code va s\'afficher sur les deux écrans.');
        else toast('Nouvel appairage autorisé à la prochaine connexion');
    }
    var keysHeld = {};
    function setKeys(names, on) {
        names.forEach(function (name) {
            if (!!keysHeld[name] === on) return;
            try {
                if (on) tizen.tvinputdevice.registerKey(name); else tizen.tvinputdevice.unregisterKey(name);
                keysHeld[name] = on;
            } catch (e) {}
        });
    }
    function updateKeys() {
        var active = streaming && !!link;
        setKeys(SESSION_KEYS, active);
        setKeys(VOLUME_KEYS, active && !audioOnTv);
    }

    // Liaison vers le PC pendant une session (null sinon) : touches, souris, texte, réglages.
    var link = null;
    var remoteMode = 'souris';
    try { if (localStorage.getItem('36screenlink.remote') === 'touches') remoteMode = 'touches'; } catch (e) {}
    var displayMode = 0, deviceCount = 1, secureScreen = false;

    function tap(vk) { if (link) { link.key(vk, true); link.key(vk, false); } }
    function combo(vks) {
        if (!link) return;
        vks.forEach(function (v) { link.key(v, true); });
        vks.slice().reverse().forEach(function (v) { link.key(v, false); });
    }

    // Pointeur piloté à la croix, en pixels de l'écran du PC montré. La position du vrai curseur,
    // renvoyée par le PC, le recale quand on ne le déplace pas.
    var pointer = { x: 960, y: 540, buttons: 0, lastDriven: 0, held: {}, since: 0, last: 0 };
    function sendPointer(wheel) {
        if (!link || !player || !player.width) return;
        var x = Math.max(1, Math.round(pointer.x * 65535 / Math.max(1, player.width - 1)));
        var y = Math.max(1, Math.round(pointer.y * 65535 / Math.max(1, player.height - 1)));
        link.mouse(pointer.buttons, Math.min(65535, x), Math.min(65535, y), wheel || 0);
    }
    function click(button) {
        pointer.buttons = button; sendPointer();
        pointer.buttons = 0; sendPointer();
    }
    // Déplacement continu tant qu'une flèche est tenue : lent au départ pour viser, puis de plus en
    // plus vite pour traverser l'écran.
    function pointerStep(now) {
        var h = pointer.held;
        var dx = (h[RC.RIGHT] ? 1 : 0) - (h[RC.LEFT] ? 1 : 0), dy = (h[RC.DOWN] ? 1 : 0) - (h[RC.UP] ? 1 : 0);
        if (!link || (!dx && !dy)) { pointer.last = now; return; }
        var dt = Math.min(0.05, (now - pointer.last) / 1000);
        pointer.last = now;
        var held = (now - pointer.since) / 1000;
        var speed = 260 + 1500 * Math.max(0, Math.min(1, (held - 0.2) / 0.9));
        if (dx && dy) speed /= Math.SQRT2;
        pointer.x = Math.max(0, Math.min(player.width - 1, pointer.x + dx * speed * dt));
        pointer.y = Math.max(0, Math.min(player.height - 1, pointer.y + dy * speed * dt));
        pointer.lastDriven = now;
        sendPointer();
        // Le curseur affiché bouge tout de suite, sans attendre l'aller-retour par le PC.
        cursor.x = pointer.x; cursor.y = pointer.y; cursor.visible = true;
        placeCursor();
    }

    // Répétition gérée ici : la télécommande n'envoie qu'un appui et un relâchement.
    var repeats = {};
    function repeatStart(k, fn, first, every) {
        fn();
        repeats[k] = setTimeout(function loop() { fn(); repeats[k] = setTimeout(loop, every || 90); }, first || 400);
    }
    function repeatStop(k) { clearTimeout(repeats[k]); delete repeats[k]; }

    var okTimer = null, okLong = false;
    var pressed = {};
    function releaseAll() {
        Object.keys(pressed).forEach(function (k) { if (pressed[k]) remoteUp(+k); });
        pressed = {};
    }

    var okConsumed = false, keysSent = {};
    function remoteDown(k) {
        // OK du menu : son relâchement ne doit pas cliquer sur le PC.
        if (menuOpen) { if (k === RC.OK) okConsumed = true; menuKey(k); return; }
        if (MENU_KEYS[k]) { openMenu(); return; }
        switch (k) {
        case KEY.INFO: toggleStats(); return;
        case RC.BACK: tap(VK.ESC); return;
        case RC.EXIT: exitApp(); return;
        case RC.PLAYPAUSE: case RC.PLAY: case RC.PAUSE: tap(VK.PLAYPAUSE); return;
        case RC.VOLUP: repeatStart(k, function () { tap(VK.VOLUP); }, 350, 120); return;
        case RC.VOLDOWN: repeatStart(k, function () { tap(VK.VOLDOWN); }, 350, 120); return;
        case RC.MUTE: tap(VK.MUTE); return;
        case RC.CHUP: case RC.CHDOWN:
            var up = k === RC.CHUP;
            if (remoteMode === 'souris') repeatStart(k, function () { sendPointer(up ? 120 : -120); }, 350, 80);
            else repeatStart(k, function () { tap(up ? VK.PGUP : VK.PGDN); });
            return;
        case RC.OK:
            if (remoteMode === 'touches') { link.key(VK.ENTER, true); keysSent[k] = VK.ENTER; return; }
            okLong = false;
            // Appui long : clic droit, déclenché sans attendre le relâchement.
            okTimer = setTimeout(function () { okLong = true; click(2); }, 550);
            return;
        }
        if (ARROW_VK[k]) {
            if (remoteMode === 'souris') {
                if (!Object.keys(pointer.held).some(function (h) { return pointer.held[h]; })) {
                    pointer.since = performance.now(); pointer.last = pointer.since;
                }
                pointer.held[k] = true;
            } else {
                // Comme un clavier tenu : appuis répétés, puis un seul relâchement.
                var vk = ARROW_VK[k];
                link.key(vk, true);
                keysSent[k] = vk;
                repeats[k] = setTimeout(function loop() { if (link) link.key(vk, true); repeats[k] = setTimeout(loop, 60); }, 400);
            }
        }
    }
    function remoteUp(k) {
        repeatStop(k);
        if (ARROW_VK[k]) pointer.held[k] = false;
        if (k === RC.OK) {
            clearTimeout(okTimer);
            if (okConsumed) { okConsumed = false; return; }
        }
        if (!link) { delete keysSent[k]; return; }
        if (keysSent[k] !== undefined) { link.key(keysSent[k], false); delete keysSent[k]; return; }
        if (k === RC.OK && remoteMode === 'souris' && !okLong && !menuOpen) click(1);
    }

    document.addEventListener('keydown', function (e) {
        var k = e.keyCode;
        if (pendingConfirm) {
            if (k === KEY.ENTER) { pendingConfirm(true); pendingConfirm = null; }
            else if (k === KEY.BACK) { pendingConfirm(false); pendingConfirm = null; }
            e.preventDefault();
            return;
        }
        if (kbdOpen) return;   // le clavier du téléviseur gère ses touches
        if (k === KEY.RED) { armPairing(); return; }
        if (!streaming || !link) {
            if (k === KEY.INFO) toggleStats();
            else if (k === KEY.BACK) exitApp();
            else if (MENU_KEYS[k]) armPairing();
            return;
        }
        e.preventDefault();
        if (pressed[k]) return;
        pressed[k] = true;
        remoteDown(k);
    });
    document.addEventListener('keyup', function (e) {
        var k = e.keyCode;
        if (!pressed[k]) return;
        pressed[k] = false;
        remoteUp(k);
    });
    window.addEventListener('blur', releaseAll);
    document.addEventListener('visibilitychange', function () { if (document.hidden) releaseAll(); });

    function exitApp() { try { tizen.application.getCurrentApplication().exit(); } catch (err) {} }
    function toggleStats() { statsEl.style.display = statsEl.style.display === 'block' ? 'none' : 'block'; }

    // ---------------------------------------------------------------- messages brefs
    var toastEl = document.getElementById('toast'), toastTimer = null, hintShown = false;
    function toast(text, ms) {
        toastEl.textContent = text;
        toastEl.style.display = 'block';
        clearTimeout(toastTimer);
        toastTimer = setTimeout(function () { toastEl.style.display = 'none'; }, ms || 3000);
    }
    var secureEl = document.getElementById('secure');
    function setSecure(on) {
        secureScreen = on;
        secureEl.style.display = on && streaming ? 'block' : 'none';
    }

    // ---------------------------------------------------------------- son du PC sur le téléviseur
    // Le PC envoie du PCM stéréo 16 bits par petits morceaux ; on les enchaîne avec l'API audio du
    // navigateur. Pendant ce temps, le PC coupe son propre haut-parleur (et le rétablit ensuite).
    var audioOnTv = false;
    var sound = { ctx: null, next: 0 };
    function soundContext() {
        if (!sound.ctx) {
            var Ctx = window.AudioContext || window.webkitAudioContext;
            try { sound.ctx = new Ctx({ latencyHint: 'interactive' }); } catch (e) { try { sound.ctx = new Ctx(); } catch (e2) {} }
        }
        if (sound.ctx && sound.ctx.state === 'suspended') sound.ctx.resume();
        return sound.ctx;
    }
    function soundChunk(rate, pcm) {
        var frames = pcm.length >> 2;
        if (!audioOnTv || !frames || rate < 8000 || rate > 192000) return;
        var ctx = soundContext();
        if (!ctx) return;
        var now = ctx.currentTime;
        // Petite réserve contre les irrégularités du réseau, jamais plus de 200 ms : au-delà, ce
        // morceau est jeté pour que le son reste calé sur l'image.
        if (sound.next < now + 0.02) sound.next = now + 0.06;
        else if (sound.next > now + 0.2) return;
        var buf = ctx.createBuffer(2, frames, rate);
        var left = buf.getChannelData(0), right = buf.getChannelData(1);
        for (var i = 0, j = 0; i < frames; i++, j += 4) {
            var l = pcm[j] | (pcm[j + 1] << 8), r = pcm[j + 2] | (pcm[j + 3] << 8);
            left[i] = (l > 32767 ? l - 65536 : l) / 32768;
            right[i] = (r > 32767 ? r - 65536 : r) / 32768;
        }
        var src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(ctx.destination);
        src.start(sound.next);
        sound.next += frames / rate;
    }
    function setAudioOnTv(on) {
        audioOnTv = on;
        if (!on) sound.next = 0;
        updateKeys();
    }
    function setAudioRoute(onTv) {
        if (!link) return;
        if (onTv) soundContext();   // créé pendant l'appui sur OK : le navigateur l'autorise à jouer
        link.audioRoute(onTv);
        toast(onTv ? 'Son : sur la télévision' : 'Son : sur le PC');
    }

    // ---------------------------------------------------------------- menu
    var menuEl = document.getElementById('menu'), menuList = document.getElementById('menu-list');
    var menuOpen = false, menuSel = 0, menuRows = [];
    function setRemoteMode(m) {
        remoteMode = m;
        try { localStorage.setItem('36screenlink.remote', m); } catch (e) {}
        toast(m === 'souris' ? 'Pilotage : souris' : 'Pilotage : touches (flèches, OK = Entrée)');
    }
    function setDisplay(v) {
        if (!link) return;
        link.control(1, v);
        toast('Affichage : ' + ['étendu', 'dupliqué', 'télévision seule', 'appareils seuls'][v] + '…');
    }
    function menuModel() {
        var rows = [
            { section: 'Pilotage' },
            { label: 'Souris', check: remoteMode === 'souris', act: function () { setRemoteMode('souris'); } },
            { label: 'Touches : flèches et Entrée', check: remoteMode === 'touches', act: function () { setRemoteMode('touches'); } },
            { section: 'Affichage' },
            { label: 'Étendu', check: displayMode === 0, act: function () { setDisplay(0); } },
            { label: 'Dupliquer', check: displayMode === 1, act: function () { setDisplay(1); } },
            { label: 'Télévision seule', check: displayMode === 2, act: function () { setDisplay(2); } }
        ];
        if (deviceCount > 1) rows.push({ label: 'Appareils seuls', check: displayMode === 3, act: function () { setDisplay(3); } });
        rows.push(
            { section: 'Son' },
            { label: 'Sur le PC', check: !audioOnTv, act: function () { setAudioRoute(false); } },
            { label: 'Sur la télévision', check: audioOnTv, act: function () { setAudioRoute(true); } }
        );
        rows.push(
            { section: 'Raccourcis' },
            { label: 'Menu Démarrer', act: function () { tap(VK.LWIN); } },
            { label: 'Afficher le bureau', act: function () { combo([VK.LWIN, VK.D]); } },
            { label: 'Fenêtre précédente', act: function () { combo([VK.ALT, VK.TAB]); } },
            { label: 'Toutes les fenêtres', act: function () { combo([VK.LWIN, VK.TAB]); } },
            { label: 'Clavier : écrire un texte', act: openKeyboard },
            { section: 'Télévision' },
            { label: 'Mesures', check: statsEl.style.display === 'block', act: toggleStats },
            { label: 'Autoriser un nouvel appairage', act: armPairing },
            { label: 'Quitter 36Screenlink', act: exitApp }
        );
        return rows;
    }
    function renderMenu() {
        menuList.innerHTML = '';
        menuRows.forEach(function (row, i) {
            var div = document.createElement('div');
            if (row.section) { div.className = 'menu-section'; div.textContent = row.section; }
            else {
                div.className = 'menu-row' + (i === menuSel ? ' sel' : '');
                div.textContent = (row.check ? '● ' : '') + row.label;
            }
            menuList.appendChild(div);
        });
        var sel = menuList.children[menuSel];
        if (sel && sel.scrollIntoView) sel.scrollIntoView({ block: 'nearest' });
    }
    function openMenu() {
        releaseAll();
        menuRows = menuModel();
        menuSel = 0;
        for (var i = 0; i < menuRows.length; i++) if (!menuRows[i].section) { menuSel = i; break; }
        menuOpen = true;
        menuEl.style.display = 'block';
        renderMenu();
    }
    function closeMenu() { menuOpen = false; menuEl.style.display = 'none'; }
    function moveMenu(dir) {
        var i = menuSel;
        do { i += dir; } while (i >= 0 && i < menuRows.length && menuRows[i].section);
        if (i >= 0 && i < menuRows.length) { menuSel = i; renderMenu(); }
    }
    function menuKey(k) {
        if (k === RC.UP) moveMenu(-1);
        else if (k === RC.DOWN) moveMenu(1);
        else if (k === RC.OK) { var row = menuRows[menuSel]; closeMenu(); if (row && row.act) row.act(); }
        else if (k === RC.BACK || MENU_KEYS[k]) closeMenu();
    }

    // ---------------------------------------------------------------- clavier du téléviseur
    var kbdEl = document.getElementById('kbd'), kbdInput = document.getElementById('kbd-input');
    var kbdOpen = false;
    function openKeyboard() {
        kbdOpen = true;
        kbdInput.value = '';
        kbdEl.style.display = 'flex';
        setTimeout(function () { kbdInput.focus(); }, 50);
    }
    function closeKeyboard(sendText) {
        if (!kbdOpen) return;
        kbdOpen = false;
        var text = kbdInput.value;
        kbdInput.blur();
        kbdEl.style.display = 'none';
        if (sendText && text && link) { link.text(text); toast('Texte envoyé au PC'); }
    }
    kbdInput.addEventListener('keydown', function (e) {
        // Valider : Entrée ou « Terminé » du clavier Samsung. Annuler : Retour ou « Annuler ».
        if (e.keyCode === 13 || e.keyCode === 65376) { e.preventDefault(); closeKeyboard(true); }
        else if (e.keyCode === KEY.BACK || e.keyCode === 65385) { e.preventDefault(); closeKeyboard(false); }
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
    // Durée déclarée de chaque image : recouvre l'écart jusqu'à la suivante, pour que le tampon du
    // lecteur reste une seule plage continue (voir player.cc).
    var SPAN = 0.25;

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
        this.appendedTotal = 0;
        this.firstClockOffset = null;
        this.audioPackets = 0;
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
        wasm.push(data.length, rel, SPAN, key ? 1 : 0);
        this.appendedTotal++;
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
    // Reconstruit le lecteur bloqué : la prochaine image clé recrée source et piste (player.cc).
    WasmPlayer.prototype.rebuild = function () {
        tvLog('lecteur bloqué : reconstruction');
        this.stuck = 0;
        wasmStarted = false;
        lastSent = -1; firstSec = -1;
        this.timeline = [];
        this.waitingKey = true;
        this.lastIdrRequest = 0;
        this.askKey();
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
        if (player && streaming && remoteMode === 'souris' && !menuOpen) pointerStep(performance.now());
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
        // Commandes de la télécommande vers le PC (le PC les ignore sur l'UAC et le verrouillage).
        link = {
            key: function (vk, down) { send(new Writer(MSG.KEY).u8(down ? 1 : 0).u16(0x8000 | vk).bytes()); },
            mouse: function (buttons, x, y, wheel) {
                send(new Writer(MSG.MOUSE).u8(buttons).u16(x).u16(y).u16(wheel & 0xFFFF).u16(0).bytes());
            },
            text: function (s) { send(Bytes.concat(new Uint8Array([MSG.TEXT]), encoder.encode(s))); },
            audioRoute: function (onTv) { send(new Writer(MSG.AUDIO_ROUTE).u8(onTv ? 1 : 0).u8(0xFF).bytes()); },
            control: function (id, v) { send(new Writer(MSG.CONTROL).u8(id).u8(v).bytes()); }
        };
        // Réglage à chaud de l'avance (mesures depuis le débogueur).
        window.__rebuild = function () { player.rebuild(); };
        window.__setSpan = function (v) { SPAN = v; tvLog('duree image : ' + (v * 1000).toFixed(0) + ' ms'); };
        window.__setLead = function (v) { LEAD = v; tvLog('avance max : ' + (v * 1000).toFixed(0) + ' ms'); };

        send(new Writer(MSG.HELLO).u16(1920).u16(1080).u16(60).u8(1).u8(KIND_TV).bytes());
        tvLog('lecteur WASM prêt, flux demandé 1920x1080');

        player = new WasmPlayer();
        player.requestIdr = function () { send(new Uint8Array([MSG.REQUEST_IDR])); };
        var statsAt = performance.now();
        var statsTimer = setInterval(function () {
            // Retard du minuteur lui-même : mesure du blocage du fil principal.
            var tickNow = performance.now(), stall = tickNow - statsAt - 1000;
            statsAt = tickNow;
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
            // Lecteur bloqué (piste refusée après un changement de taille, par exemple) : des
            // images arrivent mais aucune n'est décodée. Il ne s'en remet pas seul : on le reconstruit.
            player.stuck = s.frames > 0 && total === player.lastTotalBefore ? (player.stuck || 0) + 1 : 0;
            player.lastTotalBefore = total;
            if (player.stuck >= 6) player.rebuild();
            // Indicateurs de dérive lente : images en attente dans le décodeur (envoyées moins
            // décodées), octets en attente sur le réseau, mémoire, dérive d'horloge PC / téléviseur.
            var heap = performance.memory ? (performance.memory.usedJSHeapSize / 1e6).toFixed(0) + ' Mo' : '?';
            var drift = player.firstClockOffset === null ? 0 : (player.clockOffset - player.firstClockOffset) / 1000;
            lost += ', file decodeur ' + (player.appendedTotal - total - dropped) + ', reseau ' + (stream.length / 1024).toFixed(0) +
                    ' Ko, a dechiffrer ' + (player.inboxLength ? player.inboxLength() : 0) + ', son ' + player.audioPackets +
                    ' paquets, memoire ' + heap + ', derive horloge ' + drift.toFixed(1) + ' ms, plages ' + video.buffered.length + ', blocage ' + Math.max(0, stall).toFixed(0) + ' ms';
            player.audioPackets = 0;
            tvLog(s.frames + ' img/s, ' + mbps.toFixed(1) + ' Mb/s, avance ' + ahead.toFixed(0) + ' ms, latence ' + (s.avg / 1000).toFixed(0) + ' ms (p95 ' + (s.p95 / 1000).toFixed(0) +
                  '), reception ' + arrAvg.toFixed(0) + ' ms (max ' + arrMax.toFixed(0) + '), dechiffrement ' + dec.toFixed(1) + ' ms' + lost + ', pertes ' + s.drops);
        }, 1000);

        // Réception en deux temps : chaque message part au déchiffrement dès qu'il est lu (son nonce
        // est pris à l'appel, donc dans l'ordre), puis il est traité dans l'ordre d'arrivée. Attendre
        // chaque déchiffrement avant de lire le suivant plafonnait le nombre de messages par seconde :
        // avec le son du PC (une centaine de paquets par seconde en plus des images), la réception
        // décrochait et le retard grandissait jusqu'à plusieurs secondes.
        var inbox = [], wake = null, closed = false;
        async function receive() {
            for (;;) {
                var head = await stream.read(4);
                var length = ((head[0] << 24) >>> 0) + ((head[1] << 16) | (head[2] << 8) | head[3]);
                if (length < 16 || length > 8 * 1024 * 1024) throw new Error('enregistrement invalide');
                var sealedRec = await stream.read(length);
                var p = keys.rx.open(sealedRec);
                p.catch(function () {});   // l'erreur éventuelle est traitée par dispatch()
                inbox.push({ t0: performance.now(), p: p });
                if (wake) { var w = wake; wake = null; w(); }
            }
        }
        async function dispatch() {
            for (;;) {
                while (!inbox.length) await new Promise(function (resolve) { wake = resolve; });
                var item = inbox.shift();
                var plain = await item.p;
                if (closed) return;
                player.decSum += performance.now() - item.t0; player.decN++;
                handle(plain);
            }
        }
        player.inboxLength = function () { return inbox.length; };
        function handle(plain) {
                var r = new Reader(plain, 1);
                switch (plain[0]) {
                case MSG.STREAM_CONFIG:
                    r.u8();
                    var w = r.u16(), h = r.u16(), fps = r.u16();
                    player.configure(w, h, fps);
                    var first = !streaming;
                    streaming = true;
                    panel.style.display = 'none';
                    keepScreenOn();
                    updateKeys();
                    setSecure(secureScreen);
                    if (first && !hintShown) {
                        hintShown = true;
                        toast('Bouton Chaîne : menu · OK : clic · OK maintenu : clic droit · CH +/− : défiler', 7000);
                    }
                    break;
                case MSG.CONTROL_STATE:
                    displayMode = r.u8();
                    break;
                case MSG.DEVICES:
                    r.u8();
                    deviceCount = r.u8();
                    break;
                case MSG.SECURE_STATE:
                    setSecure(r.u8() !== 0);
                    break;
                case MSG.AUDIO_STATE:
                    setAudioOnTv(r.u8() !== 0);
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
                    if (player.firstClockOffset === null) player.firstClockOffset = player.clockOffset;
                    break;
                case MSG.CURSOR_POS:
                    cursor.visible = r.u8() !== 0;
                    cursor.x = r.i16(); cursor.y = r.i16();
                    // Le pointeur de la télécommande repart d'où est le vrai curseur, sauf pendant
                    // qu'on le déplace (la position renvoyée a alors un temps de retard).
                    if (cursor.visible && performance.now() - pointer.lastDriven > 400) { pointer.x = cursor.x; pointer.y = cursor.y; }
                    placeCursor();
                    break;
                case MSG.CURSOR_SHAPE:
                    cursorShape(r);
                    break;
                case MSG.AUDIO:
                    player.audioPackets++;
                    var rate = r.u32();
                    soundChunk(rate, plain.subarray(r.p));
                    break;
                default: break;
                }
        }

        try {
            await Promise.race([receive(), dispatch()]);
        } finally {
            closed = true;
            clearInterval(statsTimer);
            tvLog = function (text) { console.log(text); };
            window.__tvLog = null;
            releaseAll();
            closeMenu();
            closeKeyboard(false);
            link = null;
            streaming = false;
            updateKeys();
            setSecure(false);
            sound.next = 0;
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
                     (paired ? '' : 'Appuyez sur le bouton Chaîne pour autoriser l\'appairage, puis lancez 36Screenlink sur le PC. ') + '(' + e.message + ')');
            }
            await new Promise(function (r) { setTimeout(r, 1500); });
        }
    }

    show('Démarrage…', 'Appui sur le bouton Chaîne : nouvel appairage · Retour : quitter');
    main();
})();
