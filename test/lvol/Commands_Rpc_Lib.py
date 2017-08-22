import json
from subprocess import check_output

class Spdk_Rpc (object):
	def __init__(self, rpc_py):
		self.rpc_py = rpc_py
	
	def __getattr__(self, name):
		def call (*args):
			cmd = "python {} {}".format (self.rpc_py, name)
			for arg in args:
				cmd += " {}".format (arg)
			return check_output (cmd, shell=True)
		
		return call

class Commands_Rpc(object):
	def __init__(self, rpc_py):
		self.rpc = Spdk_Rpc(rpc_py)
	def check_get_bdevs_methods(self, uuid_bdev):
		print("INFO: Check RPC COMMAND get_bdevs")
		output = self.rpc.get_bdevs()
		jsonvalue = json.loads(output)
		uuid_json = jsonvalue[-1]['name']
		if uuid_bdev in uuid_json:
			print("Info: UUID:{uuid} is found in RPC Commnad: "
				  "gets_bdevs response".format(uuid=uuid_bdev))
			return 0
		else:
			print("INFO: UUID:{uuid} not found in RPC COMMAND get_bdevs:"
				  "{uuid_json}".format(uuid=uuid_bdev, uuid_json=uuid_json))
			return 1
	def get_lvol_stores(self, base_name, uuid):
		print("INFO: RPC COMMAND get_lvol_stores")
		output = self.rpc.get_lvol_stores ()
		jsonvalue = json.loads(output)
		if jsonvalue:
			uuid_json_response = jsonvalue[-1]['UUID']
			base_bdev_json_reponse = jsonvalue[-1]['base_bdev']
			if base_name in base_bdev_json_reponse:
				print("INFO: base_name:{base_name} is found in RPC Commnad: "
					  "get_lvol_stores response".format(base_name=base_name))
			else:
				print("FAILED: base_name:{base_name} not found in "
					  "RPC COMMAND get_lvol_stores: "
					  "{base_bdev}".format(base_name=base_name,
										   base_bdev=base_bdev_json_reponse))
				return 1
			if uuid in uuid_json_response:
				print("INFO: UUID:{uuid} is found in RPC Commnad: "
					  "get_lvol_stores response".format(uuid=uuid))
				
			else:
				print("FAILED: UUID:{uuid} not found in RPC COMMAND "
					  "get_bdevs:{uuid_json}".format(uuid=uuid,
						uuid_json=uuid_json_response))
				return 1
		else:
			print("INFO: Lvol store not exist")
		return 0
	def construct_malloc_bdev(self, total_size, block_size):
		print("INFO: RPC COMMAND construct_malloc_bdev")
		output = self.rpc.construct_malloc_bdev(total_size, block_size)
		return output.rstrip('\n')
	def construct_lvol_store(self, base_name):
		print("INFO: RPC COMMAND construct_lvol_store")
		output = self.rpc.construct_lvol_store(base_name)
		return output.rstrip('\n')
	def construct_lvol_bdev(self, uuid, size):
		print("INFO: RPC COMMAND construct_lvol_bdev")
		output = self.rpc.construct_lvol_bdev(uuid, size)
		return output.rstrip('\n')
	def destroy_lvol_store(self, uuid):
		print("INFO: RPC COMMAND destroy_lvol_store")
		self.rpc.destroy_lvol_store(uuid)
	def delete_bdev(self, base_name):
		print("INFO: RPC COMMAND delete_bdev")
		self.rpc.delete_bdev(base_name)
	def resize_lvol_bdev(self, uuid, new_size):
		print("INFO: RPC COMMAND resize_lvol_bdev")
		self.rpc.resize_lvol_bdev(uuid, new_size)
