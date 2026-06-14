# Editor AI

Describe a Geometry Dash level in plain words and watch it get built for you. EditorAI talks to the AI of your choice, then places the result in the editor on its own preview layer you can accept, edit, or deny — your level, your call, always.

*Consider supporting the project by starring it on [GitHub](https://github.com/entity12208/editorai).*

## Install

1. Download the [latest release](https://github.com/entity12208/editorai/releases/latest) (`.geode` file)
2. In GD, open the **Geode** page → press the **manual install** button → pick the file
3. Restart the game

> EditorAI is distributed here rather than on the Geode index, whose guidelines don't allow AI-placed objects.

## Open the panel

Press **E** anywhere in the game (desktop), or tap the floating **AI** bubble (mobile). Everything lives in that panel: starting generations, watching them run, chatting with the AI, and every setting.

## Pick an AI

Open the panel → **Settings** → choose a provider. Two are free with zero accounts:

- **Platinum** *(free, community-run — servers by [VLT GG](https://vltgg.net))* — set provider to `ollama`, enable **Use Platinum**. Your prompts run on volunteer machines, so keep personal info out of them. Want to donate compute? See the [Platinum repository](https://github.com/entity12208/editorai-platinum).
- **Ollama** *(free, fully local — recommended with 6+ GB VRAM)* — install [Ollama](https://ollama.com), pull a model, set provider to `ollama`, pick the model. Nothing leaves your machine.

For hosted providers, pick one and either press **Sign in with browser** (OpenRouter, HuggingFace — no key-copying needed) or paste an API key:

| Provider | Get a key |
|---|---|
| OpenRouter | sign-in button, or [openrouter.ai/keys](https://openrouter.ai/keys) |
| HuggingFace | sign-in button, or [huggingface.co/settings/tokens](https://huggingface.co/settings/tokens) |
| Claude | [console.anthropic.com](https://console.anthropic.com) |
| OpenAI | [platform.openai.com/api-keys](https://platform.openai.com/api-keys) |
| Mistral | [console.mistral.ai](https://console.mistral.ai/api-keys/) |
| DeepSeek | [platform.deepseek.com](https://platform.deepseek.com) |
| Gemini | [aistudio.google.com](https://aistudio.google.com/api-keys) *(tight rate limits)* |

LM Studio, llama.cpp, and any OpenAI-compatible endpoint work too — point the URL at your server.

## Use it

1. Panel → **Generate**
2. Pick a target: the current editor, a brand-new level, or any of your created levels
3. Describe what you want, set difficulty/style/length (or type your own), press **Generate**
4. Watch it think and work live in **Sessions** — you can close the panel, or even the editor, while it runs
5. The result appears on its own editor layer (the rest of the level fades back): **Accept**, **Edit**, or **Deny**
6. Keep talking to it in Sessions to refine, plan, or just ask questions
7. Rate generations when asked — your ratings teach the AI your taste over time

Every option in the panel has a hover tooltip explaining what it does.

## Fine-tune your own model

Join the [HuggingFace org](https://huggingface.co/EditorAI-Geode/) to publish models trained on EditorAI datasets.

## Support

Found a bug or have an idea? [Open an issue](https://github.com/entity12208/editorai/issues).
