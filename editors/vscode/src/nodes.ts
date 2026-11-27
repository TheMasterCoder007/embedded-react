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

// The Node Tree view of the debug sidebar: the app's nodes as the engine holds them at a stop, each with its rect, its
// element's line and the props it has set. The adapter answers the tree to its `embedded-react.nodes` request. A node
// opens its element's line, and a line an element starts on selects its node. The selected node is outlined on the
// panel while the app is stopped, where the adapter's `embedded-react.outline` request can draw there.

import * as vscode from 'vscode';

const DEBUG_TYPE = 'embedded-react';
const VIEW = 'embedded-react.nodes';
const NODES_REQUEST = 'embedded-react.nodes';
const OUTLINE_REQUEST = 'embedded-react.outline';
const REFRESH_COMMAND = 'embedded-react.refreshNodes';
const OPEN_COMMAND = 'embedded-react.openNode';
/** The requests that run the app on, after which the tree is the last stop's. */
const RESUMES = ['continue', 'next', 'stepIn', 'stepOut'];
const LONGEST_SUMMARY = 80;

/** A node, as the adapter answers it. */
interface TreeNode {
  id: number;
  type: string;
  rect: {x: number; y: number; width: number; height: number};
  source?: {name: string; path?: string; line: number; column?: number};
  /** Where the components it is made inside are used, outermost first. */
  uses?: {name: string; path?: string; line: number}[];
  props: {name: string; value: string}[];
  children: TreeNode[];
}

/** A row of the view: a node, the group of its props, or one prop. */
type Row =
  | {kind: 'node'; node: TreeNode; parent: Row | undefined}
  | {kind: 'props'; node: TreeNode; parent: Row}
  | {kind: 'prop'; name: string; value: string; id: string; parent: Row};

class NodeTree implements vscode.TreeDataProvider<Row> {
  private readonly changed = new vscode.EventEmitter<Row | undefined>();
  readonly onDidChangeTreeData = this.changed.event;

  /** The session the tree is read from. */
  private session: vscode.DebugSession | null = null;
  /** Set when the tree must be read again, which waits until the view shows. */
  private outdated = false;
  private tops: Row[] = [];
  private readonly rows = new Map<TreeNode, Row>();
  /** The id of the node the view has selected, which keeps its outline at the next stop. */
  private selected: number | null = null;
  view: vscode.TreeView<Row> | null = null;

  /**
   * Reads the tree again from a session, when the view next shows.
   *
   * @param session  The session.
   */
  read(session: vscode.DebugSession): void {
    this.session = session;
    this.outdated = true;
    this.changed.fire(undefined);
  }

  /**
   * Says the app runs on, so the tree is the last stop's.
   *
   * @param session  The session that resumed.
   */
  resumed(session: vscode.DebugSession): void {
    if (session.id === this.session?.id && this.view && this.tops.length) {
      this.view.message = 'From the last stop.';
    }
  }

  /**
   * Drops the tree of a session that ended.
   *
   * @param session  The session.
   */
  ended(session: vscode.DebugSession): void {
    if (session.id !== this.session?.id) {
      return;
    }
    this.session = null;
    this.outdated = false;
    this.show([]);
    this.changed.fire(undefined);
  }

  private show(nodes: TreeNode[], message?: string): void {
    this.rows.clear();
    const row = (node: TreeNode, parent: Row | undefined): Row => {
      const made: Row = {kind: 'node', node, parent};
      this.rows.set(node, made);
      node.children.forEach(child => row(child, made));
      return made;
    };
    this.tops = nodes.map(node => row(node, undefined));
    if (this.view) {
      this.view.message = message;
    }
  }

  private async load(): Promise<void> {
    this.outdated = false;
    const session = this.session;
    if (!session) {
      return;
    }
    try {
      const answer = await session.customRequest(NODES_REQUEST);
      this.show(answer?.nodes ?? []);
      // The view keeps a node selected by its id, and running on took its outline off.
      const kept = [...this.rows.keys()].find(
        node => node.id === this.selected,
      );
      this.selected = kept?.id ?? null;
      if (kept) {
        this.draw(kept.rect);
      }
    } catch (error) {
      this.show([], error instanceof Error ? error.message : String(error));
    }
  }

  async getChildren(row?: Row): Promise<Row[]> {
    if (!row) {
      if (this.outdated) {
        await this.load();
      }
      return this.tops;
    }
    if (row.kind === 'node') {
      const props: Row[] = row.node.props.length
        ? [{kind: 'props', node: row.node, parent: row}]
        : [];
      return [
        ...props,
        ...row.node.children.map(child => this.rows.get(child)!),
      ];
    }
    if (row.kind === 'props') {
      return row.node.props.map(prop => ({
        kind: 'prop',
        ...prop,
        id: `prop:${row.node.id}:${prop.name}`,
        parent: row,
      }));
    }
    return [];
  }

  getParent(row: Row): Row | undefined {
    return row.parent;
  }

  getTreeItem(row: Row): vscode.TreeItem {
    if (row.kind === 'prop') {
      const item = new vscode.TreeItem(row.name);
      item.id = row.id;
      item.description = row.value;
      return item;
    }
    const {node} = row;
    if (row.kind === 'props') {
      const item = new vscode.TreeItem(
        'props',
        vscode.TreeItemCollapsibleState.Collapsed,
      );
      item.id = `props:${node.id}`;
      const summary = node.props
        .map(prop => `${prop.name}: ${prop.value}`)
        .join(', ');
      item.description =
        summary.length > LONGEST_SUMMARY
          ? `${summary.slice(0, LONGEST_SUMMARY)}…`
          : summary;
      item.iconPath = new vscode.ThemeIcon('symbol-property');
      return item;
    }
    const opens = node.props.length > 0 || node.children.length > 0;
    const item = new vscode.TreeItem(
      node.type,
      !opens
        ? vscode.TreeItemCollapsibleState.None
        : row.parent
          ? vscode.TreeItemCollapsibleState.Collapsed
          : vscode.TreeItemCollapsibleState.Expanded,
    );
    item.id = `node:${node.id}`;
    const {x, y, width, height} = node.rect;
    const text = node.props.find(prop => prop.name === 'text')?.value;
    const place = node.source ? `${node.source.name}:${node.source.line}` : '';
    item.description = [text, `${x},${y} ${width}×${height}`, place]
      .filter(Boolean)
      .join('  ');
    const tooltip = new vscode.MarkdownString(
      `**${node.type}** #${node.id}${place ? ` — ${place}` : ''}\n\n`,
    );
    tooltip.appendMarkdown(
      `| | |\n|---|---|\n| rect | \`${x},${y} ${width}×${height}\` |\n`,
    );
    for (const use of [...(node.uses ?? [])].reverse()) {
      tooltip.appendMarkdown(`| used at | ${use.name}:${use.line} |\n`);
    }
    for (const prop of node.props) {
      tooltip.appendMarkdown(`| ${prop.name} | `);
      tooltip.appendText(prop.value);
      tooltip.appendMarkdown(' |\n');
    }
    item.tooltip = tooltip;
    if (node.source?.path) {
      item.command = {
        command: OPEN_COMMAND,
        title: 'Open the Element',
        arguments: [node],
      };
    }
    return item;
  }

  /**
   * Outlines the selected node on the panel, or removes the outline when no node is selected.
   *
   * @param selected  The view's selection.
   */
  outline(selected: readonly Row[]): void {
    const row = selected.find(candidate => candidate.kind === 'node');
    this.selected = row?.node.id ?? null;
    this.draw(row?.node.rect);
  }

  private draw(rect: TreeNode['rect'] | undefined): void {
    // The adapter leaves a running app alone, and an older one has no such request.
    this.session
      ?.customRequest(OUTLINE_REQUEST, {rect})
      .then(undefined, () => {});
  }

  /**
   * Selects the node of the element that starts on a line, if the view shows and the line has one.
   *
   * @param document  The file.
   * @param line  The line, counted from 1.
   */
  async reveal(document: vscode.TextDocument, line: number): Promise<void> {
    const view = this.view;
    if (!view?.visible) {
      return;
    }
    const path = document.uri.fsPath;
    // A node is its own element's, and that of each component it is made inside: `<Card />`'s line finds Card's
    // nodes.
    const made = [...this.rows.values()].filter(
      row =>
        row.kind === 'node' &&
        [row.node.source, ...(row.node.uses ?? [])].some(
          place =>
            place?.line === line &&
            place.path !== undefined &&
            vscode.Uri.file(place.path).fsPath === path,
        ),
    );
    // The top node of each use, not the nodes inside it: one per row of a list.
    const tops = made.filter(row => !row.parent || !made.includes(row.parent));
    // The first one, unless one is selected already.
    if (tops.length && !tops.some(row => view.selection.includes(row))) {
      await view.reveal(tops[0], {select: true, focus: false});
    }
  }
}

/**
 * Opens a node's element in the source.
 *
 * @param node  The node.
 */
async function openElement(node: TreeNode): Promise<void> {
  const source = node.source;
  if (!source?.path) {
    return;
  }
  const at = new vscode.Position(
    Math.max(source.line - 1, 0),
    Math.max((source.column ?? 1) - 1, 0),
  );
  await vscode.window.showTextDocument(vscode.Uri.file(source.path), {
    selection: new vscode.Range(at, at),
    preserveFocus: true,
  });
}

/**
 * Adds the Node Tree view.
 *
 * @param context  The extension's context.
 */
export function registerNodeTree(context: vscode.ExtensionContext): void {
  const tree = new NodeTree();
  const view = vscode.window.createTreeView(VIEW, {treeDataProvider: tree});
  tree.view = view;
  context.subscriptions.push(
    view,
    view.onDidChangeSelection(event => tree.outline(event.selection)),
    vscode.debug.registerDebugAdapterTrackerFactory(DEBUG_TYPE, {
      createDebugAdapterTracker(session) {
        return {
          onDidSendMessage(message) {
            if (message.type === 'event' && message.event === 'stopped') {
              tree.read(session);
            } else if (
              (message.type === 'event' && message.event === 'continued') ||
              (message.type === 'response' &&
                message.success &&
                RESUMES.includes(message.command))
            ) {
              tree.resumed(session);
            }
          },
        };
      },
    }),
    vscode.debug.onDidTerminateDebugSession(session => tree.ended(session)),
    vscode.commands.registerCommand(REFRESH_COMMAND, () => {
      const session = vscode.debug.activeDebugSession;
      if (session?.type === DEBUG_TYPE) {
        tree.read(session);
      }
    }),
    vscode.commands.registerCommand(OPEN_COMMAND, openElement),
    vscode.window.onDidChangeTextEditorSelection(event => {
      // A click or a key in the source, not the selection that opening a node's element makes.
      if (
        event.kind === vscode.TextEditorSelectionChangeKind.Mouse ||
        event.kind === vscode.TextEditorSelectionChangeKind.Keyboard
      ) {
        void tree.reveal(
          event.textEditor.document,
          event.selections[0].active.line + 1,
        );
      }
    }),
  );
}
