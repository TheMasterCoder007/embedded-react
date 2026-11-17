// A wall clock set while the app runs, as a board that learns the time after the load does: nothing is built or run
// a second time. Rows from state and from a constant, rows in a branch, props set once, a drawing and effects.
import {useEffect, useState} from 'react';
import {
  View,
  Text,
  Pressable,
  StyleSheet,
  Svg,
  Path,
  useWindowDimensions,
} from 'embedded-react';

const SHARES = [{share: 2}, {share: 3}];
const CLOCK_SET_MS = 1700000000000;

function Bar({share}: {share: number}) {
  const {width} = useWindowDimensions();
  return (
    <View
      style={{width: width / share, height: 6, backgroundColor: '#60e080'}}
    />
  );
}

export function App() {
  const {width, height} = useWindowDimensions();
  const [names, setNames] = useState(['a']);
  const [shown, setShown] = useState(false);
  const [mounts, setMounts] = useState(0);
  const [runs, setRuns] = useState(0);

  useEffect(() => {
    setMounts(mounts + 1);
  }, []);

  useEffect(() => {
    setRuns(runs + 1);
  }, [shown]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => setNames([...names, `n${names.length}`])}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => setShown(!shown)}>
          <Text style={styles.buttonText}>show</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        mounts {mounts} runs {runs}
      </Text>
      <Text style={styles.text}>
        {Date.now() > CLOCK_SET_MS ? 'clock set' : 'no clock'}
      </Text>
      <View style={{width: width / 2, height: 8, backgroundColor: '#f4a261'}} />
      {names.map((name, index) => (
        <Text style={styles.text}>
          {index}: {name}
        </Text>
      ))}
      {shown ? (
        <View style={styles.branch}>
          {names.map(name => (
            <Text style={styles.text}>shown {name}</Text>
          ))}
        </View>
      ) : null}
      {SHARES.map(row => (
        <Bar share={row.share} />
      ))}
      <Svg width={width / 2} height={height / 8} viewBox="0 0 100 50">
        <Path d="M10 5 L90 5 L90 45 C 80 30 60 30 50 45 Z" fill="#60e080" />
      </Svg>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, padding: 8, gap: 4, backgroundColor: '#101820'},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  button: {
    width: 48,
    height: 24,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 10, color: '#ffffff'},
  text: {fontSize: 10, color: '#f0f0f0'},
  branch: {gap: 2, padding: 2, backgroundColor: '#303840'},
});
