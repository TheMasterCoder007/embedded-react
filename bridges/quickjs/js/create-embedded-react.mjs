// create-embedded-react — scaffolds a new embedded-react app.
//
// Usage (from bridges/quickjs/js):  npm run create -- <name>
//        or from anywhere:          node bridges/quickjs/js/create-embedded-react.mjs <name>
//
// Generates demos/<name>/ (App.jsx + index.jsx + package.json + README + assets/) wired to the repo's
// build/sim tooling, so the new app runs immediately:  cd demos/<name> && npm run sim
//
// This is the in-repo precursor to a published `npx create-embedded-react`: today the embedded-react
// package + firmware hosts live in this monorepo, so a scaffolded app reuses them via the demos/
// convention rather than pulling published npm packages + emitting a standalone firmware project.
import { mkdirSync, writeFileSync, existsSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url)); // bridges/quickjs/js
const repoRoot = resolve(here, '../../..');

const name = process.argv[2];
if (!name) {
  console.error('Usage: npm run create -- <name>   (e.g. npm run create -- my-app)');
  process.exit(1);
}
if (!/^[a-z][a-z0-9-]*$/.test(name)) {
  console.error(`Invalid app name "${name}". Use lowercase letters, digits, and hyphens (must start with a letter).`);
  process.exit(1);
}

const appDir = resolve(repoRoot, 'demos', name);
if (existsSync(appDir)) {
  console.error(`demos/${name} already exists — pick another name or remove it first.`);
  process.exit(1);
}

const files = {
  'App.jsx': `import { useState } from 'react';
import { View, Text, Pressable, StyleSheet } from 'embedded-react';

// Your app. Edit and save — the simulator hot-reloads, and useState survives the reload
// (press R in the sim to reset). The same code runs on a device with only a backend swap.
export function App() {
  const [count, setCount] = useState(0);

  return (
    <View style={styles.screen}>
      <Text style={styles.title}>Hello, embedded-react</Text>
      <Text style={styles.subtitle}>Edit src in demos/${name} and save.</Text>
      <Pressable style={styles.button} onPress={() => setCount((c) => c + 1)}>
        <Text style={styles.buttonText}>Tapped {count} times</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: '#0f172a',
    alignItems: 'center',
    justifyContent: 'center',
    gap: 16,
    padding: 24,
  },
  title: { color: '#f8fafc', fontSize: 24, fontWeight: 'bold' },
  subtitle: { color: '#94a3b8', fontSize: 14 },
  button: { backgroundColor: '#2563eb', paddingHorizontal: 20, paddingVertical: 12, borderRadius: 10 },
  buttonText: { color: '#ffffff', fontSize: 16, fontWeight: 'bold' },
});
`,

  'index.jsx': `import { AppRegistry } from 'embedded-react';
import { App } from './App.jsx';

// Registering the app mounts it into a screen-sized root (RN idiom: running the bundle starts the app).
AppRegistry.registerComponent('${name}', () => App);
`,

  'package.json': `${JSON.stringify(
    {
      name: `${name}-app`,
      private: true,
      description: `embedded-react app "${name}" — run the simulator (hot reload) or build the bundle from here.`,
      scripts: {
        sim: `node ../../bridges/quickjs/js/sim.mjs ${name}`,
        build: `node ../../bridges/quickjs/js/build.mjs ${name}`,
      },
    },
    null,
    2,
  )}\n`,

  'README.md': `# ${name}

An embedded-react app. From this folder:

\`\`\`
npm run sim      # live-reload simulator (hot reload on save) — see /SIMULATOR.md for one-time setup
npm run build    # bundle + bake assets -> bridges/quickjs/js/dist/app.bundle.js
\`\`\`

- **Components & APIs** come from \`embedded-react\` (\`View\`, \`Text\`, \`Pressable\`, \`StyleSheet\`,
  \`Image\`, \`Animated\`, …); **hooks** come from \`react\`.
- **Assets:** \`import logo from './assets/logo.png'\` / \`import Inter from './assets/Inter.ttf'\`, then
  \`<Image source={logo}/>\` / \`fontFamily={Inter}\`. \`npm run build\`/\`sim\` bakes them automatically.
- After \`npm run build\`, run a host (\`examples/linux\` desktop, or \`examples/esp32/esp32-s3\` device).

See the [thermostat demo](../thermostat/) for a fuller example and \`/SIMULATOR.md\` for the dev loop.
`,

  'assets/.gitkeep': '',
};

mkdirSync(resolve(appDir, 'assets'), { recursive: true });
for (const [rel, contents] of Object.entries(files)) {
  writeFileSync(resolve(appDir, rel), contents);
}

console.log(`Created demos/${name}/`);
console.log('  App.jsx  index.jsx  package.json  README.md  assets/');
console.log('\nNext:');
console.log(`  cd demos/${name}`);
console.log('  npm run sim      # hot-reload simulator (build the sim binary once — see /SIMULATOR.md)');
console.log('  npm run build    # just bundle + bake assets');
