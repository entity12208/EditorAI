# Editor AI

Describe a level in plain words and watch it get built for you. EditorAI talks to the AI of your choice, then places the result in the editor on its own <cb>preview layer</c> you can accept, edit, or deny — your level, your call, always.

## Open the panel

Press <cg>E</c> anywhere in the game (desktop — rebindable in Settings), or tap the floating <cg>AI</c> bubble (mobile). Everything lives in that panel: starting generations, watching them run, chatting with the AI, and every setting.

## Pick an AI

Open the panel, go to <cy>Settings</c>, and choose a provider. Two are free with zero accounts:

- <cy>Platinum</c> (free, community-run — servers by VLT GG): set provider to <cg>ollama</c> and enable <cg>Use Platinum</c>. Prompts run on volunteer machines, so keep personal info out of them.
- <cy>Ollama</c> (free, fully local): install Ollama, pull a model, set provider to <cg>ollama</c>. Nothing leaves your machine.

For hosted providers (Claude, OpenAI, Mistral, DeepSeek, Gemini, and more), paste an API key — or use the <cg>Sign in with browser</c> button for OpenRouter and HuggingFace, no key-copying needed.

## Use it

1. Panel → <cy>Chat</c> → <cg>+ new chat</c>
2. Pick a target: the current editor, a brand-new level, or any of your created levels
3. Describe what you want, set difficulty/style/length (or type your own), press <cg>Generate</c>
4. Watch it think and work live — you can close the panel, or even the editor, while it runs
5. The result appears on its own editor layer (the rest fades back): <cg>Accept</c>, <cy>Edit</c>, or <cr>Deny</c>
6. Keep the conversation going for as long as you like — small edits, full reworks, plans, questions. Conversations survive restarts; just send another message
7. Rate generations when asked — your ratings teach the AI your taste over time

Every option in the panel has a hover tooltip explaining what it does.

Bugs or ideas? Open an issue on the GitHub repository (entity12208/editorai).
