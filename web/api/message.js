const { callDevice, clean, cleanColor, cleanSong, MAX_BITMAP_W } = require('./_device');
const { isAuthed } = require('./_auth');

// POST {text, color, song, width, bitmap}: log-in required; pushed straight to the ESP32 over an
// encrypted TCP socket. `bitmap` is base64 RGB565 (big-endian), row-major, `width` x 16 pixels:
// the browser renders the text itself, so Chinese, emoji, etc. work without fonts on the ESP32.
//
// Plaintext sent to the device ("B" request):
//   'B' | song(1) | width(2, BE) | textLen(2, BE) | R G B (3) | text (UTF-8) | pixels (width*16*2)
module.exports = async (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  if (req.method !== 'POST') {
    res.setHeader('Allow', 'POST');
    return res.status(405).json({ error: 'Method not allowed' });
  }
  if (!isAuthed(req)) return res.status(401).json({ error: 'Not logged in' });

  let body = req.body || {};
  if (typeof body === 'string') {
    try { body = JSON.parse(body); } catch (e) { body = {}; }
  }
  const text = clean(body.text);
  if (!text) return res.status(400).json({ error: 'Text is empty' });
  const color = cleanColor(body.color);
  const song = cleanSong(body.song);

  const width = Number(body.width);
  const pixels = Buffer.from(String(body.bitmap || ''), 'base64');
  if (!Number.isInteger(width) || width < 1) return res.status(400).json({ error: 'Bad image' });
  if (width > MAX_BITMAP_W) return res.status(400).json({ error: 'Message too long for the matrix' });
  if (pixels.length !== width * 16 * 2) return res.status(400).json({ error: 'Bad image' });

  const textBytes = Buffer.from(text, 'utf8');
  const head = Buffer.alloc(9);
  head[0] = 0x42;                                   // 'B'
  head[1] = song;
  head.writeUInt16BE(width, 2);
  head.writeUInt16BE(textBytes.length, 4);
  head[6] = parseInt(color.slice(1, 3), 16);
  head[7] = parseInt(color.slice(3, 5), 16);
  head[8] = parseInt(color.slice(5, 7), 16);

  try {
    const r = await callDevice(Buffer.concat([head, textBytes, pixels]));
    if (r === 'OK') return res.status(200).json({ ok: true, text, color, song });
    return res.status(502).json({ error: 'Device refused the message' });
  } catch (e) {
    return res.status(502).json({ error: e.message === 'Device is not configured' ? e.message : 'Device unreachable' });
  }
};
