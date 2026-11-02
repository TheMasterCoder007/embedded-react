// The size and spacing styles layout.jsx and style.jsx leave out: limits, each side's padding and margin, gaps, a
// flex basis, shrinking, hiding, a bottom inset and one corner's radius.
import {View, StyleSheet} from 'embedded-react';

export function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <View style={[styles.a, {flexBasis: 60}]} />
        <View style={[styles.b, {flexGrow: 1, minWidth: 40, maxWidth: 70}]} />
        <View style={[styles.c, {width: 200, flexShrink: 1}]} />
      </View>
      <View style={[styles.row, {minHeight: 30, maxHeight: 40}]}>
        <View style={[styles.a, {width: 30, height: 60}]} />
        <View style={[styles.b, {width: 30, display: 'none'}]} />
        <View style={[styles.c, {width: 30, height: 10}]} />
      </View>
      <View
        style={[
          styles.frame,
          {paddingTop: 2, paddingRight: 4, paddingBottom: 6, paddingLeft: 8},
        ]}>
        <View style={styles.fill} />
      </View>
      <View style={[styles.frame, {paddingVertical: 6}]}>
        <View style={styles.fill} />
      </View>
      <View style={styles.frame}>
        <View
          style={[
            styles.fill,
            {marginTop: 1, marginRight: 3, marginBottom: 5, marginLeft: 7},
          ]}
        />
      </View>
      <View style={styles.frame}>
        <View style={[styles.fill, {margin: 3}]} />
      </View>
      <View style={styles.frame}>
        <View
          style={[styles.fill, {marginVertical: 4, marginHorizontal: 12}]}
        />
      </View>
      <View style={{rowGap: 2}}>
        <View style={[styles.a, {height: 8}]} />
        <View style={[styles.b, {height: 8}]} />
      </View>
      <View style={[styles.row, {columnGap: 10}]}>
        <View style={[styles.a, {width: 50}]} />
        <View style={[styles.b, {width: 50}]} />
      </View>
      <View style={styles.corner} />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 4, gap: 4},
  row: {flexDirection: 'row', gap: 4, height: 24},
  frame: {height: 30, backgroundColor: '#d0d8e0'},
  fill: {flex: 1, backgroundColor: '#4080c0'},
  a: {height: 24, backgroundColor: '#e06040'},
  b: {height: 24, backgroundColor: '#40a060'},
  c: {height: 24, backgroundColor: '#8060c0'},
  corner: {
    position: 'absolute',
    right: 4,
    bottom: 4,
    width: 40,
    height: 40,
    borderBottomLeftRadius: 20,
    backgroundColor: '#f0a020',
  },
});
