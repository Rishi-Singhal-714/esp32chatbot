// POST /api/stt
// Body: raw WAV bytes (Content-Type: audio/wav)
// Returns: { text: "transcribed text" }

import OpenAI, { toFile } from "openai";

export const config = { api: { bodyParser: false } };

const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });

function rawBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

export default async function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  if (req.method === "OPTIONS") return res.status(204).end();
  if (req.method !== "POST") return res.status(405).end();

  try {
    const buf = await rawBody(req);
    const file = await toFile(buf, "audio.wav", { type: "audio/wav" });

    const transcription = await openai.audio.transcriptions.create({
      file,
      model: "gpt-4o-transcribe",
      response_format: "text",
      prompt: "Voice command for an ESP32 IoT device assistant.",
    });

    res.status(200).json({ text: transcription });
  } catch (err) {
    const detail = err?.message || String(err);
    console.error("STT error:", detail);
    res.status(500).json({ error: "Transcription failed", detail });
  }
}
