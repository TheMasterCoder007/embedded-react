// useContext: worked out at compile time from the nearest provider above each component, or the context's default
// with none. A provider's value can be state or an object with a callback, an inner provider wins, a provider a
// component returns reaches the children it is given, and a custom hook can read a context.
import {createContext, useCallback, useContext, useState} from 'react';
import type {ReactNode} from 'react';
import {View, Text, Pressable, StyleSheet} from 'embedded-react';

const Theme = createContext('plain');
const Counter = createContext({count: 0, bump: () => {}});

function Swatch() {
  const theme = useContext(Theme);
  return (
    <View
      style={[
        styles.swatch,
        {
          backgroundColor:
            theme === 'dark'
              ? '#404040'
              : theme === 'warm'
                ? '#c06030'
                : '#80a0c0',
        },
      ]}>
      <Text style={styles.text}>{theme}</Text>
    </View>
  );
}

function Bump() {
  const {count, bump} = useContext(Counter);
  return (
    <Pressable style={styles.button} onPress={bump}>
      <Text style={styles.buttonText}>+ {count}</Text>
    </Pressable>
  );
}

function WarmZone({children}: {children: ReactNode}) {
  return <Theme.Provider value="warm">{children}</Theme.Provider>;
}

function useTheme() {
  return useContext(Theme);
}

function ThemeName() {
  const theme = useTheme();
  return <Text style={styles.text}>theme {theme}</Text>;
}

export function App() {
  const [dark, setDark] = useState(false);
  const [count, setCount] = useState(0);
  const bump = useCallback(() => setCount(count + 1), [count]);

  return (
    <View style={styles.root}>
      <View style={styles.bar}>
        <Pressable style={styles.button} onPress={() => setDark(!dark)}>
          <Text style={styles.buttonText}>dark</Text>
        </Pressable>
        <Counter.Provider value={{count, bump}}>
          <Bump />
          <Bump />
        </Counter.Provider>
        <Bump />
      </View>
      <Swatch />
      <Theme.Provider value={dark ? 'dark' : 'light'}>
        <Swatch />
        <ThemeName />
        <WarmZone>
          <Swatch />
        </WarmZone>
      </Theme.Provider>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#182030', padding: 8, gap: 4},
  bar: {flexDirection: 'row', gap: 4, height: 24},
  button: {
    width: 48,
    height: 24,
    borderRadius: 4,
    backgroundColor: '#3060c0',
    alignItems: 'center',
    justifyContent: 'center',
  },
  buttonText: {fontSize: 10, color: '#ffffff'},
  swatch: {height: 20, justifyContent: 'center', paddingLeft: 4},
  text: {fontSize: 10, color: '#f0f0f0'},
});
