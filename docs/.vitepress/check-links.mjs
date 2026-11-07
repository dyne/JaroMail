import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const dist = resolve(dirname(fileURLToPath(import.meta.url)), 'dist')
const base = process.env.BASE_PATH || '/'
const origin = 'https://docs.invalid'
if (!existsSync(join(dist, 'index.html'))) throw new Error('Build the website before checking links')
function files(dir) {
  return readdirSync(dir).flatMap(name => {
    const path = join(dir, name)
    return statSync(path).isDirectory() ? files(path) : [path]
  })
}
const pages = files(dist).filter(path => path.endsWith('.html'))
const errors = []
let checked = 0
for (const page of pages) {
  const html = readFileSync(page, 'utf8')
  const url = new URL(base + page.slice(dist.length + 1), origin)
  for (const match of html.matchAll(/\b(?:href|src)="([^"<>]+)"/g)) {
    const target = new URL(match[1].replaceAll('&amp;', '&'), url)
    if (target.origin !== origin) continue
    if (!target.pathname.startsWith(base)) {
      errors.push(`${page}: URL outside base ${target.pathname}`)
      continue
    }
    let path = join(dist, decodeURIComponent(target.pathname.slice(base.length)))
    if (target.pathname.endsWith('/')) path = join(path, 'index.html')
    if (!existsSync(path)) {
      errors.push(`${page}: missing ${target.pathname}`)
      continue
    }
    if (target.hash && path.endsWith('.html')) {
      const id = decodeURIComponent(target.hash.slice(1))
      const destination = readFileSync(path, 'utf8')
      if (!destination.includes(`id="${id}"`)) errors.push(`${page}: missing anchor ${target.pathname}${target.hash}`)
    }
    checked++
  }
  for (const match of html.matchAll(/<img\b[^>]*>/g)) {
    if (!/\balt="[^"]*"/.test(match[0])) errors.push(`${page}: image has no alt attribute`)
  }
}
if (errors.length) {
  console.error(errors.join('\n'))
  process.exitCode = 1
} else console.log(`Checked ${pages.length} HTML pages and ${checked} local links/assets, including anchors and image alt attributes (${base}).`)
