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



parser = argparse.ArgumentParser(description="Graph figures varying database sizes for redis experiments.")

# Add arguments
parser.add_argument('--data_file_prefixes', type=str, help='A string argument')

parser.add_argument('--line_names', type=str, help='A comma-separated string of names')

parser.add_argument('--size_suffixes', type=str, help='A comma-separated string of integers')

parser.add_argument('--metric_name', type=str, help='p100.0 | p99.90 | p99.00 | p95.00 | p50.00')
parser.add_argument('--figure_filename', type=str, help='A string argument')

args = parser.parse_args()

data_file_prefixes =  [s for s in args.data_file_prefixes.split(',')]
line_names = [s for s in args.line_names.split(',')] 
metric_name = args.metric_name

sizes = [int(i) for i in args.size_suffixes.split(',')]

assert len(line_names) == len(data_file_prefixes)

print(line_names)

line_data = {}

for i, line_name in enumerate(line_names):
    line_data[line_name] = []
    for s in sizes:
        filename = os.path.join(data_file_prefixes[i] + str(s) + ".json")
        with open(filename) as json_data:
            d = json.load(json_data)
            line_data[line_name].append(d['ALL STATS']['Sets']['Percentile Latencies'][metric_name])


pprint(line_data)

common.set_params()
plt.rcParams['axes.labelsize'] = 6
plt.rcParams['xtick.labelsize'] = 7
plt.rcParams['ytick.labelsize'] = 7
plt.style.use("seaborn")

# def tickFormatter(x, pos):
#     if x == 0:
#         return "0"
#     if x >= 1000000:
#         s = "%dM" % (x / 1000000)
#     elif x >= 10000:
#         s = "%dK" % (x / 1000)
#     else:
#         s = "%d" % x
#     return s

MARKERSIZE=5

fig, axes = plt.subplots(figsize=(3, 1.5), ncols=1, nrows=1,
                         sharex=False, sharey=False)

#fig.tight_layout()


lw = 1

sizes = ['{0:g}'.format(x) for x in sizes]

baselines = []

for i, line_name in enumerate(line_names):
    print(line_name)
    baselines.append((line_name, line_data[line_name]))

markers = itertools.cycle(['o','s','v'])

width = 0.25
multiplier = 0
x = np.arange(len(sizes))  # the label locations

for (attr, thputs) in baselines:
    offset = width * multiplier
    axes.plot(sizes, thputs, marker=next(markers), markersize=MARKERSIZE, label=attr)
    multiplier += 1
axes.set_xticks(sizes)
axes.set_ylabel(metric_name + ' Latency (ms)', fontsize=8)
axes.set_xlabel('Redis Memory Footprint (GiB)')
#axes.set_yscale('log')
#axes.yaxis.set_major_formatter(tkr.FuncFormatter(tickFormatter))

#labels = ["B-tree", "2B-tree", "RocksDB"]
labels = line_names

#plt.tight_layout()

plt.subplots_adjust(top=0.8)
# Create custom legend
fig.legend(loc = "upper center", labels=labels, fontsize=8, frameon=False, borderpad=0, borderaxespad=0, ncol=2, columnspacing=0.8)

#plt.savefig('graphs/data_loading_btrees.pdf', bbox_inches='tight', pad_inches=0, bbox_to_anchor=(0.5, -0.05))
plt.savefig('graphs/' + args.figure_filename + '.pdf', bbox_inches='tight', pad_inches=0)
