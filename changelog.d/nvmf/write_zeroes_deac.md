Write Zeroes commands with the DEAC (Deallocate) bit set no longer fail with `INVALID_FIELD`.
The deallocation request is treated as a hint and the specified logical blocks are written
with zeroes.
