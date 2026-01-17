# Editor AI

AI-powered level generation for Geometry Dash. Describe your level in plain text and watch AI build it in the editor.

*Consider supporting the project by starring it on [Github](https://github.com/entity12208/editorai)*

## Quick Start

1. Download the [latest release](https://github.com/entity12208/editorai/releases/latest)
2. Open the game and navigate to the Geode page
3. Press the manual download button and select the file
4. Set up the mod

## AI Setup

### Ollama

*Though Ollama does use ~5.2 GB storage, it is still the recommended option for anyone who has 6+ GB VRAM, as it is free permanently and easy to set up.*

1. Download and install [Ollama](https://ollama.com)
2. Download the official model by running `ollama pull entity12208/editorai:<model>`, where `model` can be either `deepseek` (more creative) or `qwen` (more powerful). Each is 5.2 GB.
3. Ensure the setting `ollama model` is set to `entity12208/editorai:<model>`.
4. Change the other settings to your requirements.

### Gemini, Claude, ChatGPT

1. Get your API key for [Gemini](https://aistudio.google.com/u/0/api-keys), [Claude](https://console.anthropic.com), or [ChatGPT](https://platform.openai.com/api-keys).
2. Set the setting `Model` to your perfered model used by your provider
3. Once in the UI, click the lock icon.
4. Paste your API key into the box and save it.

## Usage

1. Download and set up the mod.
2. Open the editor of any level.
3. Press the AI button located in the top right.
4. Enable or disable `Clear Level`, and enter your prompt into the box.
5. Click `Generate` and wait!

## Support

Join the [Editor AI Discord server](https://discord.gg/5hwCqMUYNj) for support and ideas.
