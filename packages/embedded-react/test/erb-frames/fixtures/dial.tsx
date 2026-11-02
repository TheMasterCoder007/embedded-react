// Dial: a plain one with a knob, a range with its band, segments and a gradient, an image knob, and a value set by a
// tap on the track.
import {useState} from 'react';
import {View, Text, Dial, StyleSheet} from 'embedded-react';
import knob from './assets/sun.png';

export function App() {
  const [value, setValue] = useState(40);
  const [low, setLow] = useState(20);
  const [high, setHigh] = useState(70);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Dial
          style={styles.dial}
          value={value}
          min={0}
          max={100}
          thickness={10}
          trackColor="#30404f"
          indicatorColor="#40a0ff"
          cap="round"
          knob="circle"
          knobSize={16}
          knobColor="#ffffff"
          knobBorderColor="#102030"
          knobBorderWidth={2}
          adjustable
          onChange={next => setValue(Math.round(next))}
        />
        <Dial
          style={styles.dial}
          range
          value={high}
          valueStart={low}
          min={0}
          max={100}
          minSpan={10}
          thickness={8}
          bandThickness={14}
          bandColor="#ff8040"
          trackColor="#30404f"
          knob="circle"
          adjustable
          onChange={(next, start) => {
            setHigh(Math.round(next));
            setLow(Math.round(start));
          }}
        />
      </View>
      <View style={styles.row}>
        <Dial
          style={styles.dial}
          value={7}
          min={0}
          max={10}
          step={1}
          startAngle={200}
          sweepAngle={140}
          segments={6}
          gapAngle={6}
          thickness={12}
          cap="butt"
          trackColor="#30404f"
          indicatorGradient={{
            type: 'conic',
            stops: [{color: '#40ff80'}, {color: '#ff4040'}],
          }}
        />
        <Dial
          style={styles.dial}
          value={55}
          min={0}
          max={100}
          thickness={6}
          trackColor="#30404f"
          indicatorColor="#c080ff"
          knob="image"
          knobImage={knob}
          knobSize={20}
        />
      </View>
      <Text style={styles.label}>
        {value} from {low} to {high}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#101820', padding: 8, gap: 8},
  row: {flexDirection: 'row', gap: 8},
  dial: {width: 108, height: 108},
  label: {fontSize: 16, color: '#f0f0f0'},
});
