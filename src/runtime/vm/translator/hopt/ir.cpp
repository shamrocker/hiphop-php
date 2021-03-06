/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "runtime/vm/translator/hopt/ir.h"

#include <string.h>

#include "folly/Format.h"

#include "util/trace.h"
#include "runtime/base/string_data.h"
#include "runtime/vm/runtime.h"
#include "runtime/vm/stats.h"
#include "runtime/vm/translator/hopt/irfactory.h"
#include "runtime/vm/translator/hopt/linearscan.h"
#include "runtime/vm/translator/hopt/cse.h"
#include "runtime/vm/translator/hopt/simplifier.h"

namespace HPHP {
namespace VM {
namespace JIT{

IRInstruction::IRInstruction(IRFactory& factory, const IRInstruction* inst)
  : m_op(inst->m_op)
  , m_id(0)
  , m_numSrcs(inst->m_numSrcs)
  , m_typeParam(inst->m_typeParam)
  , m_srcs(m_numSrcs ? new (factory.arena()) SSATmp*[m_numSrcs] : nullptr)
  , m_dst(NULL)
  , m_asmAddr(NULL)
  , m_label(inst->m_label)
  , m_parent(NULL)
  , m_tca(NULL)
{
  std::copy(inst->m_srcs, inst->m_srcs + inst->m_numSrcs, m_srcs);
}

struct {
  const char* name;
  uint64_t flags;
} OpInfo[] = {
#define OPC(name, flags) { #name, flags },
  IR_OPCODES
#undef OPC
  { 0 }
};

const char* opcodeName(Opcode opcode) { return OpInfo[opcode].name; }

static bool opcodeHasFlags(Opcode opcode, uint64_t flags) {
  return OpInfo[opcode].flags & flags;
}

bool IRInstruction::hasDst() const {
  return opcodeHasFlags(getOpcode(), HasDest);
}

bool IRInstruction::isNative() const {
  return opcodeHasFlags(getOpcode(), CallsNative);
}

bool IRInstruction::producesReference() const {
  return opcodeHasFlags(getOpcode(), ProducesRC);
}

bool IRInstruction::isRematerializable() const {
  return opcodeHasFlags(getOpcode(), Rematerializable);
}

bool IRInstruction::hasMemEffects() const {
  return opcodeHasFlags(getOpcode(), MemEffects) || mayReenterHelper();
}

bool IRInstruction::canCSE() const {
  auto canCSE = opcodeHasFlags(getOpcode(), CanCSE);
  // Make sure that instructions that are CSE'able can't produce a
  // reference count or consume reference counts.
  assert(!canCSE || !producesReference());
  assert(!canCSE || !consumesReferences());
  return canCSE && !mayReenterHelper();
}

bool IRInstruction::consumesReferences() const {
  return opcodeHasFlags(getOpcode(), ConsumesRC);
}

bool IRInstruction::consumesReference(int srcNo) const {
  if (!consumesReferences()) {
    return false;
  }
  // Special case StMem, StMemNT, StProp, and StPropNT.
  // These instructions only consume the value operand.
  if ((m_op == StMem || m_op == StMemNT) && srcNo == 0) {
    // StMem[NT] <pointer>, <value>
    return false;
  }
  if ((m_op == StProp || m_op == StPropNT) && (srcNo == 0 || srcNo == 1)) {
    // StProp[NT] <base>, <offset>, <value>
    return false;
  }
  return true;
}

bool IRInstruction::mayModifyRefs() const {
  Opcode opc = getOpcode();
  // DecRefNZ does not have side effects other than decrementing the ref
  // count. Therefore, its MayModifyRefs should be false.
  if (opc == DecRef) {
    auto type = getSrc(0)->getType();
    if (isControlFlowInstruction() || Type::isString(type)) {
      // If the decref has a target label, then it exits if the destructor
      // has to be called, so it does not have any side effects on the main
      // trace.
      return false;
    }
    if (Type::isBoxed(type)) {
      Type::Tag innerType = Type::getInnerType(type);
      return innerType == Type::Obj || innerType == Type::Arr;
    }
  }
  return opcodeHasFlags(opc, MayModifyRefs) || mayReenterHelper();
}

bool IRInstruction::mayRaiseError() const {
  return opcodeHasFlags(getOpcode(), MayRaiseError) || mayReenterHelper();
}

bool IRInstruction::isEssential() const {
  Opcode opc = getOpcode();
  if (opc == DecRefNZ) {
    // If the source of a DecRefNZ is not an IncRef, mark it as essential
    // because we won't remove its source as well as itself.
    // If the ref count optimization is turned off, mark all DecRefNZ as
    // essential.
    if (!RuntimeOption::EvalHHIREnableRefCountOpt ||
        getSrc(0)->getInstruction()->getOpcode() != IncRef) {
      return true;
    }
  }
  if (isControlFlowInstruction() && opc != LdCls) {
    return true;
  }
  return opcodeHasFlags(opc, Essential) || mayReenterHelper();
}

bool IRInstruction::mayReenterHelper() const {
  if (isCmpOp(getOpcode())) {
    return cmpOpTypesMayReenter(getOpcode(),
                                getSrc(0)->getType(),
                                getSrc(1)->getType());
  }
  // Not necessarily actually false; this is just a helper for other
  // bits.
  return false;
}

Opcode queryNegateTable[] = {
  OpLte,        // OpGt
  OpLt,         // OpGte
  OpGte,        // OpLt
  OpGt,         // OpLte
  OpNeq,        // OpEq
  OpEq,         // OpNeq
  OpNSame,      // OpSame
  OpSame,       // OpNSame
  NInstanceOfD, // InstanceOfD
  InstanceOfD,  // NInstanceOfD
  IsNSet,       // IsSet
  IsNType,      // IsType
  IsSet,        // IsNSet
  IsType        // IsNType
};

Opcode queryCommuteTable[] = {
  OpLt,         // OpGt
  OpLte,        // OpGte
  OpGt,         // OpLt
  OpGte,        // OpLte
  OpEq,         // OpEq
  OpNeq,        // OpNeq
  OpSame,       // OpSame
  OpNSame       // OpNSame
};

const char* Type::Strings[(int)Type::TAG_ENUM_COUNT] = {
    #define IRT(type, name)  name,
    IR_TYPES
    #undef IRT
};

// Objects compared with strings may involve calling a user-defined
// __toString function.
bool cmpOpTypesMayReenter(Opcode op, Type::Tag t0, Type::Tag t1) {
  if (op == OpNSame || op == OpSame) return false;
  assert(t0 != Type::Gen && t1 != Type::Gen);
  return (t0 == Type::Cell || t1 == Type::Cell) ||
    ((t0 == Type::Obj || t1 == Type::Obj) &&
     (Type::isString(t0) || Type::isString(t1)));
}

TraceExitType::ExitType getExitType(Opcode opc) {
  assert(opc >= ExitTrace && opc <= ExitGuardFailure);
  return (TraceExitType::ExitType)(opc - ExitTrace);
}

Opcode getExitOpcode(TraceExitType::ExitType type) {
  return (Opcode)(ExitTrace + type);
}

bool isRefCounted(SSATmp* tmp) {
  if (!Type::isRefCounted(tmp->getType())) {
    return false;
  }
  IRInstruction* inst = tmp->getInstruction();
  Opcode opc = inst->getOpcode();
  if (opc == DefConst || opc == LdConst || opc == LdClsCns) {
    return false;
  }
  return true;
}

IRInstruction* IRInstruction::clone(IRFactory* factory) const {
  return factory->cloneInstruction(this);
}

IRInstruction* ConstInstruction::clone(IRFactory* factory) const {
  return factory->cloneInstruction(this);
}

IRInstruction* LabelInstruction::clone(IRFactory* factory) const {
  return factory->cloneInstruction(this);
}

IRInstruction* MarkerInstruction::clone(IRFactory* factory) const {
  return factory->cloneInstruction(this);
}

SSATmp* IRInstruction::getSrc(uint32 i) const {
  if (i >= getNumSrcs()) return nullptr;
  return m_srcs[i];
}

void IRInstruction::setSrc(uint32 i, SSATmp* newSrc) {
  assert(i < getNumSrcs());
  m_srcs[i] = newSrc;
}

void IRInstruction::appendSrc(IRFactory& factory, SSATmp* newSrc) {
  auto newSrcs = new (factory.arena()) SSATmp*[getNumSrcs() + 1];
  std::copy(m_srcs, m_srcs + getNumSrcs(), newSrcs);
  newSrcs[getNumSrcs()] = newSrc;
  ++m_numSrcs;
  m_srcs = newSrcs;
}

bool IRInstruction::equals(IRInstruction* inst) const {
  if (m_op != inst->m_op ||
      m_typeParam != inst->m_typeParam ||
      m_numSrcs != inst->m_numSrcs) {
    return false;
  }
  for (uint32 i = 0; i < getNumSrcs(); i++) {
    if (getSrc(i) != inst->getSrc(i)) {
      return false;
    }
  }
  // TODO: check label for ControlFlowInstructions?
  return true;
}

size_t IRInstruction::hash() const {
  size_t srcHash = 0;
  for (unsigned i = 0; i < getNumSrcs(); ++i) {
    srcHash = CSEHash::hashCombine(srcHash, getSrc(i));
  }
  return CSEHash::hashCombine(srcHash, m_op, m_typeParam);
}

void IRInstruction::printOpcode(std::ostream& ostream) {
  ostream << opcodeName(m_op);
  if (m_typeParam != Type::None) {
    ostream << '<' << Type::Strings[m_typeParam] << '>';
  }
}

void IRInstruction::printDst(std::ostream& ostream) {
  if (m_dst) {
    m_dst->print(ostream, true);
    ostream << " = ";
  }
}

void IRInstruction::printSrc(std::ostream& ostream, uint32 i) {
  SSATmp* src = getSrc(i);
  if (src != NULL) {
    if (m_id != 0 && !src->isConst() && src->getLastUseId() == m_id) {
      ostream << "~";
    }
    src->print(ostream);
  } else {
    ostream << "!!!NULL @ " << i;
  }
}

void IRInstruction::printSrcs(std::ostream& ostream) {
  bool first = true;
  if (getOpcode() == IncStat) {
    ostream << " " << Stats::g_counterNames[getSrc(0)->getConstValAsInt()] <<
               ", " << getSrc(1)->getConstValAsInt();
    return;
  }
  for (uint32 i = 0; i < m_numSrcs; i++) {
    if (!first) {
      ostream << ", ";
    } else {
      ostream << " ";
      first = false;
    }
    printSrc(ostream, i);
  }
}

void IRInstruction::print(std::ostream& ostream) {
  ostream << folly::format("({:02d}) ", m_id);
  printDst(ostream);

  bool isStMem = m_op == StMem || m_op == StMemNT || m_op == StRaw;
  bool isLdMem = m_op == LdMemNR || m_op == LdRaw;
  if (isStMem || m_op == StLoc || isLdMem) {
    ostream << opcodeName(m_op) << " [";
    printSrc(ostream, 0);
    SSATmp* offset = getSrc(1);
    if ((isStMem || isLdMem) &&
        (!offset->isConst() || offset->getConstValAsInt() != 0)) {
      ostream << " + ";
      printSrc(ostream, 1);
    }
    Type::Tag type = isStMem ? getSrc(2)->getType() : m_typeParam;
    ostream << "]:" << Type::Strings[type];
    if (!isLdMem) {
      assert(getNumSrcs() > 1);
      ostream << ", ";
      printSrc(ostream, isStMem ? 2 : 1);
    }
  } else {
    printOpcode(ostream);
    printSrcs(ostream);
  }

  if (m_label) {
    ostream << ", ";
    m_label->print(ostream);
  }
  if (m_tca) {
    ostream << ", ";
    if (m_tca == kIRDirectJccJmpActive) {
      ostream << "JccJmp_Exit ";
    }
    else
    if (m_tca == kIRDirectJccActive) {
      ostream << "Jcc_Exit ";
    }
    else
    if (m_tca == kIRDirectGuardActive) {
      ostream << "Guard_Exit ";
    }
    else {
      ostream << (void*)m_tca;
    }
  }
}

void IRInstruction::print() {
  print(std::cerr);
  std::cerr << std::endl;
}

void ConstInstruction::printConst(std::ostream& ostream) const {
  switch (getTypeParam()) {
    case Type::Int:
      ostream << m_intVal;
      break;
    case Type::Dbl:
      ostream << m_dblVal;
      break;
    case Type::Bool:
      ostream << (m_boolVal ? "true" : "false");
      break;
    case Type::Str:
    case Type::StaticStr:
      ostream << "\""
              << Util::escapeStringForCPP(m_strVal->data(), m_strVal->size())
              << "\"";
      break;
    case Type::Arr:
    {
      if (isEmptyArray()) {
        ostream << "array()";
      } else {
        ostream << "Array(" << (void*)m_arrVal << ")";
      }
      break;
    }
    case Type::Home:
      m_local.print(ostream);
      break;
    case Type::Null:
      ostream << "Null";
      break;
    case Type::Uninit:
      ostream << "Unin";
      break;
    case Type::FuncPtr:
      ostream << "Func(" << (m_func ? m_func->fullName()->data() : "0") << ")";
      break;
    case Type::ClassPtr:
      ostream << "Class(" << (m_clss ? m_clss->name()->data() : "0") << ")";
      break;
    case Type::FuncClassPtr:
      assert(false /* ConstInstruction does not hold both func* and class* */);
      break;
    case Type::None:
      ostream << "None:" << m_intVal;
      break;
    default:
      not_reached();
  }
}

bool ConstInstruction::equals(IRInstruction* inst) const {
  if (!this->IRInstruction::equals(inst)) {
    return false;
  }
  return m_intVal == ((ConstInstruction*)inst)->m_intVal;
}

size_t ConstInstruction::hash() const {
  return CSEHash::hashCombine(IRInstruction::hash(), m_intVal);
}

void ConstInstruction::print(std::ostream& ostream) {
  this->IRInstruction::print(ostream);
  ostream << " ";
  printConst(ostream);
}

bool LabelInstruction::equals(IRInstruction* inst) const {
  assert(0);
  return false;
}

size_t LabelInstruction::hash() const {
  assert(0);
  return 0;
}

bool MarkerInstruction::equals(IRInstruction* inst) const {
  assert(0);
  return false;
}

size_t MarkerInstruction::hash() const {
  assert(0);
  return 0;
}

// Thread chain of patch locations using the 4 byte space in each jmp/jcc
void LabelInstruction::prependPatchAddr(TCA patchAddr) {
  ssize_t diff = getPatchAddr() ? ((TCA)patchAddr - (TCA)getPatchAddr()) : 0;
  assert(deltaFits(diff, sz::dword));
  *(int*)(patchAddr) = (int)diff;
  m_patchAddr = patchAddr;
}

void* LabelInstruction::getPatchAddr() {
  return m_patchAddr;
}

void LabelInstruction::print(std::ostream& ostream) {
  assert(getOpcode() == DefLabel);
  ostream << "L" << m_labelId << ":";
}

void MarkerInstruction::print(std::ostream& ostream) {
  ostream << "--- bc" << m_bcOff <<
             ", spOff: " << m_stackOff;
}

int SSATmp::numNeededRegs() const {
  Type::Tag type = getType();

  // These types don't get a register because their values are static
  if (type == Type::Null || type == Type::Uninit || type == Type::None ||
      type == Type::RetAddr) {
    return 0;
  }

  // Need 2 registers for these types, for type and value, or 1 for
  // Func* and 1 for Class*.
  if (!Type::isStaticallyKnown(type) || type == Type::FuncClassPtr) {
    return 2;
  }

  // Everything else just has 1.
  return 1;
}

int SSATmp::numAllocatedRegs() const {
  // If an SSATmp is spilled, it must've actually had a full set of
  // registers allocated to it.
  if (m_isSpilled) return numNeededRegs();

  // Return the number of register slots that actually have an
  // allocated register.  We may not have allocated a full
  // numNeededRegs() worth of registers in some cases (if the value
  // of this tmp wasn't used, etc).
  int i = 0;
  while (i < kMaxNumRegs && m_regs[i] != InvalidReg) {
    ++i;
  }
  return i;
}


bool SSATmp::getConstValAsBool() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsBool();
}
int64 SSATmp::getConstValAsInt() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsInt();
}
int64 SSATmp::getConstValAsRawInt() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsRawInt();
}
double SSATmp::getConstValAsDbl() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsDbl();
}
const StringData* SSATmp::getConstValAsStr() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsStr();
}
const ArrayData* SSATmp::getConstValAsArr() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsArr();
}
const Func* SSATmp::getConstValAsFunc() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsFunc();
}
const Class* SSATmp::getConstValAsClass() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsClass();
}
uintptr_t SSATmp::getConstValAsBits() const {
  assert(isConst());
  return ((ConstInstruction*)m_inst)->getValAsBits();
}

void SSATmp::setTCA(TCA tca) {
  getInstruction()->setTCA(tca);
}
TCA SSATmp::getTCA() const {
  return getInstruction()->getTCA();
}

void SSATmp::print(std::ostream& os, bool printLastUse) {
  if (m_inst->isDefConst()) {
    ((ConstInstruction*)m_inst)->printConst(os);
    return;
  }
  os << "t" << m_id;
  if (printLastUse && m_lastUseId != 0) {
    os << "@" << m_lastUseId << "#" << m_useCount;
  }
  if (m_isSpilled || numAllocatedRegs() > 0) {
    os << '(';
    if (!m_isSpilled) {
      for (int i = 0, sz = numAllocatedRegs(); i < sz; ++i) {
        if (i != 0) os << ", ";
        os << reg::regname(Reg64(int(m_regs[i])));
      }
    } else {
      for (int i = 0, sz = numNeededRegs(); i < sz; ++i) {
        if (i != 0) os << ", ";
        os << m_spillInfo[i];
      }
    }
    os << ')';
  }
  os << ":" << Type::Strings[getType()];
}

void SSATmp::print() {
  print(std::cerr);
  std::cerr << std::endl;
}

#ifdef DEBUG

extern "C" {
#include "../tools/xed2-intel64/include/xed-interface.h"
}

static void error(std::string msg) {
  fprintf(stderr, "Error: %s\n", msg.c_str());
  exit(1);
}


#define MAX_INSTR_ASM_LEN 128
xed_state_t xed_state;

static const xed_syntax_enum_t s_xed_syntax =
  getenv("HHVM_ATT_DISAS") ? XED_SYNTAX_ATT : XED_SYNTAX_INTEL;

void printInstructions(xed_uint8_t* codeStartAddr,
                       xed_uint8_t* codeEndAddr,
                       bool printAddr,
                       int indentLevel,
                       std::ostream& os) {
  char codeStr[MAX_INSTR_ASM_LEN];
  xed_uint8_t *frontier;
  xed_decoded_inst_t xedd;
  uint64 ip;

  // Decode and print each instruction
  for (frontier = codeStartAddr, ip = (uint64)codeStartAddr;
       frontier < codeEndAddr;
      ) {
    xed_decoded_inst_zero_set_mode(&xedd, &xed_state);
    xed_decoded_inst_set_input_chip(&xedd, XED_CHIP_INVALID);
    xed_error_enum_t xed_error = xed_decode(&xedd, frontier, 15);
    if (xed_error != XED_ERROR_NONE) error("disasm error: xed_decode failed");

    // Get disassembled instruction in codeStr
    if (!xed_format_context(s_xed_syntax, &xedd, codeStr,
                            MAX_INSTR_ASM_LEN, ip, NULL)) {
      error("disasm error: xed_format_context failed");
    }

    for (int i = 0; i < indentLevel; ++i) {
      os << ' ';
    }
    if (printAddr) {
      os << folly::format("{:#10x}: ", ip);
    }
    uint32 instrLen = xed_decoded_inst_get_length(&xedd);
    if (RuntimeOption::EvalDumpIR > 5) {
      // print encoding, like in objdump
      unsigned posi = 0;
      for (; posi < instrLen; ++posi) {
        os << folly::format("{:02x} ", (uint8_t)frontier[posi]);
      }
      for (; posi < 16; ++posi) {
        os << "   ";
      }
    }
    os << codeStr << "\n";;
    frontier += instrLen;
    ip       += instrLen;


  }
}
#endif

void Trace::print(std::ostream& os, bool printAsm,
                  bool isExit /* = false */) {
#ifdef DEBUG
  xed_state_init(&xed_state, XED_MACHINE_MODE_LONG_64,
                 XED_ADDRESS_WIDTH_64b, XED_ADDRESS_WIDTH_64b);
  xed_tables_init();
#endif

  auto it = begin(m_instructionList);
  while (it != end(m_instructionList)) {
    auto* inst = *it;
    ++it;
    if (inst->getOpcode() == Marker) {
      MarkerInstruction* markerInst = (MarkerInstruction*)inst;
      if (isExit) {
        // Don't print bytecode, but print the label.
        os << std::string(6, ' ');
        inst->print(os);
        os << '\n';
        continue;
      }
      uint32 bcOffset = markerInst->getBcOff();
      if (const auto* func = markerInst->getFunc()) {
        func->unit()->prettyPrint(
          os, Unit::PrintOpts()
                .range(bcOffset, bcOffset+1)
                .noLineNumbers());
        continue;
      }
    }
    if (inst->getOpcode() == DefLabel) {
      os << std::string(6, ' ');
    } else {
      os << std::string(8, ' ');
    }
    inst->print(os);
    os << '\n';
    if (!printAsm) {
      continue;
    }
    uint8* asmAddr = (uint8*)inst->getAsmAddr();
    if (asmAddr == NULL) {
      continue;
    }
    // Find the next instruction that has an non-NULL asm address.
    IRInstruction::Iterator nextHasAsmAddr = it;
    while (nextHasAsmAddr != m_instructionList.end() &&
           (*nextHasAsmAddr)->getAsmAddr() == NULL) {
      ++nextHasAsmAddr;
    }
    uint8* endAsm;
    if (nextHasAsmAddr != m_instructionList.end()) {
      endAsm = (uint8*)(*nextHasAsmAddr)->getAsmAddr();
    } else {
      endAsm = m_lastAsmAddress;
    }
    if (asmAddr != endAsm) {
      // print out the assembly
      os << '\n';
#ifdef DEBUG
      printInstructions(asmAddr, endAsm, true, 14, os);
      os << '\n';
#endif
    }
  }

  bool firstExitTracePrinted = false;
  for (List::iterator it = m_exitTraces.begin();
       it != m_exitTraces.end();
       it++) {
    Trace* exitTrace = *it;
    if (!firstExitTracePrinted) {
      firstExitTracePrinted = true;
      // print out any extra code in astubs
      if (m_firstAstubsAddress < exitTrace->m_firstAsmAddress) {
#ifdef DEBUG
        os << std::string(8, ' ') << "AStubs:\n";
        printInstructions(m_firstAstubsAddress,
                          exitTrace->m_firstAsmAddress,
                          true,
                          14,
                          os);
#endif
        os << '\n';
      }

    }
    os << "\n      -------  Exit Trace  -------\n";
    exitTrace->print(os, printAsm, true);
  }
}

void Trace::print() {
  print(std::cout, true /* printAsm */);
}

void resetIdsAux(Trace* trace) {
  IRInstruction::Iterator it;
  IRInstruction::List instructionList = trace->getInstructionList();
  for (it = instructionList.begin();
       it != instructionList.end();
       it++) {
    IRInstruction* inst = *it;
    inst->setId(0);
    SSATmp* dst = inst->getDst();
    if (dst) {
      dst->setLastUseId(0);
      dst->setUseCount(0);
      dst->setSpillSlot(-1);
    }
  }
}

/*
 * Clears the IRInstructions' ids, and the SSATmps' use count and last use id
 * for the given trace and all its exit traces.
 */
void resetIds(Trace* trace) {
  resetIdsAux(trace);
  Trace::List& exitTraces = trace->getExitTraces();
  for (Trace::Iterator it = exitTraces.begin();
       it != exitTraces.end();
       it++) {
    resetIdsAux(*it);
  }
}

uint32 numberInstructions(Trace* trace,
                          uint32 nextId,
                          bool followControlFlow) {
  for (auto* inst : trace->getInstructionList()) {
    if (SSATmp* dst = inst->getDst()) {
      // Initialize this value for register spilling.
      dst->setSpillSlot(-1);
    }
    if (inst->getOpcode() == Marker) {
      continue; // don't number markers
    }
    uint32 id = nextId++;
    inst->setId(id);
    for (uint32 i=0; i < inst->getNumSrcs(); i++) {
      SSATmp* tmp = inst->getSrc(i);
      tmp->setLastUseId(id);
      tmp->incUseCount();
    }
    // This eagerly follows control flow edges to exit traces, so
    // if no more than 1 instruction should branch to a exit trace's label
    // otherwise that exit trace will be incorrectly processed more than once.
    // The net result is that the exit trace's instruction ids will be greater
    // than the ids of the instruction that can branch to it, which
    // is exactly the invariant we want for linear scan register allocation
    // and other analyses.
    if (followControlFlow && inst->isControlFlowInstruction()) {
      LabelInstruction* label = inst->getLabel();
      if (label) {
        nextId = numberInstructions(label->getParent(), nextId);
      }
    }
  }
  return nextId;
}

/*
 * Returns true if a label is unreachable -- that is, if a label's id is 0
 * because numbering never visited it.
 */
static bool labelIsUnreachable(const Trace* trace) {
  return trace->getLabel()->getId() == 0;
}

/*
 * Assigns ids to instructions, sets the use count and last use id for the
 * SSA tmps, and removes unreachable exit traces. This sets us up for
 * linear scan register allocation.
 */
void numberInstructions(Trace* trace) {
  resetIds(trace);
  numberInstructions(trace, 1, true);
  // any exit trace with a label whose id is 0 is unreachable and
  // can be removed
  trace->getExitTraces().remove_if(labelIsUnreachable);
}

}}}

