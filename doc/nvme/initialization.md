# NVMe Initialization {#nvme_initialization}

\msc

	app [label="Application"], nvme [label="NVMe Driver"];
	app=>nvme [label="nvme_probe()"];
	app<<nvme [label="probe_cb(pci_dev)"];
	nvme=>nvme [label="nvme_attach(devhandle)"];
	nvme=>nvme [label="nvme_ctrlr_start(nvme_controller ptr)"];
	nvme=>nvme [label="identify controller"];
	nvme=>nvme [label="create queue pairs"];
	nvme=>nvme [label="identify namespace(s)"];
	app<<nvme [label="attach_cb(pci_dev, nvme_controller)"];
	app=>app [label="create block devices based on controller's namespaces"];

\endmsc
