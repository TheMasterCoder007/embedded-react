// PanResponder in a list row's component: each row's drag reads the label its row passes it.
import {useState, useRef} from 'react';
import {View, Text, PanResponder, StyleSheet} from 'embedded-react';

function Row({label}) {
  const [moved, setMoved] = useState(0);
  const pan = useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onPanResponderMove: (e, g) =>
        setMoved((label === 'second' ? 100 : 200) + g.dx),
    }),
  ).current;
  return (
    <View {...pan.panHandlers} style={styles.row}>
      <Text style={styles.label}>
        {label} {moved}
      </Text>
    </View>
  );
}

export function App() {
  const [items] = useState([
    {id: 1, name: 'first'},
    {id: 2, name: 'second'},
    {id: 3, name: 'third'},
  ]);
  return (
    <View style={styles.root}>
      {items.map(item => (
        <Row key={item.id} label={item.name} />
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 6},
  row: {height: 40, backgroundColor: '#304060', justifyContent: 'center'},
  label: {fontSize: 14, color: '#ffffff'},
});
