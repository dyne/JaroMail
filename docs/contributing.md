# Contributing

Source code, issues and pull requests live in
[dyne/JaroMail](https://github.com/dyne/JaroMail). Community contact is through
[Dyne's chats and channels](https://dyne.org/linktree).

## Build and test Jaromail

```sh
cmake -S . -B out/build -DCMAKE_BUILD_TYPE=Release
cmake --build out/build --parallel
cmake --build out/build --target check-strict
```

See [Builds & releases](releases.md) for dependencies, version assignment and
packaging, and [maintenance inventory](maintenance-inventory.md) for test scope.

## Work on the website

Use Node.js 24 and Pandoc (for converting the original manual's tables to web
Markdown). From the repository root:

```sh
npm ci
npm run docs:dev
```

Build and check the production site:

```sh
BASE_PATH=/JaroMail/ npm run docs:build
BASE_PATH=/JaroMail/ npm run docs:check
BASE_PATH=/JaroMail/ npm run docs:preview
```

VitePress builds into `docs/.vitepress/dist`. `BASE_PATH` defaults to `/` for
local development. Use `/JaroMail/` for the repository's GitHub Pages URL.
The workflow uses GitHub Pages configuration to determine the deployed base.
For a custom domain, configure the domain in repository Pages settings; the
workflow then receives the root base path.

## Content sources

Edit `docs/jaromail-manual.md` for manual content and `docs/jaro.1.md` for the
command reference. `npm run docs:prepare` converts those sources with Pandoc,
splits the manual into chapters, and prepares the original downloads. The
generated content in `docs/.vitepress/content` is ignored by Git; do not edit it.
If the manual's chapter structure changes, update `.vitepress/chapters.mjs`.

The homepage, installation, reference and contributor pages are ordinary
Markdown files directly under `docs/`, alongside the retained technical notes.
The installer source is `build/install-release.sh`; preparation publishes it as
`install.sh` on the website.

Regenerate the installed manpages explicitly after editing their source:

```sh
cmake --build out/build --target man
```

Commit both sources and generated `.1` files. Normal native builds do not
require Node.js or Pandoc and do not regenerate the manuals.

## Deployment

In repository **Settings → Pages → Build and deployment**, select
**GitHub Actions** as the source. The documentation workflow builds and checks
pull requests, and deploys pushes or manual runs on the default branch.
Pull requests never deploy. Build artifacts are uploaded from
`docs/.vitepress/dist` using the standard
[VitePress GitHub Pages workflow](https://vuejs.github.io/vitepress/v1/guide/deploy#github-pages).

## Theme and licenses

The pinned `@dyne/vite-theme` package supplies the Dyne.org design system,
self-hosted fonts, light/dark appearance and VitePress adapter. Its package
archive and provenance are in `.vitepress/vendor/`. No adjacent theme checkout
or unpublished registry package is needed to build this website.

The theme implementation is MIT; its fonts use SIL OFL 1.1 and its included
Dyne marks use CC BY-SA 4.0, attributed to Dyne.org foundation, unmodified.
See the package's `NOTICE.md` for details. The site's manual retains its
own [GFDL notice](manual/shell-environment.md#license); Jaromail's
[software licenses](https://github.com/dyne/JaroMail/blob/HEAD/COPYING.txt) remain in force.
