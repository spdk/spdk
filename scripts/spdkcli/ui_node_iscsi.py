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
        UIISCSiConnections(self)
        UIISCSiAuthGroups(self)
        UIISCSiGlobalParams(self)


class UIISCSiGlobalParams(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "global_params", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for param, val in self.get_root().get_iscsi_global_params().items():
            UIISCSiGlobalParam("%s: %s" % (param, val), self)

    def ui_command_set_auth(self, chap_group=None, disable_chap=None,
                            require_chap=None, mutual_chap=None):
        """
        Set CHAP authentication for discovery service.

        Optional arguments:
        chap_group
        disable_chap
        require_chap
        mutual_chap
        """
        if chap_group:
            chap_group = self.ui_eval_param(chap_group, "number", None)
        if disable_chap:
            disable_chap = self.ui_eval_param(disable_chap, "bool", None)
        if require_chap:
            require_chap = self.ui_eval_param(require_chap, "bool", None)
        if mutual_chap:
            mutual_chap = self.ui_eval_param(mutual_chap, "bool", None)
        try:
            self.get_root().set_iscsi_discovery_auth(
                chap_group=chap_group, disable_chap=disable_chap,
                require_chap=require_chap, mutual_chap=mutual_chap)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()


class UIISCSiGlobalParam(UINode):
    def __init__(self, param, parent):
        UINode.__init__(self, param, parent)

    def refresh(self):
        self._children = set([])


class UIScsiDevices(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "target_nodes", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        target_nodes = self.get_root().get_target_nodes()
        for device in self.get_root().get_scsi_devices():
            for node in self.get_root().get_target_nodes():
                if hasattr(device, "device_name") and node['name']\
                        == device.device_name:
                    UIScsiDevice(device, node, self)

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
        name = name
        alias_name = alias_name
        queue_depth = self.ui_eval_param(queue_depth, "number", None)
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
            self.get_root().construct_target_node(
                luns=luns, pg_ig_maps=pg_ig_maps, name=name,
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
                    self.get_root().delete_target_node(
                        target_node_name=device.device_name)
                except JSONRPCException as e:
                    self.shell.log.error(e.message)
        else:
            try:
                self.get_root().delete_target_node(target_node_name=name)
            except JSONRPCException as e:
                self.shell.log.error(e.message)
        self.refresh()

    def ui_command_set_auth(self, chap_group=None, disable_chap=None,
                            require_chap=None, mutual_chap=None):
        if chap_group:
            chap_group = self.ui_eval_param(chap_group, "number", None)
        if disable_chap:
            disable_chap = self.ui_eval_param(disable_chap, "bool", None)
        if require_chap:
            require_chap = self.ui_eval_param(require_chap, "bool", None)
        if mutual_chap:
            mutual_chap = self.ui_eval_param(mutual_chap, "bool", None)
        try:
            self.get_root().set_iscsi_discovery_auth(
                chap_group=chap_group, disable_chap=disable_chap,
                require_chap=require_chap, mutual_chap=mutual_chap)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()

    def ui_command_add_lun(self, name, bdev_name, lun_id=None):
        if lun_id:
            lun_id = self.ui_eval_param(lun_id, "number", None)
        try:
            self.get_root().target_node_add_lun(
                name=name, bdev_name=bdev_name, lun_id=lun_id)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()


class UIScsiDevice(UINode):
    def __init__(self, device, target, parent):
        UINode.__init__(self, device.device_name, parent)
        self.device = device
        self.target = target
        self.refresh()

    def ui_command_set_auth(self, chap_group=None, disable_chap=None,
                            require_chap=None, mutual_chap=None):
        if chap_group:
            chap_group = self.ui_eval_param(chap_group, "number", None)
        if disable_chap:
            disable_chap = self.ui_eval_param(disable_chap, "bool", None)
        if require_chap:
            require_chap = self.ui_eval_param(require_chap, "bool", None)
        if mutual_chap:
            mutual_chap = self.ui_eval_param(mutual_chap, "bool", None)
        try:
            self.get_root().set_iscsi_target_node_auth(
                name=self.device.device_name, chap_group=chap_group,
                disable_chap=disable_chap,
                require_chap=require_chap, mutual_chap=mutual_chap)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()

    def refresh(self):
        self._children = set([])
        UIScsiLuns(self.target['luns'], self)
        UIScsiPgIgMaps(self.target['pg_ig_maps'], self)
        disc_service = {"disable_chap": self.target["disable_chap"],
                        "require_chap": self.target["require_chap"],
                        "mutual_chap": self.target["mutual_chap"],
                        "chap_group": self.target["chap_group"],
                        "data_digest": self.target["data_digest"]}
        UIScsiDiscoveryServices(disc_service, self)

    def summary(self):
        return "Id: %s, QueueDepth: %s" % (self.device.id,
                                           self.target['queue_depth']), None


class UIScsiDiscoveryServices(UINode):
    def __init__(self, discovery_service, parent):
        UINode.__init__(self, "discovery_service", parent)
        self.discovery_service = discovery_service
        self.refresh()

    def refresh(self):
        self._children = set([])
        UIScsiDiscoveryService(
            "disable_chap: %s" % self.discovery_service['disable_chap'], self)
        UIScsiDiscoveryService(
            "require_chap: %s" % self.discovery_service['require_chap'], self)
        UIScsiDiscoveryService(
            "mutual_chap: %s" % self.discovery_service['mutual_chap'], self)
        UIScsiDiscoveryService(
            "chap_group: %s" % self.discovery_service['chap_group'], self)
        UIScsiDiscoveryService(
            "data_digest: %s" % self.discovery_service['data_digest'], self)


class UIScsiDiscoveryService(UINode):
    def __init__(self, ds_to_display, parent):
        UINode.__init__(self, ds_to_display, parent)
        self.ds_to_display = ds_to_display
        self.refresh()

    def refresh(self):
        self._children = set([])


class UIScsiLun(UINode):
    def __init__(self, lun, parent):
        UINode.__init__(self, "%s ID:%s" % (lun['bdev_name'],
                                            lun['lun_id']), parent)
        self.lun = lun
        self.refresh()

    def refresh(self):
        self._children = set([])


class UIScsiLuns(UINode):
    def __init__(self, luns, parent):
        UINode.__init__(self, "luns", parent)
        self.luns = luns
        self.refresh()

    def refresh(self):
        self._children = set([])
        for lun in self.luns:
            UIScsiLun(lun, self)


class UIScsiLun(UINode):
    def __init__(self, lun, parent):
        UINode.__init__(self, "%s ID:%s" % (lun['bdev_name'],
                                            lun['lun_id']), parent)
        self.lun = lun
        self.refresh()

    def refresh(self):
        self._children = set([])


class UIScsiPgIgMaps(UINode):
    def __init__(self, pg_ig_maps, parent):
        UINode.__init__(self, "pg_ig_maps", parent)
        self.pg_ig_maps = pg_ig_maps
        self.refresh()

    def refresh(self):
        self._children = set([])
        for pg_ig in self.pg_ig_maps:
            UIScsiPgIg(pg_ig, self)


class UIScsiPgIg(UINode):
    def __init__(self, pg_ig, parent):
        UINode.__init__(self, "PG:%s IG:%s" % (pg_ig['pg_tag'],
                                               pg_ig['ig_tag']), parent)
        self.pg_ig = pg_ig
        self.refresh()

    def refresh(self):
        self._children = set([])


class UIPortalGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "portal_groups", parent)
        self.refresh()

    def ui_command_create(self, tag, portal_list):
        portals = []
        for portal in portal_list.strip().split("-"):
            host = portal
            cpumask = None
            if "@" in portal:
                host, cpumask = portal.split("@")
            host, port = host.rsplit(":", -1)
            portals.append({'host': host, 'port': port})
            if cpumask:
                portals[-1]['cpumask'] = cpumask
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().construct_portal_group(tag=tag, portals=portals)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete(self, tag):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_portal_group(tag=tag)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def refresh(self):
        self._children = set([])
        for pg in self.get_root().get_portal_groups():
            UIPortalGroup(pg, self)


class UIPortalGroup(UINode):
    def __init__(self, pg, parent):
        portals = ""
        for portal in pg.portals:
            portals += "%s:%s@%s," % (portal['host'],
                                      portal['port'],
                                      portal['cpumask'])
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

    def ui_command_create(self, tag, initiator_list, netmask_list):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().construct_initiator_group(
                tag=tag, initiators=initiator_list.split("-"),
                netmasks=netmask_list.split("="))
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete(self, tag):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_initiator_group(tag=tag)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_add_initiator(self, tag, initiators, netmasks):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().add_initiators_to_initiator_group(
                tag=tag, initiators=initiators.split("-"),
                netmasks=netmasks.split("-"))
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete_initiator(self, tag, initiators=None, netmasks=None):
        tag = self.ui_eval_param(tag, "number", None)
        if initiators:
            initiators = initiators.split("-")
        if netmasks:
            netmasks = netmasks.split("-")
        try:
            self.get_root().delete_initiators_from_initiator_group(
                tag=tag, initiators=initiators,
                netmasks=netmasks)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

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


class UIISCSiConnections(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "iscsi_connections", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ic in self.get_root().get_iscsi_connections():
            UIISCSiConnections(ic, self)


class UIISCSiConnection(UINode):
    def __init__(self, ig, parent):
        UINode.__init__(self, ic, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        return "", None


class UIISCSiAuthGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "auth_groups", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        iscsi_auth_groups = self.get_root().get_iscsi_auth_groups()
        if iscsi_auth_groups is None:
            return
        for ag in iscsi_auth_groups:
            UIISCSiAuthGroup(ag, self)

    def ui_command_create(self, tag, secrets=None):
        tag = self.ui_eval_param(tag, "number", None)
        if secrets:
            secrets = [dict(u.split(":") for u in a.split("-"))
                       for a in secrets.split("=")]
        try:
            self.get_root().add_iscsi_auth_group(tag=tag, secrets=secrets)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete(self, tag):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_iscsi_auth_group(tag=tag)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_add_secret(self, tag, user, secret,
                              muser=None, msecret=None):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().add_secret_to_iscsi_auth_group(
                tag=tag, user=user, secret=secret,
                muser=muser, msecret=msecret)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.refresh()

    def ui_command_delete_secret(self, tag, user):
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_secret_from_iscsi_auth_group(
                tag=tag, user=user)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.refresh()


class UIISCSiAuthGroup(UINode):
    def __init__(self, ag, parent):
        UINode.__init__(self, str(ag['tag']), parent)
        self.ag = ag
        self.refresh()

    def refresh(self):
        self._children = set([])
        for secret in self.ag['secrets']:
            UISCSiAuthSecret(secret, self)

    def summary(self):
        return "", None


class UISCSiAuthSecret(UINode):
    def __init__(self, secret, parent):
        info = ", ".join("%s=%s" % (key, val)
                         for key, val in secret.items())
        UINode.__init__(self, info, parent)
        self.secret = secret
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        return "", None
