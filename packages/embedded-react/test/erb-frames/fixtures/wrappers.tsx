// Button, ImageBackground and SectionList, as the trees their Flow A components render: a button that counts, a
// colored one that adds a row, a disabled one, text over a picture, and sections whose first one's rows are state.
import {useState} from 'react';
import {
  View,
  Text,
  Button,
  ImageBackground,
  SectionList,
  StyleSheet,
} from 'embedded-react';
import cog from './assets/cog.png';

const FIXED = ['one', 'two'];

export function App() {
  const [count, setCount] = useState(0);
  const [names, setNames] = useState(['a']);

  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Button title={`count ${count}`} onPress={() => setCount(count + 1)} />
        <Button
          title="add"
          color="#c06030"
          onPress={() => setNames([...names, `n${names.length}`])}
        />
        <Button title="off" disabled onPress={() => setCount(0)} />
      </View>
      <ImageBackground
        source={cog}
        resizeMode="cover"
        style={styles.picture}
        imageStyle={styles.pictureImage}>
        <Text style={styles.over}>over the picture</Text>
      </ImageBackground>
      <SectionList
        style={styles.list}
        sections={[
          {title: 'Saved', data: names},
          {title: 'Fixed', data: FIXED},
        ]}
        renderSectionHeader={({section}) => (
          <View style={styles.header}>
            <Text style={styles.headerText}>{section.title}</Text>
          </View>
        )}
        renderItem={({item, index}) => (
          <Text style={styles.item}>
            {index}: {item}
          </Text>
        )}
        renderSectionFooter={() => <View style={styles.footer} />}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  root: {flex: 1, backgroundColor: '#f0ece8', padding: 8, gap: 8},
  row: {flexDirection: 'row', gap: 8},
  picture: {height: 64, padding: 8, justifyContent: 'flex-end'},
  pictureImage: {borderRadius: 8},
  over: {fontSize: 16, color: '#102030'},
  list: {flex: 1},
  header: {backgroundColor: '#405060', paddingLeft: 4},
  headerText: {fontSize: 16, color: '#ffffff'},
  item: {fontSize: 16, color: '#102030', paddingLeft: 8},
  footer: {height: 6},
});
