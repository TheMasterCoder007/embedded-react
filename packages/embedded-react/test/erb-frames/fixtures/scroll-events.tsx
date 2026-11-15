// onScroll on a ScrollView: the offset kept in state by an inline handler and by a useCallback a second list
// shares, moving an indicator and hiding a header, from a drag and from a scrollTo in a loop.
import {useCallback, useRef, useState} from 'react';
import {
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
  scrollTo,
} from 'embedded-react';
import type {NodeHandle, ScrollEvent} from 'embedded-react';

const ROWS = [0, 1, 2, 3, 4, 5, 6, 7];

export function App() {
  const list = useRef<NodeHandle>(null);
  const [offset, setOffset] = useState(0);
  const [events, setEvents] = useState(0);
  const [strip, setStrip] = useState(0);
  const onStripScroll = useCallback((event: ScrollEvent) => {
    setStrip(Math.round(event.scrollY));
  }, []);
  return (
    <View style={styles.root}>
      <Text style={styles.label}>
        at {Math.round(offset)}, strip {strip}, {events > 0 ? 'moved' : 'still'}
      </Text>
      {offset < 60 && <Text style={styles.label}>header</Text>}
      <Pressable
        style={styles.button}
        onPress={() => {
          for (let step = 1; step <= 2; step++) {
            scrollTo(list.current, {y: step * 65});
          }
        }}>
        <Text style={styles.label}>jump</Text>
      </Pressable>
      <View style={styles.lists}>
        <ScrollView
          ref={list}
          style={styles.scroll}
          onScroll={event => {
            setOffset(event.scrollY);
            setEvents(count => count + 1);
          }}>
          {ROWS.map(row => (
            <View
              key={row}
              style={[
                styles.row,
                {backgroundColor: row % 2 ? '#c0d8f0' : '#f0e0c0'},
              ]}>
              <Text style={styles.label}>row {row}</Text>
            </View>
          ))}
        </ScrollView>
        <View style={styles.track}>
          <View style={[styles.thumb, {marginTop: offset / 2}]} />
        </View>
        <ScrollView style={styles.scroll} onScroll={onStripScroll}>
          {ROWS.map(row => (
            <View key={row} style={styles.row}>
              <Text style={styles.label}>strip {row}</Text>
            </View>
          ))}
        </ScrollView>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
  button: {padding: 6, backgroundColor: '#d0e0d0'},
  lists: {flexDirection: 'row', gap: 4},
  scroll: {
    width: 96,
    height: 160,
    padding: 4,
    borderWidth: 2,
    borderColor: '#405060',
  },
  track: {width: 8, height: 160, backgroundColor: '#e0e0e0'},
  thumb: {width: 8, height: 30, backgroundColor: '#405060'},
  row: {height: 50, padding: 8},
  label: {fontSize: 16, color: '#102030'},
});
