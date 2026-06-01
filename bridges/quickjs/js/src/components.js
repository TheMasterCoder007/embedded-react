// Host components. Each is just the string tag the reconciler passes to NativeUI.createNode().
// JSX `<View/>` compiles to React.createElement('View', ...), so the host config receives
// type === 'View' and maps it straight onto an ERNodeType. The `'embedded-react'` developer
// package will eventually re-export these (plus Animated, hooks, StyleSheet) — for now this is
// the minimal set the renderer understands.
export const View = 'View';
export const Text = 'Text';
export const Image = 'Image';
export const ScrollView = 'ScrollView';
export const FlatList = 'FlatList';
export const Pressable = 'Pressable';
export const TextInput = 'TextInput';
export const Switch = 'Switch';
export const ActivityIndicator = 'ActivityIndicator';
export const Modal = 'Modal';
