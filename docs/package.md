# Vayu Package System

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

The package ecosystem remains under development. The current repository has a source-module system, native compilation, C FFI, external-library linking, built-in modules, and CPython interoperability foundations, but it does not yet provide the complete planned package registry/resolver workflow.

For current project status and roadmap updates, visit the [official Vayu website](https://vayu.gt.tc).


## Current Foundation

Vayu already has source modules, `import`/`from ... import ...`, aliases, built-in modules, native compilation, C FFI, external library linking, native dependency capability, and CPython interoperability. These are foundations for the future package ecosystem.

## Current Package Status

The following remain planned rather than complete: project manifests, dependency resolution, lock files, package registry, `nva install`, publishing, package caching, reproducible package builds, and platform-aware native dependency resolution.

The current `nva install <package>` syntax describes the intended package-manager workflow; it should not be interpreted as proof that the full registry/installer is already implemented.


## Roadmap Alignment

The long-term package roadmap expands beyond the current planned `nva` workflow into `vayu install`, `vypy install`, a public package index, dependency resolution, lockfiles, reproducible builds, caching, publishing, and platform-aware native dependency handling. These remain roadmap capabilities rather than claims about the current registry implementation.
