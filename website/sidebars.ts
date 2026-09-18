import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

// This runs in Node.js - Don't use client-side code here (browser APIs, JSX...)

const sidebars: SidebarsConfig = {
  docs: [
    'intro',
    {
      type: 'category',
      label: 'Getting started',
      link: {type: 'doc', id: 'getting-started/index'},
      items: [
        'getting-started/installation',
        'getting-started/simulator',
        'getting-started/first-board',
      ],
    },
    {
      type: 'category',
      label: 'Concepts',
      link: {type: 'doc', id: 'concepts/index'},
      items: [
        'concepts/two-flows',
        'concepts/engine-and-backends',
        'concepts/rendering-pipeline',
        'concepts/layout',
        'concepts/assets',
      ],
    },
    {
      type: 'category',
      label: 'Guides',
      link: {type: 'doc', id: 'guides/index'},
      items: [
        {
          type: 'category',
          label: 'Boards',
          items: [
            'guides/boards/esp32-s3',
            'guides/boards/esp32-cyd',
            'guides/boards/rp2040',
            'guides/boards/stm32h7',
            'guides/boards/linux',
            'guides/boards/raspberry-pi',
          ],
        },
        'guides/hot-reload',
        'guides/aot-subset',
        'guides/performance',
        'guides/memory',
      ],
    },
    {
      type: 'category',
      label: 'API reference',
      link: {type: 'doc', id: 'api/index'},
      items: [
        'api/components',
        'api/hooks',
        'api/styles',
        'api/animated',
        'api/native-ui-bridge',
        'api/c-engine',
      ],
    },
    {
      type: 'category',
      label: 'Internals',
      link: {type: 'doc', id: 'internals/index'},
      items: [
        'internals/architecture',
        'internals/writing-a-backend',
        'internals/testing',
        'internals/releasing',
        'internals/contributing',
      ],
    },
    'playground',
    'roadmap',
    'changelog',
  ],
};

export default sidebars;
