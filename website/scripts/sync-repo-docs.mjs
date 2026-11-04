// Copies the repo-root CHANGELOG.md and ROADMAP.md into docs/ so the site publishes them without a
// second copy to maintain. Runs before `start` and `build`; the outputs are gitignored.
import {readFileSync, writeFileSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const siteDirectory = dirname(dirname(fileURLToPath(import.meta.url)));
const repositoryRoot = resolve(siteDirectory, '..');

const pages = [
  {source: 'CHANGELOG.md', output: 'docs/changelog.md', title: 'Changelog', slug: '/changelog'},
  {source: 'ROADMAP.md', output: 'docs/roadmap.md', title: 'Roadmap', slug: '/roadmap'},
];

for (const {source, output, title, slug} of pages) {
  let body = readFileSync(resolve(repositoryRoot, source), 'utf8');
  // Drop the file's own H1; the front matter title becomes the page heading.
  body = body.replace(/^# .*\n+/, '');
  // Relative links to repo files become GitHub links; anchors and absolute URLs are left alone.
  body = body.replace(/\]\((?!https?:|#|\/)([^)]+)\)/g, (_match, path) =>
    `](https://github.com/TheMasterCoder007/embedded-react/blob/master/${path})`,
  );
  const frontMatter = [
    '---',
    `title: ${title}`,
    `slug: ${slug}`,
    `custom_edit_url: https://github.com/TheMasterCoder007/embedded-react/edit/master/${source}`,
    'mdx:',
    '  format: md', // CommonMark, not MDX: the root files use raw <tags> and HTML comments freely.
    '---',
    '',
    `<!-- Generated from ${source} by scripts/sync-repo-docs.mjs. Edit the root file, not this one. -->`,
    '',
  ].join('\n');
  writeFileSync(resolve(siteDirectory, output), frontMatter + body);
  console.log(`synced ${source} -> website/${output}`);
}
