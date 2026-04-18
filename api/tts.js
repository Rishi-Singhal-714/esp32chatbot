// POST /api/tts
// Body: { "text": "text to speak" }
// Returns: raw PCM audio — 24000 Hz, 16-bit, mono, little-endian
// ESP32 plays this directly via I2S at 24000 Hz

import OpenAI from "openai";

const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });

export default async function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type");
  if (req.method === "OPTIONS") return res.status(204).end();
  if (req.method !== "POST") return res.status(405).end();

  const { text } = req.body || {};
  if (!text) return res.status(400).json({ error: "text required" });

  try {
    const audio = await openai.audio.speech.create({
      model: "gpt-4o-mini-tts",
      voice: "cedar",
      input: text,
      instructions: "Speak in a calm, clear, professional tone for an IoT device assistant.",
      response_format: "pcm", // 24000 Hz, 16-bit, mono, little-endian — ready for ESP32 I2S
    });

    const buffer = Buffer.from(await audio.arrayBuffer());
    res.setHeader("Content-Type", "audio/pcm");
    res.setHeader("X-Sample-Rate", "24000");
    res.setHeader("X-Bit-Depth", "16");
    res.status(200).send(buffer);
  } catch (err) {
    console.error("TTS:", err.message);
    res.status(500).json({ error: "TTS failed" });
  }
}
