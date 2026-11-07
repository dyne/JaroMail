import { defineConfig } from 'vitepress'
import footnote from 'markdown-it-footnote'
import { chapters } from './chapters.mjs'

const base = process.env.BASE_PATH || '/'
if (!/^\/(?:[A-Za-z0-9_-]+\/)*$/.test(base)) {
  throw new Error('BASE_PATH must be / or a path such as /JaroMail/')
}

export default defineConfig({
  title: 'Jaromail',
  description: 'Your humble and faithful electronic postman. The Jaromail manual, command reference and installation guide.',
  lang: 'en',
  base,
  srcDir: '.vitepress/content',
  srcExclude: ['public/**'],
  outDir: '.vitepress/dist',
  cleanUrls: false,
  markdown: {
    config(md) {
      md.use(footnote)
      // Original manpages and Markdown are downloads, not VitePress routes.
      md.core.ruler.after('inline', 'original-downloads', state => {
        for (const block of state.tokens) {
          for (const token of block.children || []) {
            const href = token.attrGet('href')
            if (token.type === 'link_open' && href?.startsWith('/downloads/')) {
              token.attrSet('download', '')
              token.attrSet('href', base + href.slice(1))
            }
          }
        }
      })
    }
  },
  vite: { ssr: { noExternal: ['@dyne/vite-theme'] } },
  themeConfig: {
    siteTitle: 'Jaromail',
    search: { provider: 'local' },
    outline: { level: [2, 3], label: 'On this page' },
    nav: [
      { text: 'Manual', link: '/manual/', activeMatch: '/manual/' },
      { text: 'Install', link: '/install' },
      { text: 'Reference', link: '/reference' },
      { text: 'Dyne.org', link: 'https://dyne.org' }
    ],
    socialLinks: [{ icon: 'github', link: 'https://github.com/dyne/JaroMail' }],
    sidebar: [
      { text: 'Jaromail', items: [
        { text: 'Documentation', link: '/' },
        { text: 'Quick install', link: '/install' }
      ] },
      { text: 'User manual', items: chapters.map(([slug, text]) => ({
        text, link: slug === 'index' ? '/manual/' : `/manual/${slug}`
      })) },
      { text: 'Reference', collapsed: false, items: [
        { text: 'Reference & downloads', link: '/reference' },
        { text: 'jaro(1) command reference', link: '/jaro.1' },
        { text: 'Complete jaromail(1) manual', link: '/jaromail-manual' },
        { text: 'Command contracts', link: '/command-contracts' },
        { text: 'Filter compatibility', link: '/filter-compatibility' },
        { text: 'Optional & legacy integrations', link: '/optional-integrations' }
      ] },
      { text: 'Development', collapsed: true, items: [
        { text: 'Contributing & website', link: '/contributing' },
        { text: 'Builds & releases', link: '/releases' },
        { text: 'Domain notes', link: '/domain-notes' },
        { text: 'Filter policy', link: '/filter-policy' },
        { text: 'Filter interfaces', link: '/filter-interfaces' },
        { text: 'Filter benchmarks', link: '/filter-benchmarks' },
        { text: 'Sieve validation tools', link: '/sieve-tools' },
        { text: 'Maintenance inventory', link: '/maintenance-inventory' }
      ] }
    ],
    docFooter: { prev: 'Previous page', next: 'Next page' },
    footer: {
      message: 'A <a href="https://dyne.org">Dyne.org</a> project. Manual: GFDL 1.3 or later. <a href="https://github.com/dyne/JaroMail/blob/HEAD/COPYING.txt">Software licenses</a>.',
      copyright: 'Jaromail contributors'
    }
  }
})
