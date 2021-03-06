/* RAM limits */
__sram_u_start  = ORIGIN(sram_u);
__sram_u_length = LENGTH(sram_u);
__sram_u_end    = __sram_u_start + __sram_u_length;
__sram_l_start  = ORIGIN(sram_l);
__sram_l_length = LENGTH(sram_l);
__sram_l_end    = __sram_l_start + __sram_l_length;

/* Define the default stack size for interrupt mode. As no context is
   saved on this stack and ISRs are supposed to be short, it can be fairly
   small. 512 byte should be a safe assumption here */
STACK_SIZE = DEFINED(STACK_SIZE) ? STACK_SIZE : 0x200; /* 512 byte */

RAMVECT_SIZE = DEFINED(RAMVECT_SIZE) ? RAMVECT_SIZE : 0;

SECTIONS
{
    /* Interrupt vectors 0x00-0x3ff. */
    .vector_table :
    {
        _vector_rom = .;
        KEEP(*(.isr_vector))
        KEEP(*(.vector_table))
    } > vectors
    ASSERT (SIZEOF(.vector_table) == 0x400, "Interrupt vector table of invalid size.")
    ASSERT (ADDR(.vector_table) == 0x00000000, "Interrupt vector table at invalid location (linker-script error?)")
    ASSERT (LOADADDR(.vector_table) == 0x00000000, "Interrupt vector table at invalid location (linker-script error?)")

    /*
     * Allocate space for interrupt vector in RAM
     * This can safely be removed to free up 0x400 bytes of RAM if the code does
     * not make use of this CPU feature.
     */
    .ramvect :
    {
        . = ALIGN(1024);
        _vector_ram_start = .;
        . = _vector_ram_start + RAMVECT_SIZE;
        _vector_ram_end = .;
    } > sram_u


    /* Flash configuration field, very important in order to not accidentally lock the device */
    /* Flash configuration field 0x400-0x40f. */
    .fcfield :
    {
        . = ALIGN(4);
        KEEP(*(.fcfield))
        . = ALIGN(4);
    } > flashsec
    ASSERT (SIZEOF(.fcfield) == 0x10, "Flash configuration field of invalid size (linker-script error?)")
    ASSERT (ADDR(.fcfield) == 0x400, "Flash configuration field at invalid position (linker-script error?)")
    ASSERT (LOADADDR(.fcfield) == 0x400, "Flash configuration field at invalid position (linker-script error?)")

    /* Program code 0x410-. */
    .text :
    {
        . = ALIGN(4);
        _text_load = LOADADDR(.text);
        _text_start = .;
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
        . = ALIGN(4);

        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);
        . = ALIGN(4);

        /* fini data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN (__fini_array_end = .);
        . = ALIGN(4);

        KEEP (*(SORT_NONE(.init)))
        KEEP (*(SORT_NONE(.fini)))
        /* Default ISR handlers */
        KEEP(*(.default_handlers))
        *(.text.unlikely .text.*_unlikely .text.unlikely.*)
        *(.text.exit .text.exit.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
        *(.text .stub .text.* .gnu.linkonce.t.*)

        /* gcc uses crtbegin.o to find the start of
           the constructors, so we make sure it is
           first.  Because this is a wildcard, it
           doesn't matter if the user does not
           actually link against crtbegin.o; the
           linker won't look for a file to match a
           wildcard.  The wildcard also means that it
           doesn't matter which directory crtbegin.o
           is in.  */
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*crtbeginTS.o(.ctors))
        /* We don't want to include the .ctor section from
           the crtend.o file until after the sorted ctors.
           The .ctor section from the crtend file contains the
           end of ctors marker and it must be last */
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))

        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*crtbeginTS.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
        . = ALIGN(4);
        _rodata_start = .;
        *(.rodata .rodata* .gnu.linkonce.r.*)
        . = ALIGN(4);
        _rodata_end = .;
        _text_end = .;
    } > flash

    .ramcode :
    {
        . = ALIGN(4);
        _ramcode_load = LOADADDR(.ramcode);
        _ramcode_start = .;
        *(.ramcode*)
        . = ALIGN(4);
        _ramcode_end = .;
    } > sram_l AT > flash

    /* The .extab, .exidx sections are used for C++ exception handling */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > flash

    PROVIDE_HIDDEN (__exidx_start = .);
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > flash
    PROVIDE_HIDDEN (__exidx_end = .);

    .eh_frame_hdr :
    {
        *(.eh_frame_hdr)
    } > flash

    .eh_frame : ONLY_IF_RO
    {
        KEEP (*(.eh_frame))
    } > flash

    .gcc_except_table : ONLY_IF_RO
    {
        *(.gcc_except_table .gcc_except_table.*)
    } > flash

/*
    .eh_frame       : ONLY_IF_RW { KEEP (*(.eh_frame)) } > sram_u AT > flash
    .gcc_except_table   : ONLY_IF_RW { *(.gcc_except_table .gcc_except_table.*) } > sram_u AT > flash
*/

    . = ALIGN(4);
    _etext = .;

    /* Program data, values stored in flash and loaded upon init. */
    .relocate : AT (_etext)
    {
        . = ALIGN(4);
        _data_load  = LOADADDR(.relocate);
        _data_start = .;
        _srelocate = .;
        *(.ramfunc .ramfunc.*);
        *(.data .data.*);
        . = ALIGN(4);
        _erelocate = .;
        _data_end = .;
    } > sram_u


    /* .bss section, zeroed out during init. */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = . ;
        __bss_start = .;
        _szero = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = . ;
        __bss_end = .;
        _ezero = .;
    } > sram_u

    /* Make sure we set _end, in case we want dynamic memory management... */
    _end = .;
    __end = .;
    __end__ = .;
    PROVIDE(end = .);

    . = ALIGN(4);
    HEAP_SIZE = ORIGIN(sram_u) + LENGTH(sram_u) - STACK_SIZE - .;

    .heap (NOLOAD):
    {
        _heap_start = .;
        PROVIDE(__heap_start = .); /*__heap_start = .; */
        . = . + HEAP_SIZE;
        _heap_end = .;
        PROVIDE(__heap_max = .);
    } > sram_u

    /* stack section */
    .stack (NOLOAD):
    {
        . = ALIGN(8);
        _sstack = .;
        . = . + STACK_SIZE;
        . = ALIGN(8);
        _estack = .;
    } > sram_u

    /* Any debugging sections */
    /* Stabs debugging sections.  */
    .stab          0 : { *(.stab) }
    .stabstr       0 : { *(.stabstr) }
    .stab.excl     0 : { *(.stab.excl) }
    .stab.exclstr  0 : { *(.stab.exclstr) }
    .stab.index    0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment       0 : { *(.comment) }
    /* DWARF debug sections.
       Symbols in the DWARF debugging sections are relative to the beginning
       of the section so we begin them at 0.  */
    /* DWARF 1 */
    .debug          0 : { *(.debug) }
    .line           0 : { *(.line) }
    /* GNU DWARF 1 extensions */
    .debug_srcinfo  0 : { *(.debug_srcinfo) }
    .debug_sfnames  0 : { *(.debug_sfnames) }
    /* DWARF 1.1 and DWARF 2 */
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    /* DWARF 2 */
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line .debug_line.* .debug_line_end ) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_macinfo  0 : { *(.debug_macinfo) }
    /* SGI/MIPS DWARF 2 extensions */
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .debug_funcnames 0 : { *(.debug_funcnames) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames  0 : { *(.debug_varnames) }
    /* DWARF 3 */
    .debug_pubtypes 0 : { *(.debug_pubtypes) }
    .debug_ranges   0 : { *(.debug_ranges) }
    /* DWARF Extension.  */
    .debug_macro    0 : { *(.debug_macro) }

    /* XXX: what is the purpose of these sections? */
    .ARM.attributes 0 : { KEEP (*(.ARM.attributes)) KEEP (*(.gnu.attributes)) }
    .note.gnu.arm.ident 0 : { KEEP (*(.note.gnu.arm.ident)) }
    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
