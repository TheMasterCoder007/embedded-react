// FlatList: rows rendered from state, with a key extractor, a press on a row, and a row added.
import {useState} from 'react';
import {View, Text, Pressable, FlatList, StyleSheet} from 'embedded-react';

export function App() {
  const [items, setItems] = useState([
    {id: 1, name: 'one'},
    {id: 2, name: 'two'},
  ]);
  const [picked, setPicked] = useState('none');

  return (
    <View style={styles.root}>
      <Pressable
        style={styles.add}
        onPress={() => setItems([...items, {id: 3, name: 'more'}])}>
        <Text style={styles.label}>add, picked {picked}</Text>
      </Pressable>
      <FlatList
        style={styles.list}
        data={items}
        keyExtractor={item => item.id}
        renderItem={({item, index}) => (
          <Pressable style={styles.row} onPress={() => setPicked(item.name)}>
            <Text style={styles.label}>
              {index}: {item.name}
            </Text>
          </Pressable>
        )}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f8f4f0', padding: 8, gap: 8},
  add: {height: 36, padding: 6, backgroundColor: '#90c0f0'},
  list: {flex: 1},
  row: {height: 32, padding: 4, borderWidth: 1, borderColor: '#a09080'},
  label: {fontSize: 16, color: '#102030'},
});
