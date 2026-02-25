# Editor AI
AI-powered level generation for Geometry Dash. Describe your level in plain text and watch AI build it in the editor.

*Consider supporting the project by starring it on [GitHub](https://github.com/entity12208/editorai)*

## Quick Start
1. Download the [latest release](https://github.com/entity12208/editorai/releases/latest)
2. Open the game and navigate to the Geode page
3. Press the manual download button and select the file
4. Set up the mod using one of the AI providers below

## AI Setup

### EditorAI Platinum *(Free — recommended)*
[EditorAI Platinum](https://github.com/entity12208/editorai-platinum) is a community proxy that lets anyone run the official EditorAI Ollama models over the internet for free.

1. In mod settings, set **AI Provider** to `ollama`
2. Enable the **Use Platinum** toggle
3. Select a model from the **Ollama Model** dropdown:
   - `entity12208/editorai:qwen` — more powerful
   - `entity12208/editorai:deepseek` — more creative

Want to donate your computer to the network? Follow the instructions in the [Platinum repository](https://github.com/entity12208/editorai-platinum).

### Ollama *(Free — recommended for 6+ GB VRAM)*
*Uses ~5.2 GB storage. Fully local and private — no internet required after setup.*

1. Download and install [Ollama](https://ollama.com)
2. Pull the official model: `ollama pull entity12208/editorai:<model>`
   - `deepseek` — more creative &nbsp;|&nbsp; `qwen` — more powerful &nbsp;(each ~5.2 GB)
3. In mod settings, set **AI Provider** to `ollama` and leave **Use Platinum** off
4. Select your model from the **Ollama Model** dropdown

### Claude *(Paid)*
1. Get an API key at [console.anthropic.com](https://console.anthropic.com)
2. In mod settings, set **AI Provider** to `claude` and choose a model
3. Paste your key into the **Claude API Key** field in mod settings

### Mistral AI *(Paid)*
1. Get an API key at [console.mistral.ai](https://console.mistral.ai/api-keys/)
2. In mod settings, set **AI Provider** to `ministral` and choose a model
3. Paste your key into the **Mistral API Key** field in mod settings

### HuggingFace *(Free tier available)*
1. Get a token at [huggingface.co/settings/tokens](https://huggingface.co/settings/tokens)
2. In mod settings, set **AI Provider** to `huggingface`
3. Enter a model ID in the model field (default: `meta-llama/Llama-3.1-8B-Instruct`)
4. Paste your token into the **HuggingFace API Key** field in mod settings

### ChatGPT *(Paid)*
1. Get an API key at [platform.openai.com/api-keys](https://platform.openai.com/api-keys)
2. In mod settings, set **AI Provider** to `openai` and choose a model
3. Paste your key into the **OpenAI API Key** field in mod settings

### Gemini *(Not recommended — tight rate limits)*
1. Get an API key at [aistudio.google.com/api-keys](https://aistudio.google.com/u/0/api-keys)
2. In mod settings, set **AI Provider** to `gemini` and choose a model
3. Paste your key into the **Gemini API Key** field in mod settings

## Usage
1. Download and set up the mod
2. Open the editor of any level
3. Press the **AI** button in the top right
4. Enable or disable **Clear Level**, and enter your prompt
5. Click **Generate** and wait!

## Advanced Features
Enable **Advanced Features** in mod settings to unlock:
- **Color Triggers** — AI can place color triggers that smoothly transition background, ground, and object colors
- **Move Triggers** — AI can create moving platforms and obstacles using grouped objects and move triggers with easing
- **Group IDs** — AI assigns group IDs to objects so move triggers can target them precisely

These features work best with smarter models (Claude, Ollama qwen, or Platinum).

## Support
Join the [Editor AI Discord server](https://discord.gg/5hwCqMUYNj) for support and ideas.
