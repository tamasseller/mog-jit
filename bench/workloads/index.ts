// The workload set, in the order the report lists them: the conventional
// trigger a fixed matrix could also do, then the two it could not.

import type {Workload} from "./workload"
import {pulseTrigger} from "./pulse-trigger"
import {iqPreamble} from "./iq-preamble"
import {median5} from "./median5"

export const WORKLOADS: readonly Workload[] = [pulseTrigger, iqPreamble, median5]

export function workloadNamed(name: string): Workload
{
    const w = WORKLOADS.find(w => w.name === name)
    if(w === undefined)
    {
        throw new Error(`no workload named ${name} — have ${WORKLOADS.map(w => w.name).join(", ")}`)
    }

    return w
}

export type {Workload, Sink} from "./workload"
