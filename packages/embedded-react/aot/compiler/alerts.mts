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

/**
 * @file compiler/alerts.mts
 * @brief Alert.alert(title, message, buttons, options): each call becomes a dialog of its own under the root,
 *        built only while a hidden state names it, and the call sets that state.
 */

import {traverse} from '@babel/core';
import {isFn} from './collect.mts';
import {emitChildren} from './control-flow.mts';
import {aotError} from './diagnostics.mts';
import {
  attributeOf,
  elementOf,
  expressionChild,
  literalObject,
} from './elements/rewrite.mts';
import {evalStatic, rethrowFoldLimit} from './static-eval.mts';
import type {NodePath} from '@babel/core';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {IrConditional, IrElement} from './ir/nodes.mts';
import type {Out} from './out.mts';
import type {Env, ScalarState, Scope, StateTable} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The package Alert is imported from. */
const PACKAGE = 'embedded-react';

// None of these is a name JS allows, so no binding of the app's can be mistaken for one.
/** What a call is rewritten to. */
export const ALERT_CALL = '#Alert.alert';

/** The hidden state: the number of the alert that is up, or 0. */
const ACTIVE = '#alert';
const SET_ACTIVE = '#setAlert';

// Alert.mts's styles. Flow B's <Modal> already fills the root and centres its content.
const FILL_STYLE = {
  position: 'absolute',
  left: 0,
  top: 0,
  right: 0,
  bottom: 0,
  alignItems: 'center',
  justifyContent: 'center',
};
const SHEET_STYLE = {
  backgroundColor: '#ffffff',
  borderRadius: 4,
  padding: 16,
  gap: 8,
};
const SHEET_MAX_WIDTH = 280;
const SHEET_MARGIN = 16;
const TITLE_STYLE = {color: '#000000', fontWeight: 'bold'};
const MESSAGE_STYLE = {color: '#444444'};
const BUTTONS_STYLE = {
  flexDirection: 'row',
  flexWrap: 'wrap',
  justifyContent: 'flex-end',
  gap: 8,
  marginTop: 8,
};
const BUTTON_TEXT_STYLE = {color: '#2196F3', fontWeight: '500', padding: 8};
const DESTRUCTIVE_COLOR = '#d32f2f';

const BUTTON_STYLES = ['default', 'cancel', 'destructive'];

const HERE_HINT =
  'call it from an event handler, an effect, a timer, or a helper one of those calls.';

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** One Alert.alert call, as lowered in one place: its dialog, and what the dialog is compiled against. */
export interface AlertSite {
  call: t.CallExpression;
  dialog: t.Expression;
  environment: Env;
  state: StateTable;
}

interface AlertButton {
  text: t.Expression | null;
  onPress: t.Expression | null;
  destructive: boolean;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

const identifier = (name: string): t.Identifier => ({type: 'Identifier', name});

const numberOf = (value: number): t.NumericLiteral => ({
  type: 'NumericLiteral',
  value,
});

const located = (
  error: AotError,
  node: t.Node | null | undefined,
): AotError => {
  if (node?.loc && !error.aotLoc) error.aotLoc = node.loc.start;
  return error;
};

/**
 * Whether a call runs as its component renders: at module level, or straight in the body of a component or a hook.
 *
 * @param path  The call's Alert.
 *
 * @returns Whether it does.
 */
function rendersIn(path: NodePath): boolean {
  const owner = path.getFunctionParent();
  if (!owner) return true;
  const {node, parent} = owner;
  const name =
    node.type === 'FunctionDeclaration'
      ? node.id?.name
      : parent.type === 'VariableDeclarator' && parent.id.type === 'Identifier'
        ? parent.id.name
        : undefined;
  return name !== undefined && /^([A-Z]|use[A-Z])/.test(name);
}

/**
 * Rewrites every `Alert.alert(…)` to a call of ALERT_CALL, and refuses any other use of Alert.
 *
 * @param ast  The parsed file, rewritten in place.
 *
 * @returns Whether the app raises an alert.
 */
export function rewriteAlerts(ast: t.File): boolean {
  const isAlert = (path: NodePath, node: t.Node): boolean => {
    if (node.type !== 'Identifier') return false;
    const binding = path.scope.getBinding(node.name);
    const specifier = binding?.path.node;
    const declaration = binding?.path.parent;
    if (
      specifier?.type !== 'ImportSpecifier' ||
      declaration?.type !== 'ImportDeclaration' ||
      declaration.source.value !== PACKAGE
    ) {
      return false;
    }
    const {imported} = specifier;
    return (
      (imported.type === 'Identifier' ? imported.name : imported.value) ===
      'Alert'
    );
  };

  let found = false;
  traverse.cache.clear();
  traverse(ast, {
    Identifier(path) {
      if (!path.isReferencedIdentifier() || !isAlert(path, path.node)) return;
      const member = path.parent;
      const call = path.parentPath?.parent;
      if (
        member.type === 'MemberExpression' &&
        member.object === path.node &&
        !member.computed &&
        member.property.type === 'Identifier' &&
        member.property.name === 'alert' &&
        call?.type === 'CallExpression' &&
        call.callee === member
      ) {
        if (rendersIn(path)) {
          throw located(
            aotError(
              "AOT: Alert.alert can't be called while rendering",
              HERE_HINT,
            ),
            call,
          );
        }
        call.callee = {...identifier(ALERT_CALL), loc: member.loc};
        found = true;
        path.skip();
        return;
      }
      throw located(
        aotError(
          'AOT: Alert has only Alert.alert(title, message, buttons, options)',
          'call it directly: Alert.alert(…). It cannot be passed around, and Alert.prompt is iOS only.',
        ),
        path.node,
      );
    },
  });
  traverse.cache.clear();
  return found;
}

/**
 * The hidden state that names the alert that is up.
 *
 * @returns The state, to add to the App's.
 */
export function alertState(): ScalarState {
  return {
    name: ACTIVE,
    slotName: ACTIVE,
    setter: SET_ACTIVE,
    kind: 'scalar',
    slotKind: 'int',
    isBoolean: false,
    initial: 0,
    host: false,
  };
}

/**
 * What lowers where a handler does but outlives it: the environment an Alert.alert's dialog is compiled against.
 *
 * @param environment  The environment a handler, effect or event is about to be lowered in.
 *
 * @returns The environment, remembering itself as it is before the body adds its locals.
 */
export const withAlertBase = (environment: Env): Env =>
  environment.alertBase
    ? environment
    : {...environment, alertBase: environment};

/**
 * Every name an expression or a function's body reads.
 *
 * @param node  The expression.
 *
 * @returns The identifiers, object keys and property names left out.
 */
function namesReadBy(node: t.Node): t.Identifier[] {
  const found: t.Identifier[] = [];
  const visit = (value: unknown, key?: string, parent?: t.Node): void => {
    if (!value || typeof value !== 'object') return;
    if (Array.isArray(value)) {
      value.forEach(element => visit(element, key, parent));
      return;
    }
    const child = value as t.Node;
    if (typeof child.type !== 'string') return;
    if (child.type === 'Identifier') {
      const isName =
        (parent?.type === 'MemberExpression' &&
          key === 'property' &&
          !parent.computed) ||
        (parent?.type === 'ObjectProperty' &&
          key === 'key' &&
          !parent.computed);
      if (!isName) found.push(child);
      return;
    }
    for (const [childKey, grandchild] of Object.entries(child)) {
      if (childKey !== 'loc') visit(grandchild, childKey, child);
    }
  };
  visit(node);
  return found;
}

/**
 * Refuses a dialog that reads what its handler works out: it is built after the handler returns, so it can read
 * only what the component itself can.
 *
 * @param parts  The call's title, message, labels and handlers.
 * @param environment  The environment the call is lowered in.
 * @param base  The environment the handler was lowered from.
 */
function refuseHandlerReads(
  parts: t.Node[],
  environment: Env,
  base: Env,
): void {
  for (const name of parts.flatMap(namesReadBy)) {
    if (
      name.name === environment.event ||
      name.name === environment.gesture ||
      environment.locals.get(name.name) !== base.locals.get(name.name) ||
      environment.constants[name.name] !== base.constants[name.name]
    ) {
      throw located(
        aotError(
          `AOT: an alert can't read "${name.name}", which the handler works out`,
          'the dialog is drawn after the handler returns, so it reads state, props and constants only. ' +
            'Keep the value in state and read that.',
        ),
        name,
      );
    }
  }
}

/**
 * Whether an argument is left out: missing, `undefined` or `null`.
 *
 * @param node  The argument.
 *
 * @returns Whether there is nothing there.
 */
const isAbsent = (node: t.Node | undefined): boolean =>
  !node ||
  node.type === 'NullLiteral' ||
  (node.type === 'Identifier' && node.name === 'undefined');

/**
 * Whether a title or a message draws nothing, as Alert.mts's hasText has it: left out, or known to be empty.
 *
 * @param node  The argument.
 * @param scope  Compile-time constants in reach.
 *
 * @returns Whether its Text is left out.
 */
function isEmpty(node: t.Node | undefined, scope: Scope): boolean {
  if (isAbsent(node)) return true;
  try {
    const value = evalStatic(node!, scope);
    return value === '' || value === null || value === undefined;
  } catch (error) {
    rethrowFoldLimit(error);
    return false; // worked out at run time
  }
}

/**
 * The fields of an object argument, by name.
 *
 * @param node  The argument.
 * @param what  What it is, for the error.
 *
 * @returns Its fields.
 */
function fieldsOf(node: t.Node, what: string): Map<string, t.Expression> {
  const fields = new Map<string, t.Expression>();
  const refuse = (at: t.Node): AotError =>
    located(
      aotError(
        `AOT: ${what} must be written out as an object: {…}`,
        'list each field by name, with no spread, method or computed key.',
      ),
      at,
    );
  if (node.type !== 'ObjectExpression') throw refuse(node);
  for (const property of node.properties) {
    if (
      property.type !== 'ObjectProperty' ||
      property.computed ||
      (property.key.type !== 'Identifier' &&
        property.key.type !== 'StringLiteral')
    ) {
      throw refuse(property);
    }
    fields.set(
      property.key.type === 'Identifier'
        ? property.key.name
        : property.key.value,
      property.value as t.Expression,
    );
  }
  return fields;
}

/**
 * A field that has to be known at build time.
 *
 * @param node  The field's value.
 * @param scope  Compile-time constants in reach.
 * @param what  The field, for the error.
 *
 * @returns Its value.
 */
function constantOf(node: t.Expression, scope: Scope, what: string): unknown {
  try {
    return evalStatic(node, scope);
  } catch (error) {
    rethrowFoldLimit(error);
    throw located(
      aotError(
        `AOT: ${what} must be a compile-time constant`,
        'it picks what the dialog is built from.',
      ),
      node,
    );
  }
}

/**
 * The buttons of a call: the ones it lists, or a single OK.
 *
 * @param node  The buttons argument.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The buttons.
 */
function buttonsOf(node: t.Node | undefined, scope: Scope): AlertButton[] {
  const ok: AlertButton = {
    text: {type: 'StringLiteral', value: 'OK'},
    onPress: null,
    destructive: false,
  };
  if (isAbsent(node)) return [ok];
  if (node!.type !== 'ArrayExpression') {
    throw located(
      aotError(
        "AOT: an alert's buttons must be written out as an array: [{text: 'OK'}]",
        'each button is compiled where the alert is raised, so the list cannot come from a variable.',
      ),
      node,
    );
  }

  const buttons = node!.elements.map(element => {
    if (!element || element.type === 'SpreadElement') {
      throw located(
        aotError(
          "AOT: an alert's buttons must each be an object: {text: 'OK'}",
        ),
        element ?? node,
      );
    }
    const fields = fieldsOf(element, "an alert's button");
    const style = fields.get('style');
    const styleValue = style
      ? constantOf(style, scope, "an alert button's style")
      : undefined;
    if (styleValue != null && !BUTTON_STYLES.includes(styleValue as string)) {
      throw located(
        aotError(
          `AOT: an alert button's style is 'default', 'cancel' or 'destructive'`,
        ),
        style,
      );
    }
    return {
      text: fields.get('text') ?? null,
      onPress: fields.get('onPress') ?? null,
      destructive: styleValue === 'destructive',
    };
  });

  return buttons.length ? buttons : [ok];
}

/**
 * A handler that closes the alert, then does what the app's own would.
 *
 * @param handler  The button's onPress or the alert's onDismiss, if it has one.
 *
 * @returns The handler.
 */
function closingHandler(handler: t.Expression | null): t.Expression {
  const close: t.Statement = {
    type: 'ExpressionStatement',
    expression: {
      type: 'CallExpression',
      callee: identifier(SET_ACTIVE),
      arguments: [numberOf(0)],
    },
  };
  let rest: t.Statement[] = [];
  if (handler && isFn(handler)) {
    if (handler.params.length) {
      throw located(
        aotError(
          "AOT: an alert's handler takes no arguments",
          'React Native calls it with nothing: () => { … }.',
        ),
        handler.params[0],
      );
    }
    rest =
      handler.body.type === 'BlockStatement'
        ? handler.body.body
        : [{type: 'ExpressionStatement', expression: handler.body}];
  } else if (handler && !isAbsent(handler)) {
    // A helper or a useCallback by name.
    rest = [
      {
        type: 'ExpressionStatement',
        expression: {
          type: 'CallExpression',
          callee: handler,
          arguments: [],
          loc: handler.loc,
        },
        loc: handler.loc,
      },
    ];
  }

  return {
    type: 'ArrowFunctionExpression',
    params: [],
    body: {type: 'BlockStatement', body: [close, ...rest], directives: []},
    async: false,
    expression: false,
    loc: handler?.loc,
  };
}

/**
 * The dialog Alert.mts draws for a call, shown while the hidden state holds the call's number.
 *
 * @param call  The call.
 * @param number  The call's number.
 * @param scope  Compile-time constants in reach.
 *
 * @returns `active === number && <Modal>…</Modal>`, and every part of it the app wrote.
 */
function dialogOf(
  call: t.CallExpression,
  number: number,
  scope: Scope,
): {dialog: t.Expression; parts: t.Node[]} {
  const [title, message, buttonList, optionList] = call.arguments;
  for (const argument of call.arguments) {
    if (argument.type === 'SpreadElement') {
      throw located(
        aotError("AOT: Alert.alert's arguments must be listed, not spread"),
        argument,
      );
    }
  }

  const from = {loc: call.loc} as t.JSXElement;
  const style = (fields: Record<string, string | number>) =>
    attributeOf('style', literalObject(fields));
  const text = (
    fields: Record<string, string | number>,
    value: t.Expression,
  ): t.JSXElement =>
    elementOf('Text', [style(fields)], [expressionChild(value)], from);

  const buttons = buttonsOf(buttonList, scope);
  const options = isAbsent(optionList)
    ? new Map<string, t.Expression>()
    : fieldsOf(optionList, "an alert's options");
  const cancelableField = options.get('cancelable');
  const cancelable = cancelableField
    ? Boolean(constantOf(cancelableField, scope, "an alert's cancelable"))
    : false;
  const onDismiss = options.get('onDismiss') ?? null;

  const sheetStyle: t.ObjectExpression = literalObject(SHEET_STYLE);
  sheetStyle.properties.push({
    type: 'ObjectProperty',
    key: identifier('width'),
    // Math.min(280, screen.width - 32), folded per panel.
    value: {
      type: 'CallExpression',
      callee: {
        type: 'MemberExpression',
        object: identifier('Math'),
        property: identifier('min'),
        computed: false,
      },
      arguments: [
        numberOf(SHEET_MAX_WIDTH),
        {
          type: 'BinaryExpression',
          operator: '-',
          left: {
            type: 'MemberExpression',
            object: identifier('screen'),
            property: identifier('width'),
            computed: false,
          },
          right: numberOf(SHEET_MARGIN * 2),
        },
      ],
    },
    computed: false,
    shorthand: false,
  });

  const shown = [title, message].map(part =>
    isEmpty(part, scope) ? null : (part as t.Expression),
  );
  const sheetChildren: t.JSXElement[] = [];
  if (shown[0]) sheetChildren.push(text(TITLE_STYLE, shown[0]));
  if (shown[1]) sheetChildren.push(text(MESSAGE_STYLE, shown[1]));
  sheetChildren.push(
    elementOf(
      'View',
      [style(BUTTONS_STYLE)],
      buttons.map(button =>
        elementOf(
          'Pressable',
          [attributeOf('onPress', closingHandler(button.onPress))],
          [
            text(
              button.destructive
                ? {...BUTTON_TEXT_STYLE, color: DESTRUCTIVE_COLOR}
                : BUTTON_TEXT_STYLE,
              button.text ?? {type: 'StringLiteral', value: ''},
            ),
          ],
          from,
        ),
      ),
      from,
    ),
  );

  // A cancelable alert's sheet is a press target of its own, so a tap on it is not a tap outside.
  const sheet = elementOf(
    cancelable ? 'Pressable' : 'View',
    [
      attributeOf('style', sheetStyle),
      ...(cancelable
        ? [
            attributeOf('onPress', {
              type: 'ArrowFunctionExpression',
              params: [],
              body: {type: 'BlockStatement', body: [], directives: []},
              async: false,
              expression: false,
            }),
          ]
        : []),
    ],
    sheetChildren,
    from,
  );

  const modal = elementOf(
    'Modal',
    [attributeOf('visible', {type: 'BooleanLiteral', value: true})],
    [
      cancelable
        ? elementOf(
            'Pressable',
            [
              style(FILL_STYLE),
              attributeOf('onPress', closingHandler(onDismiss)),
            ],
            [sheet],
            from,
          )
        : sheet,
    ],
    from,
  );

  return {
    dialog: {
      type: 'LogicalExpression',
      operator: '&&',
      left: {
        type: 'BinaryExpression',
        operator: '===',
        left: identifier(ACTIVE),
        right: numberOf(number),
      },
      right: modal,
      loc: call.loc,
    },
    parts: [
      ...shown,
      ...buttons.flatMap(button => [button.text, button.onPress]),
      onDismiss,
    ].filter((part): part is t.Expression => part != null),
  };
}

/**
 * Lowers an Alert.alert call: its dialog is queued to be built under the root, and the call itself becomes a set
 * of the hidden state.
 *
 * @param call  The call, already rewritten to ALERT_CALL.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param output  Everything emitted so far; the dialog is added to it.
 *
 * @returns The setter call that raises the alert, and what to lower it against.
 */
export function raiseAlert(
  call: t.CallExpression,
  environment: Env,
  state: StateTable,
  output: Out,
): {setter: t.CallExpression; environment: Env; state: StateTable} {
  const base = environment.alertBase;
  const active = output.alertState;
  if (!base || !active) {
    throw aotError("AOT: Alert.alert can't be called here", HERE_HINT);
  }

  // The App's hidden state, in reach of whichever component raises the alert.
  const reaching = (table: StateTable): StateTable => ({
    byName: new Map([...table.byName, [ACTIVE, active]]),
    bySetter: new Map([...table.bySetter, [SET_ACTIVE, active]]),
  });
  const number = output.alertSites.length + 1;
  const {dialog, parts} = dialogOf(call, number, environment.constants);
  refuseHandlerReads(parts, environment, base);
  output.alertSites.push({
    call,
    dialog,
    environment: {
      ...base,
      state: new Map([...base.state, [ACTIVE, active]]),
      // Under the root, whatever the handler's own node is under.
      inListRow: false,
      mounted: null,
      event: null,
      layout: false,
      scroll: false,
    },
    state: reaching(state),
  });

  return {
    setter: {
      type: 'CallExpression',
      callee: identifier(SET_ACTIVE),
      arguments: [numberOf(number)],
      loc: call.loc,
    },
    environment: {
      ...environment,
      state: new Map([...environment.state, [ACTIVE, active]]),
    },
    state: reaching(state),
  };
}

/**
 * Builds every alert's dialog as the root's last children, so each draws over the app. A button's handler can raise
 * an alert of its own, which joins the list as it is lowered.
 *
 * @param root  The app's root element.
 * @param output  Everything emitted so far.
 */
export function emitAlertDialogs(root: IrElement, output: Out): void {
  if (!output.alertSites.length) return;

  let host = root;
  while (host.kind === 'component') host = host.root;
  if (host.node.type !== 'ER_NODE_VIEW') {
    throw located(
      aotError(
        'AOT: an app that raises an alert needs a <View> as its root',
        'the dialog is added as the last child of the root, over everything else.',
      ),
      output.alertSites[0].call,
    );
  }

  // One chain of conditionals, so the app is counted as building the largest dialog, not all of them.
  let last: IrConditional | null = null;
  for (let index = 0; index < output.alertSites.length; index++) {
    const site = output.alertSites[index];
    try {
      const [dialog] = emitChildren(
        [expressionChild(site.dialog)],
        site.environment.constants,
        output,
        site.environment,
        site.state,
      );
      if (dialog?.kind !== 'conditional' || dialog.whenFalse) {
        throw new Error('AOT: an alert did not lower to a conditional');
      }
      if (last) {
        last.whenFalse = dialog;
      } else {
        host.node.children.push(dialog);
      }
      last = dialog;
    } catch (error) {
      throw located(error as AotError, site.call);
    }
  }
}
