/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

OUTPUT_ARCH( "riscv" )
ENTRY(__text_start)
EXTERN(vrdma_qp_rpc_handler)
EXTERN(vrdma_msix_handler)
EXTERN(vrdma_db_handler)
EXTERN(vrdma_dpa_msix_send_rpc_handler)

MEMORY
{
        code (rx) : ORIGIN =  0x0000000010000000, LENGTH = 128M
        sdata (rw) : ORIGIN = 0x0000000028000000, LENGTH = 128M
}

SECTIONS {
                .text : {
                        __text_start = .;
                        _text_start = .;
                        text_start = .;
                        *(.text.init)
                        *(.text.irq)
                        *(.text.idle)
                        *(.text)
                        *(.text.*)
                        *(.gnu.linkonce.t.*)
                        __text_end = .;
                        _text_end = .;
                        text_end = .;
                } > code

                .rodata  : { *(.rodata .rodata.* .gnu.linkonce.r.*) } > code
                .rodata1 : { *(.rodata1) } > code

                .got : {
                        . = ALIGN(8);
                        __global_pointer$ = .;
                        __got_start = .;
                        *(.got)
                        *(.got.*)
                        __got_end = .;
                } > sdata

                .data : {
                        __DATA_BEGIN__ = .;
                        *(.data.init_stage)
                        *(.data .data.* .gnu.linkonce.d.*)
                        SORT(CONSTRUCTORS)
                } > sdata

                .sdata : {
                        __SDATA_BEGIN__ = .;
                        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata .srodata.*)
                        *(.sdata .sdata.* .gnu.linkonce.s.*)
                } > sdata

                __bss_start = .;
                .sbss : {
                        . = ALIGN(8);
                        *(.dynsbss)
                        *(.sbss .sbss.* .gnu.linkonce.sb.*)
                        *(.scommon)
                } > sdata

                .bss : {
                        . = ALIGN(8);
                        *(.dynbss)
                        *(.bss .bss.* .gnu.linkonce.b.*)
                        *(COMMON)
                } > sdata

                __bss_end = .;
}
