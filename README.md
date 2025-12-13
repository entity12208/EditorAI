# Editor AI

AI-powered level generation for Geometry Dash 2.2074. Generate complete levels directly in the editor using Gemini, Claude, or ChatGPT.

## Quick Start

1. **Install** - Download from Geode mod browser
2. **Get API Key** - Visit [ai.google.dev](https://ai.google.dev) (free)
3. **Enter Key** - Click lock icon in AI popup
4. **Generate** - Describe your level and go!

## API Keys

### Gemini (FREE - Recommended)
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

## Usage

1. Open level editor
2. Click **AI** button (top-left, gold)
3. Click **lock icon** to enter API key
4. Type prompt: `"Medium difficulty platforming"`
5. Click **Generate**

## Prompt Examples

**Easy:** "Simple platforming with basic jumps"  
**Medium:** "Balanced ship section with portals"  
**Hard:** "Challenging wave corridor"  
**Extreme:** "Extreme demon timing section"

## Settings

**Settings → Mods → Editor AI:**

- **AI Provider** - gemini / claude / openai
- **Model** - Choose from 9 models
- **Difficulty** - easy / medium / hard / extreme
- **Style** - modern / retro / minimalist / decorated
- **Length** - short / medium / long / xl / xxl
- **Max Objects** - 10 to 1,000,000

## Available Models

**Gemini:**
- `gemini-2.5-flash` - Fast (default)
- `gemini-2.5-pro` - High quality
- `gemini-2.5-flash-lite` - Cheap

**Claude:**
- `claude-4-5-sonnet` - Balanced
- `claude-4-5-haiku` - Fast
- `claude-4.5-opus` - Best quality

**ChatGPT:**
- `gpt-5.2` - Latest
- `gpt-5-mini` - Balanced
- `gpt-5-nano` - Cheap

## Features

- ✅ Direct API integration (no server needed)
- ✅ 3 AI providers, 9 models total
- ✅ Secure local key storage
- ✅ Lock icon for key management
- ✅ Generate up to 1M objects
- ✅ All gamemodes supported
- ✅ Customizable difficulty/style/length

## API Key Management

**Add Key:**
1. Click AI button in editor
2. Click lock icon (bottom-left)
3. Paste your API key
4. Click Save

**Change/Delete:**
1. Click lock icon again
2. Choose "Change" or "Delete"

Your key is stored locally and never transmitted except to your chosen AI provider.

## Troubleshooting

**"API Key Required"**
- Click lock icon and enter your API key

**"API Error: 401/403"**
- Invalid key - check it's correct for the selected provider

**"API Error: 429"**
- Rate limit hit - wait a minute (Gemini free: 15/min)

**Objects underground**
- This is rare - regenerate or manually adjust

**No AI button**
- Restart GD
- Check mod is enabled in Geode

## Tips

- Start with Gemini (free!)
- Save your level before generating
- Try different models - each has strengths
- Be specific in prompts
- Use "Clear level" toggle wisely
- Check console for detailed logs

## Privacy

- API keys stored locally only
- Keys never logged
- Requests go directly to your chosen provider
- No telemetry or tracking

## Version

**v2.1.0** - Direct API integration, multi-provider support

## Credits

**Developer:** entity12208  
**Framework:** Geode SDK  
**AI Providers:** Google, Anthropic, OpenAI

---

**Generate levels with AI in seconds!** 🚀


##### Disclaimer
This was partially created with AI. Feel free to report bugs!