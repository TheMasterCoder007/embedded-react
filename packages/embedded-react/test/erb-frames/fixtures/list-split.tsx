// List split: a string split into lists. The first column splits the line at the separator, up to the limit, in
// render; the second keeps its pieces other than "x"; the third is a list state a button fills with a split; the
// label shows the last piece, which a handler reads from a split of its own. A list holds 5 pieces here, so a long
// line drops the rest.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [line, setLine] = useState('red,green,blue');
  const [sep, setSep] = useState(',');
  const [limit, setLimit] = useState(-1);
  const [words, setWords] = useState(['-']);
  const [last, setLast] = useState('-');
  const parts = line.split(sep, limit);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setLine(line + ',x')}>
          <Text style={styles.buttonText}>more</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setSep(sep === ',' ? 'e' : sep === 'e' ? '' : ',')}>
          <Text style={styles.buttonText}>sep</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setLimit(limit === -1 ? 2 : limit === 2 ? 0 : -1)}>
          <Text style={styles.buttonText}>limit</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => setWords(line.split(sep, limit))}>
          <Text style={styles.buttonText}>keep</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            const pieces = line.split(',');
            setLast(pieces[pieces.length - 1] ?? '?');
          }}>
          <Text style={styles.buttonText}>last</Text>
        </Pressable>
      </View>
      <Text style={styles.label}>
        {parts.length} parts, 2nd {parts[1] ?? '-'}, last {last}
      </Text>
      <View style={styles.columns}>
        <View style={styles.column}>
          {parts.map((part, index) => (
            <Text key={index} style={styles.text}>
              {index}. {part}
            </Text>
          ))}
        </View>
        <View style={styles.column}>
          {line
            .split(',')
            .filter(word => word !== 'x')
            .map((word, index) => (
              <Text key={index} style={styles.text}>
                {word}
              </Text>
            ))}
        </View>
        <View style={styles.column}>
          {words.map((word, index) => (
            <Text key={index} style={styles.text}>
              {word}
            </Text>
          ))}
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  button: {
    width: 48,
    height: 24,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 12, color: '#ffffff'},
  label: {fontSize: 10, color: '#b0c0d0'},
  columns: {flexDirection: 'row', gap: 4},
  column: {width: 72, gap: 2},
  text: {fontSize: 10, color: '#f0f0f0'},
});
