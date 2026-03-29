# Homebrew Tap for CodeTLDR

Token-efficient code analysis for LLM agents.

## Install

```sh
brew tap codetldr/tap
brew install codetldr
```

## What gets installed

- `codetldr` -- CLI for project setup, search, and daemon management
- `codetldr-daemon` -- Background analysis daemon (launched automatically by CLI)
- `codetldr-mcp` -- MCP stdio server for Claude Code and other LLM agents

## Requirements

- macOS on Apple Silicon (arm64)
- Xcode Command Line Tools (optional, for Swift LSP support): `xcode-select --install`

## Updating

```sh
brew update
brew upgrade codetldr
```

## More info

- [CodeTLDR repository](https://github.com/codetldr/codetldr)
