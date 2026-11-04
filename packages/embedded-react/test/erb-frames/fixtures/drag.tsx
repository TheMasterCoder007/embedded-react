// PanResponder: a knob claimed on touch-down and dragged along a track, and a pad claimed only once the
// finger has moved past a slop, so its travel opens at 0 from the grant. Each gesture's fields are shown.
import {useState, useRef} from 'react';
import {View, Text, PanResponder, StyleSheet} from 'embedded-react';

export function App() {
  const [knob, setKnob] = useState(20);
  const [drag, setDrag] = useState(0);
  const [active, setActive] = useState(false);
  const [grants, setGrants] = useState(0);
  const [granted, setGranted] = useState('no grant yet');
  const [moved, setMoved] = useState('no move yet');
  const [released, setReleased] = useState('no release yet');
  const [pad, setPad] = useState(0);

  const knobPan = useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onPanResponderGrant: (event, gestureState) => {
        setActive(true);
        setGrants(grants + 1);
        setGranted(
          'grant at ' +
            gestureState.x0 +
            ',' +
            gestureState.y0 +
            ' id ' +
            gestureState.stateID,
        );
      },
      onPanResponderMove: (event, gestureState) => {
        setDrag(gestureState.dx);
        setMoved(
          'move ' +
            gestureState.dx +
            ',' +
            gestureState.dy +
            ' at ' +
            gestureState.moveX +
            ',' +
            gestureState.moveY +
            ' x' +
            gestureState.numberActiveTouches,
        );
      },
      onPanResponderRelease: (event, gestureState) => {
        setKnob(Math.max(0, Math.min(180, knob + gestureState.dx)));
        setDrag(0);
        setActive(false);
        setReleased(
          'release ' +
            gestureState.dx +
            ' fast ' +
            (Math.abs(gestureState.vx) > 0.2),
        );
      },
    }),
  ).current;

  const padPan = useRef(
    PanResponder.create({
      onMoveShouldSetPanResponder: (event, gestureState) =>
        Math.abs(gestureState.dx) > 12,
      onPanResponderMove: (event, gestureState) => setPad(gestureState.dx),
      onPanResponderRelease: (event, gestureState) =>
        setReleased(
          'pad ' +
            gestureState.dx +
            ' from ' +
            gestureState.x0 +
            ' id ' +
            gestureState.stateID,
        ),
    }),
  ).current;

  return (
    <View style={styles.root}>
      <Text style={styles.label}>{granted}</Text>
      <Text style={styles.label}>
        {moved} {active ? 'down' : 'up'} {grants}
      </Text>
      <View style={styles.track}>
        <View
          {...knobPan.panHandlers}
          style={[
            styles.knob,
            {
              marginLeft: knob + drag,
              backgroundColor: active ? '#ffb040' : '#4080ff',
            },
          ]}
        />
      </View>
      <View {...padPan.panHandlers} style={styles.pad}>
        <View style={[styles.dot, {marginLeft: 100 + pad}]} />
      </View>
      <Text style={styles.label}>{released}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 10, gap: 8},
  label: {height: 20, fontSize: 14, color: '#d0e0f0'},
  track: {height: 40, borderRadius: 20, backgroundColor: '#30405a'},
  knob: {width: 40, height: 40, borderRadius: 20},
  pad: {
    height: 80,
    borderRadius: 8,
    backgroundColor: '#284030',
    justifyContent: 'center',
  },
  dot: {width: 20, height: 20, borderRadius: 10, backgroundColor: '#60e080'},
});
