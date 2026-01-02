# Contributing

Contributions welcome! This guide covers bug reports, features, and pull requests.

## Reporting Bugs

Open an issue with:
- Description and steps to reproduce
- Expected vs actual behavior
- Platform (Windows/Mac/Android) and Geode version
- Console errors if any

## Suggesting Features

- Check existing issues first
- Explain use case and benefits
- Be open to discussion

## Pull Requests

1. Fork and clone:
```bash
git clone https://github.com/Entity12208/EditorAI.git
cd EditorAI
```

2. Create branch:
```bash
git checkout -b feature/your-feature
```

3. Make changes:
- Follow existing code style
- Test thoroughly
- Use Geode SDK best practices (see docs.geode-sdk.org)
- Add comments for complex logic

4. Commit and push:
```bash
git commit -m "feat: your feature"
git push origin feature/your-feature
```

## Code Standards

- **Memory**: Use CCObject retain/release properly
- **Style**: camelCase variables, PascalCase classes
- **Testing**: Test for crashes and memory leaks
- **Geode**: Follow [docs.geode-sdk.org](https://docs.geode-sdk.org)

## Development

```bash
geode build
```

Test in GD with Geode installed.

## Help Wanted

- UI improvements
- Localization
- Mobile optimization
- Statistics tracking

## Structure

```
EditorAI/
├── src/main.cpp          # Main logic
├── resources/            # Assets
├── mod.json             # Configuration
└── CMakeLists.txt       # Build config
```

## Testing Checklist

- [ ] Compiles without errors
- [ ] Loads in GD
- [ ] No crashes or leaks
- [ ] Tested on your platform
- [ ] Docs updated

## Getting Help

- GitHub Issues
- Geode Discord
- [docs.geode-sdk.org](https://docs.geode-sdk.org)

---

**Be respectful, help others, focus on code quality.**
