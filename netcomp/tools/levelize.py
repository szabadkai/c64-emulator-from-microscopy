"""Per-phase static structure of the netlist for the levelized core (lcpu).

For each clock phase (CLK0 = 0 -> PHI1, CLK0 = 1 -> PHI2):
  det      nodes fixed by CLK0 alone (tools/phases.py), with derivation order
  pccc     phase channel-connected components: nodes joined by channel
           transistors whose gate is not fixed at 0 in that phase
  order    topological order of the pccc gate-dependency graph, as a list
           of strongly connected components (SCCs with > 1 member iterate)
"""
import sys
from collections import defaultdict
from phases import phase_constants, PADS


class Phase:
    def __init__(self, N, clk0):
        self.N = N
        vss, vcc = N.vss, N.vcc
        self.det, self.det_order = phase_constants(N, clk0)
        det = self.det
        nodes = [n for n in sorted(N.used) if n not in (vss, vcc) and n not in det]
        self.nodes = nodes

        def gate_state(g):            # 0 off, 1 on, None dynamic
            if g == vcc:
                return 1
            if g == vss:
                return 0
            return det.get(g)
        self.gate_state = gate_state
        parent = {n: n for n in nodes}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        self.chan = []               # (gate, a, b, state) channel transistors not fixed off
        for g, a, b in N.trans:
            if b in (vss, vcc):
                continue
            st = gate_state(g)
            if st == 0:
                continue
            if a in det or b in det:
                # a det node is alone by construction (all its channels off)
                raise ValueError("conducting channel at a phase-fixed node")
            self.chan.append((g, a, b, st))
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb
        comp = defaultdict(list)
        for n in nodes:
            comp[find(n)].append(n)
        self.pccc = sorted(comp.values(), key=lambda c: c[0])
        self.cid = {}
        for i, c in enumerate(self.pccc):
            for n in c:
                self.cid[n] = i
        # rail transistors per node (gates not fixed off); fixed-on gates are constants
        self.gnd = defaultdict(list)
        self.pwr = defaultdict(list)
        self.gnd_const = defaultdict(int)
        self.pwr_const = defaultdict(int)
        for g, a, b in N.trans:
            if b == vss or b == vcc:
                st = gate_state(g)
                if st == 0:
                    continue
                if a in det:
                    continue
                if st == 1:
                    (self.gnd_const if b == vss else self.pwr_const)[a] += 1
                else:
                    (self.gnd if b == vss else self.pwr)[a].append(g)
        # dependency graph between pcccs (through dynamic gates)
        self.inputs = defaultdict(set)    # pccc -> set of gate nodes it reads
        for g, a, b, st in self.chan:
            if st is None:
                self.inputs[self.cid[a]].add(g)
        for n in nodes:
            for g in self.gnd[n] + self.pwr[n]:
                self.inputs[self.cid[n]].add(g)
        E = defaultdict(set)
        self.selfgated = set()
        self.sources = defaultdict(set)   # self-gated pccc -> its nodes that gate its own transistors
        for q, gs in self.inputs.items():
            for g in gs:
                if g in self.cid:
                    p = self.cid[g]
                    if p == q:
                        self.selfgated.add(q)
                        self.sources[q].add(g)
                    else:
                        E[p].add(q)
        self.E = E
        self.order = self._scc_order(len(self.pccc), E)

    @staticmethod
    def _scc_order(n, E):
        sys.setrecursionlimit(100000)
        idx, low, st, on, out = {}, {}, [], set(), []
        counter = [0]

        def sc(v):
            idx[v] = low[v] = counter[0]
            counter[0] += 1
            st.append(v)
            on.add(v)
            for w in sorted(E.get(v, ())):
                if w not in idx:
                    sc(w)
                    low[v] = min(low[v], low[w])
                elif w in on:
                    low[v] = min(low[v], idx[w])
            if low[v] == idx[v]:
                comp = []
                while True:
                    w = st.pop()
                    on.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                out.append(sorted(comp))
        for v in range(n):
            if v not in idx:
                sc(v)
        out.reverse()             # Tarjan emits sinks first
        return out

    def cone(self, start_nodes):
        """pccc indices reachable from the pcccs of start_nodes"""
        seen = set(self.cid[n] for n in start_nodes if n in self.cid)
        todo = list(seen)
        while todo:
            p = todo.pop()
            for q in self.E.get(p, ()):
                if q not in seen:
                    seen.add(q)
                    todo.append(q)
        return seen
