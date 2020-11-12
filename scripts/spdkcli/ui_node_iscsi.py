from configshell_fb import ExecutionError
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
        iscsi_global_params = self.get_root().iscsi_get_options()
        if not iscsi_global_params:
            return
        for param, val in iscsi_global_params.items():
            UIISCSIGlobalParam("%s: %s" % (param, val), self)

    def ui_command_set_auth(self, g=None, d=None, r=None, m=None):
        """Set CHAP authentication for discovery service.

        Optional arguments:
            g = chap_group: Authentication group ID for discovery session
            d = disable_chap: CHAP for discovery session should be disabled
            r = require_chap: CHAP for discovery session should be required
            m = mutual_chap: CHAP for discovery session should be mutual
        """
        chap_group = self.ui_eval_param(g, "number", None)
        disable_chap = self.ui_eval_param(d, "bool", None)
        require_chap = self.ui_eval_param(r, "bool", None)
        mutual_chap = self.ui_eval_param(m, "bool", None)
        self.get_root().iscsi_set_discovery_auth(
            chap_group=chap_group, disable_chap=disable_chap,
            require_chap=require_chap, mutual_chap=mutual_chap)


class UIISCSIGlobalParam(UINode):
    def __init__(self, param, parent):
        UINode.__init__(self, param, parent)


class UIISCSIDevices(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "target_nodes", parent)
        self.scsi_devices = list()
        self.refresh()

    def refresh(self):
        self._children = set([])
        self.target_nodes = list(self.get_root().iscsi_get_target_nodes())
        self.scsi_devices = list(self.get_root().scsi_get_devices())
        for device in self.scsi_devices:
            for node in self.target_nodes:
                if hasattr(device, "device_name") and node['name'] \
                        == device.device_name:
                    UIISCSIDevice(device, node, self)

    def delete(self, name):
        self.get_root().iscsi_delete_target_node(target_node_name=name)

    def ui_command_create(self, name, alias_name, bdev_name_id_pairs,
                          pg_ig_mappings, queue_depth, g=None, d=None, r=None,
                          m=None, h=None, t=None):
        """Create target node

        Positional args:
           name: Target node name (ASCII)
           alias_name: Target node alias name (ASCII)
           bdev_name_id_pairs: List of bdev_name_id_pairs
           pg_ig_mappings: List of pg_ig_mappings
           queue_depth: Desired target queue depth
        Optional args:
           g = chap_group: Authentication group ID for this target node
           d = disable_chap: CHAP authentication should be disabled for this target node
           r = require_chap: CHAP authentication should be required for this target node
           m = mutual_chap: CHAP authentication should be mutual/bidirectional
           h = header_digest: Header Digest should be required for this target node
           t = data_digest: Data Digest should be required for this target node
        """
        luns = []
        print("bdev_name_id_pairs: %s" % bdev_name_id_pairs)
        print("pg_ig_mappings: %s" % pg_ig_mappings)
        for u in bdev_name_id_pairs.strip().split(" "):
            bdev_name, lun_id = u.split(":")
            luns.append({"bdev_name": bdev_name, "lun_id": int(lun_id)})
        pg_ig_maps = []
        for u in pg_ig_mappings.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        queue_depth = self.ui_eval_param(queue_depth, "number", None)
        chap_group = self.ui_eval_param(g, "number", None)
        disable_chap = self.ui_eval_param(d, "bool", None)
        require_chap = self.ui_eval_param(r, "bool", None)
        mutual_chap = self.ui_eval_param(m, "bool", None)
        header_digest = self.ui_eval_param(h, "bool", None)
        data_digest = self.ui_eval_param(t, "bool", None)
        self.get_root().iscsi_create_target_node(
            name=name, alias_name=alias_name, luns=luns,
            pg_ig_maps=pg_ig_maps, queue_depth=queue_depth,
            chap_group=chap_group, disable_chap=disable_chap,
            require_chap=require_chap, mutual_chap=mutual_chap,
            header_digest=header_digest, data_digest=data_digest)

    def ui_command_delete(self, name=None):
        """Delete a target node. If name is not specified delete all target nodes.

        Arguments:
           name - Target node name.
        """
        self.delete(name)

    def ui_command_delete_all(self):
        """Delete all target nodes"""
        rpc_messages = ""
        for device in self.scsi_devices:
            try:
                self.delete(device.device_name)
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def ui_command_add_lun(self, name, bdev_name, lun_id=None):
        """Add lun to the target node.

        Required args:
           name: Target node name (ASCII)
           bdev_name: bdev name
        Positional args:
           lun_id: LUN ID (integer >= 0)
        """
        if lun_id:
            lun_id = self.ui_eval_param(lun_id, "number", None)
        self.get_root().iscsi_target_node_add_lun(
            name=name, bdev_name=bdev_name, lun_id=lun_id)

    def summary(self):
        count = 0
        for device in self.scsi_devices:
            for node in self.target_nodes:
                if hasattr(device, "device_name") and node['name'] \
                        == device.device_name:
                    count = count + 1
        return "Target nodes: %d" % count, None


class UIISCSIDevice(UINode):
    def __init__(self, device, target, parent):
        UINode.__init__(self, device.device_name, parent)
        self.device = device
        self.target = target
        self.refresh()

    def ui_command_set_auth(self, g=None, d=None, r=None, m=None):
        """Set CHAP authentication for the target node.

        Optionals args:
           g = chap_group: Authentication group ID for this target node
           d = disable_chap: CHAP authentication should be disabled for this target node
           r = require_chap: CHAP authentication should be required for this target node
           m = mutual_chap: CHAP authentication should be mutual/bidirectional
        """
        chap_group = self.ui_eval_param(g, "number", None)
        disable_chap = self.ui_eval_param(d, "bool", None)
        require_chap = self.ui_eval_param(r, "bool", None)
        mutual_chap = self.ui_eval_param(m, "bool", None)
        self.get_root().iscsi_target_node_set_auth(
            name=self.device.device_name, chap_group=chap_group,
            disable_chap=disable_chap,
            require_chap=require_chap, mutual_chap=mutual_chap)

    def ui_command_iscsi_target_node_add_pg_ig_maps(self, pg_ig_mappings):
        """Add PG-IG maps to the target node.

        Args:
           pg_ig_maps: List of pg_ig_mappings, e.g. pg_tag:ig_tag pg_tag2:ig_tag2
        """
        pg_ig_maps = []
        for u in pg_ig_mappings.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        self.get_root().iscsi_target_node_add_pg_ig_maps(
            pg_ig_maps=pg_ig_maps, name=self.device.device_name)

    def ui_command_iscsi_target_node_remove_pg_ig_maps(self, pg_ig_mappings):
        """Remove PG-IG maps from the target node.

        Args:
           pg_ig_maps: List of pg_ig_mappings, e.g. pg_tag:ig_tag pg_tag2:ig_tag2
        """
        pg_ig_maps = []
        for u in pg_ig_mappings.strip().split(" "):
            pg, ig = u.split(":")
            pg_ig_maps.append({"pg_tag": int(pg), "ig_tag": int(ig)})
        self.get_root().iscsi_target_node_remove_pg_ig_maps(
            pg_ig_maps=pg_ig_maps, name=self.device.device_name)

    def refresh(self):
        self._children = set([])
        UIISCSILuns(self.target['luns'], self)
        UIISCSIPgIgMaps(self.target['pg_ig_maps'], self)
        auths = {"disable_chap": self.target["disable_chap"],
                 "require_chap": self.target["require_chap"],
                 "mutual_chap": self.target["mutual_chap"],
                 "chap_group": self.target["chap_group"],
                 "data_digest": self.target["data_digest"]}
        UIISCSIAuth(auths, self)

    def summary(self):
        return "Id: %s, QueueDepth: %s" % (self.device.id,
                                           self.target['queue_depth']), None


class UIISCSIAuth(UINode):
    def __init__(self, auths, parent):
        UINode.__init__(self, "auths", parent)
        self.auths = auths
        self.refresh()

    def summary(self):
        return "disable_chap: %s, require_chap: %s, mutual_chap: %s, chap_group: %s" % (
            self.auths['disable_chap'], self.auths['require_chap'],
            self.auths['mutual_chap'], self.auths['chap_group']), None


class UIISCSILuns(UINode):
    def __init__(self, luns, parent):
        UINode.__init__(self, "luns", parent)
        self.luns = luns
        self.refresh()

    def refresh(self):
        self._children = set([])
        for lun in self.luns:
            UIISCSILun(lun, self)

    def summary(self):
        return "Luns: %d" % len(self.luns), None


class UIISCSILun(UINode):
    def __init__(self, lun, parent):
        UINode.__init__(self, "lun %s" % lun['lun_id'], parent)
        self.lun = lun
        self.refresh()

    def summary(self):
        return "%s" % self.lun['bdev_name'], None


class UIISCSIPgIgMaps(UINode):
    def __init__(self, pg_ig_maps, parent):
        UINode.__init__(self, "pg_ig_maps", parent)
        self.pg_ig_maps = pg_ig_maps
        self.refresh()

    def refresh(self):
        self._children = set([])
        for pg_ig in self.pg_ig_maps:
            UIISCSIPgIg(pg_ig, self)

    def summary(self):
        return "Pg_ig_maps: %d" % len(self.pg_ig_maps), None


class UIISCSIPgIg(UINode):
    def __init__(self, pg_ig, parent):
        UINode.__init__(self, "portal_group%s - initiator_group%s" %
                        (pg_ig['pg_tag'], pg_ig['ig_tag']), parent)
        self.pg_ig = pg_ig
        self.refresh()


class UIPortalGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "portal_groups", parent)
        self.refresh()

    def delete(self, tag):
        self.get_root().iscsi_delete_portal_group(tag=tag)

    def ui_command_create(self, tag, portal_list):
        """Add a portal group.

        Args:
           portals: List of portals e.g. ip:port ip2:port2
           tag: Portal group tag (unique, integer > 0)
        """
        portals = []
        for portal in portal_list.strip().split(" "):
            host = portal
            cpumask = None
            if "@" in portal:
                host, cpumask = portal.split("@")
            if ":" not in host:
                raise ExecutionError("Incorrect format of portal group. Port is missing."
                                     "Use 'help create' to see the command syntax.")
            host, port = host.rsplit(":", -1)
            portals.append({'host': host, 'port': port})
            if cpumask:
                print("WARNING: Specifying a CPU mask for portal groups is no longer supported. Ignoring.")
        tag = self.ui_eval_param(tag, "number", None)
        self.get_root().construct_portal_group(tag=tag, portals=portals, private=None, wait=None)

    def ui_command_delete(self, tag):
        """Delete a portal group with given tag (unique, integer > 0))"""
        tag = self.ui_eval_param(tag, "number", None)
        self.delete(tag)

    def ui_command_delete_all(self):
        """Delete all portal groups"""
        rpc_messages = ""
        for pg in self.pgs:
            try:
                self.delete(pg.tag)
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def refresh(self):
        self._children = set([])
        self.pgs = list(self.get_root().iscsi_get_portal_groups())
        for pg in self.pgs:
            try:
                UIPortalGroup(pg, self)
            except JSONRPCException as e:
                self.shell.log.error(e.message)

    def summary(self):
        return "Portal groups: %d" % len(self.pgs), None


class UIPortalGroup(UINode):
    def __init__(self, pg, parent):
        UINode.__init__(self, "portal_group%s" % pg.tag, parent)
        self.pg = pg
        self.refresh()

    def refresh(self):
        self._children = set([])
        for portal in self.pg.portals:
            UIPortal(portal['host'], portal['port'], self)

    def summary(self):
        return "Portals: %d" % len(self.pg.portals), None


class UIPortal(UINode):
    def __init__(self, host, port, parent):
        UINode.__init__(self, "host=%s, port=%s" % (
            host, port), parent)
        self.refresh()


class UIInitiatorGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "initiator_groups", parent)
        self.refresh()

    def delete(self, tag):
        self.get_root().iscsi_delete_initiator_group(tag=tag)

    def ui_command_create(self, tag, initiator_list, netmask_list):
        """Add an initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
           initiators: List of initiator hostnames or IP addresses
                       separated with whitespaces, e.g. 127.0.0.1 192.168.200.100
           netmasks: List of initiator netmasks separated with whitespaces,
                     e.g. 255.255.0.0 255.248.0.0
        """
        tag = self.ui_eval_param(tag, "number", None)
        self.get_root().construct_initiator_group(
            tag=tag, initiators=initiator_list.split(" "),
            netmasks=netmask_list.split(" "))

    def ui_command_delete(self, tag):
        """Delete an initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
        """
        tag = self.ui_eval_param(tag, "number", None)
        self.delete(tag)

    def ui_command_delete_all(self):
        """Delete all initiator groups"""
        rpc_messages = ""
        for ig in self.igs:
            try:
                self.delete(ig.tag)
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def ui_command_add_initiator(self, tag, initiators, netmasks):
        """Add initiators to an existing initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
           initiators: List of initiator hostnames or IP addresses,
                       e.g. 127.0.0.1 192.168.200.100
           netmasks: List of initiator netmasks,
                     e.g. 255.255.0.0 255.248.0.0
        """
        tag = self.ui_eval_param(tag, "number", None)
        self.get_root().iscsi_initiator_group_add_initiators(
            tag=tag, initiators=initiators.split(" "),
            netmasks=netmasks.split(" "))

    def ui_command_delete_initiator(self, tag, initiators=None, netmasks=None):
        """Delete initiators from an existing initiator group.

        Args:
           tag: Initiator group tag (unique, integer > 0)
           initiators: List of initiator hostnames or IP addresses, e.g. 127.0.0.1 192.168.200.100
           netmasks: List of initiator netmasks, e.g. 255.255.0.0 255.248.0.0
        """
        tag = self.ui_eval_param(tag, "number", None)
        if initiators:
            initiators = initiators.split(" ")
        if netmasks:
            netmasks = netmasks.split(" ")
        self.get_root().iscsi_initiator_group_remove_initiators(
            tag=tag, initiators=initiators,
            netmasks=netmasks)

    def refresh(self):
        self._children = set([])
        self.igs = list(self.get_root().iscsi_get_initiator_groups())
        for ig in self.igs:
            UIInitiatorGroup(ig, self)

    def summary(self):
        return "Initiator groups: %d" % len(self.igs), None


class UIInitiatorGroup(UINode):
    def __init__(self, ig, parent):
        UINode.__init__(self, "initiator_group%s" % ig.tag, parent)
        self.ig = ig
        self.refresh()

    def refresh(self):
        self._children = set([])
        for initiator, netmask in zip(self.ig.initiators, self.ig.netmasks):
            UIInitiator(initiator, netmask, self)

    def summary(self):
        return "Initiators: %d" % len(self.ig.initiators), None


class UIInitiator(UINode):
    def __init__(self, initiator, netmask, parent):
        UINode.__init__(self, "hostname=%s, netmask=%s" % (initiator, netmask), parent)
        self.refresh()


class UIISCSIConnections(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "iscsi_connections", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        self.iscsicons = list(self.get_root().iscsi_get_connections())
        for ic in self.iscsicons:
            UIISCSIConnection(ic, self)

    def summary(self):
        return "Connections: %d" % len(self.iscsicons), None


class UIISCSIConnection(UINode):
    def __init__(self, ic, parent):
        UINode.__init__(self, "%s" % ic['id'], parent)
        self.ic = ic
        self.refresh()

    def refresh(self):
        self._children = set([])
        for key, val in self.ic.items():
            if key == "id":
                continue
            UIISCSIConnectionDetails("%s: %s" % (key, val), self)


class UIISCSIConnectionDetails(UINode):
    def __init__(self, info, parent):
        UINode.__init__(self, "%s" % info, parent)
        self.refresh()


class UIISCSIAuthGroups(UINode):
    def __init__(self, parent):
        UINode.__init__(self, "auth_groups", parent)
        self.refresh()

    def refresh(self):
        self._children = set([])
        self.iscsi_auth_groups = list(self.get_root().iscsi_get_auth_groups())
        if self.iscsi_auth_groups is None:
            self.iscsi_auth_groups = []
        for ag in self.iscsi_auth_groups:
            UIISCSIAuthGroup(ag, self)

    def delete(self, tag):
        self.get_root().iscsi_delete_auth_group(tag=tag)

    def delete_secret(self, tag, user):
        self.get_root().iscsi_auth_group_remove_secret(
            tag=tag, user=user)

    def ui_command_create(self, tag, secrets=None):
        """Add authentication group for CHAP authentication.

        Args:
           tag: Authentication group tag (unique, integer > 0).
        Optional args:
           secrets: Array of secrets objects separated by comma sign,
                    e.g. user:test secret:test muser:mutual_test msecret:mutual_test
        """
        tag = self.ui_eval_param(tag, "number", None)
        if secrets:
            secrets = [dict(u.split(":") for u in a.split(" "))
                       for a in secrets.split(",")]
        self.get_root().iscsi_create_auth_group(tag=tag, secrets=secrets)

    def ui_command_delete(self, tag):
        """Delete an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
        """
        tag = self.ui_eval_param(tag, "number", None)
        self.delete(tag)

    def ui_command_delete_all(self):
        """Delete all authentication groups."""
        rpc_messages = ""
        for iscsi_auth_group in self.iscsi_auth_groups:
            try:
                self.delete(iscsi_auth_group['tag'])
            except JSONRPCException as e:
                rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def ui_command_add_secret(self, tag, user, secret,
                              muser=None, msecret=None):
        """Add a secret to an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
           user: User name for one-way CHAP authentication
           secret: Secret for one-way CHAP authentication
        Optional args:
           muser: User name for mutual CHAP authentication
           msecret: Secret for mutual CHAP authentication
        """
        tag = self.ui_eval_param(tag, "number", None)
        self.get_root().iscsi_auth_group_add_secret(
            tag=tag, user=user, secret=secret,
            muser=muser, msecret=msecret)

    def ui_command_delete_secret(self, tag, user):
        """Delete a secret from an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
           user: User name for one-way CHAP authentication
        """
        tag = self.ui_eval_param(tag, "number", None)
        self.delete_secret(tag, user)

    def ui_command_delete_secret_all(self, tag):
        """Delete all secrets from an authentication group.

        Args:
           tag: Authentication group tag (unique, integer > 0)
        """
        rpc_messages = ""
        tag = self.ui_eval_param(tag, "number", None)
        for ag in self.iscsi_auth_groups:
            if ag['tag'] == tag:
                for secret in ag['secrets']:
                    try:
                        self.delete_secret(tag, secret['user'])
                    except JSONRPCException as e:
                        rpc_messages += e.message
        if rpc_messages:
            raise JSONRPCException(rpc_messages)

    def summary(self):
        return "Groups: %s" % len(self.iscsi_auth_groups), None


class UIISCSIAuthGroup(UINode):
    def __init__(self, ag, parent):
        UINode.__init__(self, "group" + str(ag['tag']), parent)
        self.ag = ag
        self.refresh()

    def refresh(self):
        self._children = set([])
        for secret in self.ag['secrets']:
            UISCSIAuthSecret(secret, self)

    def summary(self):
        return "Secrets: %s" % len(self.ag['secrets']), None


class UISCSIAuthSecret(UINode):
    def __init__(self, secret, parent):
        info_list = ["%s=%s" % (key, val)
                     for key, val in secret.items()]
        info_list.sort(reverse=True)
        info = ", ".join(info_list)
        UINode.__init__(self, info, parent)
        self.secret = secret
        self.refresh()
