// Protocole chiffré de 36Screenlink, côté appareil : la télévision joue le rôle de la tablette.
// Voir docs/02-protocole.md. Seul le transport change : des messages WebSocket.
'use strict';

var Secure = (function () {
    var subtle = window.crypto.subtle;
    var MAGIC = Bytes.ascii('S36X');
    var VERSION = 5;
    var MODE_PAIR = 1;
    var STATUS = { OK: 0, NOT_PAIRED: 1, BAD_VERSION: 2, PAIR_REFUSED: 3 };
    var STORE = '36screenlink.psk';

    function reply(status) { return Bytes.concat(MAGIC, new Uint8Array([status])); }

    async function hmac(key, label, data) {
        var k = await subtle.importKey('raw', key, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
        return new Uint8Array(await subtle.sign('HMAC', k, Bytes.concat(Bytes.ascii(label), data)));
    }

    // Clé P-256 éphémère ; le protocole échange les points sans l'octet 0x04 initial.
    async function ecdh() {
        var pair = await subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, true, ['deriveBits']);
        var raw = new Uint8Array(await subtle.exportKey('raw', pair.publicKey));
        return {
            publicBytes: raw.subarray(1),
            agree: async function (peer) {
                var key = await subtle.importKey('raw', Bytes.concat(new Uint8Array([4]), peer), { name: 'ECDH', namedCurve: 'P-256' }, false, []);
                return new Uint8Array(await subtle.deriveBits({ name: 'ECDH', public: key }, pair.privateKey, 256));
            }
        };
    }

    // Clé d'appairage : seule donnée gardée par l'application, dans son propre stockage.
    function loadPsk() {
        try { var s = localStorage.getItem(STORE); return s ? Bytes.fromHex(s) : null; } catch (e) { return null; }
    }
    function savePsk(psk) { try { localStorage.setItem(STORE, Bytes.hex(psk)); } catch (e) {} }
    function forget() { try { localStorage.removeItem(STORE); } catch (e) {} }

    // Identifiant public de la clé, annoncé au PC pour qu'il choisisse la bonne.
    async function keyId(psk) { return Bytes.hex((await hmac(psk, 'id', new Uint8Array(0))).subarray(0, 8)); }

    // AES-256-GCM à nonce compteur : 4 octets nuls puis le compteur sur 8 octets big-endian.
    async function gcm(rawKey) {
        var key = await subtle.importKey('raw', rawKey, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']);
        var counter = 0;
        function iv() {
            var n = new Uint8Array(12), c = counter++;
            for (var i = 11; i >= 4; i--) { n[i] = c % 256; c = Math.floor(c / 256); }
            return n;
        }
        return {
            seal: async function (plain) { return new Uint8Array(await subtle.encrypt({ name: 'AES-GCM', iv: iv() }, key, plain)); },
            open: async function (sealed) { return new Uint8Array(await subtle.decrypt({ name: 'AES-GCM', iv: iv() }, key, sealed)); }
        };
    }

    // Appairage demandé par le PC. confirm(code) montre le code et renvoie une promesse :
    // vrai si l'utilisateur confirme que le PC affiche le même.
    async function pair(stream, send, nonceC, allowPairing, confirm) {
        var peer = await stream.read(64);
        if (!allowPairing()) { send(reply(STATUS.PAIR_REFUSED)); throw new Error('appairage refusé'); }
        var key = await ecdh();
        var secret = await key.agree(peer);
        send(Bytes.concat(reply(STATUS.OK), key.publicBytes));
        var transcript = Bytes.concat(nonceC, peer, key.publicBytes);
        var sas = await hmac(secret, 'sas', transcript);
        var code = ((sas[0] * 16777216) + (sas[1] << 16) + (sas[2] << 8) + sas[3]) % 1000000;
        if (!(await confirm(code))) { send(reply(STATUS.PAIR_REFUSED)); throw new Error('code refusé'); }
        var psk = await hmac(secret, 'psk', transcript);
        var mac = await stream.read(32);
        if (!Bytes.equal(mac, await hmac(psk, 'cli', transcript))) { send(reply(STATUS.PAIR_REFUSED)); throw new Error('confirmation invalide'); }
        savePsk(psk);
        send(Bytes.concat(reply(STATUS.OK), await hmac(psk, 'srv', transcript)));
        return 'paired';
    }

    // Poignée de main complète. Renvoie { rx, tx } pour une session, ou 'paired' après un appairage.
    async function accept(stream, send, options) {
        var hello = await stream.read(6);
        if (!Bytes.equal(hello.subarray(0, 4), MAGIC)) throw new Error('PC inconnu');
        if (hello[4] !== VERSION) { send(reply(STATUS.BAD_VERSION)); throw new Error('version du PC différente : mettez les deux à jour'); }
        var nonceC = await stream.read(16);
        if (hello[5] === MODE_PAIR) return pair(stream, send, nonceC, options.allowPairing, options.confirm);

        var pubA = await stream.read(64);
        var psk = loadPsk();
        if (!psk) { send(reply(STATUS.NOT_PAIRED)); throw new Error('non appairée'); }
        // ECDH éphémère : une clé d'appairage volée plus tard ne déchiffre pas cette session.
        var key = await ecdh();
        var secret = await key.agree(pubA);
        var nonceS = Bytes.random(16);
        var context = Bytes.concat(nonceC, nonceS, pubA, key.publicBytes);
        send(Bytes.concat(reply(STATUS.OK), nonceS, key.publicBytes, await hmac(psk, 'srv', context)));
        var mac = await stream.read(32);
        if (!Bytes.equal(mac, await hmac(psk, 'cli', context))) throw new Error('PC non authentifié');
        var keyed = Bytes.concat(context, secret);
        return { rx: await gcm(await hmac(psk, 'a2t', keyed)), tx: await gcm(await hmac(psk, 't2a', keyed)) };
    }

    return { accept: accept, loadPsk: loadPsk, forget: forget, keyId: keyId };
})();
