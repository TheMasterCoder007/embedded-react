import type {ReactNode} from 'react';

// 24x24 line icons; the stroke comes from the stylesheet.
export const icons = {
  atom: (
    <>
      <ellipse cx="12" cy="12" rx="10" ry="4" />
      <ellipse cx="12" cy="12" rx="10" ry="4" transform="rotate(60 12 12)" />
      <ellipse cx="12" cy="12" rx="10" ry="4" transform="rotate(120 12 12)" />
      <circle cx="12" cy="12" r="1.4" fill="currentColor" />
    </>
  ),
  chip: (
    <>
      <rect x="6" y="6" width="12" height="12" rx="2" />
      <path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4" />
    </>
  ),
  split: <path d="M4 12h5m0 0 4-6h7M9 12l4 6h7m-3-9 3-3-3-3m0 18 3-3-3-3" />,
  feather: <path d="M20 4c-7 0-12 4-12 11v5m0-5h5c4 0 7-4 7-11ZM8 15l7-7" />,
  bolt: <path d="M13 2 4 14h7l-1 8 9-12h-7l1-8Z" />,
  touch: (
    <path d="M9 11V5a2 2 0 1 1 4 0v6m0-1a2 2 0 1 1 4 0v2m0-1a2 2 0 1 1 3 1.5V15a7 7 0 0 1-13 3.5L4.5 15a1.8 1.8 0 0 1 3-2L9 14.5" />
  ),
} satisfies Record<string, ReactNode>;

export type IconName = keyof typeof icons;
