### tpcc
import matplotlib
import argparse

matplotlib.use('Agg')

import seaborn as sns
import matplotlib.pyplot as plt

sn = "Lotus"

def set_params():
    sns.set_context("paper")
    plt.rcParams['font.size'] = 6
    plt.rcParams['axes.titlesize'] = 9
    plt.rcParams['axes.labelsize'] = 7
    plt.rcParams['xtick.labelsize'] = 6
    plt.rcParams['ytick.labelsize'] = 6
    plt.rcParams['legend.fontsize'] = 6
    plt.rcParams['lines.markersize'] = 3
    plt.rcParams['ps.fonttype'] = 42
    plt.rcParams['pdf.fonttype'] = 42
    plt.rcParams['font.family'] = 'sans-serif'
 
    sns.set_style('ticks')

import math
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import matplotlib.ticker as tkr
import numpy as np
import itertools
import sys
import json
import os
from pprint import pprint




zipf = [0, 1]
zipf = [0]
systems = ["tabby", 'vmcache', 'wired', 'lean']
labels = ["Tabby", 'vmcache', 'WiredTiger', 'LeanStore']
types = ["Throughput", "ReadIO", "WriteIO", "CPU", "SSDUtil", "Throughput-per-Core"]

# os.listdir("./data_tpcc")

alldata = {}


for system in systems:
    alldata[system] = {}
    alldata[system]["x"] = []
    for t in types:
        alldata[system][t] = []
    db = 102
    path = os.path.join('data_tpcc', "{}_database_size_dbsize_{}.csv".format(system, db))
    data = pd.read_csv(path)

    data = data.iloc[:-5]
    alldata[system]["x"] = data["ts"]
    alldata[system]["Throughput"] = data["tx"]
    alldata[system]["ReadIO"] = data["kB_read/s"]/1000
    alldata[system]["WriteIO"] = data["kB_wrtn/s"]/1000
    data["CPU"] = (data["%user"]+data["%system"])*104 / 100
    alldata[system]["CPU"] = data["CPU"]
    alldata[system]["SSDUtil"] = data["%util"]
    alldata[system]["Throughput-per-Core"] = (data["tx"] / data["CPU"])
    print(system, alldata[system]["Throughput"].sum())
    

set_params()
plt.rcParams['axes.labelsize'] = 7
plt.rcParams['xtick.labelsize'] = 9
plt.rcParams['ytick.labelsize'] = 9
plt.style.use("ggplot")

MARKERSIZE=3

fig, axes = plt.subplots(figsize=(3, 1.6), ncols=1, nrows=1,
                         sharex=False, sharey=False)

def tickFormatter(x, pos):
    if x == 0:
        return "0"
    if x >= 1000000:
        s = "%dM" % (x / 1000000)
    elif x >= 10000:
        s = "%dK" % (x / 1000)
    else:
        s = "%d" % x

width = 0.25
multiplier = 0

markers = itertools.cycle(['o','s','v','^'])
width = 0.25
multiplier = 0

print(alldata)
markers = itertools.cycle(['o','s','v','^'])
for t in types:
    fig, axes = plt.subplots(figsize=(3, 1.6), ncols=1, nrows=1,
            sharex=False, sharey=False)
    colors = {}
    colors['tabby'] = 'blue'
    colors['vmcache'] = 'red'
    colors['wired'] = 'green'
    colors['lean'] = 'yellow'
    for system in systems:
        offset = width * multiplier
        attr = alldata[system]['x']
        thputs = alldata[system][t]
        offset = width * multiplier

        # axes.scatter(attr, thputs, color=colors[system], marker=next(markers), s=MARKERSIZE, label=attr)

        # 计算趋势线
        # 对于线性趋势
        # z = np.polyfit(attr, thputs, 10)  # 1代表线性
        # p = np.poly1d(z)
        # axes.plot(attr, p(attr), color=colors[system], linestyle="dashed")

        axes.plot(attr, thputs, linestyle="dashed", marker=next(markers), markersize=MARKERSIZE, label=attr)
        multiplier += 1
    
    ylabel = t
    if t in ['Throughput']:
        ylabel = "Throughput (txns/s)"
        axes.ticklabel_format(axis='y', style='sci', scilimits=(0,0))
    if t in ['CPU']:
        ylabel = "CPU Util.(# cores)"
    if t in ['Throughput-per-Core']:
        ylabel = "Throughput/Core"
        axes.ticklabel_format(axis='y', style='sci', scilimits=(0,0))
    axes.set_ylabel(ylabel, fontsize=8)
    axes.set_xlabel('Time (seconds)')
    axes.set_ylim(ymin=0)
    # axes.set_xticks(attr)
    

    plt.subplots_adjust(top=0.75)
    #fig.legend(loc = "upper center", labels=labels, fontsize=8, frameon=False, borderpad=0, borderaxespad=0, ncol=2, columnspacing=0.8)
    plt.savefig('graph_tpcc/{}'.format(t) + '.pdf', bbox_inches='tight', pad_inches=0)