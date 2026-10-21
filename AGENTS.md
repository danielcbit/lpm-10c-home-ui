# Repository Guidelines

## Project Structure & Module Organization

Firmware patch sources are in `src/firmware_patch/`, the portable UI module in `src/lpm_ui/`, its host port in `src/host/`, fonts in `src/fonts/`, scripts in `scripts/`, tests in `tests/` and documentation in `docs/`. Vendor firmware (`firmware/`) and device captures (`evidence/`) stay local and are never committed; see `CONTRIBUTING.md`.

## Build, Test, and Development Commands

The commands and tool versions are in `README.md` and `CONTRIBUTING.md`; keep them current when tooling changes. Prefer a single standard entry point per workflow and ensure commands work from the repository root. Document any hardware prerequisites separately from software-only checks.

## Coding Style & Naming Conventions

Use the standard formatter and linter for the chosen language, and commit their configuration alongside the initial source files. Follow language conventions for indentation and identifier casing. Use descriptive names that distinguish physical connections, measurements, and test results. Keep modules focused and match existing conventions as the codebase develops.

## Testing Guidelines

`make test PYTHON=.venv/bin/python` runs the C tests, the script tests and the firmware emulator scenarios. Add tests with every behaviour change. Use descriptive test names that identify the behavior and expected outcome. Separate hardware-dependent tests from tests that can run locally without a device; document equipment and setup requirements.

## Commit & Pull Request Guidelines

Use concise, imperative commit subjects, such as `Add continuity measurement validation`. Keep changes focused. Pull requests should explain the purpose, summarize behavior changes, link relevant issues, and record validation performed. Include screenshots or hardware observations when they help reviewers verify the change.
