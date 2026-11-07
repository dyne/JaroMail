# Jaromail documentation

<!-- impeccable:product-schema 1 -->

## Platform

web

## Stack

VitePress in `docs/`, using the local Dyne Vite theme. GitHub Actions builds
and deploys to GitHub Pages. These choices were requested by the user.

## Users

People learning and using Jaromail, and maintainers looking up CLI contracts.

## Product Purpose

Make the existing manual and manpages usable as an accessible, searchable website.
The user chose manual-first navigation, preserving chapter order and complete references.

## Capabilities and Constraints

Keep the manual and CLI Markdown as authoritative sources for installed manpages.
Generate web chapters from those sources, preserving content and licensing.
Retain configuration templates and native installation behavior after `doc/` becomes `docs/`.
Provide a Linux installer for the matching architecture from the latest release.

## Brand Commitments

Use `vite-theme` as the Dyne.org theme, including its typography and light/dark tokens.

## Evidence on Hand

`docs/jaromail-manual.md`, `docs/jaro.1.md`, committed `.1` manpages,
technical notes, screenshots, and the release archive contract in `docs/releases.md`.

## Accessibility & Inclusion

The user explicitly requested accessible information organization. Preserve semantic
headings, keyboard navigation, visible focus, local search, responsive reading,
and meaningful image descriptions.
