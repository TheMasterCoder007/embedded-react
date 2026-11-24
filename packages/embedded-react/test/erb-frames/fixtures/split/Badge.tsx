import React from 'react';
import {StyleSheet, Text, View} from 'embedded-react';
import {colors} from './theme';

const styles = StyleSheet.create({
  badge: {marginTop: 8, padding: 4, backgroundColor: colors.badge},
});

export default function Badge({value}: {value: number}) {
  return (
    <View style={styles.badge}>
      <Text>{value}</Text>
    </View>
  );
}
