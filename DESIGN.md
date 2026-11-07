# Jaromail documentation design

This site inherits the Dyne Vite theme. Syne headings, Inter body text,
JetBrains Mono code, warm light surfaces and dark appearance come from
`@dyne/vite-theme/vitepress`. Brand primitives remain in that package.

The reading experience follows the manual's chapter order. A persistent sidebar
separates the manual, reference material, and contributor documentation. The
homepage provides the manual contents and a short route to installation.
No marketing claims or decorative dashboards are introduced.

VitePress retains ownership of the skip link, mobile navigation, local search,
appearance switch and document outline. Prose uses a bounded reading measure;
tables and code scroll within their own containers. Reduced-motion preferences
are honored by the shared theme. Custom CSS only adjusts reading layout.

The complete manual remains available alongside generated chapters. Each chapter
links back to the source and preserves the manual's GFDL notice. Legacy features
are identified with links to the existing optional-integration notes.

Verification: production builds and generated internal links/assets are checked
for root and GitHub Pages subpath hosting. Browser and assistive-technology
review require a connected browser and are not implied by a successful build.
