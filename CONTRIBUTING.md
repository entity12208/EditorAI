# Contributing to Editor AI

Thank you for your interest in contributing to Editor AI! This document provides guidelines for contributing to the project.

## How to Contribute

### Reporting Bugs

If you find a bug, please create an issue on GitHub with:
- A clear description of the problem
- Steps to reproduce the issue
- Expected vs actual behavior
- Your Geode version and platform (Windows/Mac/Android/iOS)
- Any relevant error messages from the console

### Suggesting Features

Feature requests are welcome! When suggesting a feature:
- Check existing issues to avoid duplicates
- Explain the use case and benefits
- Consider implementation complexity
- Be open to discussion and iteration

### Pull Requests

We welcome pull requests! Here's how to contribute code:

1. **Fork the repository**
   ```bash
   git clone https://github.com/Entity12208/EditorAI.git
   cd EditorAI
   ```

2. **Create a feature branch**
   ```bash
   git checkout -b feature/your-feature-name
   ```

3. **Make your changes**
   - Follow the existing code style
   - Test thoroughly on your platform
   - Ensure Geode SDK compliance (check docs.geode-sdk.org)
   - Add comments for complex logic

4. **Commit your changes**
   ```bash
   git add .
   git commit -m "feat: add your feature description"
   ```

5. **Push and create a pull request**
   ```bash
   git push origin feature/your-feature-name
   ```

### Code Standards

- **Use Geode SDK best practices**
  - Follow patterns from docs.geode-sdk.org
  - Use proper memory management (CCObject retain/release)
  - Avoid memory leaks with EventListener cleanup
  
- **Code style**
  - Use camelCase for variables and functions
  - Use PascalCase for classes
  - Add meaningful comments
  - Keep functions focused and readable
  
- **Testing**
  - Test on your platform before submitting
  - Verify no memory leaks or crashes
  - Check console for errors
  - Test edge cases

### Development Setup

1. Install Geode SDK (see docs.geode-sdk.org)
2. Clone this repository
3. Build with CMake:
   ```bash
   geode build
   ```
4. Test in Geometry Dash with Geode installed

### Priority Features (Help Wanted!)

Here are some features we'd love help with:

#### 🎨 **UI Overhaul**
Improve the generator popup interface:
- Better layout and spacing
- More intuitive controls
- Visual feedback during generation
- Settings preview in popup

#### 🔧 **Advanced Object Features**
- Color customization support
- Advanced trigger configuration
- Layer/Z-order management
- Custom properties

#### 📊 **Statistics & Analytics**
- Track generation history
- Object usage statistics
- Success/failure rate monitoring
- Popular prompt tracking

#### 🌐 **Localization**
- Add support for multiple languages
- Translate UI strings
- Localize error messages

#### 🎮 **Gameplay Testing**
- Auto-playtest generated sections
- Difficulty analysis
- Collision detection verification

#### 📱 **Mobile Optimization**
- Touch-friendly UI
- Responsive layout
- Platform-specific features

### Project Structure

```
EditorAI/
├── src/
│   └── main.cpp          # Main mod logic
├── resources/
│   └── object_ids.json   # Object type mappings
├── mod.json              # Mod configuration
├── CMakeLists.txt        # Build configuration
├── README.md             # User documentation
└── CONTRIBUTING.md       # This file
```

### Key Components

- **AIGeneratorPopup**: Main UI popup
- **APIKeyPopup**: API key entry dialog
- **AI Provider Integration**: API calling logic
- **Object Creation**: GameObject management
- **Settings**: Mod configuration

### Testing Checklist

Before submitting a PR, ensure:
- [ ] Code compiles without errors
- [ ] Mod loads in Geometry Dash
- [ ] Features work as expected
- [ ] No console errors
- [ ] No memory leaks
- [ ] Geode SDK compliance verified
- [ ] Tested on your platform
- [ ] Documentation updated if needed

### Getting Help

- **Discord**: Join the Geometry Dash discord
- **GitHub Issues**: Ask questions in issues
- **Documentation**: Check docs.geode-sdk.org

### Code of Conduct

- Be respectful and constructive
- Help others learn and grow
- Focus on the code, not the person
- Collaborate openly

### License

By contributing, you agree that your contributions will be licensed under the same license as the project (likely MIT or similar - check LICENSE file).

### Questions?

If you have questions about contributing, feel free to:
- Open a discussion on GitHub
- Ask in the Geometry Dash discord
- Comment on relevant issues

---

**Thank you for contributing to Editor AI!** 🚀

Every contribution, no matter how small, helps make the mod better for everyone.
