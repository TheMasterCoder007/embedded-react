// List items: `items[i]` in text and in a handler, nested, optional and boolean fields, a list of plain values, a
// constant array read by a runtime index, list state and an item by index passed to a child, and arrays built in
// render. Past the end, `items[i]?.title` and `values[i]` read as undefined.
import {useMemo, useState} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

interface Task {
  title: string;
  done: boolean;
  pos: {x: number; y: number};
  note?: string;
  due?: number;
}

const NAMES = ['zero', 'one', 'two'];
const UNITS = [{unit: 'C'}, {unit: 'F'}];

function TaskRow({task}: {task: Task}) {
  return (
    <Text style={[styles.row, {color: task.done ? '#80e080' : '#f0f0f0'}]}>
      {task.title} {task.done ? 'done' : 'open'} at {task.pos.x},{task.pos.y}{' '}
      {task.note ?? '-'} due {task.due}
    </Text>
  );
}

function TaskList({tasks}: {tasks: Task[]}) {
  return (
    <View style={styles.list}>
      <Text style={styles.label}>{tasks.length} tasks</Text>
      {tasks.map((task, index) => (
        <TaskRow key={index} task={task} />
      ))}
    </View>
  );
}

export function App() {
  const [tasks, setTasks] = useState<Task[]>([
    {title: 'wash', done: false, pos: {x: 1, y: 2}, note: 'soon'},
    {title: 'cook', done: true, pos: {x: 3, y: 4}, due: 5},
  ]);
  const [counts, setCounts] = useState([3, 1, 4]);
  const [picked, setPicked] = useState(0);
  const [copied, setCopied] = useState('none');
  const [temp, setTemp] = useState(20);
  const readings = useMemo(
    () => [
      {label: 'temp', value: temp},
      {label: 'half', value: temp / 2},
    ],
    [temp],
  );
  const facts = [
    {label: 'picked', value: picked},
    {label: 'tasks', value: tasks.length},
  ];

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setPicked(picked + 1)}>
          <Text style={styles.buttonText}>next</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => setCopied(tasks[picked]?.title ?? 'past end')}>
          <Text style={styles.buttonText}>copy</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            setTasks([
              ...tasks,
              {
                title: 'new ' + tasks.length,
                done: picked % 2 === 0,
                pos: {x: picked, y: 0},
              },
            ])
          }>
          <Text style={styles.buttonText}>add</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() => {
            setCounts([...counts, picked * 2]);
            setTemp(temp + 3);
          }}>
          <Text style={styles.buttonText}>more</Text>
        </Pressable>
      </View>
      <Text style={styles.label}>
        picked {picked}: {NAMES[picked] ?? 'none'} {tasks[picked]?.title} count{' '}
        {counts[picked]} in {UNITS[picked % 2].unit}
      </Text>
      <Text style={styles.label}>
        copied {copied}, {tasks[picked] ? 'there' : 'past end'}, first note{' '}
        {tasks[0].note === undefined ? 'missing' : tasks[0].note}
      </Text>
      <TaskRow task={tasks[picked % 2]} />
      <TaskList tasks={tasks} />
      <View style={styles.bar}>
        {counts.map((count, index) => (
          <Text key={index} style={styles.label}>
            {count}
          </Text>
        ))}
      </View>
      {readings.map(reading => (
        <Text key={reading.label} style={styles.label}>
          {reading.label} {reading.value}
        </Text>
      ))}
      {facts.map(fact => (
        <Text key={fact.label} style={styles.label}>
          {fact.label} {fact.value}
        </Text>
      ))}
      {[
        {label: 'warm', on: temp > 21},
        {label: 'many', on: tasks.length > 2},
      ].map(flag => (
        <Text key={flag.label} style={styles.label}>
          {flag.label} {flag.on ? 'yes' : 'no'}
        </Text>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 2},
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
  list: {gap: 2},
  row: {fontSize: 10, height: 12},
});
