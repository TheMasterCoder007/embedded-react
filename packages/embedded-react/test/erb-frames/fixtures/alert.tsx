// Alert: raised from the app, a child component and a list row. A button closes it and runs its handler, which can
// raise another; a cancelable one closes on a tap outside the sheet.
import {useState} from 'react';
import {View, Text, Pressable, Alert, StyleSheet} from 'embedded-react';

function Ask({name}: {name: string}) {
  const [asked, setAsked] = useState(0);

  return (
    <Pressable
      style={styles.button}
      onPress={() =>
        Alert.alert('Sure?', 'Asked by ' + name + '.', [
          {text: 'No'},
          {
            text: 'Yes',
            onPress: () => {
              setAsked(asked + 1);
              Alert.alert('Done');
            },
          },
        ])
      }>
      <Text style={styles.label}>
        {name} {asked}
      </Text>
    </Pressable>
  );
}

export function App() {
  const [count, setCount] = useState(3);
  const [last, setLast] = useState('none');
  const [items, setItems] = useState([{name: 'wash'}, {name: 'cook'}]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable
          style={styles.button}
          onPress={() =>
            Alert.alert('Reset count?', 'It is at ' + count + '.', [
              {
                text: 'Cancel',
                style: 'cancel',
                onPress: () => setLast('cancel'),
              },
              {
                text: 'Reset',
                style: 'destructive',
                onPress: () => setCount(0),
              },
            ])
          }>
          <Text style={styles.label}>reset {count}</Text>
        </Pressable>
        <Pressable
          style={styles.button}
          onPress={() =>
            Alert.alert('Note', undefined, undefined, {
              cancelable: true,
              onDismiss: () => setLast('dismissed'),
            })
          }>
          <Text style={styles.label}>note</Text>
        </Pressable>
        <Ask name="child" />
      </View>
      <Text style={styles.label}>last {last}</Text>
      {items.map((item, index) => (
        <Pressable
          key={item.name}
          style={styles.button}
          onPress={() =>
            Alert.alert('Remove ' + item.name + '?', undefined, [
              {text: 'Keep'},
              {
                text: 'Remove',
                style: 'destructive',
                onPress: () => setItems(items.filter((_, i) => i !== index)),
              },
            ])
          }>
          <Text style={styles.label}>{item.name}</Text>
        </Pressable>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#e0e8f0', padding: 12, gap: 8},
  bar: {flexDirection: 'row', gap: 8},
  button: {height: 40, borderRadius: 6, padding: 8, backgroundColor: '#90c0f0'},
  label: {fontSize: 16, color: '#102030'},
});
