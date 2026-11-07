import { execFileSync } from 'node:child_process'
import { cpSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { chapters } from './chapters.mjs'

const docs = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const content = join(docs, '.vitepress/content')
const publicDir = join(content, 'public')
const downloads = join(publicDir, 'downloads')
const sourceURL = 'https://github.com/dyne/JaroMail/blob/HEAD/'
rmSync(content, { recursive: true, force: true })
rmSync(downloads, { recursive: true, force: true })
mkdirSync(join(content, 'manual'), { recursive: true })
mkdirSync(downloads, { recursive: true })

// Pandoc understands the original manual's simple tables and escaped prose.
// Convert for the website only; installed manpage sources stay authoritative.
function convert(path) {
  return execFileSync('pandoc', ['--from=markdown', '--to=gfm', '--wrap=none', path], {
    encoding: 'utf8', maxBuffer: 8 * 1024 * 1024
  }).replace(/```\s*example\n/g, '```text\n')
    .replace(/!\[\]\(jaromail-shot.jpg\)/g, '![Jaromail terminal showing a threaded email inbox](jaromail-shot.jpg)')
    .replace(/\[Addressbook\]\[\]\*/g, '[Addressbook](manual/addressbook.md)')
    .replace(/\]\(\.\.\/([^\s)]+)\)/g, `](${sourceURL}$1)`)
    .replace(/\]\((filter-regex-qualification.json)\)/g, '](/downloads/$1)')
}

const manual = convert(join(docs, 'jaromail-manual.md'))
const footnotes = manual.match(/^\[\^\d+\]:.*(?:\n[ \t]+.*)*/gm)?.join('\n\n') || ''
const body = manual.replace(/^\[\^\d+\]:.*(?:\n[ \t]+.*)*/gm, '').trim()
// Match chapter headings outside code fences so shell comments cannot split pages.
const sections = []
let current = [], fenced = false
for (const line of body.split('\n')) {
  if (/^```/.test(line)) fenced = !fenced
  if (!fenced && /^## /.test(line)) {
    sections.push(current.join('\n'))
    current = []
  }
  current.push(line)
}
sections.push(current.join('\n'))
if (sections.length !== chapters.length) {
  throw new Error(`Manual chapter structure changed (${sections.length}); update chapters.mjs`)
}

const notice = '> Adapted from the [Jaromail manual source](' + sourceURL + 'docs/jaromail-manual.md). '
  + 'Copyleft © 2010–2023 Denis Roio. GNU Free Documentation License 1.3 or later; '
  + '[full permission notice](/manual/shell-environment#license).\n\n'
for (let i = 0; i < sections.length; i++) {
  const [slug, title] = chapters[i]
  let text = sections[i]
  if (i === 0) text = text.replace(/^# (.+)\n/, '*$1*\n').trim()
  else text = text.replace(/^## .+\n/, '')
  text = text.replace(/^(#{3,6}) /gm, (_, hashes) => `${hashes.slice(1)} `)
    .replace(/\]\(#addressbook\)/g, '](/manual/addressbook.md)')
    .replace(/\]\((?!https?:|\/|#)([^)]+)\)/g, '](../$1)')
    .replace(/\.\.\/manual\/addressbook.md/g, 'addressbook.md')
  const extra = slug === 'setup'
    ? '::: tip Install a Linux release\nFor the latest prebuilt package, use the [quick installer](/install). The original build instructions follow.\n:::\n\n'
    : ['statistics', 'shell-environment'].includes(slug)
      ? '::: info Optional integrations\nSome features in this chapter are legacy or platform-specific. See [optional integrations](/optional-integrations) for their maintenance status.\n:::\n\n'
      : ''
  writeFileSync(join(content, 'manual', `${slug}.md`), `# ${title}\n\n${notice}${extra}${text}\n\n${footnotes}\n`)
}

for (const item of readdirSync(docs, { withFileTypes: true })) {
  if (!item.isFile()) continue
  const path = join(docs, item.name)
  if (item.name.endsWith('.md')) {
    writeFileSync(join(content, item.name), convert(path))
  } else {
    cpSync(path, join(downloads, item.name))
    if (/\.(png|jpg|svg)$/.test(item.name)) cpSync(path, join(content, item.name))
  }
}
// Publish both manpages and their sources, plus the original print editions.
for (const name of ['jaromail-manual.md', 'jaro.1.md']) {
  cpSync(join(docs, name), join(downloads, name))
}
cpSync(join(docs, 'Accounts'), join(downloads, 'Accounts'), { recursive: true })
cpSync(join(docs, '../build/install-release.sh'), join(publicDir, 'install.sh'))
writeFileSync(join(content, 'jaromail.md'), '# jaromail(1)\n\nThe [complete manual](jaromail-manual.md) is the source of `jaromail(1)`. '
  + 'Read it [chapter by chapter](manual/index.md) or [download the manpage](/downloads/jaromail.1).\n')
console.log(`Prepared ${sections.length} manual chapters, reference pages, and original downloads.`)
