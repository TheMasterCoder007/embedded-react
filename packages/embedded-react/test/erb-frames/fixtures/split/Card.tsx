import React from 'react';
import {Pressable, StyleSheet, Text} from 'embedded-react';
import {colors} from './theme';

interface Props {
  title: string;
  value: number;
  onPress: () => void;
}

const styles = StyleSheet.create({
  card: {padding: 12, borderRadius: 6, backgroundColor: colors.card},
  title: {color: colors.text, fontSize: 14},
});

export function Card({title, value, onPress}: Props) {
  return (
    <Pressable style={styles.card} onPress={onPress}>
      <Text style={styles.title}>
        {title}: {value}
      </Text>
    </Pressable>
  );
}
