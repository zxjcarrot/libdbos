import matplotlib
import argparse

matplotlib.use('Agg')

import seaborn as sns
import matplotlib.pyplot as plt

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



ratios = [100, 50]
# ratios = [100]
#dbSize = [2, 4, 8, 16, 32]
# dbSize = [1, 2, 4]
# wtype = "im" # in memory
wtype = "om" # out of memory
dbSize = [8, 16, 32]
zipf = ["0", "0.9"]
#zipf = [0]
systems = ["tabby", 'vmcache', 'wired', 'lean']
systems_to_label = {"tabby":"Tabby", 'vmcache':"vmcache", 'wired' : "WiredTiger", 'lean':"LeanStore"}
labels = ["Tabby", 'vmcache', 'WiredTiger', 'LeanStore']
types = ["Throughput", "ReadIO", "WriteIO", "CPU", "SSDUtil"]

os.listdir("./data")

alldata = {}

def log_tick_formatter(val, pos=None):
    return f"{val:.2g}"

for ratio in ratios:
    for ff in zipf:
        for system in systems:
            if system not in alldata:
                alldata[system] = {}
            if ratio not in alldata[system]:
                alldata[system][ratio] = {}
            if ff not in alldata[system][ratio]:
                alldata[system][ratio][ff] = {}
            if "x" not in alldata[system][ratio][ff]:
                alldata[system][ratio][ff]["x"] = []
            for t in types:
                alldata[system][ratio][ff][t] = []
            alldata[system][ratio][ff]["Throughput-per-Core"] = []
            # print("{} {} {}".format(system, ratio, ff))

            for db in dbSize:
                path = os.path.join('data', "{}_database_size_dbsize_{}_{}_{}.csv".format(system, db, ratio, ff))
                data = pd.read_csv(path)

                data = data.iloc[-63:-3]
                #print(data)
                alldata[system][ratio][ff]["x"].append(str(db))
                alldata[system][ratio][ff]["Throughput"].append(data["tx"].mean())
                alldata[system][ratio][ff]["ReadIO"].append(data["kB_read/s"].mean()/1000)
                alldata[system][ratio][ff]["WriteIO"].append(data["kB_wrtn/s"].mean()/1000)
                data["CPU"] = (data["%user"]+data["%system"])*104 / 100
                alldata[system][ratio][ff]["CPU"].append(data["CPU"].mean())
                alldata[system][ratio][ff]["Throughput-per-Core"].append(data["tx"].mean() / data["CPU"].mean())
                alldata[system][ratio][ff]["SSDUtil"].append(data["%util"].mean())

types.append("Throughput-per-Core")
print(alldata)
set_params()
plt.rcParams['axes.labelsize'] = 7
plt.rcParams['xtick.labelsize'] = 9
plt.rcParams['ytick.labelsize'] = 9
plt.style.use("ggplot")

MARKERSIZE=4

def tickFormatter(x, pos):
    if x == 0:
        return "0"
    if x >= 1000000:
        s = "%fM" % (x / 1000000)
    elif x >= 10000:
        s = "%fK" % (x / 1000)
    else:
        s = "%f" % x

width = 0.25
multiplier = 0

markers = itertools.cycle(['o','s','v','^'])
width = 0.25
multiplier = 0

markers = itertools.cycle(['o','s','v','^'])
for t in types:
    for ratio in ratios:
        for ff in zipf:
            fig, axes = plt.subplots(figsize=(3, 1.6), ncols=1, nrows=1,
                         sharex=False, sharey=False)

            for system in systems:
                offset = width * multiplier
                #print("{} {} {}".format(system, ratio, ff))
                attr = alldata[system][ratio][ff]["x"]
                thputs = alldata[system][ratio][ff][t]
                print(thputs)
                offset = width * multiplier
                axes.plot(attr, thputs, marker=next(markers), markersize=MARKERSIZE, label=systems_to_label[system])
                multiplier += 1
            #plt.tight_layout(pad=2.0)
            # axes.set_xscale('log')
            #axes.yaxis.set_major_formatter(tkr.FuncFormatter(tickFormatter))
            # axes.xaxis.set_major_formatter(tkr.FuncFormatter(log_tick_formatter))
#             plt.subplots_adjust(top=0.8)

            # if t in ["CPU"]:
            #     axes.set_ylim(0.0, 25)
            
            # if t in ["Throughput"]:
            #     axes.set_yscale('log')
            #     axes.set_ylim(100000, 4000000)

            ylabel = t
            if t in ['Throughput']:
                ylabel = "Throughput (txns/s)"
                #axes.ticklabel_format(axis='y', style='sci', scilimits=(0,0))
                if wtype in ["im"]:
                    pass
                    #axes.set_yscale('log')
                    #axes.set_yticks(np.array([0.0, 10, 100, 1000, 10000, 100000, 1000000, 10000000]))
                else:
                    axes.ticklabel_format(axis='y', style='sci', scilimits=(0,0))
            if t in ['CPU']:
                ylabel = "CPU Util.(# cores)"
            if t in ['Throughput-per-Core']:
                ylabel = "Throughput/Core"
                axes.ticklabel_format(axis='y', style='sci', scilimits=(0,0))
            #axes.yaxis.set_major_formatter(tkr.FuncFormatter(tickFormatter))
            axes.set_ylabel(ylabel, fontsize=8)
            axes.set_xlabel('Database Size (GiB)')
            axes.set_xticks(attr)
            axes.set_ylim(ymin=0)

            plt.subplots_adjust(top=1)
            #legend = axes.legend(loc = "upper center", labels=labels, fontsize=9, frameon=True, borderpad=0, borderaxespad=0, ncol=4, columnspacing=0.8)
            #handles,mylabels = axes.get_legend_handles_labels()
            plt.savefig('graph/{}_ratio_{}_zipf_{}_{}'.format(t, ratio, ff, wtype) + '.pdf', bbox_inches='tight', pad_inches=0)
            print('graph/{}_ratio_{}_zipf_{}_{}'.format(t, ratio, ff, wtype) + '.pdf')

            # print("handles", handles)
            # print("labels", mylabels)
            # legend_fig = plt.figure()
            # legend_fig.legend(loc='center', handles=handles, labels=mylabels,frameon=True, ncol=4, columnspacing=0.8)
            # legend_fig.canvas.draw()
            # legend_fig.savefig('graph/legend.pdf', bbox_inches='tight', pad_inches=0)