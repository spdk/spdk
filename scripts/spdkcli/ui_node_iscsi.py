from rpc.client import JSONRPCException
from .ui_node import UINode


class UIISCSI(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "iscsi", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        UIISCSIDevices(self)
        UIPortalGroups(self)
        UIInitiatorGroups(self)
        UIISCSIConnections(self)
        UIISCSIAuthGroups(self)
        UIISCSIGlobalParams(self)


class UIISCSIGlobalParams(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "global_params", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for param, val in self.get_root().get_iscsi_global_params().items():
            UIISCSIGlobalParam("%s: %s" % (param, val), self)

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


class UIISCSIGlobalParam(UINode):
    def __init__(self, param, parent):
        UINode.__init__(self, param, parent)

    def refresh(self):
        self._children = set([])


class UIISCSIDevices(UINode):
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
                    UIISCSIDevice(device, node, self)

    def ui_command_create(self, name, alias_name, bdev_name_id_pairs,
                          pg_ig_mappings, queue_depth, g=None, d=None, r=None,
                          m=None, h=None, t=None):
        """
        Create target node

        Positional args:
           bdev_name_id_pairs: List of bdev_name_id_pairs
           pg_ig_mappings: List of pg_ig_mappings
           name: Target node name (ASCII)
           alias_name: Target node alias name (ASCII)
           queue_depth: Desired target queue depth
        Optional args:
           g: Authentication group ID for this target node
           d: CHAP authentication should be disabled for this target node
           r: CHAP authentication should be required for this target node
           m: CHAP authentication should be mutual/bidirectional
           h: Header Digest should be required for this target node
           t: Data Digest should be required for this target node
        """
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
        Delete subsystem with given nqn. If nqn is empty remove all subsystems.

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
        """
        Set CHAP authentication for discovery service.

        Optional args:
           chap_group: Authentication group ID for this target node
           disable_chap: CHAP authentication should be disabled for this target node
           require_chap: CHAP authentication should be required for this target node
           mutual_chap: CHAP authentication should be mutual/bidirectional
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

    def ui_command_add_lun(self, name, bdev_name, lun_id=None):
        """
        Add lun to the target node.

        Required args:
           name: Target node name (ASCII)
           bdev_name: bdev name
       Positional args:
           lun_id: LUN ID (integer >= 0)
        """
        if lun_id:
            lun_id = self.ui_eval_param(lun_id, "number", None)
        try:
            self.get_root().target_node_add_lun(
                name=name, bdev_name=bdev_name, lun_id=lun_id)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()


class UIISCSIDevice(UINode):
    def __init__(self, device, target, parent):
        UINode.__init__(self, device.device_name, parent)
        self.device = device
        self.target = target
        self.refresh()

    def ui_command_set_auth(self, chap_group=None, disable_chap=None,
                            require_chap=None, mutual_chap=None):
        """
        Set CHAP authentication for the target node.

        Optionals args:
           chap_group: Authentication group ID for this target node
           disable_chap: CHAP authentication should be disabled for this target node
           require_chap: CHAP authentication should be required for this target node
           mutual_chap: CHAP authentication should be mutual/bidirectional
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
            self.get_root().set_iscsi_target_node_auth(
                name=self.device.device_name, chap_group=chap_group,
                disable_chap=disable_chap,
                require_chap=require_chap, mutual_chap=mutual_chap)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()

    def ui_command_add_pg_ig_maps(self, pg_ig_mappings):
        """
        Add PG-IG maps to the target node.

        Args:
           pg_ig_maps: List of pg_ig_mappings, e.g. pg_tag:ig_tag-pg_tag:ig_tag
        """
        pg_ig_maps = []
        for u in pg_ig_mappings.strip().split("-"):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        try:
            self.get_root().add_pg_ig_maps(
                pg_ig_maps=pg_ig_maps, name=self.device.device_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()

    def ui_command_delete_pg_ig_maps(self, pg_ig_mappings):
        """
        Add PG-IG maps to the target node.

        Args:
           pg_ig_maps: List of pg_ig_mappings, e.g. pg_tag:ig_tag-pg_tag:ig_tag
        """
        pg_ig_maps = []
        for u in pg_ig_mappings.strip().split("-"):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        try:
            self.get_root().delete_pg_ig_maps(
                pg_ig_maps=pg_ig_maps, name=self.device.device_name)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.parent.refresh()

    def refresh(self):
        self._children = set([])
        UIISCSILuns(self.target['luns'], self)
        UIISCSIPgIgMaps(self.target['pg_ig_maps'], self)
        disc_service = {"disable_chap": self.target["disable_chap"],
                        "require_chap": self.target["require_chap"],
                        "mutual_chap": self.target["mutual_chap"],
                        "chap_group": self.target["chap_group"],
                        "data_digest": self.target["data_digest"]}
        UIISCSIDiscoveryServices(disc_service, self)

    def summary(self):
        return "Id: %s, QueueDepth: %s" % (self.device.id,
                                           self.target['queue_depth']), None


class UIISCSIDiscoveryServices(UINode):
    def __init__(self, discovery_service, parent):
        UINode.__init__(self, "discovery_service", parent)
        self.discovery_service = discovery_service
        self.refresh()

    def refresh(self):
        self._children = set([])
        UIISCSIDiscoveryService(
            "disable_chap: %s" % self.discovery_service['disable_chap'], self)
        UIISCSIDiscoveryService(
            "require_chap: %s" % self.discovery_service['require_chap'], self)
        UIISCSIDiscoveryService(
            "mutual_chap: %s" % self.discovery_service['mutual_chap'], self)
        UIISCSIDiscoveryService(
            "chap_group: %s" % self.discovery_service['chap_group'], self)
        UIISCSIDiscoveryService(
            "data_digest: %s" % self.discovery_service['data_digest'], self)


class UIISCSIDiscoveryService(UINode):
    def __init__(self, ds_to_display, parent):
        UINode.__init__(self, ds_to_display, parent)
        self.ds_to_display = ds_to_display
        self.refresh()

    def refresh(self):
        self._children = set([])


class UIISCSILuns(UINode):
    def __init__(self, luns, parent):
        UINode.__init__(self, "luns", parent)
        self.luns = luns
        self.refresh()

    def refresh(self):
        self._children = set([])
        for lun in self.luns:
            UIISCSILun(lun, self)


class UIISCSILun(UINode):
    def __init__(self, lun, parent):
        UINode.__init__(self, "%s ID:%s" % (lun['bdev_name'],
                                            lun['lun_id']), parent)
        self.lun = lun
        self.refresh()

    def refresh(self):
        self._children = set([])


class UIISCSIPgIgMaps(UINode):
    def __init__(self, pg_ig_maps, parent):
        UINode.__init__(self, "pg_ig_maps", parent)
        self.pg_ig_maps = pg_ig_maps
        self.refresh()

    def refresh(self):
        self._children = set([])
        for pg_ig in self.pg_ig_maps:
            UIISCSIPgIg(pg_ig, self)


class UIISCSIPgIg(UINode):
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
        """
        Add a portal group.

        Args:
           portals: List of portals
           tag: Initiator group tag (unique, integer > 0)
        """
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
        """
        Delete a portal group with given tag(unique, integer > 0))
        """
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
        """
        Add an initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
           initiators: List of initiator hostnames or IP addresses
                       separated with minus sign, e.g. 127.0.0.1-192.168.200.100
           netmasks: List of initiator netmasks separated with minus sign,
                     e.g. 255.255.0.0-255.248.0.0
        """
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().construct_initiator_group(
                tag=tag, initiators=initiator_list.split("-"),
                netmasks=netmask_list.split("-"))
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete(self, tag):
        """
        Delete an initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
        """
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_initiator_group(tag=tag)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_add_initiator(self, tag, initiators, netmasks):
        """
        Add initiators to an existing initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
           initiators: List of initiator hostnames or IP addresses,
                       e.g. 127.0.0.1-192.168.200.100
           netmasks: List of initiator netmasks,
                     e.g. 255.255.0.0-255.248.0.0
        """
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().add_initiators_to_initiator_group(
                tag=tag, initiators=initiators.split("-"),
                netmasks=netmasks.split("-"))
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_delete_initiator(self, tag, initiators=None, netmasks=None):
        """
        Delete initiators from an existing initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
           initiators: List of initiator hostnames or IP addresses, e.g. 127.0.0.1-192.168.200.100
           netmasks: List of initiator netmasks, e.g. 255.255.0.0-255.248.0.0
        """
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
        for igg in ig.initiators:
            print "IGGG: %s" % igg
        igs = ",".join(ig.initiators)
        UINode.__init__(self, "%s" % igs, parent)
        self.ig = ig
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        netmasks = ""
        netmasks = ",".join(self.ig.netmasks)
        return "Tag: %s, Netmasks: %s" % (self.ig.tag, netmasks), None


class UIISCSIConnections(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "iscsi_connections", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        for ic in self.get_root().get_iscsi_connections():
            UIISCSIConnections(ic, self)


class UIISCSIConnection(UINode):
    def __init__(self, ig, parent):
        UINode.__init__(self, ic, parent)
        self.refresh()

    def refresh(self):
        self._children = set([])

    def summary(self):
        return "", None


class UIISCSIAuthGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "auth_groups", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        iscsi_auth_groups = self.get_root().get_iscsi_auth_groups()
        if iscsi_auth_groups is None:
            return
        for ag in iscsi_auth_groups:
            UIISCSIAuthGroup(ag, self)

    def ui_command_create(self, tag, secrets=None):
        """
        Add authentication group for CHAP authentication.

        Args:
           tag: Authentication group tag (unique, integer > 0).
           secrets: Array of secrets objects (optional),
                    e.g. user:test-secret:test-muser:mutual_test-msecret:mutual_test
        """
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
        """
        Delete an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
        """
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_iscsi_auth_group(tag=tag)
        except JSONRPCException as e:
            self.shell.log.error(e.message)

        self.refresh()

    def ui_command_add_secret(self, tag, user, secret,
                              muser=None, msecret=None):
        """
        Add a secret to an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
           user: User name for one-way CHAP authentication
           secret: Secret for one-way CHAP authentication
           muser: User name for mutual CHAP authentication (optional)
           msecret: Secret for mutual CHAP authentication (optional)
        """
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().add_secret_to_iscsi_auth_group(
                tag=tag, user=user, secret=secret,
                muser=muser, msecret=msecret)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.refresh()

    def ui_command_delete_secret(self, tag, user):
        """
        Delete a secret from an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
           user: User name for one-way CHAP authentication
        """
        tag = self.ui_eval_param(tag, "number", None)
        try:
            self.get_root().delete_secret_from_iscsi_auth_group(
                tag=tag, user=user)
        except JSONRPCException as e:
            self.shell.log.error(e.message)
        self.refresh()


class UIISCSIAuthGroup(UINode):
    def __init__(self, ag, parent):
        UINode.__init__(self, str(ag['tag']), parent)
        self.ag = ag
        self.refresh()

    def refresh(self):
        self._children = set([])
        for secret in self.ag['secrets']:
            UISCSIAuthSecret(secret, self)

    def summary(self):
        return "", None


class UISCSIAuthSecret(UINode):
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
