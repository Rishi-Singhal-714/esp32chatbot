// GET  /api/message  → returns last 10 messages for the web page
// POST /api/message  → ESP32 posts { user, bot } after each exchange

// In-memory store — persists within a Vercel function instance.
// For production, replace with Vercel KV or any DB.
const messages = [];

export default function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type");
  if (req.method === "OPTIONS") return res.status(204).end();

  if (req.method === "GET") {
    return res.status(200).json({ messages: messages.slice(-10) });
  }

  if (req.method === "POST") {
    const { user, bot } = req.body || {};
    if (user || bot) {
      messages.push({ user, bot, ts: Date.now() });
      if (messages.length > 50) messages.shift(); // cap at 50
    }
    return res.status(200).json({ ok: true });
  }

  res.status(405).end();
}
