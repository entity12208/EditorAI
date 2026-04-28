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

### EditorAI Platinum *(Free - servers WIP)*
[EditorAI Platinum](https://github.com/entity12208/editorai-platinum) is a community proxy that lets anyone run the official EditorAI Ollama models over the internet for free (subject to availability).

1. Open settings → set **Provider** to `ollama`
2. Switch to the **Provider** tab and enable **Platinum**. If it says any http error, Platinum is offline.
3. Available models are auto-detected from the Platinum network

Want to donate your computer to the network? Follow the instructions in the [Platinum repository](https://github.com/entity12208/editorai-platinum).

### Ollama *(Free — recommended for 6+ GB VRAM)*
*Uses ~5.2 GB storage. Fully local and private — no internet required after setup.*

1. Download and install [Ollama](https://ollama.com)
2. Pull any model
3. Open settings → set **Provider** to `ollama`
4. Select the model you want

### Claude *(Paid)*
1. Get an API key at [console.anthropic.com](https://console.anthropic.com)
2. Open settings → set **Provider** to `claude` → switch to **Provider** tab
3. Select a model and paste your API key

### Mistral AI *(Paid)*
1. Get an API key at [console.mistral.ai](https://console.mistral.ai/api-keys/)
2. Open settings → set **Provider** to `ministral` → switch to **Provider** tab
3. Select a model and paste your API key

### HuggingFace *(Free tier available)*
1. Get a write token at [huggingface.co/settings/tokens](https://huggingface.co/settings/tokens) 
2. Open settings → set **Provider** to `huggingface` → switch to **Provider** tab
3. Enter a model ID and paste your token 

### ChatGPT *(Paid)*
1. Get an API key at [platform.openai.com/api-keys](https://platform.openai.com/api-keys)
2. Open settings → set **Provider** to `openai` → switch to **Provider** tab
3. Select a model and paste your API key

### Gemini *(Not recommended — tight rate limits)*
1. Get an API key at [aistudio.google.com/api-keys](https://aistudio.google.com/u/0/api-keys)
2. Open settings → set **Provider** to `gemini` → switch to **Provider** tab
3. Select a model and paste your API key

## Usage
1. Open the editor of any level
2. Press the **AI** button in the editor toolbar
3. Type a description of what you want (e.g. "medium difficulty platforming level")
4. Click **Generate** and wait — a timer shows elapsed time
5. AI-generated objects appear as **blue ghost previews**
6. Choose **Accept**, **Edit**, or **Deny**:
   - **Accept** — makes objects permanent
   - **Edit** — makes objects solid so you can move/delete them, then press **Done**
   - **Deny** — removes all generated objects
7. Optionally rate the generation 1-10 to help the AI learn

Use the **up/down arrows** next to the text input to recall previous prompts. Press **Cancel** during generation to abort.

## Settings

Click the **gear icon** in the top-left of the AI popup to access all settings, organized into three tabs:

| Tab | Settings |
|-----|----------|
| **General** | Provider, difficulty, style, length, max objects, spawn speed |
| **Provider** | Model selection, API keys, Ollama/Platinum config, timeout |
| **Advanced** | Triggers & colors, rate limiting, feedback & learning |

## Advanced Features
Enable **Triggers/Colors** in the Advanced settings tab to unlock:
- **Color Triggers** — smooth color transitions for background, ground, and objects
- **Move Triggers** — moving platforms and obstacles with easing curves
- **Rotate Triggers** — spinning decorations and obstacles around center points
- **Alpha Triggers** — fade objects in and out
- **Toggle Triggers** — show/hide groups of objects
- **Pulse Triggers** — flash colors on groups for visual emphasis
- **Spawn & Stop Triggers** — chain trigger sequences and cancel animations
- **Speed Portals** — control game speed (0.5x to 4x)
- **Player Visibility** — show/hide the player icon and trail
- **Color Channels** — assign objects to color channels for coordinated color changes
- **Group IDs** — assign objects to groups for trigger targeting
- **Multi-Activate** — make orbs, pads, and triggers fire on every touch

These features work best with smarter models (Claude, Gemini, Ollama qwen, or Platinum).

## Feedback & Learning
When enabled (on by default), a rating popup appears after you accept/edit/deny a generation. Your ratings and optional text feedback are stored locally and injected into future AI prompts as examples — the AI learns what you like and avoids what you don't. 

## Fine-Tune Your Own Model
Feel free to join our [Huggingface org](https://huggingface.co/EditorAI-Geode/) to publish your models trained on a EditorAI dataset.

## Support
Join the [Editor AI Discord server](https://discord.gg/5hwCqMUYNj) for support and ideas.
