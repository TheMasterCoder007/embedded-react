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

// Runtime e2e: an <Image> mounts as an ER_NODE_IMAGE, forwards imageName/resizeMode/tintColor through
// buildProps -> the bridge, lays out at its style size, and re-renders without crashing. The headless
// harness has no images registered (er_image_load is C-side), so the engine's "unknown name renders
// nothing" path is exercised — pixels aren't observable from JS, so we assert layout + no-crash.
import {createRoot} from '../../src/renderer.js';
import {View, Image} from 'embedded-react';
import {check, report} from './harness.js';

const layouts = {};

function Panel({mode}) {
  return (
    <View style={{width: 200, height: 200}}>
      <Image
        imageName="logo"
        resizeMode={mode}
        tintColor="#ff0000"
        style={{width: 64, height: 64}}
        onLayout={e => (layouts.img = e.layout)}
      />
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});

root.render(<Panel mode="contain" />);
check(layouts.img != null, 'Image node rendered (onLayout fired)');
check(
  layouts.img && layouts.img.width === 64 && layouts.img.height === 64,
  'Image laid out at its style size (64x64)',
);

// Re-render with a different resize mode + the same (unregistered) name — must not crash.
root.render(<Panel mode="cover" />);
check(true, 're-render with new resizeMode did not crash');

// An <Image> with no registered source mounts and renders nothing (graceful), without crashing.
root.render(
  <View style={{width: 200, height: 200}}>
    <Image style={{width: 32, height: 32}} />
  </View>,
);
check(true, 'Image without a registered source renders without crash');

// The RN-style `source` prop (string name, and { uri } object) resolves to imageName via buildProps.
root.render(
  <View style={{width: 200, height: 200}}>
    <Image source="logo" style={{width: 48, height: 48}} />
  </View>,
);
check(true, 'Image source="name" mounts without crash');
root.render(
  <View style={{width: 200, height: 200}}>
    <Image source={{uri: 'logo'}} style={{width: 48, height: 48}} />
  </View>,
);
check(true, 'Image source={{uri}} mounts without crash');

report('image');
