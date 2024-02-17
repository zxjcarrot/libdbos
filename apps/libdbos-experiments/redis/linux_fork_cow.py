import matplotlib
import argparse

matplotlib.use('Agg')

import math
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import matplotlib.ticker as tkr
import numpy as np
import common
import itertools
import sys
import json
import os
from pprint import pprint

df = pd.read_csv('data/LinuxFork.csv', sep='\t')

pprint(df)

common.set_params()
plt.rcParams['axes.labelsize'] = 6
plt.rcParams['xtick.labelsize'] = 7
plt.rcParams['ytick.labelsize'] = 7
plt.style.use("seaborn")

MARKERSIZE=5

def tickFormatter(x, pos):
    if x == 0:
        return "0"
    if x >= 1000000:
        s = "%dM" % (x / 1000000)
    elif x >= 10000:
        s = "%dK" % (x / 1000)
    else:
        s = "%d" % x
    return s


fig, axes = plt.subplots(figsize=(3, 1.6), ncols=1, nrows=1,
                         sharex=False, sharey=False)

#fig.tight_layout()


lw = 1

sizes = ['{0:g}'.format(x) for x in df['Memory-Footprint-(GiB)']]

baselines = {
    'Linux Fork' : df["Linux-Fork"]
}

markers = itertools.cycle(['o','s','v'])

width = 0.2
multiplier = 0
x = np.arange(len(sizes))  # the label locations

for (attr, thputs) in baselines.items():
    offset = width * multiplier
    axes.plot(sizes, thputs, marker=next(markers), markersize=MARKERSIZE, label=attr)
    multiplier += 1
axes.set_xticks(sizes)
axes.set_ylabel('Fork Latency (ms)', fontsize=8)
axes.set_xlabel('Memory Footprint (GiB)')
axes.yaxis.set_major_formatter(tkr.FuncFormatter(tickFormatter))


plt.savefig('graphs/LinuxFork.pdf', bbox_inches='tight', pad_inches=0.05)







df = pd.read_csv('data/LinuxCoW.csv', sep='\t')

fig, axes = plt.subplots(figsize=(3, 1.6), ncols=1, nrows=1,
                         sharex=False, sharey=False)

#fig.tight_layout()


lw = 1

sizes = ['{0:g}'.format(x) for x in df['#-Writer-Threads']]

baselines = {
    'Linux CoW' : df["Linux-CoW"]
}

markers = itertools.cycle(['o','s','v'])

width = 0.2
multiplier = 0
x = np.arange(len(sizes))  # the label locations

for (attr, thputs) in baselines.items():
    offset = width * multiplier
    axes.plot(sizes, thputs, marker=next(markers), markersize=MARKERSIZE, label=attr)
    multiplier += 1
axes.set_xticks(sizes)
axes.set_ylabel('Page Fault Latency (cycles)', fontsize=8)
axes.set_xlabel('# Writer Threads')
axes.yaxis.set_major_formatter(tkr.FuncFormatter(tickFormatter))

plt.savefig('graphs/LinuxCoW.pdf', bbox_inches='tight', pad_inches=0.05)
