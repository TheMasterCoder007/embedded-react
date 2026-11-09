// List edits: remove and update items in place. A row's press flips it with `with`, and its x removes it with
// `filter`. The buttons remove by `filter`, `toSpliced` and a pair of slices, and update by `map` (one row by index,
// every row by a condition, a nested field, a field set to undefined), `with`, and `map` over plain values.
import {useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

interface Task {
  title: string;
  done: boolean;
  n: number;
  pos: {x: number; y: number};
  note?: string;
}

export function App() {
  const [tasks, setTasks] = useState<Task[]>([
    {title: 'wash', done: false, n: 1, pos: {x: 1, y: 2}},
    {title: 'cook', done: true, n: 2, pos: {x: 3, y: 4}, note: 'soon'},
    {title: 'shop', done: false, n: 3, pos: {x: 5, y: 6}},
    {title: 'read', done: false, n: 4, pos: {x: 7, y: 8}},
  ]);
  const [counts, setCounts] = useState([3, 1, 4, 1]);
  const [picked, setPicked] = useState(0);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() => setPicked((picked + 1) % 5)}>
          <Text style={styles.buttonText}>next</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setTasks(tasks.filter((_, i) => i !== picked))}>
          <Text style={styles.buttonText}>drop</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setTasks(prev => prev.toSpliced(-1, 1))}>
          <Text style={styles.buttonText}>cut</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setCounts([...counts.slice(0, picked), ...counts.slice(picked + 1)])
          }>
          <Text style={styles.buttonText}>skip</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks(
              tasks.map((task, i) =>
                i === picked
                  ? {...task, n: task.n + 10, title: task.title.toUpperCase()}
                  : task,
              ),
            )
          }>
          <Text style={styles.buttonText}>bump</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks(
              tasks.map(task =>
                task.done
                  ? {...task, done: false, note: undefined}
                  : {
                      ...task,
                      done: true,
                      pos: {...task.pos, x: task.pos.x + 1},
                    },
              ),
            )
          }>
          <Text style={styles.buttonText}>flip</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks(
              tasks.with(picked, {...tasks[picked], title: 'new', note: 'set'}),
            )
          }>
          <Text style={styles.buttonText}>swap</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setCounts(counts.map((count, i) => (i % 2 ? count * 2 : count)))
          }>
          <Text style={styles.buttonText}>twice</Text>
        </Pressable>
      </View>
      <Text style={styles.label}>
        picked {picked}, {tasks.length} tasks
      </Text>
      {tasks.map((task, index) => (
        <View key={index} style={styles.row}>
          <Pressable
            style={styles.item}
            onPress={() =>
              setTasks(tasks.with(index, {...task, done: !task.done}))
            }>
            <Text
              style={[styles.text, {color: task.done ? '#80e080' : '#f0f0f0'}]}>
              {task.title} n{task.n} at {task.pos.x},{task.pos.y}{' '}
              {task.note ?? '-'}
            </Text>
          </Pressable>
          <Pressable
            style={styles.remove}
            onPress={() => setTasks(tasks.filter((_, i) => i !== index))}>
            <Text style={styles.text}>x</Text>
          </Pressable>
        </View>
      ))}
      <View style={styles.bar}>
        {counts.map((count, index) => (
          <Text key={index} style={styles.label}>
            {count}
          </Text>
        ))}
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
  row: {flexDirection: 'row', gap: 4, height: 16},
  item: {width: 180, backgroundColor: '#304050', justifyContent: 'center'},
  remove: {
    width: 20,
    backgroundColor: '#804040',
    alignItems: 'center',
    justifyContent: 'center',
  },
  text: {fontSize: 10, color: '#f0f0f0'},
});
