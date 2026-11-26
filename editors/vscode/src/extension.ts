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
// the debug file are one version. When a session debugs the app in the simulator, it shows the simulator's page.

import {existsSync, readFileSync} from 'node:fs';
import {delimiter, dirname, join, resolve} from 'node:path';
import * as vscode from 'vscode';

const DEBUG_TYPE = 'embedded-react';
const PACKAGE_NAME = 'embedded-react';
/** The adapter's event that says where the simulator's page is. */
const SIMULATOR_EVENT = 'embedded-react.simulator';

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
 * The version of a package that has no debug adapter, from its CLI.
 *
 * @param cli  The CLI's script.
 *
 * @returns The package's version when it is too old to debug with, or null.
 */
function versionWithoutAdapter(cli: string): string | null {
  const packageFolder = dirname(cli);
  const manifest = join(packageFolder, 'package.json');
  if (!existsSync(manifest)) {
    return null;
  }
  const {name, version} = JSON.parse(readFileSync(manifest, 'utf8'));
  // The published package has the adapter compiled, and the repository's has its source.
  const hasAdapter = ['debug-adapter.mjs', 'debug-adapter.mts'].some(file =>
    existsSync(join(packageFolder, file)),
  );
  return name === PACKAGE_NAME && !hasAdapter ? String(version) : null;
}

/**
 * A program's full path, from PATH.
 *
 * @param program  The program's name.
 *
 * @returns The path, or the name as it was when PATH has none.
 */
function findOnPath(program: string): string {
  const extensions =
    process.platform === 'win32'
      ? (process.env.PATHEXT ?? '.EXE;.CMD').split(';')
      : [''];
  for (const folder of (process.env.PATH ?? '').split(delimiter)) {
    for (const extension of extensions) {
      const candidate = join(folder, program + extension);
      if (folder && existsSync(candidate)) {
        return candidate;
      }
    }
  }
  return program;
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
        const projectFolder: unknown = session.configuration.cwd;
        const starts = [
          typeof debugFile === 'string' ? dirname(debugFile) : null,
          typeof projectFolder === 'string' ? projectFolder : null,
          folder,
        ].filter((start): start is string => typeof start === 'string');
        const named: unknown = session.configuration.cli;
        const cli =
          (typeof named === 'string' && named) ||
          settings.get<string>('cli') ||
          starts.map(findCli).find(found => found !== null);
        if (!cli) {
          throw new Error(
            'No embedded-react package in the project\'s node_modules. Install it there, or name its CLI in the configuration\'s "cli" or the "embedded-react.cli" setting.',
          );
        }
        const tooOld = versionWithoutAdapter(cli);
        if (tooOld) {
          throw new Error(
            `embedded-react ${tooOld} in this project is older than debugging from the editor needs. Update it with "npm install embedded-react@latest".`,
          );
        }
        // The editor runs a command that is just `node` in its own runtime, with require(), which can't load the
        // CLI: it is an ES module. A full path runs the Node.js it names.
        const node = settings.get<string>('node') || 'node';
        return new vscode.DebugAdapterExecutable(
          node === 'node' ? findOnPath(node) : node,
          [cli, 'debug-adapter'],
          {cwd: folder},
        );
      },
    }),
    vscode.debug.onDidReceiveDebugSessionCustomEvent(event => {
      if (
        event.session.type === DEBUG_TYPE &&
        event.event === SIMULATOR_EVENT
      ) {
        void showSimulator(String(event.body?.url ?? ''), event.session);
      }
    }),
    vscode.debug.onDidTerminateDebugSession(session => {
      if (session.id === simulatorSession) {
        simulatorSession = null;
        simulatorPanel?.dispose();
      }
    }),
  );
}

/** The simulator's tab, while one is open, and the launch session that started its simulator. */
let simulatorPanel: vscode.WebviewPanel | null = null;
let simulatorSession: string | null = null;

/**
 * Shows the simulator's page, where the setting says.
 *
 * @param url  The page.
 * @param session  The session that says so.
 */
async function showSimulator(
  url: string,
  session: vscode.DebugSession,
): Promise<void> {
  const where = vscode.workspace
    .getConfiguration(DEBUG_TYPE)
    .get<string>('openSimulator');
  if (!url || where === 'never') {
    return;
  }
  const page = await vscode.env.asExternalUri(vscode.Uri.parse(url));
  if (where === 'browser') {
    await vscode.env.openExternal(page);
    return;
  }
  // A split to the right of the code, which keeps the focus.
  if (!simulatorPanel) {
    simulatorPanel = vscode.window.createWebviewPanel(
      'embedded-react.simulator',
      'Simulator',
      {viewColumn: vscode.ViewColumn.Beside, preserveFocus: true},
      {enableScripts: true, retainContextWhenHidden: true},
    );
    simulatorPanel.onDidDispose(() => {
      simulatorPanel = null;
    });
  }
  // A launch's simulator ends with its session, and its tab closes with it.
  simulatorSession =
    session.configuration.request === 'launch' ? session.id : null;
  const address = page.toString(true);
  simulatorPanel.webview.html = `<!doctype html>
<html>
  <head>
    <meta charset="utf-8" />
    <meta http-equiv="Content-Security-Policy" content="default-src 'none'; frame-src http: https:; style-src 'unsafe-inline'" />
    <style>
      html, body, iframe { margin: 0; padding: 0; width: 100%; height: 100%; border: 0; overflow: hidden; }
    </style>
  </head>
  <body>
    <iframe src="${address.replace(/"/g, '&quot;')}" sandbox="allow-scripts allow-same-origin"></iframe>
  </body>
</html>`;
}

/** Nothing to undo: the registration goes with the extension's subscriptions. */
export function deactivate(): void {}
