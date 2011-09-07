// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_VERIFY_H_
#define ART_SRC_DEX_VERIFY_H_

#include "dex_file.h"
#include "dex_instruction.h"
#include "macros.h"
#include "object.h"

namespace art {

#define kMaxMonitorStackDepth   (sizeof(MonitorEntries) * 8)

/*
 * Set this to enable dead code scanning.  This is not required, but it's
 * very useful when testing changes to the verifier (to make sure we're not
 * skipping over stuff). The only reason not to do it is that it slightly
 * increases the time required to perform verification.
 */
#ifndef NDEBUG
# define DEAD_CODE_SCAN  true
#else
# define DEAD_CODE_SCAN  false
#endif

/*
 * We need an extra "pseudo register" to hold the return type briefly.  It
 * can be category 1 or 2, so we need two slots.
 */
#define kExtraRegs  2
#define RESULT_REGISTER(_insnRegCount)  (_insnRegCount)

class DexVerifier {
 public:
  /*
   * RegType holds information about the type of data held in a register.
   * For most types it's a simple enum.  For reference types it holds a
   * pointer to the ClassObject, and for uninitialized references it holds
   * an index into the UninitInstanceMap.
   */
  typedef uint32_t RegType;

  /*
   * A bit vector indicating which entries in the monitor stack are
   * associated with this register.  The low bit corresponds to the stack's
   * bottom-most entry.
   */
  typedef uint32_t MonitorEntries;

  /*
   * InsnFlags is a 32-bit integer with the following layout:
   *   0-15  instruction length (or 0 if this address doesn't hold an opcode)
   *  16-31  single bit flags:
   *    InTry: in "try" block; exceptions thrown here may be caught locally
   *    BranchTarget: other instructions can branch to this instruction
   *    GcPoint: this instruction is a GC safe point
   *    Visited: verifier has examined this instruction at least once
   *    Changed: set/cleared as bytecode verifier runs
   */
  typedef uint32_t InsnFlags;

  enum InsnFlag {
    kInsnFlagWidthMask    = 0x0000ffff,
    kInsnFlagInTry        = (1 << 16),
    kInsnFlagBranchTarget = (1 << 17),
    kInsnFlagGcPoint      = (1 << 18),
    kInsnFlagVisited      = (1 << 30),
    kInsnFlagChanged      = (1 << 31),
  };

  /*
   * "Direct" and "virtual" methods are stored independently.  The type of call
   * used to invoke the method determines which list we search, and whether
   * we travel up into superclasses.
   *
   * (<clinit>, <init>, and methods declared "private" or "static" are stored
   * in the "direct" list.  All others are stored in the "virtual" list.)
   */
  enum MethodType {
    METHOD_UNKNOWN  = 0,
    METHOD_DIRECT,      // <init>, private
    METHOD_STATIC,      // static
    METHOD_VIRTUAL,     // virtual, super
    METHOD_INTERFACE    // interface
  };

  /*
   * We don't need to store the register data for many instructions, because
   * we either only need it at branch points (for verification) or GC points
   * and branches (for verification + type-precise register analysis).
   */
  enum RegisterTrackingMode {
    kTrackRegsBranches,
    kTrackRegsGcPoints,
    kTrackRegsAll,
  };

  /*
   * Enumeration for register type values.  The "hi" piece of a 64-bit value
   * MUST immediately follow the "lo" piece in the enumeration, so we can check
   * that hi==lo+1.
   *
   * Assignment of constants:
   *   [-MAXINT,-32768)   : integer
   *   [-32768,-128)      : short
   *   [-128,0)           : byte
   *   0                  : zero
   *   1                  : one
   *   [2,128)            : posbyte
   *   [128,32768)        : posshort
   *   [32768,65536)      : char
   *   [65536,MAXINT]     : integer
   *
   * Allowed "implicit" widening conversions:
   *   zero -> boolean, posbyte, byte, posshort, short, char, integer, ref (null)
   *   one -> boolean, posbyte, byte, posshort, short, char, integer
   *   boolean -> posbyte, byte, posshort, short, char, integer
   *   posbyte -> posshort, short, integer, char
   *   byte -> short, integer
   *   posshort -> integer, char
   *   short -> integer
   *   char -> integer
   *
   * In addition, all of the above can convert to "float".
   *
   * We're more careful with integer values than the spec requires.  The
   * motivation is to restrict byte/char/short to the correct range of values.
   * For example, if a method takes a byte argument, we don't want to allow
   * the code to load the constant "1024" and pass it in.
   */
  enum {
    kRegTypeUnknown = 0,    /* initial state; use value=0 so calloc works */
    kRegTypeUninit = 1,     /* MUST be odd to distinguish from pointer */
    kRegTypeConflict,       /* merge clash makes this reg's type unknowable */

    /*
     * Category-1nr types.  The order of these is chiseled into a couple
     * of tables, so don't add, remove, or reorder if you can avoid it.
     */
#define kRegType1nrSTART    kRegTypeZero
    kRegTypeZero,           /* 32-bit 0, could be Boolean, Int, Float, or Ref */
    kRegTypeOne,            /* 32-bit 1, could be Boolean, Int, Float */
    kRegTypeBoolean,        /* must be 0 or 1 */
    kRegTypeConstPosByte,   /* const derived byte, known positive */
    kRegTypeConstByte,      /* const derived byte */
    kRegTypeConstPosShort,  /* const derived short, known positive */
    kRegTypeConstShort,     /* const derived short */
    kRegTypeConstChar,      /* const derived char */
    kRegTypeConstInteger,   /* const derived integer */
    kRegTypePosByte,        /* byte, known positive (can become char) */
    kRegTypeByte,
    kRegTypePosShort,       /* short, known positive (can become char) */
    kRegTypeShort,
    kRegTypeChar,
    kRegTypeInteger,
    kRegTypeFloat,
#define kRegType1nrEND      kRegTypeFloat
    kRegTypeConstLo,        /* const derived wide, lower half */
    kRegTypeConstHi,        /* const derived wide, upper half */
    kRegTypeLongLo,         /* lower-numbered register; endian-independent */
    kRegTypeLongHi,
    kRegTypeDoubleLo,
    kRegTypeDoubleHi,

    /*
     * Enumeration max; this is used with "full" (32-bit) RegType values.
     *
     * Anything larger than this is a ClassObject or uninit ref.  Mask off
     * all but the low 8 bits; if you're left with kRegTypeUninit, pull
     * the uninit index out of the high 24.  Because kRegTypeUninit has an
     * odd value, there is no risk of a particular ClassObject pointer bit
     * pattern being confused for it (assuming our class object allocator
     * uses word alignment).
     */
    kRegTypeMAX
  };
#define kRegTypeUninitMask  0xff
#define kRegTypeUninitShift 8

  /*
   * Register type categories, for type checking.
   *
   * The spec says category 1 includes boolean, byte, char, short, int, float,
   * reference, and returnAddress.  Category 2 includes long and double.
   *
   * We treat object references separately, so we have "category1nr".  We
   * don't support jsr/ret, so there is no "returnAddress" type.
   */
  enum TypeCategory {
    kTypeCategoryUnknown = 0,
    kTypeCategory1nr = 1,         // boolean, byte, char, short, int, float
    kTypeCategory2 = 2,           // long, double
    kTypeCategoryRef = 3,         // object reference
  };

  /* An enumeration of problems that can turn up during verification. */
  enum VerifyError {
    VERIFY_ERROR_NONE = 0,      /* no error; must be zero */
    VERIFY_ERROR_GENERIC,       /* VerifyError */

    VERIFY_ERROR_NO_CLASS,      /* NoClassDefFoundError */
    VERIFY_ERROR_NO_FIELD,      /* NoSuchFieldError */
    VERIFY_ERROR_NO_METHOD,     /* NoSuchMethodError */
    VERIFY_ERROR_ACCESS_CLASS,  /* IllegalAccessError */
    VERIFY_ERROR_ACCESS_FIELD,  /* IllegalAccessError */
    VERIFY_ERROR_ACCESS_METHOD, /* IllegalAccessError */
    VERIFY_ERROR_CLASS_CHANGE,  /* IncompatibleClassChangeError */
    VERIFY_ERROR_INSTANTIATION, /* InstantiationError */
  };

  /*
   * Identifies the type of reference in the instruction that generated the
   * verify error (e.g. VERIFY_ERROR_ACCESS_CLASS could come from a method,
   * field, or class reference).
   *
   * This must fit in two bits.
   */
  enum VerifyErrorRefType {
    VERIFY_ERROR_REF_CLASS  = 0,
    VERIFY_ERROR_REF_FIELD  = 1,
    VERIFY_ERROR_REF_METHOD = 2,
  };
#define kVerifyErrorRefTypeShift 6

  /*
   * During verification, we associate one of these with every "interesting"
   * instruction.  We track the status of all registers, and (if the method
   * has any monitor-enter instructions) maintain a stack of entered monitors
   * (identified by code unit offset).
   *
   * If live-precise register maps are enabled, the "liveRegs" vector will
   * be populated.  Unlike the other lists of registers here, we do not
   * track the liveness of the method result register (which is not visible
   * to the GC).
   */
  struct RegisterLine {
    RegType*        reg_types_;
    MonitorEntries* monitor_entries_;
    uint32_t*       monitor_stack_;
    uint32_t        monitor_stack_top_;

    /* Default constructor. */
    RegisterLine() {
      reg_types_ = NULL;
      monitor_entries_ = NULL;
      monitor_stack_ = NULL;
      monitor_stack_top_ = 0;
    }

    /* Default destructor. */
    ~RegisterLine() {
      delete reg_types_;
      delete monitor_entries_;
      delete monitor_stack_;
    }

    /* Allocate space for the fields. */
    void Alloc(size_t size, bool track_monitors) {
      reg_types_ = new RegType[size]();
      if (track_monitors) {
        monitor_entries_ = new MonitorEntries[size];
        monitor_stack_ = new uint32_t[kMaxMonitorStackDepth];
      }
    }
  };

  /* Big fat collection of register data. */
  struct RegisterTable {
    /*
     * Array of RegisterLine structs, one per address in the method.  We only
     * set the pointers for certain addresses, based on instruction widths
     * and what we're trying to accomplish.
     */
    RegisterLine* register_lines_;

    /*
     * Number of registers we track for each instruction.  This is equal
     * to the method's declared "registersSize" plus kExtraRegs (2).
     */
    size_t      insn_reg_count_plus_;

    /* Storage for a register line we're currently working on. */
    RegisterLine work_line_;

    /* Storage for a register line we're saving for later. */
    RegisterLine saved_line_;

    /* Default constructor. */
    RegisterTable() {
      register_lines_ = NULL;
      insn_reg_count_plus_ = 0;
    }

    /* Default destructor. */
    ~RegisterTable() {
      delete [] register_lines_;
    }
  };

  /* Entries in the UninitInstanceMap. */
  struct UninitInstanceMapEntry {
    /* Code offset, or -1 for method arg ("this"). */
    int addr_;

    /* Class created at this address. */
    Class* klass_;
  };

  /*
   * Table that maps uninitialized instances to classes, based on the
   * address of the new-instance instruction.  One per method.
   */
  struct UninitInstanceMap {
    int num_entries_;
    UninitInstanceMapEntry* map_;

    /* Basic constructor */
    UninitInstanceMap(int num_entries) {
      num_entries_ = num_entries;
      map_ = new UninitInstanceMapEntry[num_entries]();
    }

    /* Default destructor */
    ~UninitInstanceMap() {
      delete map_;
    }
  };
  #define kUninitThisArgAddr  (-1)
  #define kUninitThisArgSlot  0

  /* Various bits of data used by the verifier and register map generator. */
  struct VerifierData {
    /* The method we're working on. */
    Method* method_;

    /* The dex file containing the method. */
    const DexFile* dex_file_;

    /* The code item containing the code for the method. */
    const DexFile::CodeItem* code_item_;

    /* Instruction widths and flags, one entry per code unit. */
    InsnFlags* insn_flags_;

    /*
     * Uninitialized instance map, used for tracking the movement of
     * objects that have been allocated but not initialized.
     */
    UninitInstanceMap* uninit_map_;

    /*
     * Array of RegisterLine structs, one entry per code unit.  We only need
     * entries for code units that hold the start of an "interesting"
     * instruction.  For register map generation, we're only interested
     * in GC points.
     */
    RegisterLine* register_lines_;

    /* The number of occurrences of specific opcodes. */
    size_t new_instance_count_;
    size_t monitor_enter_count_;

    /* Basic constructor. */
    VerifierData(Method* method, const DexFile* dex_file,
        const DexFile::CodeItem* code_item)
        : method_(method), dex_file_(dex_file), code_item_(code_item),
          insn_flags_(NULL), uninit_map_(NULL), register_lines_(NULL),
          new_instance_count_(0), monitor_enter_count_(0) { }
  };

  /*
   * Merge result table for primitive values.  The table is symmetric along
   * the diagonal.
   *
   * Note that 32-bit int/float do not merge into 64-bit long/double.  This
   * is a register merge, not a widening conversion.  Only the "implicit"
   * widening within a category, e.g. byte to short, is allowed.
   *
   * Dalvik does not draw a distinction between int and float, but we enforce
   * that once a value is used as int, it can't be used as float, and vice
   * versa. We do not allow free exchange between 32-bit int/float and 64-bit
   * long/double.
   *
   * Note that Uninit+Uninit=Uninit.  This holds true because we only
   * use this when the RegType value is exactly equal to kRegTypeUninit, which
   * can only happen for the zeroeth entry in the table.
   *
   * "Unknown" never merges with anything known.  The only time a register
   * transitions from "unknown" to "known" is when we're executing code
   * for the first time, and we handle that with a simple copy.
   */
  static const char merge_table_[kRegTypeMAX][kRegTypeMAX];

  /*
   * Returns "true" if the flags indicate that this address holds the start
   * of an instruction.
   */
  static inline bool InsnIsOpcode(const InsnFlags insn_flags[], int addr) {
    return (insn_flags[addr] & kInsnFlagWidthMask) != 0;
  }

  /* Extract the unsigned 16-bit instruction width from "flags". */
  static inline int InsnGetWidth(const InsnFlags insn_flags[], int addr) {
    return insn_flags[addr] & kInsnFlagWidthMask;
  }

  /* Utilities to check and set kInsnFlagChanged. */
  static inline bool InsnIsChanged(const InsnFlags insn_flags[], int addr) {
    return (insn_flags[addr] & kInsnFlagChanged) != 0;
  }
  static inline void InsnSetChanged(InsnFlags insn_flags[], int addr,
      bool changed) {
    if (changed)
      insn_flags[addr] |= kInsnFlagChanged;
    else
      insn_flags[addr] &= ~kInsnFlagChanged;
  }

  /* Utilities to check and set kInsnFlagVisited. */
  static inline bool InsnIsVisited(const InsnFlags insn_flags[], int addr) {
      return (insn_flags[addr] & kInsnFlagVisited) != 0;
  }
  static inline void InsnSetVisited(InsnFlags insn_flags[], int addr,
      bool visited) {
    if (visited)
      insn_flags[addr] |= kInsnFlagVisited;
    else
      insn_flags[addr] &= ~kInsnFlagVisited;
  }

  static inline bool InsnIsVisitedOrChanged(const InsnFlags insn_flags[],
      int addr) {
    return (insn_flags[addr] & (kInsnFlagVisited |
                                kInsnFlagChanged)) != 0;
  }

  /* Utilities to check and set kInsnFlagInTry. */
  static inline bool InsnIsInTry(const InsnFlags insn_flags[], int addr) {
    return (insn_flags[addr] & kInsnFlagInTry) != 0;
  }
  static inline void InsnSetInTry(InsnFlags insn_flags[], int addr) {
    insn_flags[addr] |= kInsnFlagInTry;
  }

  /* Utilities to check and set kInsnFlagBranchTarget. */
  static inline bool InsnIsBranchTarget(const InsnFlags insn_flags[], int addr)
  {
    return (insn_flags[addr] & kInsnFlagBranchTarget) != 0;
  }
  static inline void InsnSetBranchTarget(InsnFlags insn_flags[], int addr) {
    insn_flags[addr] |= kInsnFlagBranchTarget;
  }

  /* Utilities to check and set kInsnFlagGcPoint. */
  static inline bool InsnIsGcPoint(const InsnFlags insn_flags[], int addr) {
    return (insn_flags[addr] & kInsnFlagGcPoint) != 0;
  }
  static inline void InsnSetGcPoint(InsnFlags insn_flags[], int addr) {
    insn_flags[addr] |= kInsnFlagGcPoint;
  }

  /* Get the class object at the specified index. */
  static inline Class* GetUninitInstance(const UninitInstanceMap* uninit_map,
      int idx) {
    assert(idx >= 0 && idx < uninit_map->num_entries_);
    return uninit_map->map_[idx].klass_;
  }

  /* Determine if "type" is actually an object reference (init/uninit/zero) */
  static inline bool RegTypeIsReference(RegType type) {
    return (type > kRegTypeMAX || type == kRegTypeUninit ||
            type == kRegTypeZero);
  }

  /* Determine if "type" is an uninitialized object reference */
  static inline bool RegTypeIsUninitReference(RegType type) {
    return ((type & kRegTypeUninitMask) == kRegTypeUninit);
  }

  /*
   * Convert the initialized reference "type" to a Class pointer
   * (does not expect uninit ref types or "zero").
   */
  static Class* RegTypeInitializedReferenceToClass(RegType type) {
    assert(RegTypeIsReference(type) && type != kRegTypeZero);
    if ((type & 0x01) == 0) {
      return (Class*) type;
    } else {
      LOG(ERROR) << "VFY: attempted to use uninitialized reference";
      return NULL;
    }
  }

  /* Extract the index into the uninitialized instance map table. */
  static inline int RegTypeToUninitIndex(RegType type) {
    assert(RegTypeIsUninitReference(type));
    return (type & ~kRegTypeUninitMask) >> kRegTypeUninitShift;
  }

  /* Convert the reference "type" to a Class pointer. */
  static Class* RegTypeReferenceToClass(RegType type,
      const UninitInstanceMap* uninit_map) {
    assert(RegTypeIsReference(type) && type != kRegTypeZero);
    if (RegTypeIsUninitReference(type)) {
      assert(uninit_map != NULL);
      return GetUninitInstance(uninit_map, RegTypeToUninitIndex(type));
    } else {
        return (Class*) type;
    }
  }

  /* Convert the ClassObject pointer to an (initialized) register type. */
  static inline RegType RegTypeFromClass(Class* klass) {
    return (uint32_t) klass;
  }

  /* Return the RegType for the uninitialized reference in slot "uidx". */
  static inline RegType RegTypeFromUninitIndex(int uidx) {
    return (uint32_t) (kRegTypeUninit | (uidx << kRegTypeUninitShift));
  }

  /* Verify a class. Returns "true" on success. */
  static bool VerifyClass(Class* klass);

 private:
  /*
   * Perform verification on a single method.
   *
   * We do this in three passes:
   *  (1) Walk through all code units, determining instruction locations,
   *      widths, and other characteristics.
   *  (2) Walk through all code units, performing static checks on
   *      operands.
   *  (3) Iterate through the method, checking type safety and looking
   *      for code flow problems.
   *
   * Some checks may be bypassed depending on the verification mode.  We can't
   * turn this stuff off completely if we want to do "exact" GC.
   *
   * Confirmed here:
   * - code array must not be empty
   * Confirmed by ComputeWidthsAndCountOps():
   * - opcode of first instruction begins at index 0
   * - only documented instructions may appear
   * - each instruction follows the last
   * - last byte of last instruction is at (code_length-1)
   */
  static bool VerifyMethod(Method* method);

  /*
   * Perform static verification on all instructions in a method.
   *
   * Walks through instructions in a method calling VerifyInstruction on each.
   */
  static bool VerifyInstructions(VerifierData* vdata);

  /*
   * Perform static verification on an instruction.
   *
   * As a side effect, this sets the "branch target" flags in InsnFlags.
   *
   * "(CF)" items are handled during code-flow analysis.
   *
   * v3 4.10.1
   * - target of each jump and branch instruction must be valid
   * - targets of switch statements must be valid
   * - operands referencing constant pool entries must be valid
   * - (CF) operands of getfield, putfield, getstatic, putstatic must be valid
   * - (CF) operands of method invocation instructions must be valid
   * - (CF) only invoke-direct can call a method starting with '<'
   * - (CF) <clinit> must never be called explicitly
   * - operands of instanceof, checkcast, new (and variants) must be valid
   * - new-array[-type] limited to 255 dimensions
   * - can't use "new" on an array class
   * - (?) limit dimensions in multi-array creation
   * - local variable load/store register values must be in valid range
   *
   * v3 4.11.1.2
   * - branches must be within the bounds of the code array
   * - targets of all control-flow instructions are the start of an instruction
   * - register accesses fall within range of allocated registers
   * - (N/A) access to constant pool must be of appropriate type
   * - code does not end in the middle of an instruction
   * - execution cannot fall off the end of the code
   * - (earlier) for each exception handler, the "try" area must begin and
   *   end at the start of an instruction (end can be at the end of the code)
   * - (earlier) for each exception handler, the handler must start at a valid
   *   instruction
   */
  static bool VerifyInstruction(VerifierData* vdata,
      const Instruction* inst, uint32_t code_offset);

  /* Perform detailed code-flow analysis on a single method. */
  static bool VerifyCodeFlow(VerifierData* vdata);

  /*
   * Compute the width of the instruction at each address in the instruction
   * stream, and store it in vdata->insn_flags.  Addresses that are in the
   * middle of an instruction, or that are part of switch table data, are not
   * touched (so the caller should probably initialize "insn_flags" to zero).
   *
   * The "new_instance_count_" and "monitor_enter_count_" fields in vdata are
   * also set.
   *
   * Performs some static checks, notably:
   * - opcode of first instruction begins at index 0
   * - only documented instructions may appear
   * - each instruction follows the last
   * - last byte of last instruction is at (code_length-1)
   *
   * Logs an error and returns "false" on failure.
   */
  static bool ComputeWidthsAndCountOps(VerifierData* vdata);

  /*
   * Set the "in try" flags for all instructions protected by "try" statements.
   * Also sets the "branch target" flags for exception handlers.
   *
   * Call this after widths have been set in "insn_flags".
   *
   * Returns "false" if something in the exception table looks fishy, but
   * we're expecting the exception table to be somewhat sane.
   */
  static bool ScanTryCatchBlocks(VerifierData* vdata);

  /*
   * Extract the relative offset from a branch instruction.
   *
   * Returns "false" on failure (e.g. this isn't a branch instruction).
   */
  static bool GetBranchOffset(const DexFile::CodeItem* code_item,
      const InsnFlags insn_flags[], uint32_t cur_offset, int32_t* pOffset,
      bool* pConditional, bool* selfOkay);

  /*
   * Verify an array data table.  "cur_offset" is the offset of the
   * fill-array-data instruction.
   */
  static bool CheckArrayData(const DexFile::CodeItem* code_item,
      uint32_t cur_offset);

  /*
   * Perform static checks on a "new-instance" instruction.  Specifically,
   * make sure the class reference isn't for an array class.
   *
   * We don't need the actual class, just a pointer to the class name.
   */
  static bool CheckNewInstance(const DexFile* dex_file, uint32_t idx);

  /*
   * Perform static checks on a "new-array" instruction.  Specifically, make
   * sure they aren't creating an array of arrays that causes the number of
   * dimensions to exceed 255.
   */
  static bool CheckNewArray(const DexFile* dex_file, uint32_t idx);

  /*
   * Perform static checks on an instruction that takes a class constant.
   * Ensure that the class index is in the valid range.
   */
  static bool CheckTypeIndex(const DexFile* dex_file, uint32_t idx);

  /*
   * Perform static checks on a field get or set instruction.  All we do
   * here is ensure that the field index is in the valid range.
   */
  static bool CheckFieldIndex(const DexFile* dex_file, uint32_t idx);

  /*
   * Perform static checks on a method invocation instruction.  All we do
   * here is ensure that the method index is in the valid range.
   */
  static bool CheckMethodIndex(const DexFile* dex_file, uint32_t idx);

  /* Ensure that the string index is in the valid range. */
  static bool CheckStringIndex(const DexFile* dex_file, uint32_t idx);

  /* Ensure that the register index is valid for this code item. */
  static bool CheckRegisterIndex(const DexFile::CodeItem* code_item,
      uint32_t idx);

  /* Ensure that the wide register index is valid for this code item. */
  static bool CheckWideRegisterIndex(const DexFile::CodeItem* code_item,
      uint32_t idx);

  /*
   * Check the register indices used in a "vararg" instruction, such as
   * invoke-virtual or filled-new-array.
   *
   * vA holds word count (0-5), args[] have values.
   *
   * There are some tests we don't do here, e.g. we don't try to verify
   * that invoking a method that takes a double is done with consecutive
   * registers.  This requires parsing the target method signature, which
   * we will be doing later on during the code flow analysis.
   */
  static bool CheckVarArgRegs(const DexFile::CodeItem* code_item, uint32_t vA,
      uint32_t arg[]);

  /*
   * Check the register indices used in a "vararg/range" instruction, such as
   * invoke-virtual/range or filled-new-array/range.
   *
   * vA holds word count, vC holds index of first reg.
   */
  static bool CheckVarArgRangeRegs(const DexFile::CodeItem* code_item,
      uint32_t vA, uint32_t vC);

  /*
   * Verify a switch table. "cur_offset" is the offset of the switch
   * instruction.
   *
   * Updates "insnFlags", setting the "branch target" flag.
   */
  static bool CheckSwitchTargets(const DexFile::CodeItem* code_item,
      InsnFlags insn_flags[], uint32_t cur_offset);

  /*
   * Verify that the target of a branch instruction is valid.
   *
   * We don't expect code to jump directly into an exception handler, but
   * it's valid to do so as long as the target isn't a "move-exception"
   * instruction.  We verify that in a later stage.
   *
   * The dex format forbids certain instructions from branching to itself.
   *
   * Updates "insnFlags", setting the "branch target" flag.
   */
  static bool CheckBranchTarget(const DexFile::CodeItem* code_item,
      InsnFlags insn_flags[], uint32_t cur_offset);

  /*
   * Initialize the RegisterTable.
   *
   * Every instruction address can have a different set of information about
   * what's in which register, but for verification purposes we only need to
   * store it at branch target addresses (because we merge into that).
   *
   * By zeroing out the regType storage we are effectively initializing the
   * register information to kRegTypeUnknown.
   *
   * We jump through some hoops here to minimize the total number of
   * allocations we have to perform per method verified.
   */
  static bool InitRegisterTable(VerifierData* vdata, RegisterTable* reg_table,
      RegisterTrackingMode track_regs_for);

  /* Get the register line for the given instruction in the current method. */
  static inline RegisterLine* GetRegisterLine(const RegisterTable* reg_table,
      int insn_idx) {
    return &reg_table->register_lines_[insn_idx];
  }

  /* Copy a register line. */
  static inline void CopyRegisterLine(RegisterLine* dst,
      const RegisterLine* src, size_t num_regs) {
    memcpy(dst->reg_types_, src->reg_types_, num_regs * sizeof(RegType));

    assert((src->monitor_entries_ == NULL && dst->monitor_entries_ == NULL) ||
           (src->monitor_entries_ != NULL && dst->monitor_entries_ != NULL));
    if (dst->monitor_entries_ != NULL) {
      assert(dst->monitor_stack_ != NULL);
      memcpy(dst->monitor_entries_, src->monitor_entries_,
          num_regs * sizeof(MonitorEntries));
      memcpy(dst->monitor_stack_, src->monitor_stack_,
          kMaxMonitorStackDepth * sizeof(uint32_t));
      dst->monitor_stack_top_ = src->monitor_stack_top_;
    }
  }

  /* Copy a register line into the table. */
  static inline void CopyLineToTable(RegisterTable* reg_table, int insn_idx,
      const RegisterLine* src) {
    RegisterLine* dst = GetRegisterLine(reg_table, insn_idx);
    assert(dst->reg_types_ != NULL);
    CopyRegisterLine(dst, src, reg_table->insn_reg_count_plus_);
  }

  /* Copy a register line out of the table. */
  static inline void CopyLineFromTable(RegisterLine* dst,
      const RegisterTable* reg_table, int insn_idx) {
    RegisterLine* src = GetRegisterLine(reg_table, insn_idx);
    assert(src->reg_types_ != NULL);
    CopyRegisterLine(dst, src, reg_table->insn_reg_count_plus_);
  }

#ifndef NDEBUG
  /*
   * Compare two register lines.  Returns 0 if they match.
   *
   * Using this for a sort is unwise, since the value can change based on
   * machine endianness.
   */
  static inline int CompareLineToTable(const RegisterTable* reg_table,
      int insn_idx, const RegisterLine* line2) {
    const RegisterLine* line1 = GetRegisterLine(reg_table, insn_idx);
    if (line1->monitor_entries_ != NULL) {
      int result;

      if (line2->monitor_entries_ == NULL)
        return 1;
      result = memcmp(line1->monitor_entries_, line2->monitor_entries_,
          reg_table->insn_reg_count_plus_ * sizeof(MonitorEntries));
      if (result != 0) {
        LOG(ERROR) << "monitor_entries_ mismatch";
        return result;
      }
      result = line1->monitor_stack_top_ - line2->monitor_stack_top_;
      if (result != 0) {
        LOG(ERROR) << "monitor_stack_top_ mismatch";
        return result;
      }
      result = memcmp(line1->monitor_stack_, line2->monitor_stack_,
            line1->monitor_stack_top_);
      if (result != 0) {
        LOG(ERROR) << "monitor_stack_ mismatch";
        return result;
      }
    }
    return memcmp(line1->reg_types_, line2->reg_types_,
        reg_table->insn_reg_count_plus_ * sizeof(RegType));
  }
#endif

  /*
   * Create a new uninitialized instance map.
   *
   * The map is allocated and populated with address entries.  The addresses
   * appear in ascending order to allow binary searching.
   *
   * Very few methods have 10 or more new-instance instructions; the
   * majority have 0 or 1.  Occasionally a static initializer will have 200+.
   *
   * TODO: merge this into the static pass or initRegisterTable; want to
   * avoid walking through the instructions yet again just to set up this table
   */
  static UninitInstanceMap* CreateUninitInstanceMap(VerifierData* vdata);

  /* Returns true if this method is a constructor. */
  static bool IsInitMethod(const Method* method);

  /*
   * Look up a class reference given as a simple string descriptor.
   *
   * If we can't find it, return a generic substitute when possible.
   */
  static Class* LookupClassByDescriptor(const Method* method,
      const char* descriptor, VerifyError* failure);

  /*
   * Look up a class reference in a signature.  Could be an arg or the
   * return value.
   *
   * Advances "*sig" to the last character in the signature (that is, to
   * the ';').
   *
   * NOTE: this is also expected to verify the signature.
   */
  static Class* LookupSignatureClass(const Method* method, std::string sig,
      VerifyError* failure);

  /*
   * Look up an array class reference in a signature.  Could be an arg or the
   * return value.
   *
   * Advances "*sig" to the last character in the signature.
   *
   * NOTE: this is also expected to verify the signature.
   */
  static Class* LookupSignatureArrayClass(const Method* method,
      std::string sig, VerifyError* failure);

  /*
   * Set the register types for the first instruction in the method based on
   * the method signature.
   *
   * This has the side-effect of validating the signature.
   *
   * Returns "true" on success.
   */
  static bool SetTypesFromSignature(VerifierData* vdata, RegType* reg_types);

  /*
   * Set the class object associated with the instruction at "addr".
   *
   * Returns the map slot index, or -1 if the address isn't listed in the map
   * (shouldn't happen) or if a class is already associated with the address
   * (bad bytecode).
   *
   * Entries, once set, do not change -- a given address can only allocate
   * one type of object.
   */
  static int SetUninitInstance(UninitInstanceMap* uninit_map, int addr,
      Class* klass);

  /*
   * Perform code flow on a method.
   *
   * The basic strategy is as outlined in v3 4.11.1.2: set the "changed" bit
   * on the first instruction, process it (setting additional "changed" bits),
   * and repeat until there are no more.
   *
   * v3 4.11.1.1
   * - (N/A) operand stack is always the same size
   * - operand stack [registers] contain the correct types of values
   * - local variables [registers] contain the correct types of values
   * - methods are invoked with the appropriate arguments
   * - fields are assigned using values of appropriate types
   * - opcodes have the correct type values in operand registers
   * - there is never an uninitialized class instance in a local variable in
   *   code protected by an exception handler (operand stack is okay, because
   *   the operand stack is discarded when an exception is thrown) [can't
   *   know what's a local var w/o the debug info -- should fall out of
   *   register typing]
   *
   * v3 4.11.1.2
   * - execution cannot fall off the end of the code
   *
   * (We also do many of the items described in the "static checks" sections,
   * because it's easier to do them here.)
   *
   * We need an array of RegType values, one per register, for every
   * instruction.  If the method uses monitor-enter, we need extra data
   * for every register, and a stack for every "interesting" instruction.
   * In theory this could become quite large -- up to several megabytes for
   * a monster function.
   *
   * NOTE:
   * The spec forbids backward branches when there's an uninitialized reference
   * in a register.  The idea is to prevent something like this:
   *   loop:
   *     move r1, r0
   *     new-instance r0, MyClass
   *     ...
   *     if-eq rN, loop  // once
   *   initialize r0
   *
   * This leaves us with two different instances, both allocated by the
   * same instruction, but only one is initialized.  The scheme outlined in
   * v3 4.11.1.4 wouldn't catch this, so they work around it by preventing
   * backward branches.  We achieve identical results without restricting
   * code reordering by specifying that you can't execute the new-instance
   * instruction if a register contains an uninitialized instance created
   * by that same instrutcion.
   */
  static bool CodeFlowVerifyMethod(VerifierData* vdata,
      RegisterTable* reg_table);

  /*
   * Perform verification for a single instruction.
   *
   * This requires fully decoding the instruction to determine the effect
   * it has on registers.
   *
   * Finds zero or more following instructions and sets the "changed" flag
   * if execution at that point needs to be (re-)evaluated.  Register changes
   * are merged into "reg_types_" at the target addresses.  Does not set or
   * clear any other flags in "insn_flags".
   */
  static bool CodeFlowVerifyInstruction(VerifierData* vdata,
      RegisterTable* reg_table, uint32_t insn_idx, size_t* start_guess);

  /*
   * Replace an instruction with "throw-verification-error".  This allows us to
   * defer error reporting until the code path is first used.
   *
   * This is expected to be called during "just in time" verification, not
   * from within dexopt.  (Verification failures in dexopt will result in
   * postponement of verification to first use of the class.)
   *
   * The throw-verification-error instruction requires two code units.  Some
   * of the replaced instructions require three; the third code unit will
   * receive a "nop".  The instruction's length will be left unchanged
   * in "insn_flags".
   *
   * The VM postpones setting of debugger breakpoints in unverified classes,
   * so there should be no clashes with the debugger.
   *
   * Returns "true" on success.
   */
  static bool ReplaceFailingInstruction(const DexFile::CodeItem* code_item,
      InsnFlags* insn_flags, int insn_idx, VerifyError failure);

  /* Handle a monitor-enter instruction. */
  static void HandleMonitorEnter(RegisterLine* work_line, uint32_t reg_idx,
      uint32_t insn_idx, VerifyError* failure);

  /* Handle a monitor-exit instruction. */
  static void HandleMonitorExit(RegisterLine* work_line, uint32_t reg_idx,
      uint32_t insn_idx, VerifyError* failure);

  /*
   * Look up an instance field, specified by "field_idx", that is going to be
   * accessed in object "obj_type".  This resolves the field and then verifies
   * that the class containing the field is an instance of the reference in
   * "obj_type".
   *
   * It is possible for "obj_type" to be kRegTypeZero, meaning that we might
   * have a null reference.  This is a runtime problem, so we allow it,
   * skipping some of the type checks.
   *
   * In general, "obj_type" must be an initialized reference.  However, we
   * allow it to be uninitialized if this is an "<init>" method and the field
   * is declared within the "obj_type" class.
   *
   * Returns a Field on success, returns NULL and sets "*failure" on failure.
   */
  static Field* GetInstField(VerifierData* vdata, RegType obj_type,
      int field_idx, VerifyError* failure);

  /*
   * Look up a static field.
   *
   * Returns a StaticField on success, returns NULL and sets "*failure"
   * on failure.
   */
  static Field* GetStaticField(VerifierData* vdata, int field_idx,
      VerifyError* failure);
  /*
   * For the "move-exception" instruction at "insn_idx", which must be at an
   * exception handler address, determine the first common superclass of
   * all exceptions that can land here.  (For javac output, we're probably
   * looking at multiple spans of bytecode covered by one "try" that lands
   * at an exception-specific "catch", but in general the handler could be
   * shared for multiple exceptions.)
   *
   * Returns NULL if no matching exception handler can be found, or if the
   * exception is not a subclass of Throwable.
   */
  static Class* GetCaughtExceptionType(VerifierData* vdata, int insn_idx,
      VerifyError* failure);

  /*
   * Get the type of register N.
   *
   * The register index was validated during the static pass, so we don't
   * need to check it here.
   */
  static inline RegType GetRegisterType(const RegisterLine* register_line,
      uint32_t vsrc) {
    return register_line->reg_types_[vsrc];
  }

  /*
   * Return the register type for the method.  We can't just use the
   * already-computed DalvikJniReturnType, because if it's a reference type
   * we need to do the class lookup.
   *
   * Returned references are assumed to be initialized.
   *
   * Returns kRegTypeUnknown for "void".
   */
  static RegType GetMethodReturnType(const DexFile* dex_file,
      const Method* method);

  /*
   * Get the value from a register, and cast it to a Class.  Sets
   * "*failure" if something fails.
   *
   * This fails if the register holds an uninitialized class.
   *
   * If the register holds kRegTypeZero, this returns a NULL pointer.
   */
  static Class* GetClassFromRegister(const RegisterLine* register_line,
      uint32_t vsrc, VerifyError* failure);

  /*
   * Get the "this" pointer from a non-static method invocation.  This
   * returns the RegType so the caller can decide whether it needs the
   * reference to be initialized or not.  (Can also return kRegTypeZero
   * if the reference can only be zero at this point.)
   *
   * The argument count is in vA, and the first argument is in vC, for both
   * "simple" and "range" versions.  We just need to make sure vA is >= 1
   * and then return vC.
   */
  static RegType GetInvocationThis(const RegisterLine* register_line,
      const Instruction::DecodedInstruction* dec_insn, VerifyError* failure);

  /*
   * Set the type of register N, verifying that the register is valid.  If
   * "new_type" is the "Lo" part of a 64-bit value, register N+1 will be
   * set to "new_type+1".
   *
   * The register index was validated during the static pass, so we don't
   * need to check it here.
   *
   * TODO: clear mon stack bits
   */
  static void SetRegisterType(RegisterLine* register_line, uint32_t vdst,
      RegType new_type);

  /*
   * Verify that the contents of the specified register have the specified
   * type (or can be converted to it through an implicit widening conversion).
   *
   * This will modify the type of the source register if it was originally
   * derived from a constant to prevent mixing of int/float and long/double.
   *
   * If "vsrc" is a reference, both it and the "vsrc" register must be
   * initialized ("vsrc" may be Zero).  This will verify that the value in
   * the register is an instance of check_type, or if check_type is an
   * interface, verify that the register implements check_type.
   */
  static void VerifyRegisterType(RegisterLine* register_line, uint32_t vsrc,
      RegType check_type, VerifyError* failure);

  /* Set the type of the "result" register. */
  static void SetResultRegisterType(RegisterLine* register_line,
      const int insn_reg_count, RegType new_type);

  /*
   * Update all registers holding "uninit_type" to instead hold the
   * corresponding initialized reference type.  This is called when an
   * appropriate <init> method is invoked -- all copies of the reference
   * must be marked as initialized.
   */
  static void MarkRefsAsInitialized(RegisterLine* register_line,
      int insn_reg_count, UninitInstanceMap* uninit_map, RegType uninit_type,
      VerifyError* failure);

  /*
   * Implement category-1 "move" instructions.  Copy a 32-bit value from
   * "vsrc" to "vdst".
   */
  static void CopyRegister1(RegisterLine* register_line, uint32_t vdst,
      uint32_t vsrc, TypeCategory cat, VerifyError* failure);

  /*
   * Implement category-2 "move" instructions.  Copy a 64-bit value from
   * "vsrc" to "vdst".  This copies both halves of the register.
   */
  static void CopyRegister2(RegisterLine* register_line, uint32_t vdst,
      uint32_t vsrc, VerifyError* failure);

  /*
   * Implement "move-result".  Copy the category-1 value from the result
   * register to another register, and reset the result register.
   */
  static void CopyResultRegister1(RegisterLine* register_line,
      const int insn_reg_count, uint32_t vdst, TypeCategory cat,
      VerifyError* failure);

  /*
   * Implement "move-result-wide".  Copy the category-2 value from the result
   * register to another register, and reset the result register.
   */
  static void CopyResultRegister2(RegisterLine* register_line,
      const int insn_reg_count, uint32_t vdst, VerifyError* failure);

  /*
   * Compute the "class depth" of a class.  This is the distance from the
   * class to the top of the tree, chasing superclass links.  java.lang.Object
   * has a class depth of 0.
   */
  static int GetClassDepth(Class* klass);

  /*
   * Given two classes, walk up the superclass tree to find a common
   * ancestor.  (Called from findCommonSuperclass().)
   *
   * TODO: consider caching the class depth in the class object so we don't
   * have to search for it here.
   */
  static Class* DigForSuperclass(Class* c1, Class* c2);

  /*
   * Merge two array classes.  We can't use the general "walk up to the
   * superclass" merge because the superclass of an array is always Object.
   * We want String[] + Integer[] = Object[].  This works for higher dimensions
   * as well, e.g. String[][] + Integer[][] = Object[][].
   *
   * If Foo1 and Foo2 are subclasses of Foo, Foo1[] + Foo2[] = Foo[].
   *
   * If Class implements Type, Class[] + Type[] = Type[].
   *
   * If the dimensions don't match, we want to convert to an array of Object
   * with the least dimension, e.g. String[][] + String[][][][] = Object[][].
   *
   * Arrays of primitive types effectively have one less dimension when
   * merging.  int[] + float[] = Object, int[] + String[] = Object,
   * int[][] + float[][] = Object[], int[][] + String[] = Object[].  (The
   * only time this function doesn't return an array class is when one of
   * the arguments is a 1-dimensional primitive array.)
   *
   * This gets a little awkward because we may have to ask the VM to create
   * a new array type with the appropriate element and dimensions.  However, we
   * shouldn't be doing this often.
   */
  static Class* FindCommonArraySuperclass(Class* c1, Class* c2);

  /*
   * Find the first common superclass of the two classes.  We're not
   * interested in common interfaces.
   *
   * The easiest way to do this for concrete classes is to compute the "class
   * depth" of each, move up toward the root of the deepest one until they're
   * at the same depth, then walk both up to the root until they match.
   *
   * If both classes are arrays, we need to merge based on array depth and
   * element type.
   *
   * If one class is an interface, we check to see if the other class/interface
   * (or one of its predecessors) implements the interface.  If so, we return
   * the interface; otherwise, we return Object.
   *
   * NOTE: we continue the tradition of "lazy interface handling".  To wit,
   * suppose we have three classes:
   *   One implements Fancy, Free
   *   Two implements Fancy, Free
   *   Three implements Free
   * where Fancy and Free are unrelated interfaces.  The code requires us
   * to merge One into Two.  Ideally we'd use a common interface, which
   * gives us a choice between Fancy and Free, and no guidance on which to
   * use.  If we use Free, we'll be okay when Three gets merged in, but if
   * we choose Fancy, we're hosed.  The "ideal" solution is to create a
   * set of common interfaces and carry that around, merging further references
   * into it.  This is a pain.  The easy solution is to simply boil them
   * down to Objects and let the runtime invokeinterface call fail, which
   * is what we do.
   */
  static Class* FindCommonSuperclass(Class* c1, Class* c2);

  /*
   * Merge two RegType values.
   *
   * Sets "*changed" to "true" if the result doesn't match "type1".
   */
  static RegType MergeTypes(RegType type1, RegType type2, bool* changed);

  /*
   * Merge the bits that indicate which monitor entry addresses on the stack
   * are associated with this register.
   *
   * The merge is a simple bitwise AND.
   *
   * Sets "*pChanged" to "true" if the result doesn't match "ents1".
   */
  static MonitorEntries MergeMonitorEntries(MonitorEntries ents1,
      MonitorEntries ents2, bool* changed);

  /*
   * We're creating a new instance of class C at address A.  Any registers
   * holding instances previously created at address A must be initialized
   * by now.  If not, we mark them as "conflict" to prevent them from being
   * used (otherwise, MarkRefsAsInitialized would mark the old ones and the
   * new ones at the same time).
   */
  static void MarkUninitRefsAsInvalid(RegisterLine* register_line,
      int insn_reg_count, UninitInstanceMap* uninit_map, RegType uninit_type);

  /*
   * Control can transfer to "next_insn".
   *
   * Merge the registers from "work_line" into "reg_table" at "next_insn", and
   * set the "changed" flag on the target address if any of the registers
   * has changed.
   *
   * Returns "false" if we detect mismatched monitor stacks.
   */
  static bool UpdateRegisters(InsnFlags* insn_flags, RegisterTable* reg_table,
      int next_insn, const RegisterLine* work_line);

  /*
   * Determine whether we can convert "src_type" to "check_type", where
   * "check_type" is one of the category-1 non-reference types.
   *
   * Constant derived types may become floats, but other values may not.
   */
  static bool CanConvertTo1nr(RegType src_type, RegType check_type);

  /* Determine whether the category-2 types are compatible. */
  static bool CanConvertTo2(RegType src_type, RegType check_type);

  /* Convert a VM PrimitiveType enum value to the equivalent RegType value. */
  static RegType PrimitiveTypeToRegType(Class::PrimitiveType prim_type);

  /*
   * Convert a const derived RegType to the equivalent non-const RegType value.
   * Does nothing if the argument type isn't const derived.
   */
  static RegType ConstTypeToRegType(RegType const_type);

  /*
   * Given a 32-bit constant, return the most-restricted RegType enum entry
   * that can hold the value. The types used here indicate the value came
   * from a const instruction, and may not correctly represent the real type
   * of the value. Upon use, a constant derived type is updated with the
   * type from the use, which will be unambiguous.
   */
  static char DetermineCat1Const(int32_t value);

  /*
   * If "field" is marked "final", make sure this is the either <clinit>
   * or <init> as appropriate.
   *
   * Sets "*failure" on failure.
   */
  static void CheckFinalFieldAccess(const Method* method, const Field* field,
      VerifyError* failure);

  /*
   * Make sure that the register type is suitable for use as an array index.
   *
   * Sets "*failure" if not.
   */
  static void CheckArrayIndexType(const Method* method, RegType reg_type,
      VerifyError* failure);

  /*
   * Check constraints on constructor return.  Specifically, make sure that
   * the "this" argument got initialized.
   *
   * The "this" argument to <init> uses code offset kUninitThisArgAddr, which
   * puts it at the start of the list in slot 0.  If we see a register with
   * an uninitialized slot 0 reference, we know it somehow didn't get
   * initialized.
   *
   * Returns "true" if all is well.
   */
  static bool CheckConstructorReturn(const Method* method,
      const RegisterLine* register_line, const int insn_reg_count);

  /*
   * Verify that the target instruction is not "move-exception".  It's important
   * that the only way to execute a move-exception is as the first instruction
   * of an exception handler.
   *
   * Returns "true" if all is well, "false" if the target instruction is
   * move-exception.
   */
  static bool CheckMoveException(const uint16_t* insns, int insn_idx);

  /*
   * See if "type" matches "cat".  All we're really looking for here is that
   * we're not mixing and matching 32-bit and 64-bit quantities, and we're
   * not mixing references with numerics.  (For example, the arguments to
   * "a < b" could be integers of different sizes, but they must both be
   * integers.  Dalvik is less specific about int vs. float, so we treat them
   * as equivalent here.)
   *
   * For category 2 values, "type" must be the "low" half of the value.
   *
   * Sets "*failure" if something looks wrong.
   */
  static void CheckTypeCategory(RegType type, TypeCategory cat,
      VerifyError* failure);

  /*
   * For a category 2 register pair, verify that "type_h" is the appropriate
   * high part for "type_l".
   *
   * Does not verify that "type_l" is in fact the low part of a 64-bit
   * register pair.
   */
  static void CheckWidePair(RegType type_l, RegType type_h,
      VerifyError* failure);

  /*
   * Verify types for a simple two-register instruction (e.g. "neg-int").
   * "dst_type" is stored into vA, and "src_type" is verified against vB.
   */
  static void CheckUnop(RegisterLine* register_line,
      Instruction::DecodedInstruction* dec_insn, RegType dst_type,
      RegType src_type, VerifyError* failure);

  /*
   * Verify types for a simple three-register instruction (e.g. "add-int").
   * "dst_type" is stored into vA, and "src_type1"/"src_type2" are verified
   * against vB/vC.
   */
  static void CheckBinop(RegisterLine* register_line,
      Instruction::DecodedInstruction* dec_insn, RegType dst_type,
      RegType src_type1, RegType src_type2, bool check_boolean_op,
      VerifyError* failure);

  /*
   * Verify types for a binary "2addr" operation.  "src_type1"/"src_type2"
   * are verified against vA/vB, then "dst_type" is stored into vA.
   */
  static void CheckBinop2addr(RegisterLine* register_line,
      Instruction::DecodedInstruction* dec_insn, RegType dst_type,
      RegType src_type1, RegType src_type2, bool check_boolean_op,
      VerifyError* failure);

  /*
   * Treat right-shifting as a narrowing conversion when possible.
   *
   * For example, right-shifting an int 24 times results in a value that can
   * be treated as a byte.
   *
   * Things get interesting when contemplating sign extension.  Right-
   * shifting an integer by 16 yields a value that can be represented in a
   * "short" but not a "char", but an unsigned right shift by 16 yields a
   * value that belongs in a char rather than a short.  (Consider what would
   * happen if the result of the shift were cast to a char or short and then
   * cast back to an int.  If sign extension, or the lack thereof, causes
   * a change in the 32-bit representation, then the conversion was lossy.)
   *
   * A signed right shift by 17 on an integer results in a short.  An unsigned
   * right shfit by 17 on an integer results in a posshort, which can be
   * assigned to a short or a char.
   *
   * An unsigned right shift on a short can actually expand the result into
   * a 32-bit integer.  For example, 0xfffff123 >>> 8 becomes 0x00fffff1,
   * which can't be represented in anything smaller than an int.
   *
   * javac does not generate code that takes advantage of this, but some
   * of the code optimizers do.  It's generally a peephole optimization
   * that replaces a particular sequence, e.g. (bipush 24, ishr, i2b) is
   * replaced by (bipush 24, ishr).  Knowing that shifting a short 8 times
   * to the right yields a byte is really more than we need to handle the
   * code that's out there, but support is not much more complex than just
   * handling integer.
   *
   * Right-shifting never yields a boolean value.
   *
   * Returns the new register type.
   */
  static RegType AdjustForRightShift(RegisterLine* register_line, int reg,
      unsigned int shift_count, bool is_unsigned_shift, VerifyError* failure);

  /*
   * We're performing an operation like "and-int/2addr" that can be
   * performed on booleans as well as integers.  We get no indication of
   * boolean-ness, but we can infer it from the types of the arguments.
   *
   * Assumes we've already validated reg1/reg2.
   *
   * TODO: consider generalizing this.  The key principle is that the
   * result of a bitwise operation can only be as wide as the widest of
   * the operands.  You can safely AND/OR/XOR two chars together and know
   * you still have a char, so it's reasonable for the compiler or "dx"
   * to skip the int-to-char instruction.  (We need to do this for boolean
   * because there is no int-to-boolean operation.)
   *
   * Returns true if both args are Boolean, Zero, or One.
   */
  static bool UpcastBooleanOp(RegisterLine* register_line, uint32_t reg1,
      uint32_t reg2);

  /*
   * Verify types for A two-register instruction with a literal constant
   * (e.g. "add-int/lit8").  "dst_type" is stored into vA, and "src_type" is
   * verified against vB.
   *
   * If "check_boolean_op" is set, we use the constant value in vC.
   */
  static void CheckLitop(RegisterLine* register_line,
      Instruction::DecodedInstruction* dec_insn, RegType dst_type,
      RegType src_type, bool check_boolean_op, VerifyError* failure);

  /*
   * Verify that the arguments in a filled-new-array instruction are valid.
   *
   * "res_class" is the class refered to by dec_insn->vB_.
   */
  static void VerifyFilledNewArrayRegs(const Method* method,
      RegisterLine* register_line,
      const Instruction::DecodedInstruction* dec_insn, Class* res_class,
      bool is_range, VerifyError* failure);

  /* See if the method matches the MethodType. */
  static bool IsCorrectInvokeKind(MethodType method_type, Method* res_method);

  /*
   * Verify the arguments to a method.  We're executing in "method", making
   * a call to the method reference in vB.
   *
   * If this is a "direct" invoke, we allow calls to <init>.  For calls to
   * <init>, the first argument may be an uninitialized reference.  Otherwise,
   * calls to anything starting with '<' will be rejected, as will any
   * uninitialized reference arguments.
   *
   * For non-static method calls, this will verify that the method call is
   * appropriate for the "this" argument.
   *
   * The method reference is in vBBBB.  The "is_range" parameter determines
   * whether we use 0-4 "args" values or a range of registers defined by
   * vAA and vCCCC.
   *
   * Widening conversions on integers and references are allowed, but
   * narrowing conversions are not.
   *
   * Returns the resolved method on success, NULL on failure (with *failure
   * set appropriately).
   */
  static Method* VerifyInvocationArgs(VerifierData* vdata,
      RegisterLine* register_line, const int insn_reg_count,
      const Instruction::DecodedInstruction* dec_insn, MethodType method_type,
      bool is_range, bool is_super, VerifyError* failure);

  DISALLOW_COPY_AND_ASSIGN(DexVerifier);
};

}  // namespace art

#endif  // ART_SRC_DEX_VERIFY_H_
