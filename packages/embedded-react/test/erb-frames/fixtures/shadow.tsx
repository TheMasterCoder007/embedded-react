// Shadows: a static one, an elevation, and one that state drives.
import {useState} from 'react';
import {View, Pressable, StyleSheet} from 'embedded-react';

export function App() {
  const [raised, setRaised] = useState(false);

  return (
    <View style={styles.root}>
      <View style={styles.card} />
      <View style={styles.lifted} />
      <Pressable
        style={[
          styles.button,
          {
            shadowColor: raised ? '#203080' : '#000000',
            shadowOpacity: raised ? 0.8 : 0.3,
            shadowRadius: raised ? 10 : 2,
          },
        ]}
        onPress={() => setRaised(!raised)}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: '#e8ecf4',
    alignItems: 'center',
    justifyContent: 'space-evenly',
  },
  card: {
    width: 140,
    height: 60,
    borderRadius: 12,
    backgroundColor: '#ffffff',
    shadowColor: '#402060',
    shadowOffset: {width: 4, height: 6},
    shadowOpacity: 0.5,
    shadowRadius: 8,
  },
  lifted: {
    width: 140,
    height: 60,
    backgroundColor: '#ffffff',
    elevation: 6,
  },
  button: {
    width: 140,
    height: 60,
    borderRadius: 30,
    backgroundColor: '#ffd23d',
    shadowOffset: {width: 0, height: 4},
  },
});
