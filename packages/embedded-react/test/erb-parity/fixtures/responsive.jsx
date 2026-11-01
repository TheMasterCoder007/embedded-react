// One tree per breakpoint, with values, state and a drawing worked out from the screen, and no size named.
import {useState} from 'react';
import {View, Text, Pressable, Svg, Circle, Path, G} from 'embedded-react';

const WIDE = screen.width >= 480;
const LANDSCAPE = screen.width > screen.height;
const PAD = LANDSCAPE ? 4 : 8;

function Columns({label}) {
  return (
    <View style={{flexDirection: 'row', gap: PAD, height: 40}}>
      <View style={{flex: 1, backgroundColor: '#203048'}} />
      <View style={{flex: 2, backgroundColor: '#305048'}}>
        <Text style={{color: '#ffffff'}}>{label}</Text>
      </View>
    </View>
  );
}

export function App() {
  const [count, setCount] = useState(Math.floor(screen.height / 10));
  const [half] = useState(screen.width / 2);
  const [big] = useState(screen.width > 300);

  return (
    <View
      style={{
        width: screen.width,
        height: screen.height,
        padding: PAD,
        gap: PAD,
        backgroundColor: LANDSCAPE ? '#101820' : '#181010',
      }}>
      <Svg
        width={screen.width / 2}
        height={screen.height / 4}
        viewBox="0 0 100 50">
        <G scale={2}>
          <Circle
            cx={10}
            cy={10}
            r={8}
            fill="#16202f"
            stroke="#f4a261"
            strokeWidth={2}
          />
        </G>
        <Path d="M50 5 L90 5 L90 45 C 80 30 60 30 50 45 Z" fill="#60e080" />
      </Svg>
      {WIDE ? (
        <Columns label="wide" />
      ) : (
        <Text style={{color: '#ffffff'}}>narrow</Text>
      )}
      <Pressable
        onPress={() => setCount(count + 1)}
        style={{
          width: screen.width - 2 * PAD,
          height: 32,
          backgroundColor: '#404040',
        }}>
        <Text style={{color: big ? '#ffff00' : '#00ffff'}}>
          {count} / {half}
        </Text>
      </Pressable>
    </View>
  );
}
