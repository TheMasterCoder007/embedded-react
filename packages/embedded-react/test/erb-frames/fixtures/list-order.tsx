// List order: filter and sort, in a setter and in render. The buttons filter and sort the tasks and the counts in
// place; the columns show the tasks as they are, the open ones by name, and those with n at least `min`, highest
// first. A press on a task in the first two columns flips it.
import {useMemo, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

interface Task {
  title: string;
  n: number;
  done: boolean;
}

export function App() {
  const [tasks, setTasks] = useState<Task[]>([
    {title: 'wash', n: 3, done: false},
    {title: 'cook', n: 1, done: true},
    {title: 'shop', n: 4, done: false},
    {title: 'Read', n: 1, done: false},
    {title: 'mend', n: 5, done: true},
  ]);
  const [counts, setCounts] = useState([3, 1, 4, 1, 5]);
  const [min, setMin] = useState(0);
  const open = tasks.filter(task => !task.done);
  const byName = useMemo(
    () => open.toSorted((a, b) => a.title.localeCompare(b.title)),
    [open],
  );
  const flip = (title: string) =>
    setTasks(
      tasks.map(task =>
        task.title === title ? {...task, done: !task.done} : task,
      ),
    );

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setMin((min + 1) % 6)}>
          <Text style={styles.buttonText}>min</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setTasks(tasks.filter(task => !task.done))}>
          <Text style={styles.buttonText}>clean</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setTasks(tasks.toSorted((a, b) => a.n - b.n))}>
          <Text style={styles.buttonText}>by n</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks(
              [...tasks].sort((a, b) =>
                a.title < b.title ? -1 : a.title > b.title ? 1 : 0,
              ),
            )
          }>
          <Text style={styles.buttonText}>by abc</Text>
        </Pressable>
      </View>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks([...tasks, {title: 'nap', n: min, done: false}])
          }>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks(prev =>
              prev
                .slice()
                .sort(
                  (a, b) => (b.done ? 1 : 0) - (a.done ? 1 : 0) || b.n - a.n,
                ),
            )
          }>
          <Text style={styles.buttonText}>done</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setCounts(counts.toSorted((a, b) => b - a))}>
          <Text style={styles.buttonText}>high</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setCounts(counts.filter(count => count % 2 === 1))}>
          <Text style={styles.buttonText}>odd</Text>
        </Pressable>
      </View>
      <Text style={styles.label}>
        min {min}, {open.length} open,{' '}
        {tasks.filter(task => task.n >= min).length} at min
      </Text>
      <View style={styles.columns}>
        <View style={styles.column}>
          <Text style={styles.label}>all</Text>
          {tasks.map((task, index) => (
            <Pressable
              key={index}
              style={styles.row}
              onPress={() => flip(task.title)}>
              <Text
                style={[
                  styles.text,
                  {color: task.done ? '#80e080' : '#f0f0f0'},
                ]}>
                {task.title} {task.n}
              </Text>
            </Pressable>
          ))}
        </View>
        <View style={styles.column}>
          <Text style={styles.label}>open abc</Text>
          {byName.map((task, index) => (
            <Pressable
              key={index}
              style={styles.row}
              onPress={() => flip(task.title)}>
              <Text style={styles.text}>
                {index}. {task.title}
              </Text>
            </Pressable>
          ))}
        </View>
        <View style={styles.column}>
          <Text style={styles.label}>n at min</Text>
          {tasks
            .filter(task => task.n >= min)
            .toSorted((a, b) => b.n - a.n)
            .map((task, index) => (
              <View key={index} style={styles.row}>
                <Text style={styles.text}>
                  {task.title} {task.n}
                </Text>
              </View>
            ))}
        </View>
      </View>
      <View style={styles.bar}>
        {counts.map((count, index) => (
          <Text key={index} style={styles.label}>
            {count}
          </Text>
        ))}
        <Text style={styles.label}>|</Text>
        {counts
          .toSorted((a, b) => a - b)
          .map((count, index) => (
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
  columns: {flexDirection: 'row', gap: 4},
  column: {width: 72, gap: 2},
  row: {height: 16, backgroundColor: '#304050', justifyContent: 'center'},
  text: {fontSize: 10, color: '#f0f0f0'},
});
