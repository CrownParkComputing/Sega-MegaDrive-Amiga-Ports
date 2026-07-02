/*
 * md_m68k_interface.c - Musashi memory callbacks -> md_machine bus.
 */

#include "hal/md_machine.h"

unsigned int m68k_read_memory_8(unsigned int address)
{
    return md_read8(address);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    return md_read16(address);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    unsigned int hi = md_read16(address);
    return (hi << 16) | md_read16(address + 2);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    md_write8(address, (uint8_t)value);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    md_write16(address, (uint16_t)value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    md_write16(address, (uint16_t)(value >> 16));
    md_write16(address + 2, (uint16_t)value);
}

/* disassembler callbacks (m68kdasm.c) */
unsigned int m68k_read_disassembler_8(unsigned int address)  { return md_read8(address); }
unsigned int m68k_read_disassembler_16(unsigned int address) { return md_read16(address); }
unsigned int m68k_read_disassembler_32(unsigned int address)
{
    unsigned int hi = md_read16(address);
    return (hi << 16) | md_read16(address + 2);
}
