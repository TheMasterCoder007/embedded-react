/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Alert — RN's Alert.alert(title, message, buttons, options), drawn as a stock <Modal>. There is no OS
// dialog to hand it to, so alerts wait in a queue and <AlertHost>, which AppRegistry mounts beside the
// app, shows the first one. With nothing queued the host renders no nodes.
import {createElement, useSyncExternalStore} from 'react';
import {Modal, Text, View} from './components.mts';
import {useWindowDimensions} from './Dimensions.mts';
import {Pressable} from './Pressable.mts';
import type {Alert as PublicAlert, AlertButton, TextStyle} from './index.d.ts';

/** An alert waiting its turn. */
interface QueuedAlert {
  title: string;
  message?: string;
  buttons: AlertButton[];
  cancelable: boolean;
  onDismiss?: () => void;
}

const SHEET_MAX_WIDTH = 280;
const SHEET_MARGIN = 16;

const styles: Record<string, TextStyle> = {
  modal: {
    position: 'absolute',
    left: 0,
    top: 0,
    alignItems: 'center',
    justifyContent: 'center',
  },
  sheet: {backgroundColor: '#ffffff', borderRadius: 4, padding: 16, gap: 8},
  title: {color: '#000000', fontWeight: 'bold'},
  message: {color: '#444444'},
  buttons: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'flex-end',
    gap: 8,
    marginTop: 8,
  },
  buttonText: {color: '#2196F3', fontWeight: '500', padding: 8},
  destructiveText: {color: '#d32f2f'},
};

const DEFAULT_BUTTONS: AlertButton[] = [{text: 'OK'}];

const queue: QueuedAlert[] = [];
const listeners = new Set<() => void>();

const subscribe = (listener: () => void) => {
  listeners.add(listener);
  return () => listeners.delete(listener);
};

const currentAlert = () => queue[0];

// Takes the alert off the queue, if it is still the one showing.
function close(alert: QueuedAlert): boolean {
  if (queue[0] !== alert) return false;
  queue.shift();
  listeners.forEach(listener => listener());
  return true;
}

export const Alert: typeof PublicAlert = {
  alert(title, message, buttons, options) {
    queue.push({
      title,
      message,
      buttons: buttons && buttons.length > 0 ? buttons : DEFAULT_BUTTONS,
      cancelable: Boolean(options && options.cancelable),
      onDismiss: options && options.onDismiss,
    });
    if (queue.length === 1) listeners.forEach(listener => listener());
  },
};

const hasText = (value: unknown) =>
  value !== undefined && value !== null && value !== '';

export function AlertHost() {
  const alert = useSyncExternalStore(subscribe, currentAlert);
  const {width, height} = useWindowDimensions();
  if (!alert) return null;

  const sheetWidth = Math.min(SHEET_MAX_WIDTH, width - SHEET_MARGIN * 2);

  const buttons = alert.buttons.map((button, index) =>
    createElement(
      Pressable,
      {
        key: index,
        // The alert closes first, so a handler that raises another alert queues it behind nothing.
        onPress: () => {
          if (close(alert) && button.onPress) button.onPress();
        },
      },
      createElement(
        Text,
        {
          style:
            button.style === 'destructive'
              ? [styles.buttonText, styles.destructiveText]
              : styles.buttonText,
        },
        hasText(button.text) ? String(button.text) : '',
      ),
    ),
  );

  const sheet = createElement(
    // A cancelable alert's sheet is a press target of its own, so a tap on it is not a tap outside.
    alert.cancelable ? Pressable : View,
    {
      style: [styles.sheet, {width: sheetWidth}],
      onPress: alert.cancelable ? () => {} : undefined,
    },
    hasText(alert.title)
      ? createElement(Text, {style: styles.title}, String(alert.title))
      : null,
    hasText(alert.message)
      ? createElement(Text, {style: styles.message}, String(alert.message))
      : null,
    createElement(View, {style: styles.buttons}, buttons),
  );

  const fullScreen = {width, height};

  return createElement(
    Modal,
    {visible: true, style: [styles.modal, fullScreen]},
    alert.cancelable
      ? createElement(
          Pressable,
          {
            style: [styles.modal, fullScreen],
            onPress: () => {
              if (close(alert) && alert.onDismiss) alert.onDismiss();
            },
          },
          sheet,
        )
      : sheet,
  );
}
