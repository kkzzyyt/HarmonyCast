/* Minimal musl ELF entry point for an SDK that lacks RISC-V startup objects. */
.text
.global _start
.type _start, @function
_start:
  mv a0, sp
  andi sp, sp, -16
  tail harmonycast_start
