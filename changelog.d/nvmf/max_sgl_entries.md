Added `max_sgl_entries` parameter to the `nvmf_create_transport` RPC. It selects the maximum number
of SGL entries per request, which is the MSDBD advertised to the initiators, and it cannot exceed
the build time maximum.
