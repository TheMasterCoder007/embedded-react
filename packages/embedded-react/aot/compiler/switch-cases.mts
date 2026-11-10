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

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from './diagnostics.mts';
import type * as t from '@babel/types';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** Cases that run the same statements: empty cases written above one with a body. */
interface CaseGroup {
  tests: t.Expression[];
  hasDefault: boolean;
  statements: t.Statement[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A case's statements without the `break` that ends them, and whether they end there (a `break` or a `return`)
 * rather than falling through to the next case.
 *
 * @param statements  The case's statements.
 *
 * @returns The statements, and whether they end.
 */
function endOfCase(statements: t.Statement[]): {
  statements: t.Statement[];
  ends: boolean;
} {
  const last = statements.at(-1);
  if (last?.type === 'BreakStatement' && !last.label) {
    return {statements: statements.slice(0, -1), ends: true};
  }

  if (last?.type === 'ReturnStatement') return {statements, ends: true};

  // `case 1: { …; break; }`
  if (last?.type === 'BlockStatement') {
    const inner = endOfCase(last.body);
    return {
      statements: [
        ...statements.slice(0, -1),
        {...last, body: inner.statements},
      ],
      ends: inner.ends,
    };
  }

  return {statements, ends: false};
}

/**
 * Refuses a `break` left in a case's statements, one that does not end the case.
 *
 * @param node  The statements, or a node in them.
 */
function refuseInnerBreak(node: unknown): void {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) {
    node.forEach(refuseInnerBreak);
    return;
  }

  const statement = node as t.Node;
  if (statement.type === 'BreakStatement') {
    const error = aotError(
      'AOT: a `break` must be the last statement of its case',
      'end the case with break, and write a break that depends on a test as if/else inside the case.',
    );
    if (statement.loc) error.aotLoc = statement.loc.start;
    throw error;
  }

  // A nested switch has breaks of its own, and a function's body is not this case's.
  if (
    statement.type === 'SwitchStatement' ||
    statement.type === 'ArrowFunctionExpression' ||
    statement.type === 'FunctionExpression' ||
    statement.type === 'FunctionDeclaration'
  ) {
    return;
  }

  if (statement.type === 'IfStatement') {
    refuseInnerBreak(statement.consequent);
    refuseInnerBreak(statement.alternate);
  } else if (statement.type === 'BlockStatement') {
    refuseInnerBreak(statement.body);
  }
}

/**
 * Whether an expression reads the same each time it is worked out: a name, a field of one, or a literal.
 *
 * @param node  The expression.
 *
 * @returns Whether it does.
 */
const readsSame = (node: t.Expression): boolean =>
  node.type === 'Identifier' ||
  node.type === 'NumericLiteral' ||
  node.type === 'StringLiteral' ||
  node.type === 'BooleanLiteral' ||
  (node.type === 'MemberExpression' &&
    !node.computed &&
    readsSame(node.object as t.Expression));

/**
 * A `switch` as the `if … else if …` chain it runs as. Each case tests `discriminant === test`, and runs its own
 * statements then the next cases' until one ends. Empty cases share the next one's statements, and the
 * default is the last `else`. A discriminant that is not a plain read is worked out once, into a `const`.
 *
 * @param statement  The switch.
 *
 * @returns The chain.
 */
export function switchAsIf(statement: t.SwitchStatement): t.Statement {
  if (!readsSame(statement.discriminant)) {
    const name: t.Identifier = {
      type: 'Identifier',
      // `@` keeps it from naming anything the app declares.
      name: `switch@${statement.start ?? 0}`,
    };
    return {
      type: 'BlockStatement',
      body: [
        {
          type: 'VariableDeclaration',
          kind: 'const',
          declarations: [
            {
              type: 'VariableDeclarator',
              id: name,
              init: statement.discriminant,
              loc: statement.discriminant.loc,
            },
          ],
          loc: statement.discriminant.loc,
        },
        switchAsIf({...statement, discriminant: name}),
      ],
      directives: [],
    };
  }

  const {cases, discriminant} = statement;

  // What each case runs: its own statements, then the next cases' until one ends.
  const runs = cases.map((_, index) => {
    const run: t.Statement[] = [];
    for (let at = index; at < cases.length; at++) {
      const {statements, ends} = endOfCase(cases[at].consequent);
      run.push(...statements);
      if (ends) break;
    }
    refuseInnerBreak(run);
    return run;
  });

  // Empty cases join the next case with a body.
  const groups: CaseGroup[] = [];
  let pending: CaseGroup = {tests: [], hasDefault: false, statements: []};
  cases.forEach((switchCase, index) => {
    if (switchCase.test) pending.tests.push(switchCase.test);
    else pending.hasDefault = true;
    if (switchCase.consequent.length || index === cases.length - 1) {
      pending.statements = runs[index];
      groups.push(pending);
      pending = {tests: [], hasDefault: false, statements: []};
    }
  });

  // The default's group runs for every value the other cases miss, its own tests included.
  const fallback = groups.find(group => group.hasDefault);
  const block = (statements: t.Statement[]): t.BlockStatement => ({
    type: 'BlockStatement',
    body: statements,
    directives: [],
  });
  let chain: t.Statement | null = fallback ? block(fallback.statements) : null;
  for (const group of groups.reverse()) {
    if (group.hasDefault || (!fallback && !group.statements.length)) continue;
    const test = group.tests
      .map((caseTest): t.Expression => ({
        type: 'BinaryExpression',
        operator: '===',
        left: discriminant,
        right: caseTest,
        loc: caseTest.loc,
      }))
      .reduce((left, right): t.Expression => ({
        type: 'LogicalExpression',
        operator: '||',
        left,
        right,
        loc: left.loc,
      }));
    chain = {
      type: 'IfStatement',
      test,
      consequent: block(group.statements),
      alternate: chain,
      loc: test.loc,
    };
  }

  return chain ?? block([]);
}
