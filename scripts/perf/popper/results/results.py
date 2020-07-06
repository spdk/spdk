import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
import seaborn as sns
import json
import pandas

# READ BENCHMARK DATA
spdk_read_bench = json.load(open('bdev_read.json'))
spdkread = spdk_read_bench["jobs"][0]["read"]
lib_read_bench = json.load(open('libaio_read.json'))
libread = lib_read_bench["jobs"][0]["read"]
# WRITE BENCHMARK DATA
spdk_write_bench = json.load(open('bdev_write.json'))
spdkwrite = spdk_write_bench["jobs"][0]["write"]
lib_write_bench = json.load(open('libaio_write.json'))
libwrite = lib_write_bench["jobs"][0]["write"]
# MIX BENCHMARK DATA
spdk_mix_bench = json.load(open('bdev_mix.json'))
spdkmixread = spdk_mix_bench["jobs"][0]["read"]
spdkmixwrite = spdk_mix_bench["jobs"][0]["write"]

lib_mix_bench = json.load(open('libaio_mix.json'))
libmixread = lib_mix_bench["jobs"][0]["read"]
libmixwrite = lib_mix_bench["jobs"][0]["write"]


frame_keys = ['min', 'max', 'mean']
dict_keys = ['iops_min', 'iops_max', 'iops_mean']

readspdkiops = {x: spdkread[k] for x,k in zip(frame_keys, dict_keys)}
readlibiops = {x: libread[k] for x,k in zip(frame_keys, dict_keys)}
writespdkiops = {x: spdkwrite[k] for x,k in zip(frame_keys, dict_keys)}
writelibiops = {x: libwrite[k] for x,k in zip(frame_keys, dict_keys)}

read_data = write_data = {
    'metric': ['iops', 'iops', 'iops', 'iops','iops', 'iops',
               'lat', 'lat', 'lat', 'lat', 'lat', 'lat'],

    'engine': ['spdk_bdev', 'spdk_bdev', 'spdk_bdev', 'libaio', 'libaio', 'libaio',
               'spdk_bdev', 'spdk_bdev', 'spdk_bdev', 'libaio', 'libaio', 'libaio'],

    'variable': ['min', 'mean', 'max', 'min', 'mean', 'max',
                 'min', 'mean', 'max','min', 'mean', 'max'],
}

mix_data = {
    'engine': ['spdk_bdev', 'libaio'],
      'iops': [spdkmixread['iops_mean'] + spdkmixwrite['iops_mean'],
               libmixread['iops_mean'] + libmixwrite['iops_mean']]
}

read_data['value'] = [readspdkiops['min'],readspdkiops['mean'],readspdkiops['max'],
                     readlibiops['min'], readlibiops['mean'], readlibiops['max'],
                     spdkread['lat_ns']['min'], spdkread['lat_ns']['mean'], spdkread['lat_ns']['max'],
                     libread['lat_ns']['min'], libread['lat_ns']['mean'],libread['lat_ns']['max']]

write_data['value'] = [writespdkiops['min'],writespdkiops['mean'],writespdkiops['max'],
                      writelibiops['min'], writelibiops['mean'], writelibiops['max'],
                      spdkwrite['lat_ns']['min'], spdkwrite['lat_ns']['mean'], spdkwrite['lat_ns']['max'],
                      libwrite['lat_ns']['min'], libwrite['lat_ns']['mean'],libwrite['lat_ns']['max']]


dfr = pandas.DataFrame(read_data)
dfw = pandas.DataFrame(write_data)
dfm = pandas.DataFrame(mix_data)
pp = PdfPages('report.pdf')

sns.barplot(x = 'engine', y = 'value', hue = 'variable', data = dfr[dfr['metric']=='iops'], ci='sd')
plt.title('IOPS for 4K random read workload')
plt.ylabel('IOPS')

ax = plt.gca()
y_max = dfr['value'].value_counts().max()
ax.set_ylim(1)
for p in ax.patches:
    ax.text(p.get_x() + p.get_width()/2., p.get_height(), '{0:.1f}'.format(p.get_height()),
        fontsize=8, color='black', ha='center', va='bottom')

plt.savefig(pp, format='pdf')
plt.close()

# READ LATENCY PLOT
sns.barplot(x = 'engine', y = 'value', hue = 'variable', data = dfr[dfr['metric']=='lat'], ci='sd')
plt.title('Latency for 4K random read workload')
plt.ylabel('Latency (ns)')

ax = plt.gca()
y_max = dfr['value'].value_counts().max()
ax.set_ylim(1)
for p in ax.patches:
    ax.text(p.get_x() + p.get_width()/2., p.get_height(), '{0:.1f}'.format(p.get_height()),
        fontsize=8, color='black', ha='center', va='bottom')
plt.savefig(pp, format='pdf')
plt.close()


# WRITE IOPS PLOT
sns.barplot(x = 'engine', y = 'value', hue = 'variable', data = dfw[dfw['metric']=='iops'], ci='sd')
plt.title('IOPS for 4K random write workload')
plt.ylabel('IOPS')

ax = plt.gca()
y_max = dfw['value'].value_counts().max()
ax.set_ylim(1)
for p in ax.patches:
    ax.text(p.get_x() + p.get_width()/2., p.get_height(), '{0:.1f}'.format(p.get_height()),
        fontsize=8, color='black', ha='center', va='bottom')
plt.savefig(pp, format='pdf')
plt.close()


# WRITE LATENCY PLOT
sns.barplot(x = 'engine', y = 'value', hue = 'variable', data = dfw[dfw['metric']=='lat'], ci='sd')
plt.title('Latency for 4K random write workload')
plt.ylabel('Latency (ns)')

ax = plt.gca()
y_max = dfw['value'].value_counts().max()
ax.set_ylim(1)
for p in ax.patches:
    ax.text(p.get_x() + p.get_width()/2., p.get_height(), '{0:.1f}'.format(p.get_height()),
        fontsize=8, color='black', ha='center', va='bottom')
plt.savefig(pp, format='pdf')
plt.close()

# MIX IOPS PLOT
sns.barplot(x = 'engine', y = 'iops', data = dfm)
plt.title('IOPS for 4K mix workload')
plt.ylabel('IOPS')

ax = plt.gca()
y_max = dfm['iops'].value_counts().max()
ax.set_ylim(1)
for p in ax.patches:
    ax.text(p.get_x() + p.get_width()/2., p.get_height(), '{0:.1f}'.format(p.get_height()),
        fontsize=8, color='black', ha='center', va='bottom')
plt.savefig(pp, format='pdf')
plt.close()
pp.close()
