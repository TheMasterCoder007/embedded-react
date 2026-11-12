// The host's probe module as a firmware declares it: tools/erb-frames/probe_module.h is generated from this file, and
// the host packs with it.
import {nativeModule} from 'embedded-react';
import type {Probe as ProbeInterface} from './native-probe';

export const Probe = nativeModule<ProbeInterface>('probe', 1);
