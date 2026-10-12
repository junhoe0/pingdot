const net = require('net');
const crypto = require('crypto');

const MAX_TEXT_LEN = 100;
const SONG_COUNT = 8;            // 0 = none; keep in sync with the firmware and index.html
const DEFAULT_COLOR = '#00c8ff';

// Any Unicode (Chinese, emoji, ...) is fine now: the browser turns the text into a picture.
// Control characters are dropped; at most MAX_TEXT_LEN characters (code points).
function clean(t) {
  return Array.from(String(t || ''))
    .filter(ch => { const c = ch.codePointAt(0); return c >= 32 && c !== 127 && !(c >= 0x80 && c < 0xa0); })
    .slice(0, MAX_TEXT_LEN).join('').trim();
}

function cleanColor(c) {
  return /^#[0-9a-fA-F]{6}$/.test(String(c)) ? String(c).toLowerCase() : DEFAULT_COLOR;
}

function cleanSong(s) {
  const n = Number.parseInt(s, 10);
  return Number.isInteger(n) && n >= 0 && n < SONG_COUNT ? n : 0;
}

// Encrypted TCP exchange with the ESP32 (no TLS, no certificates):
//   1. ESP32 -> us:  16 random bytes (challenge, fresh for every connection)
//   2. us -> ESP32:  [2-byte length][12-byte IV | AES-256-GCM ciphertext | 16-byte tag]
//                    key = DEVICE_KEY, AAD = challenge          (so a recorded request can't be replayed)
//   3. ESP32 -> us:  same framing, AAD = challenge + "R"
// Plaintext requests: "S" (status) or a binary "B" message, see message.js.
function callDevice(plaintext) {
  return new Promise((resolve, reject) => {
    const keyHex = process.env.DEVICE_KEY || '';
    if (!process.env.DEVICE_HOST || !/^[0-9a-fA-F]{64}$/.test(keyHex)) {
      return reject(new Error('Device is not configured'));
    }
    const key = Buffer.from(keyHex, 'hex');
    const sock = net.connect({ host: process.env.DEVICE_HOST, port: Number(process.env.DEVICE_PORT || 8443) });
    sock.setTimeout(7000, () => sock.destroy(new Error('Timed out')));
    sock.setNoDelay(true);

    let buf = Buffer.alloc(0);
    let challenge = null;
    let done = false;
    const finish = (err, val) => { if (done) return; done = true; sock.destroy(); err ? reject(err) : resolve(val); };

    sock.on('error', e => finish(e));
    sock.on('close', () => finish(new Error('Device closed the connection (wrong DEVICE_KEY?)')));
    sock.on('data', d => {
      buf = Buffer.concat([buf, d]);
      if (!challenge) {
        if (buf.length < 16) return;
        challenge = buf.subarray(0, 16);
        buf = buf.subarray(16);
        const iv = crypto.randomBytes(12);
        const c = crypto.createCipheriv('aes-256-gcm', key, iv);
        c.setAAD(challenge);
        const ct = Buffer.concat([c.update(Buffer.isBuffer(plaintext) ? plaintext : Buffer.from(plaintext, 'utf8')), c.final()]);
        const frame = Buffer.concat([iv, ct, c.getAuthTag()]);
        const len = Buffer.alloc(2);
        len.writeUInt16BE(frame.length);
        sock.write(Buffer.concat([len, frame]));
      }
      if (challenge && buf.length >= 2) {
        const n = buf.readUInt16BE(0);
        if (n < 28 || n > 512) return finish(new Error('Bad reply'));
        if (buf.length < 2 + n) return;
        const f = buf.subarray(2, 2 + n);
        try {
          const dc = crypto.createDecipheriv('aes-256-gcm', key, f.subarray(0, 12));
          dc.setAAD(Buffer.concat([challenge, Buffer.from('R')]));
          dc.setAuthTag(f.subarray(f.length - 16));
          finish(null, Buffer.concat([dc.update(f.subarray(12, f.length - 16)), dc.final()]).toString('utf8'));
        } catch (e) {
          finish(new Error('Reply failed authentication'));
        }
      }
    });
  });
}

module.exports = { callDevice, clean, cleanColor, cleanSong, MAX_BITMAP_W: 800 };
