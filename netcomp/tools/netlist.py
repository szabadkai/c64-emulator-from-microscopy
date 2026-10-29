"""Parse harness/gen/netlist6502.h (the single source of truth for the CPU)
and rebuild exactly the data structures harness/src/netsim.c builds in
netsim_create(): the same transistor clean-up, the same classification and
the same list orders.  Everything ncc.py emits is derived from this."""
import re
from collections import defaultdict


def parse_header(path):
    txt = open(path).read()

    def define(name):
        return int(re.search(r"#define %s (\d+)" % name, txt).group(1))

    nn = define("NET6502_NODES")
    m = re.search(r"net6502_pullups\[NET6502_PULLUPS\] = \{(.*?)\};", txt, re.S)
    pullups = [int(x) for x in re.findall(r"\d+", m.group(1))]
    m = re.search(r"net6502_trans\[NET6502_TRANSISTORS\]\[3\] = \{(.*?)\};", txt, re.S)
    trans = [tuple(int(v) for v in t) for t in re.findall(r"\{(\d+),(\d+),(\d+)\}", m.group(1))]
    m = re.search(r"net6502_names\[NET6502_NAMES\] = \{(.*?)\n\};", txt, re.S)
    names = [(a, int(b)) for a, b in re.findall(r'\{"((?:[^"\\]|\\.)*)",(-?\d+)\}', m.group(1))]
    assert len(pullups) == define("NET6502_PULLUPS")
    assert len(trans) == define("NET6502_TRANSISTORS")
    assert len(names) == define("NET6502_NAMES")
    return nn, pullups, trans, names


class Netlist:
    def __init__(self, path):
        nn, pullups, trans, names = parse_header(path)
        self.nn = nn
        self.names = names
        self.byname = {}
        for a, b in names:
            self.byname.setdefault(a.replace("\\\\", "\\"), b)
        self.vss = vss = self.byname["vss"]
        self.vcc = vcc = self.byname["vcc"]
        pull = [0] * nn
        for p in pullups:
            pull[p] = 1
        pull[vss] = pull[vcc] = 0
        self.pullup = pull
        self.raw_trans = trans

        # --- netsim_create: drop c1==c2 and rail-rail channels, rail on c2,
        # de-duplicate (same gate, same unordered channel), keep file order
        tt, seen = [], set()
        for g, c1, c2 in trans:
            if c1 == c2:
                continue
            if c1 in (vss, vcc) and c2 in (vss, vcc):
                continue
            if c1 in (vss, vcc):
                c1, c2 = c2, c1
            key = (g, min(c1, c2), max(c1, c2))
            if key in seen:
                continue
            seen.add(key)
            tt.append((g, c1, c2))
        self.trans = tt

        # --- classification and per-gate lists, in netsim's sorted orders
        self.chan_count = [0] * nn         # channel entries per node (netsim 'chan')
        self.chan_trans = []               # netsim t index of every channel transistor, ascending
        self.gnd_init = [0] * nn           # rail transistors permanently on (gate = vcc)
        self.pwr_init = [0] * nn
        gchan, ggnd, gpwr, dep = [], [], [], []
        self.pulldowns = defaultdict(list)  # node -> gates of its transistors to vss (non-rail gates)
        self.pullups_vcc = defaultdict(list)
        for t, (g, c1, c2) in enumerate(tt):
            gate_rail = g in (vss, vcc)
            if c2 == vss:
                if g == vcc:
                    self.gnd_init[c1] += 1
                if not gate_rail:
                    ggnd.append((g, c1))
                    self.pulldowns[c1].append(g)
            elif c2 == vcc:
                if g == vcc:
                    self.pwr_init[c1] += 1
                if not gate_rail:
                    gpwr.append((g, c1))
                    self.pullups_vcc[c1].append(g)
            else:
                self.chan_count[c1] += 1
                self.chan_count[c2] += 1
                self.chan_trans.append(t)
                if not gate_rail:
                    gchan.append((g, t))
                    dep.append((g, c1))
                    dep.append((g, c2))
        self.gchan = self._lists(gchan, False)
        self.ggnd = self._lists(ggnd, False)
        self.gpwr = self._lists(gpwr, False)
        self.dep = self._lists(dep, True)
        self.simple = [1 if self.chan_count[n] == 0 else 0 for n in range(nn)]

        # node names (first name in nodenames.js wins)
        self.nodename = {}
        for a, b in names:
            if 0 <= b < nn and b not in self.nodename:
                self.nodename[b] = a
        used = set(pullups)
        for g, c1, c2 in trans:
            used.update((g, c1, c2))
        self.used = used

    def _lists(self, pairs, uniq):
        pairs = sorted(pairs)
        out = defaultdict(list)
        prev = None
        for k, v in pairs:
            if uniq and prev == (k, v):
                continue
            prev = (k, v)
            out[k].append(v)
        return out

    def name(self, n):
        return self.nodename.get(n, "n%d" % n)

    def ccc(self):
        """channel-connected components (through non-rail channels)"""
        parent = list(range(self.nn))

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        for g, c1, c2 in self.trans:
            if c2 in (self.vss, self.vcc):
                continue
            a, b = find(c1), find(c2)
            if a != b:
                parent[a] = b
        comp = defaultdict(list)
        for n in sorted(self.used):
            if n in (self.vss, self.vcc):
                continue
            comp[find(n)].append(n)
        return list(comp.values())
