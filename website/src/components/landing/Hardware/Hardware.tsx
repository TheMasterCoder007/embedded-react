import type {ReactNode} from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';

import Section from '../Section/Section';
import card from '../card.module.css';

import styles from './Hardware.module.css';

const boards = [
  {name: 'ESP32-S3', note: '800×480 RGB panel · Flow A', to: '/guides/boards/esp32-s3'},
  {name: 'ESP32 “CYD”', note: 'No PSRAM · SPI display · Flow B', to: '/guides/boards/esp32-cyd'},
  {name: 'RP2040', note: '240×280 SPI display · Flow B', to: '/guides/boards/rp2040'},
  {name: 'STM32H7', note: 'SDRAM · Chrom-ART backend · Flow A', to: '/guides/boards/stm32h7'},
  {name: 'Linux', note: 'SDL desktop host', to: '/guides/boards/linux'},
  {name: 'Raspberry Pi', note: 'Planned', to: '/guides/boards/raspberry-pi'},
];

export default function Hardware(): ReactNode {
  return (
    <Section eyebrow="Hardware" title="From a 7-inch RGB panel down to a watch-sized SPI display.">
      <div className={styles.boards}>
        {boards.map((b) => (
          <Link key={b.name} to={b.to} className={clsx(card.card, card.interactive, styles.board)}>
            <strong>{b.name}</strong>
            <span>{b.note}</span>
          </Link>
        ))}
      </div>
    </Section>
  );
}
