// A type-level FIXTURE, not a runtime test: `npm run typecheck` compiles it alongside index.d.ts.
// Dimensions and useWindowDimensions report the panel's size as numbers.
import {
  Dimensions,
  useWindowDimensions,
  View,
} from '../../src/embedded-react/index';
import type {ScaledSize} from '../../src/embedded-react/index';

const panel: ScaledSize = Dimensions.get('window');

export function HalfWidth() {
  const {width, height} = useWindowDimensions();
  return (
    <View style={{width: width / 2, height: Math.min(height, panel.height)}} />
  );
}

// @ts-expect-error -- only 'window' and 'screen' name a size
Dimensions.get('display');
