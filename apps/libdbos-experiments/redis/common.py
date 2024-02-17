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
