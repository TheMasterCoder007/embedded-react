// Host components. Each is the string tag the reconciler passes to NativeUI.createNode(), which
// maps it onto an ERNodeType. JSX `<View/>` → createElement('View', ...) → createInstance('View').
export const View = 'View';
export const Text = 'Text';
export const Image = 'Image';
export const ScrollView = 'ScrollView';
export const FlatList = 'FlatList';
export const Pressable = 'Pressable';
// TouchableOpacity is RN's press-with-opacity wrapper; for now it maps to the same Pressable node.
export const TouchableOpacity = 'Pressable';
export const TextInput = 'TextInput';
export const Switch = 'Switch';
export const ActivityIndicator = 'ActivityIndicator';
export const Modal = 'Modal';
