# Vayu Package System

> Current implementation status: **package ecosystem foundation present; full registry/resolver workflow is still roadmap work.**

Vayu is intended to have a dedicated package ecosystem for distributing libraries, applications, native integrations, and reusable language components.

## Package Command

The planned command style is:

```text
nva install <package>
```

The package-manager command and implementation details may evolve as the compiler and ecosystem mature.

## Planned Capabilities

The package system is intended to provide:

- package installation
- dependency management
- version management
- project configuration
- lock files
- package publishing
- build integration
- native-library packages
- AI/ML packages
- GUI libraries
- game-development libraries

## Native Dependencies

A major goal is to make packages capable of exposing or consuming native libraries where appropriate. This is important for graphics, systems programming, AI/ML, scientific computing, and platform APIs.

## Ecosystem Direction

The long-term package workflow is intended to connect source packages with the Vayu compiler, build system, standard library, native toolchain, and future IDE/tooling ecosystem.

## Current Status

The package ecosystem remains under development. The current repository already provides the language/module foundations needed by packages:

- `.vyu` source modules
- `import` / `from ... import ...`
- module aliases
- built-in runtime modules
- native compilation
- C FFI and external-library linking
- native dependency support
- CPython interoperability
- compiler and developer tooling through `vls`, `vfmt`, and `vlint`

The complete package registry, dependency resolver, lockfile workflow, publishing pipeline, and reproducible package-build system are **not yet complete**.

For current project status and roadmap updates, visit the [official Vayu website](https://vayu.gt.tc).


## Current Foundation

Vayu already has source modules, `import`/`from ... import ...`, aliases, built-in modules, native compilation, C FFI, external library linking, native dependency capability, and CPython interoperability. These are foundations for the future package ecosystem.

## Current Package Status

The following remain planned rather than complete: project manifests, dependency resolution, lock files, package registry, `nva install`, publishing, package caching, reproducible package builds, and platform-aware native dependency resolution.

The current `nva install <package>` syntax describes the intended package-manager workflow; it should not be interpreted as proof that the full registry/installer is already implemented.


## Roadmap Alignment

The long-term package roadmap expands beyond the current `nva` design into:

- project manifests
- dependency resolution
- lockfiles
- package caching
- publishing
- a public package index
- reproducible builds
- platform-aware native dependency resolution
- `vayu install`
- `vypy install`

These remain roadmap capabilities rather than claims that the full registry/installer is already implemented.


## Relationship to the Compiler

Packages are intended to become a layer above the existing source/module system rather than replacing it. The long-term flow is:

```text
Vayu project
    ↓
Package manifest
    ↓
Dependency resolver
    ↓
Package cache / registry
    ↓
Vayu compiler + native toolchain
    ↓
Executable / library
```

The future package system must also account for native dependencies, compiler versions, target platforms, and the native backend transition toward VCB.

## VS Code and Developer Tooling

The official [Vayu VS Code extension](https://marketplace.visualstudio.com/items?itemName=Fliczo.vayu) is part of the current developer ecosystem. Package-aware editor features can build on the existing LSP and configuration infrastructure as the package system matures.
