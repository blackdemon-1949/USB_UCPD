/*
******************************************************************************
**
** @file        : LinkerScript.ld
**
** @author      : STM32CubeIDE
**
**  Abstract    : Linker script for STM32H7Rxx Device
**                      8 MBytes XSPI1 XiP (PY25Q64HA)
**                      456KBytes RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
******************************************************************************
** @attention
**
** Copyright (c) 2026 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
******************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(DTCM) + LENGTH(DTCM); /* end of "DTCM" Ram type memory */

_Min_Heap_Size = 0x4000; /* USBPD stack + CDC CLI (malloc'd PD DMA buffers live here) */
_Min_Stack_Size = 0x1000;

__FLASH_BEGIN  = 0x90000000;
__FLASH_SIZE   = 0x00800000; /* PY25Q64HA: 64 Mbit = 8 MByte */


__RAM_BEGIN    = 0x24000000;
__RAM_SIZE     = 0x6E000; /* cacheable AXI SRAM; DMA window begins at 0x2406E000 */
__RAM_NONCACHEABLEBUFFER_SIZE = 0x2000; /* 8 KiB DMA/USB CDC */

/* Memories definition */
MEMORY
{
  RAM       (xrw) : ORIGIN = __RAM_BEGIN,    LENGTH = __RAM_SIZE
  RAM_NONCACHEABLEBUFFER (xrw) : ORIGIN = __RAM_BEGIN + __RAM_SIZE,  LENGTH = __RAM_NONCACHEABLEBUFFER_SIZE

  ITCM      (xrw) : ORIGIN = 0x00001000,    LENGTH = 0x0000F000
  DTCM       (rw) : ORIGIN = 0x20000000,    LENGTH = 0x00010000
  SRAMAHB   (rw)  : ORIGIN = 0x30000000,  LENGTH = 0x00008000
  BKPSRAM   (rw)  : ORIGIN = 0x38800000,  LENGTH = 0x00001000

  FLASH     (xrw) : ORIGIN = __FLASH_BEGIN, LENGTH = __FLASH_SIZE
}

/* Sections */
SECTIONS
{
  /* The startup code into "FLASH" Rom type memory */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data into "FLASH" Rom type memory */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data into "FLASH" Rom type memory */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH
  .ARM (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

  .preinit_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections into "RAM" Ram type memory */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */

  } >RAM AT> FLASH

  /* Code that must keep running while the XSPI memory-mapped window is
     switched off (external NOR programming, see ext_nor.c).

     It CANNOT live in the XiP flash window like the rest of .text: leaving
     memory-mapped mode stops instruction fetch from 0x90000000, so the first
     fetch after the mode switch would fault and the board would look dead.
     It cannot live in cacheable AXI SRAM either (the CPU would have to worry
     about I/D coherency and the MPU would have to make that RAM executable).

     ITCM is the right home: CPU-local, always executable, never cached, and
     it does not collide with any MPU region the application configures.  The
     startup code copies it out of the .data image below. */
  .ramfunc :
  {
    . = ALIGN(4);
    _sramfunc = .;
    KEEP(*(.ramfunc))
    KEEP(*(.ramfunc.*))
    . = ALIGN(4);
    _eramfunc = .;
  } >ITCM AT> FLASH

  _siramfunc = LOADADDR(.ramfunc);

  /* Uninitialized data section into "RAM" Ram type memory */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  RW_NONCACHEABLE (NOLOAD):
  {
    __NONCACHEABLEBUFFER_BEGIN = .;/* create symbol for start of section */
    KEEP(*(noncacheable_buffer))
    /* NOR scratch: the XSPI controller reads/writes this buffer through the
       AHB as a bus master, so it must not sit behind the D-cache. */
    . = ALIGN(32);
    KEEP(*(.nor_scratch))
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_END = .;  /* create symbol for start of section */
  } > RAM_NONCACHEABLEBUFFER

  /* newlib heap.  MUST live in DMA-accessible AXI SRAM, NOT DTCM: the USBPD
     stack allocates its RX/TX DMA buffers with malloc(), and on STM32H7R3
     only the CPU and HPDMA can reach the TCM memories (AN6062) - the UCPD's
     GPDMA1 (channels 0/1) cannot.  A DTCM heap makes every malloc'd DMA
     buffer unreachable, so PD messages are never received while CC
     attach/detach (which needs no DMA) still works. */
  ._user_heap :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = ALIGN(8);
  } >RAM

  /* MSP stack stays in fast, CPU-only DTCM (it is never touched by DMA). */
  ._user_stack :
  {
    . = ALIGN(8);
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCM

  /* Upper bound of the newlib heap: start of the non-cacheable window. */
  _Heap_Limit = __RAM_BEGIN + __RAM_SIZE;

  /* ---- placement invariants ------------------------------------------------
     These turn a class of bugs that only shows up on the bench ("the board
     goes dead as soon as anything touches the external flash") into a link
     error.  They exist because exactly that happened: the NOR driver's
     .ramfunc section was an orphan, the linker put it in the XiP flash window
     like any other code, and it then switched that window off underneath
     itself.

       * the RAM-mode driver must be ITCM-resident (CPU-local, always
         executable, never cached);
       * its scratch buffer must sit in the non-cacheable DMA window, because
         the XSPI controller reads it as a bus master and would otherwise see
         stale cached data. */
  ASSERT(_sramfunc >= 0x00000000 && _eramfunc <= 0x00010000,
         "BUG: .ramfunc must be linked into ITCM (0x00000000..0x0000FFFF)")
  ASSERT(_eramfunc > _sramfunc,
         "BUG: .ramfunc came out empty - the NOR RAM-mode driver is missing")
  ASSERT(EXT_NOR_SCRATCH >= __NONCACHEABLEBUFFER_BEGIN &&
         (EXT_NOR_SCRATCH + 2048) <= __NONCACHEABLEBUFFER_END,
         "BUG: the NOR scratch buffer must be inside the non-cacheable window")

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
