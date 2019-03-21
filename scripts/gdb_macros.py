import gdb


class SpdkTailqList(object):

    def __init__(self, list_pointer, list_member, tailq_name_list):
        self.list_pointer = list_pointer
        self.tailq_name_list = tailq_name_list
        self.list_member = list_member
        self.list = gdb.parse_and_eval(self.list_pointer)

    def __iter__(self):
        curr = self.list['tqh_first']
        while curr:
            yield self.list_member(curr)
            for tailq_name in self.tailq_name_list:
                curr = curr[tailq_name]
            curr = curr['tqe_next']


class SpdkNormalTailqList(SpdkTailqList):

    def __init__(self, list_pointer, list_member):
        super(SpdkNormalTailqList, self).__init__(list_pointer, list_member,
                                                  ['tailq'])


class SpdkArr(object):

    def __init__(self, arr_pointer, num_elements, element_type):
        self.arr_pointer = arr_pointer
        self.num_elements = num_elements
        self.element_type = element_type

    def __iter__(self):
        for i in range(0, self.num_elements):
            curr = (self.arr_pointer + i).dereference()
            if (curr == 0x0):
                continue
            yield self.element_type(curr)


class SpdkPrintCommand(gdb.Command):

    def __init__(self, name, element_list):
        self.element_list = element_list
        gdb.Command.__init__(self, name,
                             gdb.COMMAND_DATA,
                             gdb.COMPLETE_SYMBOL,
                             True)

    def print_element_list(self, element_list):
        first = True
        for element in element_list:
            if first:
                first = False
            else:
                print("---------------")
            print("\n" + str(element) + "\n")

    def invoke(self, arg, from_tty):
        self.print_element_list(self.element_list)


class SpdkObject(object):

    def __init__(self, gdb_obj):
        self.obj = gdb_obj

    def get_name(self):
        return self.obj['name']

    def __str__(self):
        s = "SPDK object of type %s at %s" % (self.type_name, str(self.obj))
        s += '\n((%s*) %s)' % (self.type_name, str(self.obj))
        s += '\nname %s' % self.get_name()
        return s


class IoDevice(SpdkObject):

    type_name = 'struct io_device'


class IoDevices(SpdkTailqList):

    def __init__(self):
        super(IoDevices, self).__init__('g_io_devices', IoDevice, ['tailq'])


class spdk_print_io_devices(SpdkPrintCommand):

    def __init__(self):
        io_devices = IoDevices()
        name = 'spdk_print_io_devices'
        super(spdk_print_io_devices, self).__init__(name, io_devices)


class Bdev(SpdkObject):

    type_name = 'struct spdk_bdev'


class BdevMgrBdevs(SpdkTailqList):

    def __init__(self):
        tailq_name_list = ['internal', 'link']
        super(BdevMgrBdevs, self).__init__('g_bdev_mgr->bdevs', Bdev, tailq_name_list)


class spdk_print_bdevs(SpdkPrintCommand):
    name = 'spdk_print_bdevs'

    def __init__(self):
        bdevs = BdevMgrBdevs()
        super(spdk_print_bdevs, self).__init__(self.name, bdevs)


class spdk_find_bdev(spdk_print_bdevs):

    name = 'spdk_find_bdev'

    def invoke(self, arg, from_tty):
        print(arg)
        bdev_query = [bdev for bdev in self.element_list
                      if str(bdev.get_name()).find(arg) != -1]
        if bdev_query == []:
            print("Cannot find bdev with name %s" % arg)
            return

        self.print_element_list(bdev_query)


class NvmfSubsystem(SpdkObject):

    type_name = 'struct spdk_nvmf_subsystem'

    def __init__(self, ptr):
        self.ptr = ptr
        gdb_obj = self.ptr.cast(gdb.lookup_type(self.type_name).pointer())
        super(NvmfSubsystem, self).__init__(gdb_obj)

    def get_name(self):
        return self.obj['subnqn']

    def get_id(self):
        return int(self.obj['id'])

    def get_ns_list(self):
        max_nsid = int(self.obj['max_nsid'])
        ns_list = []
        for i in range(0, max_nsid):
            nsptr = (self.obj['ns'] + i).dereference()
            if nsptr == 0x0:
                continue
            ns = nsptr.cast(gdb.lookup_type('struct spdk_nvmf_ns').pointer())
            ns_list.append(ns)
        return ns_list

    def __str__(self):
        s = super(NvmfSubsystem, self).__str__()
        s += '\nnqn %s' % self.get_name()
        s += '\nID %d' % self.get_id()
        for ns in self.get_ns_list():
            s + '\t%s' % str(ns)
        return s


class SpdkNvmfTgtSubsystems(SpdkArr):

    def get_num_subsystems(self):
        try:  # version >= 18.11
            return int(self.spdk_nvmf_tgt['max_subsystems'])
        except RuntimeError:  # version < 18.11
            return int(self.spdk_nvmf_tgt['opts']['max_subsystems'])

    def __init__(self):
        self.spdk_nvmf_tgt = gdb.parse_and_eval("g_spdk_nvmf_tgt")
        subsystems = gdb.parse_and_eval("g_spdk_nvmf_tgt->subsystems")
        super(SpdkNvmfTgtSubsystems, self).__init__(subsystems,
                                                    self.get_num_subsystems(),
                                                    NvmfSubsystem)


class spdk_print_nvmf_subsystems(SpdkPrintCommand):

    def __init__(self):
        name = 'spdk_print_nvmf_subsystems'
        nvmf_tgt_subsystems = SpdkNvmfTgtSubsystems()
        super(spdk_print_nvmf_subsystems, self).__init__(name, nvmf_tgt_subsystems)


class IoChannel(SpdkObject):

    type_name = 'struct spdk_io_channel'

    def get_ref(self):

        return int(self.obj['ref'])

    def get_device(self):
        return self.obj['dev']

    def get_device_name(self):
        return self.obj['dev']['name']

    def get_name(self):
        return ""

    def __str__(self):
        s = super(IoChannel, self).__str__() + '\n'
        s += 'ref %d\n' % self.get_ref()
        s += 'device %s (%s)\n' % (self.get_device(), self.get_device_name())
        return s


# TODO - create TailqList type that gets a gdb object instead of a pointer
class IoChannels(SpdkTailqList):

    def __init__(self, list_obj):
        self.tailq_name_list = ['tailq']
        self.list_member = IoChannel
        self.list = list_obj


class SpdkThread(SpdkObject):

    type_name = 'struct spdk_thread'

    def __init__(self, gdb_obj):
        super(SpdkThread, self).__init__(gdb_obj)
        self.io_channels = IoChannels(self.obj['io_channels'])

    def __str__(self):
        s = super(SpdkThread, self).__str__() + '\n'
        s += "IO Channels:\n"
        for io_channel in self.get_io_channels():
            channel_lines = str(io_channel).split('\n')
            s += '\n'.join('\t%s' % line for line in channel_lines if line is not '')
            s += '\n'
            s += '\t---------------\n'
            s += '\n'
        return s

    def get_io_channels(self):
        return self.io_channels


class SpdkThreads(SpdkNormalTailqList):

    def __init__(self):
        super(SpdkThreads, self).__init__('g_threads', SpdkThread)


class spdk_print_threads(SpdkPrintCommand):

    def __init__(self):
        name = "spdk_print_threads"
        threads = SpdkThreads()
        super(spdk_print_threads, self).__init__(name, threads)


class spdk_load_macros(gdb.Command):

    def __init__(self):
        gdb.Command.__init__(self, 'spdk_load_macros',
                             gdb.COMMAND_DATA,
                             gdb.COMPLETE_SYMBOL,
                             True)
        self.loaded = False

    def invoke(self, arg, from_tty):
        if arg == '--reload':
            print('Reloading spdk information')
            reload = True
        else:
            reload = False

        if self.loaded and not reload:
            return

        spdk_print_threads()
        spdk_print_bdevs()
        spdk_print_io_devices()
        spdk_print_nvmf_subsystems()
        spdk_find_bdev()


spdk_load_macros()
