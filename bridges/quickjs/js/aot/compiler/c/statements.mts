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

import {cScalarType} from '../c-syntax.mts';
import {printCondition, printExpr} from './expressions.mts';
import {formatArgs, printText} from './text.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {
  IrAppendItem,
  IrSetStateText,
  IrStmt,
  IrTimer,
} from '../ir/statements.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** One level of indentation. */
const INDENT = '    ';

/** The indent list operations print at, whatever block they sit in. */
const LIST_INDENT = INDENT;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Matches `<member>` as a whole C lvalue, so `s_state.label` does not also match `s_state.label2`.
 *
 * @param member  A C lvalue.
 *
 * @returns A pattern that finds it in the C source.
 */
const readsMember = (member: string): RegExp =>
  new RegExp(`(^|[^\\w.])${member.replace(/\./g, '\\.')}(?![\\w])`);

/**
 * A timer as the C call that starts it and evaluates to its id. A timestamp or float delay goes through
 * ToInt32 and a floor at 0, as Flow A's setTimeout does.
 *
 * @param timer  The timer.
 *
 * @returns The C call.
 */
function printTimerAdd(timer: IrTimer): string {
  const delay = timer.delay;
  const delayMs = !delay
    ? '0'
    : delay.cType === 'i64'
      ? `app_delay_ms64(${printExpr(delay)})`
      : delay.cType === 'float'
        ? `app_delay_msf(${printExpr(delay)})`
        : printExpr(delay);
  return `er_timer_add((int)(${delayMs}), ${timer.isRepeating ? 'true' : 'false'}, ${timer.fn})`;
}

/**
 * A string state write. snprintf's source and destination may not overlap, so a value that reads the slot
 * itself (`setLabel(label + '!')`) is built in a temporary first.
 *
 * @param stmt  The write.
 * @param indent  The statement's indentation.
 *
 * @returns The C statement (several lines, joined, for a value that reads the slot).
 */
function printSetStateText(stmt: IrSetStateText, indent: string): string {
  const member = stmt.state.cMember;
  const format = printText(stmt.text);
  if (format.args.some(formatArg => readsMember(member).test(formatArg))) {
    return [
      `${indent}{`,
      `${indent}    char next[sizeof(${member})];`,
      `${indent}    snprintf(next, sizeof(next), ${formatArgs(format)});`,
      `${indent}    memcpy(${member}, next, strlen(next) + 1);`,
      `${indent}}`,
    ].join('\n');
  }

  return `${indent}snprintf(${member}, sizeof(${member}), ${formatArgs(format)});`;
}

/**
 * An appended list item: fill the next free slot's fields and count it, unless the list is full.
 *
 * @param stmt  The append.
 *
 * @returns The C lines.
 */
function printAppendItem(stmt: IrAppendItem): string[] {
  const {arrayName, countMember, cap} = stmt.list;
  const lines = [
    `${LIST_INDENT}if (${countMember} < ${cap})`,
    `${LIST_INDENT}{`,
  ];
  for (const itemValue of stmt.values) {
    const slot = `${arrayName}[${countMember}].${itemValue.field.key}`;
    if ('text' in itemValue) {
      lines.push(
        `${LIST_INDENT}${INDENT}snprintf(${slot}, sizeof(${slot}), ${formatArgs(printText(itemValue.text))});`,
      );
    } else {
      lines.push(
        `${LIST_INDENT}${INDENT}${slot} = ${printExpr(itemValue.value)};`,
      );
    }
  }

  lines.push(`${LIST_INDENT}${INDENT}${countMember}++;`, `${LIST_INDENT}}`);
  return lines;
}

/**
 * Prints a statement as C lines.
 *
 * @param stmt  The statement.
 * @param indent  Its indentation.
 *
 * @returns Its C lines.
 */
function printStmt(stmt: IrStmt, indent: string): string[] {
  switch (stmt.kind) {
    case 'declareLocal': {
      // A string local is a buffer of its own, so a later write to the state it copied cannot change it.
      const init: IrExpr = stmt.init;
      if (init.cType === 'string') {
        const declaration = stmt.isHoisted
          ? []
          : [`${indent}char ${stmt.name}[${stmt.stringCap}];`];
        return [
          ...declaration,
          `${indent}snprintf(${stmt.name}, sizeof(${stmt.name}), "%s", ${printExpr(init)});`,
        ];
      }

      const declaredType = stmt.isHoisted ? '' : cScalarType(init.cType) + ' ';
      return [`${indent}${declaredType}${stmt.name} = ${printExpr(init)};`];
    }
    case 'startTimer': {
      const timerAdd = printTimerAdd(stmt.timer);
      if (!stmt.idLocal) {
        return [`${indent}${timerAdd};`];
      }

      // The id is only read by a later clear*(); `(void)` keeps the compiler quiet when nothing does.
      const {name, isHoisted} = stmt.idLocal;
      return [
        `${indent}${isHoisted ? '' : 'int '}${name} = ${timerAdd};`,
        `${indent}(void)${name};`,
      ];
    }
    case 'clearTimer':
      return [`${indent}er_timer_clear(${printExpr(stmt.id)});`];
    case 'setState':
      return [`${indent}${stmt.state.cMember} = ${printExpr(stmt.value)};`];
    case 'setStateText':
      return [printSetStateText(stmt, indent)];
    case 'clearList':
      return [`${LIST_INDENT}${stmt.list.countMember} = 0;`];
    case 'appendItem':
      return printAppendItem(stmt);
    case 'dropLastItem': {
      const count = stmt.list.countMember;
      return [`${LIST_INDENT}if (${count} > 0) ${count}--;`];
    }
    case 'keepFirstItems': {
      const count = stmt.list.countMember;
      return [
        `${LIST_INDENT}${count} = (${count} < ${stmt.count}) ? ${count} : ${stmt.count};`,
      ];
    }
    case 'sliceItems': {
      const count = stmt.list.countMember;
      return [
        `${LIST_INDENT}${count} = app_slice_len(${count}, ${printExpr(stmt.end)});`,
      ];
    }
    case 'writeRef':
      return [`${indent}${stmt.ref.cVar} ${stmt.op} ${printExpr(stmt.value)};`];
    case 'stepRef':
      return [`${indent}${stmt.ref.cVar}${stmt.op};`];
    case 'stopAnimations':
      return stmt.handles.map(
        handle =>
          `${indent}er_anim_value_set(${handle}, er_anim_value_get(${handle}));`,
      );
    case 'if': {
      // Each branch is a nested C block.
      const lines = [
        `${indent}if (${printCondition(stmt.test)})`,
        `${indent}{`,
        ...printStmts(stmt.consequent, indent + INDENT),
        `${indent}}`,
      ];
      if (stmt.alternate) {
        lines.push(
          `${indent}else`,
          `${indent}{`,
          ...printStmts(stmt.alternate, indent + INDENT),
          `${indent}}`,
        );
      }

      return lines;
    }
    case 'return':
      return [`${indent}return;`];
    case 'armCleanup':
      return [`${indent}${stmt.flag} = 1;`];
    case 'foreign':
      return stmt.isIndented
        ? stmt.lines
        : stmt.lines.map(line => indent + line);
  }
}

/**
 * Prints statements as C lines.
 *
 * @param stmts  The statements.
 * @param indent  Their indentation.
 *
 * @returns Their C lines.
 */
export function printStmts(stmts: IrStmt[], indent: string): string[] {
  return stmts.flatMap(stmt => printStmt(stmt, indent));
}
