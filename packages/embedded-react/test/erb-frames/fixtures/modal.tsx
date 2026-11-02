// Modal: shown by state over the screen, with its backdrop, and closed from inside. A second stays shown.
import {useState} from 'react';
import {View, Text, Pressable, Modal, StyleSheet} from 'embedded-react';

export function App() {
  const [open, setOpen] = useState(false);
  const [closes, setCloses] = useState(0);

  return (
    <View style={styles.root}>
      <Pressable style={styles.button} onPress={() => setOpen(true)}>
        <Text style={styles.label}>open, closed {closes}</Text>
      </Pressable>
      <Modal style={styles.modal} visible={open} backdropColor="#80002040">
        <View style={styles.sheet}>
          <Text style={styles.label}>a modal</Text>
          <Pressable
            style={styles.button}
            onPress={() => {
              setOpen(false);
              setCloses(closes + 1);
            }}>
            <Text style={styles.label}>close</Text>
          </Pressable>
        </View>
      </Modal>
    </View>
  );
}

const styles = StyleSheet.create({
  modal: {padding: 4},
  root: {flex: 1, backgroundColor: '#e0e8f0', padding: 12},
  sheet: {
    width: 180,
    padding: 12,
    gap: 8,
    borderRadius: 10,
    backgroundColor: '#ffffff',
  },
  button: {height: 40, borderRadius: 6, padding: 8, backgroundColor: '#90c0f0'},
  label: {fontSize: 16, color: '#102030'},
});
