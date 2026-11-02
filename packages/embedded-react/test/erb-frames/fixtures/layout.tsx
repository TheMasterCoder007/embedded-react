// Flex layout with no state: directions, alignment, gaps, padding, borders, percentages and absolute boxes.
import {View, StyleSheet} from 'embedded-react';

export function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.box, {backgroundColor: '#e05050'}]} />
        <View style={[styles.box, {backgroundColor: '#50e050', flexGrow: 1}]} />
        <View style={[styles.box, {backgroundColor: '#5050e0'}]} />
      </View>
      <View style={styles.wrap}>
        {[0, 1, 2, 3].map(index => (
          <View
            key={index}
            style={[styles.chip, {opacity: 0.4 + index * 0.08}]}
          />
        ))}
      </View>
      <View style={styles.column}>
        <View style={[styles.bar, {width: '30%'}]} />
        <View style={[styles.bar, {width: '60%', alignSelf: 'center'}]} />
        <View style={[styles.bar, {width: '90%', alignSelf: 'flex-end'}]} />
      </View>
      <View style={styles.badge} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#14181f', padding: 12, gap: 10},
  row: {flexDirection: 'row', gap: 6, height: 48},
  box: {width: 48, borderRadius: 6, borderWidth: 2, borderColor: '#ffffff'},
  wrap: {flexDirection: 'row', gap: 4, justifyContent: 'space-between'},
  chip: {width: 48, height: 22, borderRadius: 11, backgroundColor: '#f0b030'},
  column: {
    flex: 1,
    justifyContent: 'space-around',
    borderWidth: 1,
    borderColor: '#405060',
  },
  bar: {
    height: 14,
    backgroundColor: '#30a0c0',
    borderTopRightRadius: 7,
    borderBottomRightRadius: 7,
  },
  badge: {
    position: 'absolute',
    right: 8,
    top: 8,
    width: 20,
    height: 20,
    borderRadius: 10,
    backgroundColor: '#ff3060',
  },
});
