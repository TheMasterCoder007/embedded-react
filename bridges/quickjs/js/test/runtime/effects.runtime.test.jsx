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

// Runtime e2e: with §2 timers + job-queue pumping in place, React's passive effects (useEffect)
// must flush. Passive effects are scheduled (not run during the commit), so they only run once the
// host pumps — which NativeUI.tick does. This is the concrete payoff of wiring the scheduler clock.
import {createRoot} from '../../src/renderer.js';
import {useEffect, useState} from 'react';
import {View} from 'embedded-react';
import {check, report} from './harness.js';

let mountRuns = 0;
let cleanupRuns = 0;
let depRuns = 0;
let lastDep = null;

function Comp({n}) {
  // Mount-only effect with a cleanup.
  useEffect(() => {
    mountRuns++;
    return () => {
      cleanupRuns++;
    };
  }, []);

  // Dependency-tracked effect: re-runs when n changes.
  useEffect(() => {
    depRuns++;
    lastDep = n;
  }, [n]);

  return <View style={{width: 10, height: 10}} />;
}

const root = createRoot({width: screen.width, height: screen.height});

root.render(<Comp n={1} />);
// Effects are scheduled during commit but run asynchronously; a pump flushes them.
NativeUI.tick(0);
check(
  mountRuns === 1,
  `mount effect ran after first commit + pump (runs=${mountRuns})`,
);
check(
  depRuns === 1 && lastDep === 1,
  `dep effect ran with n=1 (runs=${depRuns}, n=${lastDep})`,
);

// Re-render with the same n: the [] effect must not re-run, and the [n] effect must not re-run.
root.render(<Comp n={1} />);
NativeUI.tick(0);
check(mountRuns === 1, 'mount-only effect did not re-run on re-render');
check(depRuns === 1, 'dep effect did not re-run when n was unchanged');

// Re-render with a new n: the [n] effect re-runs (after the previous one is cleaned up).
root.render(<Comp n={2} />);
NativeUI.tick(0);
check(
  depRuns === 2 && lastDep === 2,
  `dep effect re-ran when n changed to 2 (runs=${depRuns})`,
);
check(mountRuns === 1, 'mount-only effect still did not re-run');

// Unmount: the mount effect's cleanup must run.
root.render(null);
NativeUI.tick(0);
check(cleanupRuns === 1, `cleanup ran on unmount (cleanups=${cleanupRuns})`);

report('effects');
