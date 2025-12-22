# Editor AI

AI-powered level generation for Geometry Dash 2.2074. Generate complete levels directly in the editor using Gemini, Claude, or ChatGPT. Now with rate limiting and experimental features!

## Quick Start

1. **Install** - Download from Geode mod browser
2. **Get API Key** - Visit [ai.google.dev](https://ai.google.dev) (free)
3. **Enter Key** - Click lock icon in AI popup
4. **Generate** - Describe your level and go!

## ⭐ New in v2.2.0

### 🛡️ Rate Limiting (Default: ON)
- Prevents excessive API calls
- Saves your tokens/money
- Configurable cooldown (default: 3 seconds)
- Can be disabled in settings

### 🧪 Experimental Features
Enable in settings for beta features:

- **AI Color Control** - Objects can have custom HEX colors
- **Group ID Assignment** - AI can assign objects to groups
- **Toggle Triggers** - Create toggle triggers (including touch-activated)
- **Level Settings** - AI can set ground/background types

## API Keys

While choosing an API to use, note that cheaper models will not handle the mod as well and will fail far more often.
Though Gemini is free, Claude and ChatGPT are **Highly** recommended.

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

**With Experimental:**
- "Medium level with red blocks and yellow spikes"
- "Hard section with objects in groups 1 and 2"
- "Easy level with blue background and grass ground"

## Settings

**Settings → Mods → Editor AI:**

### Core Settings
- **AI Provider** - gemini / claude / openai
- **Model** - Choose from 9 models
- **Difficulty** - easy / medium / hard / extreme
- **Style** - modern / retro / minimalist / decorated
- **Length** - short / medium / long / xl / xxl
- **Max Objects** - 10 to 1,000,000

### Rate Limiting
- **Enable Rate Limiting** - ON (recommended)
- **Minimum Seconds Between Requests** - 3 (default)

### Experimental Features (⚠️ Beta)
- **Enable Experimental Features** - Master toggle
- **AI Color Control** - HEX color assignment
- **AI Group IDs** - Group ID management
- **Toggle Triggers** - Toggle trigger creation
- **Level Background/Ground** - Background/ground control

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
- ✅ Rate limiting to prevent token waste
- ✅ Experimental color/group/trigger features
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

## Rate Limiting Explained

Rate limiting prevents you from:
- Accidentally spamming the API
- Wasting tokens/money on rapid clicks
- Hitting provider rate limits

**Default:** 3 second cooldown between requests  
**Configurable:** 1-60 seconds  
**Can be disabled** if you need rapid testing

## Experimental Features Guide

### AI Color Control
When enabled, AI can set colors on objects:
```json
{
  "type": "block_black_gradient_square",
  "x": 100,
  "y": 30,
  "color": "#FF0000"  // Red
}
```

### Group ID Assignment
AI can assign objects to groups for triggers:
```json
{
  "type": "spike_black_gradient_spike",
  "x": 200,
  "y": 0,
  "groups": [1, 5, 10]
}
```

### Toggle Triggers ⚠️ (Partially Implemented)
Toggle triggers can be created, but customization is not yet available:
```json
{
  "type": "toggle_trigger",  // ID: 1049
  "x": 300,
  "y": 100
  // Note: target_group, activate_group, and touch_triggered
  // are not yet functional due to Geode binding limitations
}
```

### Level Settings ⚠️ (Not Yet Implemented)
Ground/background setting is planned but not yet available:
```json
{
  // These are parsed but not applied yet
  // Waiting for Geode to expose LevelSettingsObject API
  "ground_type": 5,      // 1-15 (planned)
  "background_type": 10  // 1-30 (planned)
}
```

**Note:** These features are experimental. Color control and group IDs work fully, but toggle triggers and level settings are limited by current Geode bindings. We'll expand these as the SDK improves. Report issues on GitHub!

## Troubleshooting

**"Please wait X seconds before generating"**
- Rate limiting is active
- Wait for cooldown or disable in settings

**"API Key Required"**
- Click lock icon and enter your API key

**"API Error: 401/403"**
- Invalid key - check it's correct for the selected provider

**"API Error: 429"**
- Rate limit hit - wait a minute or enable rate limiting

**Objects underground**
- This is rare - regenerate or manually adjust

**No AI button**
- Restart GD
- Check mod is enabled in Geode

**Experimental features not working**
- Ensure "Enable Experimental Features" is ON
- Enable specific feature toggles
- Check console for errors
- Some features may be incomplete

## Tips

- **Start with Gemini** (free!)
- **Enable rate limiting** to save money
- **Try experimental features** for more control
- Save your level before generating
- Try different models - each has strengths
- Be specific in prompts
- Use "Clear level" toggle wisely
- Check console for detailed logs

## Contributing

Want to help improve Editor AI? Check out [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on:
- Reporting bugs
- Suggesting features
- Submitting pull requests
- Code standards

We especially need help with:
- UI improvements
- Advanced object features
- Testing experimental features
- Documentation

## Privacy

- API keys stored locally only
- Keys never logged (masked in logs)
- Requests go directly to your chosen provider
- No telemetry or tracking
- Rate limiting data stored locally

## Credits

**Developer:** Entity12208  
**Framework:** Geode SDK  
**AI Providers:** Google, Anthropic, OpenAI  
**Contributors:** See [CONTRIBUTING.md](CONTRIBUTING.md)

## Links

- **GitHub:** [github.com/Entity12208/EditorAI](https://github.com/Entity12208/EditorAI)
- **Geode:** [geode-sdk.org](https://geode-sdk.org)
- **Discord:** [discord.gg/geometrydash](https://discord.gg/geometrydash)

---

**Generate levels with AI in seconds!** 🚀

##### Disclaimer
This was partially created with AI. Feel free to report bugs!
