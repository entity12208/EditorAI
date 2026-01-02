# Editor AI

AI-powered level generation for Geometry Dash. Describe your level in plain text and watch AI build it in the editor.

## Features

- 🤖 **Multiple AI Providers** - Gemini (free), Claude, ChatGPT, or Ollama (local)
- 📦 **1000+ Objects** - Full GD object library with auto-updates
- 🎨 **Full Control** - Colors, groups, triggers, and advanced properties
- ⚡ **Fast & Stable** - Progressive creation with live progress
- 🔒 **Private** - Keys stored locally, no telemetry

## Quick Start

1. Install from Geode mod browser
2. Open editor → Click **AI** button (top-right)
3. **For Ollama**: Download from [ollama.com](https://ollama.com), run `ollama run llama2`, skip API key
4. **For Cloud AI**: Click lock icon → Enter API key from [ai.google.dev](https://ai.google.dev) (free)
5. Type prompt and generate!

## Setup

### Ollama (Recommended - FREE & Fast)
1. Download [ollama.com](https://ollama.com)
2. Run `ollama run llama2`
3. No API key needed
4. **Benefits**: Free, unlimited, private, offline

### Cloud APIs
- **Gemini** - [ai.google.dev](https://ai.google.dev) - Free (15 req/min)
- **Claude** - [console.anthropic.com](https://console.anthropic.com) - Paid (~$0.001-0.02)
- **ChatGPT** - [platform.openai.com](https://platform.openai.com) - Paid (~$0.001-0.03)

**Note**: Gemini is free but less capable. Claude and ChatGPT handle complex levels better.

## Example Prompts

```
"Simple platforming section with basic jumps"
"Medium difficulty ship corridor with portals"
"Hard wave section with red blocks and yellow spikes"
"Extreme demon timing section with groups 1 and 2"
```

## Settings

**Settings → Mods → Editor AI:**

- **AI Provider** - gemini / claude / openai / ollama
- **Model** - Choose from 9+ models
- **Difficulty** - easy / medium / hard / extreme
- **Style** - modern / retro / minimalist / decorated
- **Length** - short / medium / long / xl / xxl
- **Max Objects** - 10 to 1,000,000
- **Features** - Colors, groups, triggers (toggle in settings)
- **Rate Limiting** - Recommended ON (3s default)

## ⚠️ Known Issues

### Editor Collab Incompatibility
**`Editor Collab` mod is NOT compatible with Editor AI.** They break each other. You must:
- Disable Editor Collab before using Editor AI, OR
- Uninstall Editor Collab

This is a known incompatibility and cannot be fixed without changes to both mods.

## Troubleshooting

**"API Key Required"**
- Click lock icon and enter key
- For Ollama: No key needed, ensure Ollama is running

**Objects Not Appearing**
- Check max objects setting
- Try smaller generation first
- Verify level isn't locked

**Slow Generation**
- Normal! Creates 20-50 objects/second
- Watch progress indicator
- Prevents crashes and ensures stability

## Platform Support

- ✅ Windows (Tested)
- ✅ macOS (Tested)  
- ✅ Android (Tested)

## Links

- **GitHub**: [entity12208/EditorAI](https://github.com/entity12208/EditorAI)
- **Geode Docs**: [docs.geode-sdk.org](https://docs.geode-sdk.org)
- **Report Issues**: GitHub Issues or Geode Discord

## Credits

- **Developer**: entity12208
- **Framework**: Geode SDK
- **AI Providers**: Google, Anthropic, OpenAI, Ollama

---

**Version**: 2.1.2  
**Game**: Geometry Dash 2.2074  
**Geode**: 4.10.0+  
**Status**: ✅ Stable
