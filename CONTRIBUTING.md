# Contributing

Thank you for contributing to the project.

## Before Contributing

Please:

- Read the README and relevant documentation.
- Check existing issues and pull requests before opening a new one.
- Keep changes focused and avoid unrelated modifications.
- Respect the project's license and third-party license requirements.
- Follow the Code of Conduct.

## Development

When working on compiler or backend code:

- Preserve the existing architecture unless the change specifically requires an architectural update.
- Keep frontend changes separate from backend changes when possible.
- Add or update tests for behavior that changes.
- Update documentation when public behavior, syntax, commands, or architecture changes.
- Do not commit generated build output, temporary files, local caches, or personal configuration.

## Pull Requests

A good pull request should:

1. Explain what changed.
2. Explain why the change was needed.
3. Identify affected components.
4. Include tests or explain why tests are not applicable.
5. Include documentation updates when needed.
6. Keep unrelated formatting or refactoring out of the change.

Before submitting, run the relevant test suite and make sure the project still builds.

## Issues

For bug reports, include:

- Operating system and architecture
- Project version or commit
- Reproduction steps
- Expected behavior
- Actual behavior
- Relevant compiler or runtime output

For feature requests, explain the use case and proposed behavior.

## Third-Party Components

Do not replace, modify, or redistribute third-party executables or libraries without checking their applicable license and notice requirements.

See Third_Party.md for the third-party components used by the project.

## Review

Maintainers may request changes, reject changes that conflict with the project's direction, or ask for additional tests or documentation.

All contributors are expected to follow CODE_OF_CONDUCT.md.
