from rpc.client import JSONRPCException
from .ui_node import UINode


class UIIScsi(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "iscsi", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UIScsiDevices(self)
        UIPortalGroups(self)
        UIInitiatorGroups(self)


class UIScsiDevices(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "scsi_devices", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for device in self.get_root().get_scsi_devices():
            UIScsiDevice(device, self)

    def ui_command_create(self, name, alias_name, bdev_name_id_pairs,
                          pg_ig_mappings, queue_depth, g=None, d=None, r=None,
                          m=None, h=None, t=None):
        print("%s %s %s %s %s" % (name, alias_name, bdev_name_id_pairs,
              pg_ig_mappings, queue_depth))
        luns = []
        for u in bdev_name_id_pairs.strip().split("-"):
            bdev_name, lun_id = u.split(":")
            luns.append({"bdev_name": bdev_name, "lun_id": int(lun_id)})
        pg_ig_maps = []
        for u in pg_ig_mappings.strip().split("-"):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        print("jkjkhjk")
        name = name
        alias_name = alias_name
        queue_depth = queue_depth
        chap_group = 0
        if g:
            chap_group = g
        disable_chap = False
        if d:
            disable_chap = True
        require_chap = False
        if r:
            require_chap = True
        mutual_chap = False
        if m:
            mutual_chap = True
        header_digest = False
        if h:
            header_digest = True
        data_digest = False
        if t:
            data_digest = True
        try:
            print "adadad"
            self.get_root().construct_target_node(luns=luns, pg_ig_maps=pg_ig_maps, name=name,
                                                  alias_name=alias_name, queue_depth=queue_depth,
                                                  chap_group=chap_group, disable_chap=disable_chap,
                                                  require_chap=require_chap, mutual_chap=mutual_chap,
                                                  header_digest=header_digest, data_digest=data_digest)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete(self, name=None):
        """
        Delete subsystem qith given nqn. If nqn is empty remove all subsystems

        Arguments:
        name - Target node name.
        """
        if name is None:
            for device in self.devices:
                try:
                     self.get_root().delete_target_node(name=device.device_name)
                except JSONRPCException as e:
                    self.shell.log.error(e.message)
        else:
            try:
                self.get_root().delete_target_node(name=name)
            except JSONRPCException as e:
                self.shell.log.error(e.message)
        # self.get_root().refresh()
        self.refresh()


class UIScsiDevice(UINode):
    def __init__(self, device, parent):
        UINode.__init__(self, device.device_name, parent)
        self.device = device
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        return "Id: %s" % self.device.id, None


class UIPortalGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "portal_groups", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for pg in self.get_root().get_portal_groups():
            UIPortalGroup(pg, self)


class UIPortalGroup(UINode):
    def __init__(self, pg, parent):
        portals = ""
        for portal in pg.portals:
            portals += "%s:%s@%s," %(portal['host'], portal['port'], portal['cpumask'])
        portals = portals[:-1]
        UINode.__init__(self, portals, parent)
        self.pg = pg
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        return "Tag: %s" % self.pg.tag, None


class UIInitiatorGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "initiator_groups", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ig in self.get_root().get_initiator_groups():
            UIInitiatorGroup(ig, self)


class UIInitiatorGroup(UINode):
    def __init__(self, ig, parent):
        igs = ""
        for initiator_group in ig.initiators:
            igs += initiator_group + ","
        igs = igs[:-1]
        UINode.__init__(self, "%s" % igs, parent)
        self.ig = ig
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        netmasks = ""
        for netmask in self.ig.netmasks:
            netmasks += "%s," % netmask
        netmasks = netmasks[:-1]
        return "Tag: %s, Netmasks: %s" % (self.ig.tag, netmasks), None

