# Changelog

All notable changes to Editor AI will be documented in this file.

## [v2.1.1] - 2024-12-22

### 🎉 Major Changes

- **Full Cross-Platform Support Added** 🌍
  - ✅ Windows (2.2074)
  - ✅ Android 32-bit (2.2074)
  - ✅ Android 64-bit (2.2074) 
  - ✅ MacOS (2.2074)
  - ✅ iOS (2.2074)
  - Verified compatibility with Geode SDK 4.10.0
  - CMakeLists.txt configured for all platforms
  - Platform-specific compilation support

- **Rate Limiting System**
  - Prevents excessive API calls to save tokens
  - Default enabled with 3-second cooldown
  - Configurable from 1-60 seconds
  - Can be disabled in settings
  - Reduces accidental token waste

### Added
- Platform support field in mod.json
- Better feature status logging on startup
- Cross-platform compatibility indicators

### Changed
- Version bumped to v2.1.1
- Updated mod description to highlight cross-platform support
- Features section now clearly shows what can be toggled
- Default settings: all features enabled by default

### Technical
- Ensured all Geode SDK calls are cross-platform compatible
- Updated CMakeLists.txt with proper architecture handling
- Verified against Geode documentation for multi-platform builds
- All features tested to work across platforms

## [v2.1.0] - 2024-12-XX

### Added
- Direct API integration (no server needed)
- Multi-provider support (Gemini, Claude, OpenAI)
- 9 AI models to choose from
- Secure local API key storage
- Lock icon for key management
- Object ID caching system
- GitHub-hosted object_ids.json
- Comprehensive error handling

### Features
- Generate up to 1M objects
- All gamemodes supported
- Customizable difficulty levels
- Multiple visual styles
- Length presets (short to XXL)
- Clear level toggle option

### UI
- Gold AI button in editor
- API key popup with password mode
- Status updates during generation
- Loading circle animation
- Info popup with current settings

## [v2.0.0] - Initial Release

### Added
- Basic level generation
- Gemini API support
- Simple object creation
- Editor integration

---

## Future Plans

### Planned Features
- **UI Overhaul** (High Priority)
  - Better layout and spacing
  - More intuitive controls
  - Visual feedback improvements
  - Settings preview in popup

- **Advanced Object Features**
  - Custom properties support
  - Layer/Z-order management
  - Advanced trigger configuration
  - Particle effects

- **Statistics & Analytics**
  - Generation history tracking
  - Success/failure rate monitoring
  - Popular prompt tracking
  - Token usage statistics

- **Localization**
  - Multi-language support
  - Translated UI strings
  - Localized error messages

- **Mobile Optimization**
  - Touch-friendly UI
  - Responsive layout
  - Platform-specific features

### Experimental Features Roadmap
- Expand color control to gradients
- Advanced trigger chains
- Custom object templates
- Level theme presets

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on contributing to this project.

## Links

- **GitHub**: https://github.com/Entity12208/EditorAI
- **Geode**: https://geode-sdk.org
- **Docs**: https://docs.geode-sdk.org
