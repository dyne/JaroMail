# Pinned Dyne Vite theme

`dyne-vite-theme-0.1.0.tgz` was produced with `npm pack --ignore-scripts` from
the adjacent `dyne/vite-theme` repository at commit
`c485943e103fc1e7a50c6f45853d190f16f57583`.

The package is committed so a clean Jaromail checkout can build without an
adjacent theme checkout or a registry publication. npm's lockfile records its
integrity. The implementation is MIT, fonts SIL OFL 1.1, and the included Dyne
marks CC BY-SA 4.0; the package retains LICENSE and NOTICE.md.

To update, pack a reviewed theme revision into this directory, update the
dependency if its filename changes, run `npm install`, and verify both root
and project-path builds. Theme release tasks belong to the theme repository.
