// Styling with no state: radii, borders, opacity, z-order and an image.
import {View, Image, StyleSheet} from 'embedded-react';
import cog from '../../../../../demos/thermostat/assets/cog.png';

export function App() {
  return (
    <View style={styles.root}>
      <View style={styles.card} />
      <View style={styles.pill} />
      <View style={styles.stack}>
        <View style={styles.under} />
        <View style={styles.over} />
      </View>
      <View style={styles.faded} />
      <Image
        source={cog}
        style={styles.icon}
        resizeMode="contain"
        tintColor="#9ad0ff"
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: '#20242c',
    alignItems: 'center',
    justifyContent: 'space-evenly',
  },
  card: {
    width: 140,
    height: 50,
    borderRadius: 12,
    backgroundColor: '#f4f4f8',
    borderWidth: 3,
    borderColor: '#5070ff',
  },
  pill: {
    width: 160,
    height: 40,
    borderTopLeftRadius: 20,
    borderBottomRightRadius: 20,
    backgroundColor: '#ff6a3d',
  },
  stack: {width: 80, height: 50},
  under: {
    position: 'absolute',
    left: 0,
    top: 0,
    width: 50,
    height: 50,
    backgroundColor: '#3dd68c',
    zIndex: 2,
  },
  over: {
    position: 'absolute',
    left: 30,
    top: 0,
    width: 50,
    height: 50,
    backgroundColor: '#3d8cd6',
    zIndex: 1,
  },
  faded: {width: 120, height: 24, backgroundColor: '#ffd23d', opacity: 0.45},
  icon: {width: 48, height: 48},
});
