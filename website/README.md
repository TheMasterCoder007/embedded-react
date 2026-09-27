# website/ — embedded-react.dev

The documentation site, built with [Docusaurus](https://docusaurus.io/) and deployed to GitHub Pages by
`.github/workflows/docs.yml` on every push to `master` that touches it.

```bash
npm install
npm start        # http://localhost:3000, live reload
npm run build    # static site in build/
npm run typecheck
```

`docs/changelog.md` and `docs/roadmap.md` are generated from the repo-root `CHANGELOG.md` and `ROADMAP.md`
before every start/build (`scripts/sync-repo-docs.mjs`) and are not committed. Edit the root files.

Pages live in `docs/`, the sidebar order in `sidebars.ts`, and site-wide settings in `docusaurus.config.ts`.
