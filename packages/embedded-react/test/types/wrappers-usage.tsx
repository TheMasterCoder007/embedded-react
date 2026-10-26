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

// A type-level FIXTURE, not a runtime test: `npm run typecheck` compiles it alongside index.d.ts.
// It uses the JS-only RN wrappers (<Button>, <ImageBackground>, <SectionList>) the way an app does, so
// a signature that is well-formed but WRONG — a `section` that loses the app's own fields, an
// `index` that isn't a number, a `renderItem` that can't return an element — fails the build here.
// No `console`: the lib is ES2020 with no DOM, matching the device runtime.
import {
  Button,
  ImageBackground,
  SectionList,
  Text,
  View,
} from '../../src/embedded-react/index';
import type {
  ButtonProps,
  ImageBackgroundProps,
  SectionListData,
  SectionListProps,
  SectionListRenderItemInfo,
} from '../../src/embedded-react/index';

const pressed: string[] = [];

// The section shape is the app's, not a fixed one: `title` below is ours, and it has to survive into
// renderSectionHeader / renderItem still typed as a string.
type Contact = {id: number; name: string};
type Group = {title: string};

const sections: SectionListData<Contact, Group>[] = [
  {title: 'A', data: [{id: 1, name: 'Ada'}]},
  {
    title: 'B',
    key: 'b',
    data: [{id: 2, name: 'Bea'}],
    // Per-section overrides, as in RN.
    renderItem: ({item}) => <Text>{item.name}</Text>,
    keyExtractor: item => item.id,
  },
];

export function WrapperUsage() {
  return (
    <View style={{flex: 1}}>
      <Button
        title="Save"
        color="#2196F3"
        disabled={false}
        onPress={e => pressed.push(e.type)}
      />

      <ImageBackground
        source={{uri: 'wallpaper'}}
        resizeMode="cover"
        style={{flex: 1, padding: 8}}
        imageStyle={{opacity: 0.4, borderRadius: 6}}>
        <Text style={{color: '#fff'}}>on top of the picture</Text>
      </ImageBackground>

      <SectionList
        sections={sections}
        style={{flex: 1}}
        keyExtractor={(item, index) => `${item.id}-${index}`}
        renderSectionHeader={({section}) => <Text>{section.title}</Text>}
        renderSectionFooter={({section}) => (
          <Text>{section.data.length} contacts</Text>
        )}
        renderItem={({item, index, section}) => (
          <Text>
            {index}. {item.name} of {section.title}
          </Text>
        )}
      />
    </View>
  );
}

// The exported prop types must stay usable by name — apps annotate their own wrappers with them.
export const titleOf = (p: ButtonProps): string => p.title ?? '';
export const imageStyleOf = (p: ImageBackgroundProps) => p.imageStyle;
export const rowLabel = (
  info: SectionListRenderItemInfo<Contact, Group>,
): string => `${info.section.title}/${info.item.name}/${info.index}`;
export const emptyList: SectionListProps<Contact, Group> = {sections: []};
