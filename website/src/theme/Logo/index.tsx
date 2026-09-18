import type {ReactNode} from 'react';
import Link from '@docusaurus/Link';
import useBaseUrl from '@docusaurus/useBaseUrl';
import {useThemeConfig} from '@docusaurus/theme-common';
import ThemedImage from '@theme/ThemedImage';
import type {Props} from '@theme/Logo';

import Wordmark from '@site/src/components/Wordmark';

/**
 * The stock navbar logo, with the plain-text title swapped for the wordmark.
 */
export default function Logo(props: Props): ReactNode {
  const {
    navbar: {logo},
  } = useThemeConfig();
  const {imageClassName, titleClassName, ...propsRest} = props;
  const logoLink = useBaseUrl(logo?.href || '/');
  const sources = {
    light: useBaseUrl(logo?.src ?? ''),
    dark: useBaseUrl(logo?.srcDark || logo?.src || ''),
  };
  return (
    <Link to={logoLink} {...propsRest}>
      {logo && (
        <div className={imageClassName}>
          {/* Decorative: the wordmark beside it carries the name. */}
          <ThemedImage sources={sources} height={logo.height} width={logo.width} alt="" />
        </div>
      )}
      <b className={titleClassName}>
        <Wordmark />
      </b>
    </Link>
  );
}
