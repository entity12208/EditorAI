# Editor AI
AI-powered level generation for Geometry Dash. Describe your level in plain text and watch AI build it in the editor.

*Consider supporting the project by starring it on [GitHub](https://github.com/entity12208/editorai)*

## Quick Start
1. Download the [latest release](https://github.com/entity12208/editorai/releases/latest)
2. Open the game and navigate to the Geode page
3. Press the manual download button and select the file
4. Open the editor and press the **AI** button
5. Click the **gear icon** to configure your AI provider

## AI Setup

All settings are configured in-game via the **gear icon** in the AI popup.

### EditorAI Platinum *(Free — recommended)*
[EditorAI Platinum](https://github.com/entity12208/editorai-platinum) is a community proxy that lets anyone use the official EditorAI models for free.

1. Open settings → set **Provider** to `ollama`
2. Switch to the **Provider** tab and enable **Platinum**
3. Models are auto-detected from the Platinum network

### Ollama *(Free — local)*
1. Install [Ollama](https://ollama.com) and pull a model: `ollama pull entity12208/editorai:qwen`
2. Open settings → set **Provider** to `ollama`
3. Installed models are auto-detected

### Cloud Providers
Claude, OpenAI, Gemini, Mistral, and HuggingFace are supported. Open settings → select your provider → switch to the Provider tab to enter your API key and select a model.

## Features
- **Blueprint Preview** — AI objects appear as blue ghosts. Accept, edit, or deny before committing.
- **Edit Mode** — make previewed objects solid, rearrange them, then press Done.
- **15 Trigger Types** — color, move, rotate, alpha, toggle, pulse, spawn, stop triggers, speed portals, player visibility, and more.
- **Color Channels** — assign objects to channels for coordinated color changes.
- **Feedback & Learning** — rate generations 1-10 with optional text. The AI learns your style over time.
- **Prompt History** — recall previous prompts with up/down arrows.
- **Cancel Generation** — abort a request at any time.
- **Error Codes** — every error includes a diagnostic code (e.g. `[EAI-G2001]`) for fast troubleshooting.
- **In-Game Settings** — all configuration via a tabbed settings popup, no digging through Geode menus.
- **Auto-Detect Models** — Ollama and Platinum models are listed automatically.

## Support
Join the [Editor AI Discord server](https://discord.gg/5hwCqMUYNj) for support and ideas.
