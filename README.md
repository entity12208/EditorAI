# Editor AI v2.1.2 (Ollama Integration)

AI-powered level generation for Geometry Dash 2.2074. Generate complete levels directly in the editor using Gemini, Claude, ChatGPT, or **Ollama (local AI)**.

## 🔧 v2.1.2 - Ollama Integration & Crash Fixes

**NEW**: 🎉 **Full Ollama Support** - Run AI locally, 100% free and private!  
**FIXED**: All crashes with `alk.editor-collab` and other editor mods  
**FIXED**: Memory access violations during object creation  
**NEW**: Progressive object creation - one per frame for complete stability  
**IMPROVED**: Enhanced error handling and validation

**This update is 100% stable with local AI option!**

## 🎉 Features

- ✨ **AI Level Generation** - Describe in plain text, AI builds it
- 📦 **1000+ Object Types** - Full GD object library support (auto-updates from GitHub!)
- 🎨 **Color Control** - HEX color assignment
- 🔗 **Group IDs** - AI manages object groups
- ⚡ **Triggers** - Alpha, Move, Toggle triggers
- 🆓 **Multiple AI Options** - Gemini (free), Claude, ChatGPT, Ollama (free local)
- 🌐 **Cross-Platform** - Windows, macOS, Android
- 🔄 **Auto-Update** - Object library updates automatically from GitHub

## Quick Start

1. **Install** - Download from Geode mod browser
2. **Get API Key** - Visit [ai.google.dev](https://ai.google.dev) (free) OR use Ollama locally
3. **Enter Key** - Click lock icon in AI popup (skip for Ollama)
4. **Generate** - Describe your level and go!

## API Keys

While choosing an API to use, note that cheaper models will not handle the mod as well and will fail far more often.
Though Gemini is free, Claude and ChatGPT are **Highly** recommended.

### Ollama (FREE - Local)
- Download [ollama.com](https://ollama.com)
- Install and run `ollama run llama2`
- No API key needed - runs on your machine!
- **Free:** Unlimited, fully private
- Perfect for: Privacy, offline use, no costs

### Gemini (FREE or Paid)
- Visit [ai.google.dev](https://ai.google.dev)
- Click "Get API Key"
- Copy key (starts with `AIza...`)
- **Free:** 15 requests/minute

### Claude (Paid)
- Visit [console.anthropic.com](https://console.anthropic.com)
- Create account + add payment
- Generate key (starts with `sk-ant...`)
- **Cost:** ~$0.001-0.02 per generation

### ChatGPT (Paid)
- Visit [platform.openai.com](https://platform.openai.com)
- Create account + add payment
- Generate key (starts with `sk...`)
- **Cost:** ~$0.001-0.03 per generation

## Object Library Auto-Update

The mod automatically keeps your object library up to date!

**How it works:**
1. 💾 **Startup:** Loads from local `object_ids.json` (instant, ~1000 objects)
2. 🌐 **Background:** Checks GitHub for updates asynchronously
3. 📢 **Notification:** Shows popup if newer version found and updated
4. ⚠️ **Fallback:** Uses 5 default objects if no local file

**Benefits:**
- ✅ Instant startup - no waiting for network
- ✅ Always have the latest object library
- ✅ New objects added automatically in background
- ✅ No manual updates needed
- ✅ Works offline with local file
- ✅ Notification when updates are available

**Note:** Updates happen in the background while you play. You'll see a notification if new objects are available!

## Usage

1. Open level editor
2. Click **AI** button (top-right, gold)
3. Click **lock icon** to enter API key (skip for Ollama)
4. Type prompt: `"Medium difficulty platforming"`
5. Click **Generate**
6. Watch progressive creation with live progress indicator!

## Prompt Examples

**Easy:** "Simple platforming with basic jumps"  
**Medium:** "Balanced ship section with portals"  
**Hard:** "Challenging wave corridor"  
**Extreme:** "Extreme demon timing section"

**With Features:**
- "Medium level with red blocks and yellow spikes"
- "Hard section with objects in groups 1 and 2"
- "Easy platformer with moving platforms"

## Settings

**Settings → Mods → Editor AI:**

### Core Settings
- **AI Provider** - gemini / claude / openai / ollama
- **Model** - Choose from 9+ models
- **Difficulty** - easy / medium / hard / extreme
- **Style** - modern / retro / minimalist / decorated
- **Length** - short / medium / long / xl / xxl
- **Max Objects** - 10 to 1,000,000

### Rate Limiting
- **Enable Rate Limiting** - ON (recommended)
- **Minimum Seconds Between Requests** - 3 (default)

### Features (Enable in Settings)
- **Color Control** - HEX color assignment for objects
- **Group IDs** - AI manages object groups
- **Trigger Support** - Alpha, Move, and Toggle triggers

### Ollama Settings (if using local AI)
- **Ollama URL** - http://localhost:11434 (default)
- **Ollama Model** - llama2, mistral, codellama, etc.

## Troubleshooting

### "API Key Required"
- Click lock icon → Enter API key
- For Ollama: No key needed, just ensure Ollama is running

### Objects Not Appearing
- Check max objects setting
- Ensure level isn't locked
- Try smaller generation first

### Crashes (v2.1.2 fixes these!)
- Update to v2.1.2
- Disable conflicting mods temporarily
- Report issue on GitHub

### Slow Generation
- Normal! Progressive creation takes ~20-50 objects/second
- Watch progress indicator
- This prevents crashes and ensures stability

## Compatibility

### ✅ Works With:
- `alk.editor-collab` (fully tested)
- `hjfod.betteredit`
- `alphalaneous.creative_mode`
- All major editor mods

### Platform Support:
- ✅ Windows (Fully tested)
- ✅ macOS (Tested)
- ✅ Android (Tested)

## Technical Details

### Progressive Object Creation
Objects are created one per frame (0.05s delay) to ensure:
- Complete stability with all mods
- Proper object initialization
- No memory access violations
- Smooth progress indication

### Safety Features
- Comprehensive null pointer checks
- Object state validation
- Range validation for all properties
- Graceful error recovery
- Detailed error logging

### Performance
- 20-50 objects per second
- No frame drops
- Smooth progress updates
- Auto-closes on completion

## Contributing

Found a bug? Want a feature?
- GitHub: [entity12208/EditorAI](https://github.com/entity12208/EditorAI)
- Discord: Report in Geode Discord
- In-game: Thumbs down button with feedback

## Credits

- **Developer**: entity12208
- **Framework**: Geode SDK
- **AI Providers**: Google, Anthropic, OpenAI, Ollama
- **Testers**: Community beta testers
- **Special Thanks**: alk (editor-collab) for testing

## License

This mod is free and open source. Check the repository for license details.

---

**Version**: 2.1.2 (Crash Fix Update)  
**Released**: December 27, 2024  
**Game Version**: 2.2074  
**Geode Version**: 4.10.0+

**Status**: ✅ Stable - Zero crashes reported in testing
