// Octets : concaténation, entiers big-endian et lecture d'un flux reçu en messages WebSocket.
'use strict';

var Bytes = {
    concat: function () {
        var total = 0, i;
        for (i = 0; i < arguments.length; i++) total += arguments[i].length;
        var out = new Uint8Array(total), at = 0;
        for (i = 0; i < arguments.length; i++) { out.set(arguments[i], at); at += arguments[i].length; }
        return out;
    },
    ascii: function (text) {
        var out = new Uint8Array(text.length);
        for (var i = 0; i < text.length; i++) out[i] = text.charCodeAt(i);
        return out;
    },
    equal: function (a, b) {
        if (a.length !== b.length) return false;
        var diff = 0;
        for (var i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
        return diff === 0;
    },
    random: function (n) {
        var out = new Uint8Array(n);
        window.crypto.getRandomValues(out);
        return out;
    },
    hex: function (a) {
        var s = '';
        for (var i = 0; i < a.length; i++) s += (a[i] < 16 ? '0' : '') + a[i].toString(16);
        return s;
    },
    fromHex: function (s) {
        var out = new Uint8Array(s.length / 2);
        for (var i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
        return out;
    }
};

// Rédacteur big-endian, comme java.nio.ByteBuffer et proto::Writer.
function Writer(type) {
    this.parts = [];
    this.u8(type);
}
Writer.prototype.u8 = function (v) { this.parts.push(v & 255); return this; };
Writer.prototype.u16 = function (v) { this.parts.push((v >>> 8) & 255, v & 255); return this; };
Writer.prototype.u32 = function (v) { this.parts.push((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255); return this; };
// Entiers jusqu'à 2^53 : assez pour des microsecondes.
Writer.prototype.u64 = function (v) {
    var high = Math.floor(v / 4294967296), low = v - high * 4294967296;
    return this.u32(high).u32(low);
};
Writer.prototype.bytes = function () { return new Uint8Array(this.parts); };

function Reader(bytes, offset) {
    this.b = bytes;
    this.p = offset || 0;
}
Reader.prototype.remaining = function () { return this.b.length - this.p; };
Reader.prototype.u8 = function () { return this.b[this.p++]; };
Reader.prototype.u16 = function () { var v = (this.b[this.p] << 8) | this.b[this.p + 1]; this.p += 2; return v; };
Reader.prototype.i16 = function () { var v = this.u16(); return v > 32767 ? v - 65536 : v; };
Reader.prototype.u32 = function () { var v = ((this.b[this.p] << 24) >>> 0) + ((this.b[this.p + 1] << 16) | (this.b[this.p + 2] << 8) | this.b[this.p + 3]); this.p += 4; return v; };
Reader.prototype.u64 = function () { var high = this.u32(), low = this.u32(); return high * 4294967296 + low; };
// Entier signé sur 64 bits (décalage d'horloge) : complément à deux.
Reader.prototype.i64 = function () {
    var high = this.u32(), low = this.u32();
    if (high >= 0x80000000) return -((~high >>> 0) * 4294967296 + (~low >>> 0) + 1);
    return high * 4294967296 + low;
};
Reader.prototype.take = function (n) { var v = this.b.subarray(this.p, this.p + n); this.p += n; return v; };

// Flux d'octets reconstitué à partir des messages WebSocket : lecture d'une taille exacte.
function ByteStream() {
    this.chunks = [];
    this.head = 0;   // premier morceau non lu : avancer un index coûte moins que décaler le tableau
    this.length = 0;
    this.waiting = null;
    this.closed = false;
}
ByteStream.prototype.push = function (chunk) {
    this.chunks.push(chunk);
    this.length += chunk.length;
    this.wake();
};
ByteStream.prototype.close = function () {
    this.closed = true;
    this.wake();
};
ByteStream.prototype.wake = function () {
    if (!this.waiting) return;
    var w = this.waiting;
    if (this.closed && this.length < w.n) { this.waiting = null; w.reject(new Error('connexion fermée')); return; }
    if (this.length < w.n) return;
    this.waiting = null;
    w.resolve(this.consume(w.n));
};
ByteStream.prototype.consume = function (n) {
    var c = this.chunks[this.head], out;
    if (c.length === n) {
        // Cas courant : un message WebSocket contient exactement ce qui est demandé, sans recopie.
        out = c;
        this.chunks[this.head++] = null;
    } else {
        out = new Uint8Array(n);
        var at = 0;
        while (at < n) {
            c = this.chunks[this.head];
            var take = Math.min(c.length, n - at);
            out.set(c.subarray(0, take), at);
            at += take;
            if (take === c.length) this.chunks[this.head++] = null; else this.chunks[this.head] = c.subarray(take);
        }
    }
    this.length -= n;
    if (this.head > 256 && this.head * 2 > this.chunks.length) { this.chunks = this.chunks.slice(this.head); this.head = 0; }
    return out;
};
ByteStream.prototype.read = function (n) {
    var self = this;
    if (this.length >= n) return Promise.resolve(this.consume(n));
    if (this.closed) return Promise.reject(new Error('connexion fermée'));
    return new Promise(function (resolve, reject) { self.waiting = { n: n, resolve: resolve, reject: reject }; });
};
