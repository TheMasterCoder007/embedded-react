// Strings built at run time: template literals, String(), toFixed, padStart, slice, toUpperCase and includes, in text,
// a string state, a handler's local and a list's string field. The step lands on halves, which toFixed rounds away
// from zero.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

const UNIT = 'C';
const TITLE = `Strings, in ${UNIT} to ${(1.25).toFixed(1)}`;

export function App() {
  const [temperature, setTemperature] = useState(21.5);
  const [minutes, setMinutes] = useState(65);
  const [last, setLast] = useState('none');
  const [log, setLog] = useState([{line: `start at ${(21.5).toFixed(1)}`}]);
  const [city] = useState('paris');
  const [mood, setMood] = useState('--');

  const step = (by: number) => {
    const next = temperature + by;
    const line = `${String(log.length).padStart(2, '0')}: ${next.toFixed(1)}${UNIT}`;
    setTemperature(next);
    setMinutes(minutes + 7);
    const word = by > 0 ? 'warmer' : 'cooler';
    setLast(line);
    setMood(word.slice(0, 4).toUpperCase());
    setLog([...log, {line: `${by > 0 ? 'up' : 'down'} to ${next.toFixed(0)}`}]);
  };

  return (
    <View style={styles.root}>
      <Text style={styles.title}>{TITLE}</Text>
      <Text style={styles.big}>{`${temperature.toFixed(1)} °${UNIT}`}</Text>
      <Text style={styles.label}>
        whole {temperature.toFixed(0)}, {temperature.toFixed(2)}
      </Text>
      <Text style={styles.label}>
        {`${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}`}
        {' | '}
        {minutes.toFixed(1).padStart(7, '*')}
      </Text>
      <Text style={styles.label}>last {last}</Text>
      <View style={styles.row}>
        <Pressable style={styles.button} onPress={() => step(-0.25)}>
          <Text style={styles.buttonText}>-</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => step(0.5)}>
          <Text style={styles.buttonText}>+</Text>
        </Pressable>
      </View>
      <Text style={styles.label}>
        {last.toUpperCase()} | {last.slice(-5)} |{' '}
        {last.includes('22.') ? 'has 22.' : 'no 22.'}
      </Text>
      <Text style={styles.label}>
        {city.toUpperCase()} {city.slice(0, 3)}, {mood}
      </Text>
      {log.map((entry, index) => (
        <Text key={index} style={styles.label}>
          {entry.line}
        </Text>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 10, gap: 6},
  title: {fontSize: 14, color: '#80c0ff'},
  big: {fontSize: 32, fontWeight: 'bold', color: '#f0f0f0'},
  label: {fontSize: 14, color: '#b0c0d0'},
  row: {flexDirection: 'row', gap: 12},
  button: {
    width: 60,
    height: 36,
    borderRadius: 8,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 20, color: '#ffffff'},
});
