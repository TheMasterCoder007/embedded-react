// Tagged records: object state of several shapes, told apart by a tag field. Tests on the tag narrow the fields that
// can be read, in render, in handlers and in a setter's ternary. A boolean tag narrows when read as a test. Each
// light holds a tagged state of its own.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

type Job =
  | {kind: 'idle'}
  | {kind: 'busy'; pct: number}
  | {kind: 'done'; took: number; note: string};
type Check = {ok: true; value: number} | {ok: false; error: string};
type Lamp = {on: false} | {on: true; level: number};

function Light() {
  const [lamp, setLamp] = useState<Lamp>({on: false});
  return (
    <Pressable
      style={styles.button}
      onPress={() =>
        setLamp(
          lamp.on
            ? lamp.level >= 3
              ? {on: false}
              : {...lamp, level: lamp.level + 1}
            : {on: true, level: 1},
        )
      }>
      <View
        style={{
          width: lamp.on ? lamp.level * 12 : 4,
          height: 8,
          backgroundColor: lamp.on ? '#e0a030' : '#606060',
        }}
      />
    </Pressable>
  );
}

export function App() {
  const [job, setJob] = useState<Job>({kind: 'idle'});
  const [check, setCheck] = useState<Check>({ok: true, value: 0});

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => setJob({kind: 'busy', pct: 0})}>
          <Text style={styles.buttonText}>start</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            if (job.kind === 'busy') {
              setJob(
                job.pct >= 50
                  ? {kind: 'done', took: job.pct / 20, note: 'ok'}
                  : {...job, pct: job.pct + 25},
              );
            } else if (job.kind === 'done') {
              setJob({kind: 'idle'});
            }
          }}>
          <Text style={styles.buttonText}>step</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setJob(
              job.kind === 'busy'
                ? {kind: 'done', took: 0.5, note: 'skipped'}
                : job,
            )
          }>
          <Text style={styles.buttonText}>skip</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setCheck(
              check.ok && check.value > 0
                ? {ok: false, error: `bad ${check.value}`}
                : {ok: true, value: job.kind === 'busy' ? job.pct : 0},
            )
          }>
          <Text style={styles.buttonText}>check</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => {
            setJob({kind: 'idle'});
            setCheck({ok: false, error: 'reset'});
          }}>
          <Text style={styles.buttonText}>reset</Text>
        </Pressable>
        <Light />
        <Light />
      </View>
      <Text style={styles.text}>{job.kind}</Text>
      {job.kind === 'done' ? (
        <Text style={styles.text}>
          {job.note} in {job.took}s
        </Text>
      ) : (
        <Text style={styles.text}>{job.kind === 'busy' ? job.pct : 0}%</Text>
      )}
      <View style={styles.track}>
        {job.kind === 'busy' && (
          <View
            style={{
              width: job.pct * 2,
              height: 8,
              backgroundColor: job.pct >= 50 ? '#e0a030' : '#30a0e0',
            }}
          />
        )}
      </View>
      <Text style={styles.text}>{check.ok ? 'ok' : check.error}</Text>
      <Text style={styles.text}>{check.ok ? check.value : -1}</Text>
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
  buttonText: {fontSize: 10, color: '#ffffff'},
  text: {fontSize: 10, color: '#f0f0f0'},
  track: {width: 200, height: 8, backgroundColor: '#202838'},
});
