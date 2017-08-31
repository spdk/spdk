# BlobKV (Blobstore Object) Design Proposal

## Use Case

Serverless computing is a FaaS ( Function as a Service ) framework.  When client sends k-v request ( GET/PUT, for example ), Serverless can receive your request and start a docker image the function to handle it. If the request involves data processing, Serverless will invoke the back-end storage service to connect with the application level k-v (such as AWS S3, Swift). For this reason, the speed of service and performance is critical, and therefore it would be great to use SPDK to accelerate the storage service at the back end.

## Proposed Architecture Overview

![SPDK BlobKV Archtecture Proposal](https://github.com/hellowaywewe/spdk/blob/spdk-objectstore/doc/blobkv.png)

- blobkv provides object storage semantics and key-value pair based API, similar to the filesystem interface provided by blobfs.

- blobkv builds upon existing infrastructure provided by blobstore and bdev layers
    
## Blobkv design proposal
![SPDK BlobKV Archtecture Proposal](https://github.com/hellowaywewe/spdk/blob/spdk-objectstore/doc/kv_api.png)

Since the blobstore have had the functions of using the original blob of SPDK to operate directly the device, thus the blobkv should be able to provide the interfaces of k-v which can adapt the blobstore. For example, the param k of the kv_blob_read(struct spdk_bs_kv *k) is needed to be converted to the param blob of spdk_bs_io_read_blob(struct spdk_blob *blob). For this reason, the device abstraction layer of k-v should be builded to work with data type conversion.

