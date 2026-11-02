// Image: each resize mode, a tint, a name known at build time, and a source state picks.
import {useState} from 'react';
import {View, Pressable, Image, StyleSheet} from 'embedded-react';
import cog from './assets/cog.png';
import sun from './assets/sun.png';

export function App() {
  const [useSun, setUseSun] = useState(false);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Image style={styles.wide} source={cog} resizeMode="contain" />
        <Image style={styles.wide} source={cog} resizeMode="cover" />
        <Image style={styles.wide} source={cog} resizeMode="stretch" />
      </View>
      <View style={styles.row}>
        <Image style={styles.square} source={cog} tintColor="#ff4040" />
        {/* @ts-expect-error The public types lack imageName and require source. */}
        <Image style={styles.square} imageName="sun" />
        <Pressable style={styles.square} onPress={() => setUseSun(!useSun)}>
          <Image style={styles.square} source={useSun ? sun : cog} />
        </Pressable>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#d0d8e0', padding: 8, gap: 8},
  row: {flexDirection: 'row', gap: 8},
  wide: {width: 70, height: 40},
  square: {width: 48, height: 48},
});
