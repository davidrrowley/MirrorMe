# Contributing to MirrorMe

Thanks for your interest in improving MirrorMe.

## Development Setup

1. Configure:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

2. Build:

```powershell
cmake --build build --config Release
```

3. Run:

```powershell
.\build\Release\MirrorMe.exe
```

## Contribution Guidelines

- Keep changes focused and small.
- Preserve current behavior unless the change explicitly targets a behavior update.
- Prefer clear, maintainable code over clever implementations.
- Update docs when user-visible behavior changes.

## Pull Request Checklist

- Build succeeds locally in Release configuration.
- Any new behavior is documented in README.md.
- No unrelated formatting churn.

## Reporting Issues

Please include:

- Windows version
- Steps to reproduce
- Expected behavior
- Actual behavior
- Any relevant logs or screenshots
