/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The VS Code side of `embedded-react debug-adapter`: it registers the `embedded-react` debug type and runs the
// adapter from the project's own embedded-react package, so the adapter, the GDB script and the compiler that wrote
// the debug file are one version.

import {existsSync, readFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import * as vscode from 'vscode';

const DEBUG_TYPE = 'embedded-react';
const PACKAGE_NAME = 'embedded-react';

/**
 * The embedded-react CLI installed at or above a folder.
 *
 * @param start  The folder to look from.
 *
 * @returns The CLI's script, or null when no folder up to the root has the package.
 */
function findCli(start: string): string | null {
  for (let folder = resolve(start); ; folder = dirname(folder)) {
    const manifest = join(folder, 'node_modules', PACKAGE_NAME, 'package.json');
    if (existsSync(manifest)) {
      const bin = JSON.parse(readFileSync(manifest, 'utf8')).bin;
      const script = typeof bin === 'string' ? bin : bin?.[PACKAGE_NAME];
      if (script) {
        return join(dirname(manifest), script);
      }
    }
    if (dirname(folder) === folder) {
      return null;
    }
  }
}

/**
 * Registers the debug type.
 *
 * @param context  The extension's context.
 */
export function activate(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory(DEBUG_TYPE, {
      createDebugAdapterDescriptor(session) {
        const settings = vscode.workspace.getConfiguration(DEBUG_TYPE);
        const folder = session.workspaceFolder?.uri.fsPath;
        // The app's project is where its debug file is, which may not be the workspace's root.
        const debugFile: unknown = session.configuration.debugFile;
        const starts = [
          typeof debugFile === 'string' ? dirname(debugFile) : null,
          folder,
        ].filter((start): start is string => typeof start === 'string');
        const cli =
          settings.get<string>('cli') ||
          starts.map(findCli).find(found => found !== null);
        if (!cli) {
          throw new Error(
            'No embedded-react package in the project\'s node_modules. Install it there, or name its CLI in the "embedded-react.cli" setting.',
          );
        }
        return new vscode.DebugAdapterExecutable(
          settings.get<string>('node') || 'node',
          [cli, 'debug-adapter'],
          {cwd: folder},
        );
      },
    }),
  );
}

/** Nothing to undo: the registration goes with the extension's subscriptions. */
export function deactivate(): void {}
