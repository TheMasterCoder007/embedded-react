// <Svg source> with text: the .svg's <text> and <tspan> bake to outlines from the fonts this file imports.
import {View, Svg, StyleSheet} from 'embedded-react';
import Inter from '../../../../../assets/fonts/Inter-Regular.ttf';
import Bold from '../../../../../assets/fonts/Inter-Bold.ttf';
import compass from './assets/compass.svg';

export function App() {
  return (
    <View style={styles.root}>
      <Svg source={compass} width={200} height={200} />
      <Svg source={compass} width={80} height={80} />
    </View>
  );
}

// An .svg cannot import a font, so its families are the ones imported here.
export const FONTS = [Inter, Bold];

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#ffffff', padding: 8, gap: 8},
});
