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



ratios = [100]
wtype = "om" # out of memory
dbSize = [16, 4]
zipf = ["0"]
#zipf = [0]
systems = ["leanstore", 'leanstore_priv']
systems_to_label = {"leanstore":"Baseline", 'leanstore_priv':"+Privileged"}
# labels = ["Tabby", 'vmcache', 'WiredTiger', 'LeanStore']
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
            alldata[system][ratio][ff]["Throughput"] = []
            for db in dbSize:
                path = os.path.join('data', "{}_ablation_dbsize_{}_{}_{}.csv".format(system, db, ratio, ff))
                data = pd.read_csv(path)

                data = data.iloc[-63:-3]
                print(data)
                alldata[system][ratio][ff]["x"].append(str(db))
                alldata[system][ratio][ff]["Throughput"].append(data["tx"].mean())
                alldata[system][ratio][ff]["ReadIO"].append(data["kB_read/s"].mean()/1000)
                alldata[system][ratio][ff]["WriteIO"].append(data["kB_wrtn/s"].mean()/1000)
                data["CPU"] = (data["%user"]+data["%system"])*104 / 100
                alldata[system][ratio][ff]["CPU"].append(data["CPU"].mean())
                #alldata[system][ratio][ff]["Throughput-per-Core"].append(data["tx"].mean() / data["CPU"].mean())
                alldata[system][ratio][ff]["Throughput-per-Core"].append(data["tx"].mean())
                alldata[system][ratio][ff]["SSDUtil"].append(data["%util"].mean())

types.append("Throughput-per-Core")
print(alldata)
# set_params()
# plt.rcParams['axes.labelsize'] = 7
# plt.rcParams['xtick.labelsize'] = 9
# plt.rcParams['ytick.labelsize'] = 9
plt.style.use("ggplot")

MARKERSIZE=4

def tickFormatter(x, pos):
    
    if x == 0:
        return "0"
    if x >= 1000000:
        s = "%.0fM" % (x / 1000000)
    elif x >= 10000:
        s = "%.0fK" % (x / 1000)
    else:
        s = "%.0f" % x
    return s

data = {
    'leanstore': alldata['leanstore'][100]['0']['Throughput-per-Core'][0],
    'leanstore_priv': alldata['leanstore_priv'][100]['0']['Throughput-per-Core'][0],
}
def draw(data, xlables, figure_name, title):
    # Prepare data for plotting
    techniques = list(data.keys())
    values = list(data.values())

    # Calculate percentage changes
    baseline = values[0]
    percentage_changes = [0]  # First bar is the baseline
    for i in range(1, len(values)):
        percentage_change = ((values[i] - values[i-1]) / values[i-1]) * 100
        percentage_changes.append(percentage_change)

    # Create the plot
    fig, ax = plt.subplots(figsize=(3.5, 3))

    # Define the bar width and positions
    bar_width = 0.3
    r = np.arange(len(techniques))

    # Plot bars with new style
    bars = ax.bar(r, values, color=['#3498db', '#e74c3c'], width=bar_width, 
                edgecolor='black', alpha=0.8)

    # Add hatching to bars
    # hatches = ['//', '\\\\', 'xx']
    # for bar, hatch in zip(bars, hatches):
    #     bar.set_hatch(hatch)

    # Add value labels on top of each bar
    def add_value_labels(bars):
        for i, bar in enumerate(bars):
            height = bar.get_height()
            if height < 1000:
                t = f'{height:.2f}'
            else:
                t = f'{height/1000:.3f}k'
            ax.text(bar.get_x() + bar.get_width()/2., height,
                    t,
                    ha='center', va='bottom', fontsize=10)
            
            # Add percentage change for non-baseline bars
            if i > 0:
                fontcolor = 'green'
                if percentage_changes[i] < 0:
                    fontcolor = 'red'
                ax.text(bar.get_x() + bar.get_width()/2., height / 4,
                        f'{percentage_changes[i]:+.0f}%',
                        ha='center', va='bottom', fontsize=10, color=fontcolor,
                        bbox=dict(facecolor='white', boxstyle='square'))

    add_value_labels(bars)
    # Customize the plot
    ax.set_ylabel('Transactions/s', fontsize=12, fontweight='bold')
    ax.yaxis.set_major_formatter(tkr.FuncFormatter(tickFormatter))
    ax.set_title(title, fontsize=12, fontweight='bold')
    ax.set_xticks(r)
    ax.set_xticklabels(xlables, 
                    rotation=0, ha='center', fontsize=12)
    plt.yticks(fontsize=12)

    # Improve overall appearance
    fig.tight_layout()

    fig.savefig(f'graph/{figure_name}.pdf', bbox_inches='tight')

data = {
    'leanstore': alldata['leanstore'][100]['0']['Throughput-per-Core'][0],
    'leanstore_priv': alldata['leanstore_priv'][100]['0']['Throughput-per-Core'][0],
}
draw(data, ['LeanStore', '+Elevated\nPrivilege'], 'ablation_study_leanstore_om_throughput_per_core', '8GB Mem., 16GB Data, YCSB-C')


data = {
    'leanstore': alldata['leanstore'][100]['0']['Throughput-per-Core'][1],
    'leanstore_priv': alldata['leanstore_priv'][100]['0']['Throughput-per-Core'][1],
}
draw(data, ['LeanStore', '+Elevated\nPrivilege'], 'ablation_study_leanstore_im_throughput_per_core', '8GB Mem., 4GB Data, YCSB-C')

plt.close()

