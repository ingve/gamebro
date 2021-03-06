#include "cpu.hpp"

#include "machine.hpp"
#include <cassert>
#include <cstring>
#include "instructions.cpp"

namespace gbc
{
  CPU::CPU(Memory& mem) noexcept
    : m_memory(mem), m_machine(mem.machine())
  {
    this->reset();
  }

  void CPU::reset() noexcept
  {
    // gameboy Z80 initial register values
    registers().af = 0x01b0;
    registers().bc = 0x0013;
    registers().de = 0x00d8;
    registers().hl = 0x014d;
    registers().sp = 0xfffe;
    registers().pc = 0x0100;
    this->m_cycles_total = 0;
  }

  void CPU::simulate()
  {
    // 1. read instruction from memory
    this->m_cur_opcode = this->readop8(0);
    // 2. execute instruction
    unsigned time = this->execute(this->m_cur_opcode);
    // 3. pass the time (in T-states)
    this->incr_cycles(time);
    // 4. handle interrupts
    this->handle_interrupts();
  }

  unsigned CPU::execute(const uint8_t opcode)
  {
    if (this->m_singlestep || this->m_break) {
      // pause for each instruction
      this->print_and_pause(*this, opcode);
      this->m_break = false;
    }
    else {
      // look for breakpoints
      auto it = m_breakpoints.find(registers().pc);
      if (it != m_breakpoints.end()) {
        /*unsigned ret =*/ it->second(*this, opcode);
      }
    }
    // decode into executable instruction
    auto& instr = decode(opcode);
    // print the instruction (when enabled)
    char prn[128];
    instr.printer(prn, sizeof(prn), *this, opcode);
    printf("%9lu: [pc 0x%04x] opcode 0x%02x: %s\n",
            gettime(), registers().pc,  opcode, prn);
    // increment program counter
    registers().pc += 1;
    // run instruction handler
    unsigned ret = instr.handler(*this, opcode);
    // print out the resulting flags reg
    if (m_last_flags != registers().flags)
    {
      m_last_flags = registers().flags;
      char fbuf[5];
      printf("* Flags changed: [%s]\n",
              cstr_flags(fbuf, registers().flags));
    }
    // return cycles used
    return ret;
  }

  // it takes 2 instruction-cycles to toggle interrupts
  void CPU::enable_interrupts() noexcept {
    m_intr_enable_pending = 2;
  }
  void CPU::disable_interrupts() noexcept {
    m_intr_disable_pending = 2;
  }

  void CPU::handle_interrupts()
  {
    // enable/disable interrupts over cycles
    if (m_intr_enable_pending > 0) {
      m_intr_enable_pending--;
      if (!m_intr_enable_pending) m_intr_master_enable = true;
    }
    if (m_intr_disable_pending > 0) {
      m_intr_disable_pending--;
      if (!m_intr_disable_pending) m_intr_master_enable = false;
    }
    // check if interrupts are enabled
    if (this->ime())
    {
      // 5. execute pending interrupts
      const uint8_t imask = machine().io.interrupt_mask();
      if (imask) this->execute_interrupts(imask);
    }

  }
  void CPU::execute_interrupts(const uint8_t imask)
  {
    auto& io = machine().io;
    if (imask &  0x1) io.interrupt(io.vblank);
    if (imask &  0x2) io.interrupt(io.lcd_stat);
    if (imask &  0x4) io.interrupt(io.timer);
    if (imask &  0x8) io.interrupt(io.serial);
    if (imask & 0x10) io.interrupt(io.joypad);
  }

  uint8_t CPU::readop8(const int dx)
  {
    return memory().read8(registers().pc + dx);
  }
  uint16_t CPU::readop16(const int dx)
  {
    return memory().read16(registers().pc + dx);
  }

  instruction_t& CPU::decode(const uint8_t opcode)
  {
    if (opcode == 0) return instr_NOP;
    if (opcode == 0x08) return instr_LD_N_SP;

    if ((opcode & 0xc0) == 0x40) {
      if (opcode == 0x76) return instr_HALT;
      return instr_LD_D_D;
    }
    if ((opcode & 0xcf) == 0x1)  return instr_LD_R_N;
    if ((opcode & 0xe7) == 0x2)  return instr_LD_R_A_R;
    if ((opcode & 0xc7) == 0x3)  return instr_INC_DEC_R;
    if ((opcode & 0xc6) == 0x4)  return instr_INC_DEC_D;
    if ((opcode & 0xe7) == 0x7)  return instr_RLC_RRC;
    if (opcode == 0x10) return instr_STOP;
    if (opcode == 0x18) return instr_JR_N;
    if ((opcode & 0xe7) == 0x20) return instr_JR_N;
    if ((opcode & 0xc7) == 0x6)  return instr_LD_D_N;
    if ((opcode & 0xe7) == 0x22) return instr_LDID_HL_A;
    if ((opcode & 0xf7) == 0x37) return instr_SCF_CCF;
    if ((opcode & 0xc7) == 0xc6) return instr_ALU_A_N_D;
    if ((opcode & 0xc0) == 0x80) return instr_ALU_A_N_D;
    if ((opcode & 0xcb) == 0xc1) return instr_PUSH_POP;
    if ((opcode & 0xe7) == 0xc0) return instr_RET; // cond ret
    if ((opcode & 0xef) == 0xc9) return instr_RET; // ret / reti
    if ((opcode & 0xc7) == 0xc7) return instr_RST;
    if ((opcode & 0xff) == 0xc3) return instr_JP; // direct
    if ((opcode & 0xe7) == 0xc2) return instr_JP; // conditional
    if ((opcode & 0xff) == 0xc4) return instr_CALL; // direct
    if ((opcode & 0xcd) == 0xcd) return instr_CALL; // conditional
    if ((opcode & 0xef) == 0xea) return instr_LD_N_A_N;
    if ((opcode & 0xef) == 0xe0) return instr_LD_xxx_A; // FF00+N
    if ((opcode & 0xef) == 0xe2) return instr_LD_xxx_A; // C
    if ((opcode & 0xef) == 0xea) return instr_LD_xxx_A; // N
    if ((opcode & 0xf7) == 0xf3) return instr_DI_EI;
    // instruction set extension opcodes
    if (opcode == 0xcb) return instr_CB_EXT;

    return instr_MISSING;
  }

  void CPU::incr_cycles(int count)
  {
    assert(count >= 0);
    this->m_cycles_total += count;
  }

  void CPU::stop()
  {
    this->m_running = false;
  }

  void CPU::wait()
  {
    this->m_waiting = true;
  }

  unsigned CPU::push_and_jump(uint16_t address)
  {
    registers().sp -= 2;
    memory().write16(registers().sp, registers().pc);
    registers().pc = address;
    return 8;
  }

  void CPU::print_and_pause(CPU& cpu, const uint8_t opcode)
  {
    char buffer[512];
    cpu.decode(opcode).printer(buffer, sizeof(buffer), cpu, opcode);
    printf("Breakpoint at [pc 0x%04x] opcode 0x%02x: %s\n",
           cpu.registers().pc, opcode, buffer);
    // CPU registers
    printf("%s\n", cpu.registers().to_string().c_str());
    // I/O interrupt registers
    auto& io = cpu.machine().io;
    printf("\tIF = 0x%02x  IE = 0x%02x  IME 0x%x\n",
           io.read_io(IO::REG_IF), io.read_io(IO::REG_IE), cpu.ime());
    try {
      auto& mem = cpu.memory();
      printf("\t(HL) = 0x%04x  (SP) = 0x%04x  (0xA000) = 0x%04x\n",
            mem.read16(cpu.registers().hl), mem.read16(cpu.registers().sp),
            mem.read16(0xA000));
    } catch (...) {}
    printf("Press any key to continue...\n");
    getchar(); // press any key
  }

}
