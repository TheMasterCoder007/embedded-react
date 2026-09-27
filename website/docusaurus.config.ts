import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// This runs in Node.js - Don't use client-side code here (browser APIs, JSX...)

const repoUrl = 'https://github.com/TheMasterCoder007/embedded-react';

const config: Config = {
  title: 'Embedded React',
  tagline: 'React Native for embedded MCUs. Write React, flash it, and the UI runs on the chip.',
  favicon: 'img/favicon.png',

  future: {
    v4: true,
    faster: {
      rspackPersistentCache: false,
    },
  },

  url: 'https://embedded-react.dev',
  baseUrl: '/',
  trailingSlash: false,

  // GitHub Pages: the site is deployed by .github/workflows/docs.yml, not `docusaurus deploy`.
  organizationName: 'TheMasterCoder007',
  projectName: 'embedded-react',

  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',
  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          routeBasePath: '/',
          sidebarPath: './sidebars.ts',
          editUrl: `${repoUrl}/tree/master/website/`,
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    image: 'img/social-card.png',
    colorMode: {
      respectPrefersColorScheme: true,
    },
    navbar: {
      title: 'Embedded React',
      logo: {
        alt: 'Embedded React',
        src: 'img/logo.svg',
      },
      items: [
        {type: 'docSidebar', sidebarId: 'docs', position: 'left', label: 'Docs'},
        {to: '/api', label: 'API', position: 'left'},
        {to: '/playground', label: 'Playground', position: 'left'},
        {to: '/changelog', label: 'Changelog', position: 'right'},
        {href: repoUrl, label: 'GitHub', position: 'right'},
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Learn',
          items: [
            {label: 'Introduction', to: '/intro'},
            {label: 'Getting started', to: '/getting-started/installation'},
            {label: 'Concepts', to: '/concepts/two-flows'},
          ],
        },
        {
          title: 'Reference',
          items: [
            {label: 'API', to: '/api'},
            {label: 'Roadmap', to: '/roadmap'},
            {label: 'Changelog', to: '/changelog'},
          ],
        },
        {
          title: 'Project',
          items: [
            {label: 'GitHub', href: repoUrl},
            {label: 'Issues', href: `${repoUrl}/issues`},
            {label: 'npm', href: 'https://www.npmjs.com/package/embedded-react'},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Cory Lamming. Apache-2.0.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['c', 'cmake', 'bash', 'json'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
