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

// A type-level fixture, compiled by `npm run typecheck`: Alert.alert called the way an app calls it.
import {Alert} from '../../src/embedded-react/index';
import type {AlertButton} from '../../src/embedded-react/index';

const pressed: string[] = [];

const buttons: AlertButton[] = [
  {text: 'Cancel', style: 'cancel'},
  {text: 'Delete', style: 'destructive', onPress: () => pressed.push('delete')},
];

export function confirmDelete() {
  Alert.alert('Saved');
  Alert.alert('Delete?', 'This cannot be undone.', buttons);
  Alert.alert('Note', undefined, undefined, {
    cancelable: true,
    onDismiss: () => pressed.push('dismiss'),
  });

  // @ts-expect-error a title is required
  Alert.alert();
  // @ts-expect-error not a button style
  Alert.alert('Bad', '', [{text: 'OK', style: 'primary'}]);
}
