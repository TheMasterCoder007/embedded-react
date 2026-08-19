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

// Runtime e2e for `display: 'none'` — the page-cache primitive, end to end through the real bridge
// and C engine (props.js -> NativeUI.setProps -> apply_props -> ERProps.display -> layout).
//
// The point of the prop is what it does NOT do: unmount. So this contrasts it with the conditional
// render (`{show && <Page/>}`) it replaces. Both hide the page; only display:none keeps the React
// tree — and the native nodes behind it — alive, so switching back is a repaint rather than a
// rebuild of every node in interpreted QuickJS. That is the whole reason the prop exists, and it is
// invisible to a layout-only assertion, so it is asserted directly here:
//
//   - a hidden subtree is pruned from layout (its box collapses, its children stop being measured),
//   - showing it restores the layout it had,
//   - across the whole cycle the page is never unmounted: its effect never re-runs, its cleanup
//     never fires, and its useState survives,
//   - `visible={false}` is the same switch under the other spelling,
//   - and on a <Modal>, `visible` still means the Modal's own show/hide — not the generic alias.
import {createRoot} from '../../src/renderer.js';
import {View, Text, Modal} from 'embedded-react';
import {useState, useEffect} from 'react';
import {check, report} from './harness.js';

const PAGE_W = 120;
const PAGE_H = 80;

let pageRect = null;
let contentRect = null;
let mounts = 0; // effect runs — one per real mount
let unmounts = 0; // effect cleanups — one per real unmount
let seenCounter = null; // the page's own state, as of the last render

/** A cached page: expensive to build, so it must survive being hidden. */
function Page({counter}) {
  useEffect(() => {
    mounts++;
    return () => {
      unmounts++;
    };
  }, []);
  seenCounter = counter;
  return (
    <View
      style={{width: PAGE_W, height: PAGE_H}}
      onLayout={e => (contentRect = e.layout)}>
      <Text>page {counter}</Text>
    </View>
  );
}

function App({hidden, counter, useVisibleProp}) {
  const style = useVisibleProp ? {} : {display: hidden ? 'none' : 'flex'};
  const visible = useVisibleProp ? !hidden : undefined;
  return (
    <View style={{width: 200, height: 200}}>
      <View
        style={style}
        visible={visible}
        onLayout={e => (pageRect = e.layout)}>
        <Page counter={counter} />
      </View>
    </View>
  );
}

const root = createRoot({width: screen.width, height: screen.height});

/** Renders and pumps, so React's passive effects (the mount/unmount counters) have flushed. */
const render = el => {
  root.render(el);
  NativeUI.tick(0); // passive effects are scheduled during commit and run on a pump
};

// --- shown: the page lays out normally -------------------------------------------------------
render(<App hidden={false} counter={1} />);
check(
  pageRect && pageRect.height === PAGE_H,
  `a shown page lays out (got height ${pageRect && pageRect.height})`,
);
check(
  contentRect && contentRect.width === PAGE_W,
  `its content lays out (got width ${contentRect && contentRect.width})`,
);
check(mounts === 1 && unmounts === 0, 'the page mounted once');

// --- hidden: pruned from layout, still mounted -----------------------------------------------
const contentAtHide = contentRect;
render(<App hidden={true} counter={1} />);
check(
  pageRect && pageRect.width === 0 && pageRect.height === 0,
  `display:'none' collapses the subtree's box (got ${pageRect && pageRect.width}x${pageRect && pageRect.height})`,
);
check(
  contentRect === contentAtHide,
  'children of a hidden subtree are not re-laid out',
);
check(
  mounts === 1 && unmounts === 0,
  'hiding does NOT unmount the page (this is what a conditional render cannot do)',
);

// --- still updatable while hidden: React keeps rendering into it ------------------------------
render(<App hidden={true} counter={2} />);
check(seenCounter === 2, 'a hidden page still receives props');
check(
  mounts === 1 && unmounts === 0,
  'updating a hidden page does not remount it',
);

// --- shown again: the layout comes back, the page was never rebuilt ---------------------------
render(<App hidden={false} counter={2} />);
check(
  pageRect && pageRect.height === PAGE_H,
  `showing restores the subtree's layout (got height ${pageRect && pageRect.height})`,
);
check(
  contentRect && contentRect.width === PAGE_W,
  `its content is laid out again (got width ${contentRect && contentRect.width})`,
);
check(
  mounts === 1 && unmounts === 0,
  'a full hide/show cycle never mounted or unmounted the page',
);

// --- the contrast: a conditional render DOES tear the page down -------------------------------
function Conditional({show}) {
  return (
    <View style={{width: 200, height: 200}}>
      {show && <Page counter={9} />}
    </View>
  );
}
// (Swapping the root element type already unmounts the App tree, so measure from here.)
render(<Conditional show={true} />);
const mountsBefore = mounts;
const unmountsBefore = unmounts;
render(<Conditional show={false} />);
check(
  unmounts === unmountsBefore + 1,
  'for contrast: a conditional render unmounts the page (the cost display:none avoids)',
);
render(<Conditional show={true} />);
check(
  mounts === mountsBefore + 1,
  'for contrast: showing it again rebuilds it from scratch',
);

// --- the `visible` spelling drives the same switch --------------------------------------------
render(<App hidden={false} counter={1} useVisibleProp={true} />);
check(
  pageRect && pageRect.height === PAGE_H,
  `visible={true} lays out (got height ${pageRect && pageRect.height})`,
);
render(<App hidden={true} counter={1} useVisibleProp={true} />);
check(
  pageRect && pageRect.width === 0 && pageRect.height === 0,
  `visible={false} prunes the subtree (got ${pageRect && pageRect.width}x${pageRect && pageRect.height})`,
);

// --- the actual win, counted: native node churn on a page switch ------------------------------
// The issue this prop exists for is that a page change rebuilds a few hundred nodes in interpreted
// QuickJS. Wall-clock isn't measurable in here (NativeUI.now() is a logical clock), but the node
// churn that dominates it is — and it is deterministic, so it is asserted rather than reported.
const PAGE_NODES = 60;
let creates = 0;
let destroys = 0;
const realCreate = NativeUI.createNode;
const realDestroy = NativeUI.destroyNode;
NativeUI.createNode = t => {
  creates++;
  return realCreate(t);
};
NativeUI.destroyNode = h => {
  destroys++;
  return realDestroy(h);
};

const rows = Array.from({length: PAGE_NODES}, (_, i) => i);
const Rows = () => (
  <View>
    {rows.map(i => (
      <View key={i} style={{height: 2}} />
    ))}
  </View>
);

/** Two pages, both always mounted; the inactive one is hidden. */
function Cached({page}) {
  return (
    <View style={{width: 200, height: 200}}>
      <View style={{display: page === 'a' ? 'flex' : 'none'}}>
        <Rows />
      </View>
      <View style={{display: page === 'b' ? 'flex' : 'none'}}>
        <Rows />
      </View>
    </View>
  );
}

/** The same two pages, mounted on demand. */
function Rebuilt({page}) {
  return (
    <View style={{width: 200, height: 200}}>
      {page === 'a' && <Rows />}
      {page === 'b' && <Rows />}
    </View>
  );
}

render(<Cached page="a" />);
creates = 0;
destroys = 0;
render(<Cached page="b" />);
const cachedChurn = creates + destroys;
check(
  cachedChurn === 0,
  `switching pages with display:none creates and destroys NOTHING (churn=${cachedChurn})`,
);

render(<Rebuilt page="a" />);
creates = 0;
destroys = 0;
render(<Rebuilt page="b" />);
// (One destroyNode per switch, not per node — the engine reclaims a deleted subtree's descendants
// itself. The creates are the interpreted-QuickJS cost the issue is about, and there is one per node.)
check(
  creates >= PAGE_NODES,
  `for contrast: the conditional switch rebuilds a node per row (created=${creates}, destroyed=${destroys})`,
);

NativeUI.createNode = realCreate;
NativeUI.destroyNode = realDestroy;

// --- ...but on a <Modal>, `visible` is still the Modal's own show/hide -------------------------
let sheetRect = null;
function WithModal({open}) {
  return (
    <View style={{width: 200, height: 200}}>
      <Modal
        visible={open}
        style={{
          position: 'absolute',
          left: 0,
          top: 0,
          width: 200,
          height: 200,
        }}>
        <View
          style={{width: 60, height: 40}}
          onLayout={e => (sheetRect = e.layout)}
        />
      </Modal>
    </View>
  );
}
render(<WithModal open={true} />);
check(
  sheetRect && sheetRect.width === 60,
  `an open <Modal> lays its content out (got width ${sheetRect && sheetRect.width})`,
);
const sheetAtClose = sheetRect;
render(<WithModal open={false} />);
check(
  sheetRect === sheetAtClose,
  'a closed <Modal> prunes its content from layout (visible was not swallowed by the alias)',
);

report('display-none');
