// Copies the repo-root CHANGELOG.md and ROADMAP.md into docs/ so the site publishes them without a
// second copy to maintain. Runs before `start` and `build`; the outputs are gitignored.
import {readFileSync, writeFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const site = dirname(dirname(fileURLToPath(import.meta.url)));
const root = resolve(site, '..');

const pages = [
  {src: 'CHANGELOG.md', out: 'docs/changelog.md', title: 'Changelog', slug: '/changelog'},
  {src: 'ROADMAP.md', out: 'docs/roadmap.md', title: 'Roadmap', slug: '/roadmap'},
];

for (const {src, out, title, slug} of pages) {
  let body = readFileSync(resolve(root, src), 'utf8');
  // Drop the file's own H1; the front matter title becomes the page heading.
  body = body.replace(/^# .*\n+/, '');
  // Relative links to repo files become GitHub links; anchors and absolute URLs are left alone.
  body = body.replace(/\]\((?!https?:|#|\/)([^)]+)\)/g, (_m, p) =>
    `](https://github.com/TheMasterCoder007/embedded-react/blob/master/${p})`,
  );
  const fm = [
    '---',
    `title: ${title}`,
    `slug: ${slug}`,
    'mdx:',
    '  format: md', // CommonMark, not MDX: the root files use raw <tags> and HTML comments freely.
    '---',
    '',
    `<!-- Generated from ${src} by scripts/sync-repo-docs.mjs. Edit the root file, not this one. -->`,
    '',
  ].join('\n');
  writeFileSync(resolve(site, out), fm + body);
  console.log(`synced ${src} -> website/${out}`);
}
