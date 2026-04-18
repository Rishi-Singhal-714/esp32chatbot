// POST /api/chat
// Body: { "message": "user text" }
// Returns: { "reply": "assistant text" }

import OpenAI from "openai";

const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });

const SYSTEM_PROMPT = `You are a calm, professional voice assistant embedded in an ESP32 device.

Rules:
- Only answer questions related to device control, status, or IoT commands.
- If a question is out of scope, say: "I'm not able to help with that, but I'm here for device-related assistance."
- Never express frustration or negative emotions. Stay composed and helpful.
- Keep responses short and clear — they will be spoken aloud.
- Use simple language. No markdown, bullet points, or special characters.`;

export default async function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type");
  if (req.method === "OPTIONS") return res.status(204).end();
  if (req.method !== "POST") return res.status(405).end();

  const { message } = req.body || {};
  if (!message) return res.status(400).json({ error: "message required" });

  try {
    const completion = await openai.chat.completions.create({
      model: "gpt-4o-mini",
      messages: [
        { role: "system", content: SYSTEM_PROMPT },
        { role: "user", content: message },
      ],
      max_tokens: 150,
      temperature: 0.4,
    });

    const reply = completion.choices[0].message.content.trim();
    res.status(200).json({ reply });
  } catch (err) {
    console.error("Chat:", err.message);
    res.status(500).json({ error: "GPT failed" });
  }
}
