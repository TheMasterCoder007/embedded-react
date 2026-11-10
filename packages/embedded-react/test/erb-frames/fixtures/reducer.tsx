// useReducer: each dispatch runs the reducer in place, keeping only the case its action's type picks, so a `throw`
// for a type no dispatch sends never runs. A number, an object, a tagged object and a list, a reducer written inline
// that takes plain string actions, and an action picked at run time.
import {useReducer, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

type Job = {kind: 'idle'} | {kind: 'busy'; pct: number};
type CountAction = {type: 'inc'} | {type: 'add'; by: number} | {type: 'reset'};
type WalkAction = {type: 'step'; dx: number} | {type: 'down'};
type ListAction =
  {type: 'add'; value: number} | {type: 'drop'} | {type: 'clear'};

function count(state: number, action: CountAction): number {
  switch (action.type) {
    case 'inc':
      return state + 1;
    case 'add':
      return state + action.by;
    case 'reset':
      return 0;
    default:
      throw new Error('unknown action');
  }
}

function walk(state: {x: number; y: number}, action: WalkAction) {
  if (action.type === 'step') {
    const x = state.x + action.dx;
    if (x > 4) return {x: 0, y: state.y + 1};
    return {...state, x};
  }
  return {...state, y: state.y + 1};
}

function job(state: Job, action: {type: 'start'} | {type: 'tick'}): Job {
  switch (action.type) {
    case 'start':
      return {kind: 'busy', pct: 0};
    case 'tick':
      if (state.kind !== 'busy') return state;
      if (state.pct >= 75) return {kind: 'idle'};
      return {kind: 'busy', pct: state.pct + 25};
  }
}

function items(state: number[], action: ListAction) {
  switch (action.type) {
    case 'add':
      return [...state, action.value];
    case 'drop':
      return state.filter((_, index) => index !== 0);
    case 'clear':
      return [];
  }
}

export function App() {
  const [n, dispatchCount] = useReducer(count, 0);
  const [p, dispatchWalk] = useReducer(walk, {x: 0, y: 0});
  const [task, dispatchJob] = useReducer(job, {kind: 'idle'} as Job);
  const [list, dispatchList] = useReducer(items, [1, 2]);
  const [mode, dispatchMode] = useReducer(
    (current: string, action: string) =>
      action === 'next' ? (current === 'a' ? 'b' : 'a') : 'a',
    'a',
  );
  const [step, setStep] = useState(3);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => dispatchCount({type: 'inc'})}>
          <Text style={styles.buttonText}>inc</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchCount({type: 'add', by: step})}>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchCount({type: 'reset'})}>
          <Text style={styles.buttonText}>reset</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            dispatchCount(step > 3 ? {type: 'add', by: step} : {type: 'inc'});
            setStep(step + 1);
          }}>
          <Text style={styles.buttonText}>pick</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => dispatchWalk({type: 'step', dx: 2})}>
          <Text style={styles.buttonText}>step</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchWalk({type: 'down'})}>
          <Text style={styles.buttonText}>down</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchJob({type: 'start'})}>
          <Text style={styles.buttonText}>start</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchJob({type: 'tick'})}>
          <Text style={styles.buttonText}>tick</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => dispatchList({type: 'add', value: list.length * 10})}>
          <Text style={styles.buttonText}>push</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchList({type: 'drop'})}>
          <Text style={styles.buttonText}>drop</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => dispatchList({type: 'clear'})}>
          <Text style={styles.buttonText}>clear</Text>
        </Pressable>
        <Pressable style={styles.button} onPress={() => dispatchMode('next')}>
          <Text style={styles.buttonText}>mode</Text>
        </Pressable>
      </View>
      <Text style={styles.text}>
        n {n} step {step}
      </Text>
      <Text style={styles.text}>
        p {p.x},{p.y}
      </Text>
      <Text style={styles.text}>
        job {task.kind === 'busy' ? task.pct : -1}
      </Text>
      <View style={styles.row}>
        {list.map(value => (
          <Text key={value} style={styles.text}>
            {value}
          </Text>
        ))}
      </View>
      <Text style={styles.text}>mode {mode}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  row: {flexDirection: 'row', gap: 8, height: 14},
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
});
