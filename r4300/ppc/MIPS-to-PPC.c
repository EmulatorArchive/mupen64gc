/* MIPS-to-PPC.c - convert MIPS code into PPC (take 2 1/2)
   by Mike Slegeir for Mupen64-GC
 ************************************************
   FIXME: Review all branch destinations
   TODO: Complete adding register mapping (invalidate on branches)
         Create FP register mapping and recompile those
         Rewrite any interpretation to behave like normal function calls
 */

#include <string.h>
#include "MIPS-to-PPC.h"
#include "Interpreter.h"
#include "Wrappers.h"

// Prototypes for functions used and defined in this file
static void genCallInterp(MIPS_instr);
#define JUMPTO_REG  0
#define JUMPTO_OFF  1
#define JUMPTO_ADDR 2
#define JUMPTO_REG_SIZE  14
#define JUMPTO_OFF_SIZE  (JUMPTO_REG_SIZE+2)
#define JUMPTO_ADDR_SIZE (JUMPTO_REG_SIZE+2)
static void genJumpTo(unsigned int loc, unsigned int type);
static int inline mips_is_jump(MIPS_instr);
void jump_to(unsigned int);

// Register Mapping
// r13 holds reg
#define MIPS_REG_HI 32
#define MIPS_REG_LO 33
static int regMap[34];
static int availableRegsDefault[32] = {
	0, /* r0 is mostly used for saving/restoring lr: usable */
	0, /* sp: leave alone! */
	0, /* gp: leave alone! */
	1,1,1,1,1,1,1,1, /* Volatile argument registers */
	0,0, /* Volatile registers used for special purposes: dunno */
	/* Non-volatile registers: using might be too costly */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
	};
static int availableRegs[32];

static int flushRegisters(void){
	PowerPC_instr ppc;
	int i, flushed = 0;
	for(i=0; i<34; ++i)
		if(regMap[i] >= 0){
			if(i != 0){
				GEN_STW(ppc, regMap[i], i*8+4, 13);
				set_next_dst(ppc);
			}
			regMap[i] = -1;
			++flushed;
		}
	memcpy(availableRegs, availableRegsDefault, 32*sizeof(int));
	return flushed;
}

static void invalidateRegisters(void){
	int i;
	for(i=0; i<34; ++i)
		regMap[i] = -1;
	memcpy(availableRegs, availableRegsDefault, 32*sizeof(int));
}

static int mapRegisterNew(int reg){
	if(regMap[reg] >= 0) return regMap[reg];
	int i;
	for(i=0; i<32; ++i)
		if(availableRegs[i]){
			availableRegs[i] = 0;
			return regMap[reg] = i;
		}
	flushRegisters();
	availableRegs[3] = 0;
	return regMap[reg] = 3;
}

static int mapRegister(int reg){
	PowerPC_instr ppc;
	if(regMap[reg] >= 0) return regMap[reg];
	int i;
	for(i=0; i<sizeof(availableRegs)/4; ++i)
		if(availableRegs[i]){
			if(reg != 0){
				GEN_LWZ(ppc, i, reg*8+4, 13);
				set_next_dst(ppc);
			} else {
				GEN_LI(ppc, i, 0, 0);
				set_next_dst(ppc);
			}
			availableRegs[i] = 0;
			return regMap[reg] = i;
		}
	flushRegisters();
	GEN_LWZ(ppc, 3, reg*8+4, 13);
	set_next_dst(ppc);
	availableRegs[3] = 0;
	return regMap[reg] = 3;
}

// Initialize register mappings
void start_new_block(void){
	invalidateRegisters();
}

// Variable to indicate whether the current recompiled instruction
//   is a delay slot (which needs to have its registers flushed)
static int isDelaySlot;
// This should be called before the jump is recompiled
static inline int check_delaySlot(void){
	if(peek_next_src() == 0){ // MIPS uses 0 as a NOP
		get_next_src();   // Get rid of the NOP
		return 0;
	} else {
		if(mips_is_jump(peek_next_src())) return CONVERT_WARNING;
		isDelaySlot = 1;
		convert(); // This just moves the delay slot instruction ahead of the branch
		return 1;
	}
}

static inline int signExtend(int value, int size){
	int signMask = 1 << (size-1);
	int negMask = 0xffffffff << (size-1);
	if(value & signMask) value |= negMask;
	return value;
}


static int (*gen_ops[64])(MIPS_instr);

int convert(void){
	MIPS_instr mips = get_next_src();
	int result = gen_ops[MIPS_GET_OPCODE(mips)](mips);
	if(isDelaySlot){ flushRegisters(); isDelaySlot = 0; }
	return result;
}

static int NI(MIPS_instr mips){
	return CONVERT_ERROR;
}

// -- Primary Opcodes --

static int J(MIPS_instr mips){
	PowerPC_instr  ppc;
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	flushRegisters();
	
#ifdef INTERPRET_J
	genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
#else // INTERPRET_J
	// If we're jumping out, we can't just use a branch instruction
	if(is_j_out(MIPS_GET_LI(mips), 1)){
		genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
	} else {
		// Even though this is an absolute branch
		//   in pass 2, we generate a relative branch
		GEN_B(ppc, add_jump(MIPS_GET_LI(mips), 1, 0), 0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_J
	return INTERPRETED;
#else // INTERPRET_J
	return CONVERT_SUCCESS;
#endif
}

static int JAL(MIPS_instr mips){
	PowerPC_instr  ppc;
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	// Set LR to next instruction
	int lr = mapRegisterNew(MIPS_REG_LR);
	// lis	lr, pc@ha(0)
	GEN_LIS(ppc, lr, get_src_pc()>>16);
	set_next_dst(ppc);
	// la	lr, pc@l(lr)
	GEN_LI(ppc, lr, lr, get_src_pc());
	set_next_dst(ppc);
	
	flushRegisters();
	
#ifdef INTERPRET_JAL
	genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
#else // INTERPRET_JAL
	// If we're jumping out, we can't just use a branch instruction
	if(is_j_out(MIPS_GET_LI(mips), 1)){
		genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
	} else {
		// Even though this is an absolute branch
		//   in pass 2, we generate a relative branch
		// TODO: If I can figure out using the LR, use it!
		GEN_B(ppc, add_jump(MIPS_GET_LI(mips), 1, 0), 0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		isDelaySlot = 1;
		// Step over the already executed delay slot if we ever
		//   actually use the real LR for JAL
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_JAL
	return INTERPRETED;
#else // INTERPRET_JAL
	return CONVERT_SUCCESS;
#endif
}

static int BEQ(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        7);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	flushRegisters();
	
#ifdef INTERPRET_BEQ
	// bne <past jumpto & delay>
	GEN_BNE(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
	set_next_dst(ppc);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BEQ
	// If we're jumping out, we can't just use a branch instruction
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// bne <past jumpto & delay>
		GEN_BNE(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
		set_next_dst(ppc);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// The actual branch
		GEN_BEQ(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, 0),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		isDelaySlot = 1;
		// Step over the already executed delay slot if
		//   the branch isn't taken
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_BEQ
	return INTERPRETED;
#else // INTERPRET_BEQ
	return CONVERT_SUCCESS;
#endif
}

static int BNE(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        7);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	flushRegisters();
	
#ifdef INTERPRET_BNE
	// beq <past jumpto & delay>
	GEN_BEQ(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
	set_next_dst(ppc);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BNE
	// If we're jumping out, we can't just branch
	if(j_out = is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// beq <past jumpto & delay>
		GEN_BEQ(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
		set_next_dst(ppc);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// The actual branch
		GEN_BNE(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, 0),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		isDelaySlot = 1;
		// Step over the already executed delay slot if
		//   the branch isn't taken
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_BNE
	return INTERPRETED;
#else // INTERPRET_BNE
	return CONVERT_SUCCESS;
#endif
}

static int BLEZ(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 7);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	flushRegisters();
	
#ifdef INTERPRET_BLEZ
	// bgt <past jumpto & delay>
	GEN_BGT(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
	set_next_dst(ppc);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BLEZ
	// If we're jumping out, we can't just branch
	if(j_out = is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// bgt <past jumpto & delay>
		GEN_BGT(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
		set_next_dst(ppc);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// The actual branch
		GEN_BLE(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, 0),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		isDelaySlot = 1;
		// Step over the already executed delay slot if
		//   the branch isn't taken
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_BLEZ
	return INTERPRETED;
#else // INTERPRET_BLEZ
	return CONVERT_SUCCESS;
#endif
}

static int BGTZ(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 7);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	flushRegisters();
	
#ifdef INTERPRET_BGTZ
	// ble <past jumpto & delay>
	GEN_BLE(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
	set_next_dst(ppc);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BGTZ
	// If we're jumping out, we can't just branch
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// ble <past jumpto & delay>
		GEN_BLE(ppc, 7, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0);
		set_next_dst(ppc);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// The actual branch
		GEN_BGT(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, 0),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		isDelaySlot = 1;
		// Step over the already executed delay slot if
		//   the branch isn't taken
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_BGTZ
	return INTERPRETED;
#else // INTERPRET_BGTZ
	return CONVERT_SUCCESS;
#endif
}

static int ADDIU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ADDI(ppc,
	         mapRegisterNew( MIPS_GET_RT(mips) ),
	         rs,
	         MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	return CONVERT_SUCCESS;
}

static int ADDI(MIPS_instr mips){
	return ADDIU(mips);
}

static int SLTI(MIPS_instr mips){
	PowerPC_instr ppc;
	int shiftSrc ;
	// If immed != 0: rd <- rs - immed
	if( MIPS_GET_IMMED(mips) ){
		int rs = mapRegister( MIPS_GET_RS(mips) );
		shiftSrc = mapRegisterNew( MIPS_GET_RT(mips) );
		GEN_ADDI(ppc,
		         shiftSrc,
		         rs,
		         -signExtend(MIPS_GET_IMMED(mips),16));
		set_next_dst(ppc);
	} else shiftSrc = mapRegister( MIPS_GET_RS(mips) );
	// Shift the sign bit to the LSb
	GEN_SRWI(ppc, mapRegisterNew( MIPS_GET_RT(mips) ), shiftSrc, 31);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SLTIU(MIPS_instr mips){
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegisterNew( MIPS_GET_RT(mips) );
	PowerPC_instr ppc;
	// rd <- rs - immed
	GEN_ADDI(ppc, rt, rs, -MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	// Shift the sign bit to the LSb
	GEN_SRWI(ppc, rt, rt, 31);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int ANDI(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ANDI(ppc,
	         mapRegisterNew( MIPS_GET_RT(mips) ),
	         rs,
	         MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int ORI(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ORI(ppc,
	        mapRegisterNew( MIPS_GET_RT(mips) ),
	        rs,
	        MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int XORI(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_XORI(ppc,
	         mapRegisterNew( MIPS_GET_RT(mips) ),
	         rs,
	         MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int LUI(MIPS_instr mips){
	PowerPC_instr ppc;
	GEN_LIS(ppc,
	        mapRegisterNew( MIPS_GET_RT(mips) ),
	        MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int BEQL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        7);
	set_next_dst(ppc);
	
	flushRegisters();
	
	// bne <past jumpto & delay>
	int likely_id = add_jump_special(0);
	GEN_BNE(ppc, 7, likely_id, 0, 0);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_BEQL
	// Jump over the generated jump, and both delay slots
	set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BEQL
	// If we're jumping out, we can't just branch
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, 1+2*delaySlot+1);
		// The actual branch
		GEN_BEQ(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, 0),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_BEQL
	return INTERPRETED;
#else // INTERPRET_BEQL
	return CONVERT_SUCCESS;
#endif
}

static int BNEL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        7);
	set_next_dst(ppc);
	
	flushRegisters();
	
	// beq <past jumpto & delay>
	int likely_id = add_jump_special(0);
	GEN_BEQ(ppc, 7, likely_id, 0, 0);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_BEQL
	// Jump over the generated jump, and both delay slots
	set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BNEL
	// If we're jumping out, we need to do more than just branch
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, 1+2*delaySlot+1);
		// The actual branch
		GEN_BNE(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, j_out),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_BNEL
	return INTERPRETED;
#else // INTERPRET_BNEL
	return CONVERT_SUCCESS;
#endif
}

static int BLEZL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 7);
	set_next_dst(ppc);
	
	flushRegisters();
	
	// bgt <past jumpto & delay>
	int likely_id = add_jump_special(0);
	GEN_BGT(ppc, 7, likely_id, 0, 0);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_BLEZL
	// Jump over the generated jump, and both delay slots
	set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BLEZL
	// If we're jumping out, we can't just branch
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, 1+2*delaySlot+1);
		// The actual branch
		GEN_BLE(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, j_out),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_BLEZL
	return INTERPRETED;
#else // INTERPRET_BLEZL
	return CONVERT_SUCCESS;
#endif
}

static int BGTZL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 7);
	set_next_dst(ppc);
	
	flushRegisters();
	
	// ble <past jumpto & delay>
	int likely_id = add_jump_special(0);
	GEN_BLE(ppc, 7, likely_id, 0, 0);
	set_next_dst(ppc);
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_BGTZL
	// Jump over the generated jump, and both delay slots
	set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BGTZL
	// If we're jumping out, we need to trampoline
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else {
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, 1+2*delaySlot+1);
		// The actual branch
		GEN_BGT(ppc, 7,
		        add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, j_out),
		        0, 0);
		set_next_dst(ppc);
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_BGTZL
	return INTERPRETED;
#else // INTERPRET_BGTZL
	return CONVERT_SUCCESS;
#endif
}

static int DADDIU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: daddiu
	return CONVERT_ERROR;
#endif
}

static int DADDI(MIPS_instr mips){
	// FIXME: Is there a difference?
	return DADDIU(mips);
}

static int LDL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: ldl
	return CONVERT_ERROR;
#endif
}

static int LDR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: ldr
	return CONVERT_ERROR;
#endif
}

static int LB(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LB
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LB
	// TODO: lb
	return CONVERT_ERROR;
#endif
}

static int LH(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LH
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LH
	// TODO: lh
	return CONVERT_ERROR;
#endif
}

static int LWL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWL
	// TODO: lwl
	return CONVERT_ERROR;
#endif
}

static int LW(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LW
	// TODO: lw
	return CONVERT_ERROR;
#endif
}

static int LBU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LBU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LBU
	// TODO: lbu
	return CONVERT_ERROR;
#endif
}

static int LHU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LHU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LHU
	// TODO: lhu
	return CONVERT_ERROR;
#endif
}

static int LWR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWR
	// TODO: lwr
	return CONVERT_ERROR;
#endif
}

static int LWU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWU
	// TODO: lwu
	return CONVERT_ERROR;
#endif
}

static int SB(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SB
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SB
	// TODO: sb
	return CONVERT_ERROR;
#endif
}

static int SH(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SH
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SH
	// TODO: sh
	return CONVERT_ERROR;
#endif
}

static int SWL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SWL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SWL
	// TODO: swl
	return CONVERT_ERROR;
#endif
}

static int SW(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SW
	// TODO: sw
	return CONVERT_ERROR;
#endif
}

static int SDL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SDL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SDL
	// TODO: sdl
	return CONVERT_ERROR;
#endif
}

static int SDR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SDR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SDR
	// TODO: sdr
	return CONVERT_ERROR;
#endif
}

static int SWR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SWR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SWR
	// TODO: swr
	return CONVERT_ERROR;
#endif
}

static int LD(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LD
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LD
	// TODO: ld
	return CONVERT_ERROR;
#endif
}

static int SD(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SD
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SD
	// TODO: sd
	return CONVERT_ERROR;
#endif
}

static int LWC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWC1
	// TODO: lwc1
	return CONVERT_ERROR;
#endif
}

static int LDC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LDC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LDC1
	// TODO: ldc1
	return CONVERT_ERROR;
#endif
}

static int SWC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SWC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SWC1
	// TODO: swc1
	return CONVERT_ERROR;
#endif
}

static int SDC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SDC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SDC1
	// TODO: sdc1
	return CONVERT_ERROR;
#endif
}

static int CACHE(MIPS_instr mips){
	return CONVERT_ERROR;
}

static int LL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LL
	// TODO: ll
	return CONVERT_ERROR;
#endif
}

static int SC(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SC
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SC
	// TODO: sc
	return CONVERT_ERROR;
#endif
}

// -- Special Functions --

static int SLL(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int sa = mapRegister( MIPS_GET_SA(mips) );
	GEN_SLWI(ppc,
	         mapRegisterNew( MIPS_GET_RD(mips) ),
	         rt,
	         sa);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRL(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int sa = mapRegister( MIPS_GET_SA(mips) );
	GEN_SRWI(ppc,
	         mapRegisterNew( MIPS_GET_RD(mips) ),
	         rt,
	         sa);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRA(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int sa = mapRegister( MIPS_GET_SA(mips) );
	GEN_SRAWI(ppc,
	          mapRegisterNew( MIPS_GET_RD(mips) ),
	          rt,
	          sa);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SLLV(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SLW(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rt,
	        rs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRLV(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SRW(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rt,
	        rs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRAV(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SRAW(ppc,
	         mapRegisterNew( MIPS_GET_RD(mips) ),
	         rt,
	         rs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int JR(MIPS_instr mips){
	PowerPC_instr ppc;
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;	
	
#ifdef INTERPRET_JR
	genJumpTo(mapRegister(MIPS_GET_RS(mips)), JUMPTO_REG);
#else // INTERPRET_JR
	// TODO: jr
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_JR
	return INTERPRETED;
#else // INTERPRET_JR
	return CONVER_ERROR;
#endif
}

static int JALR(MIPS_instr mips){
	PowerPC_instr  ppc;
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	// TODO: If I can figure out using the LR,
	//         this might only be necessary for interp
	// Set LR to next instruction
	int rd = mapRegisterNew(MIPS_GET_RD(mips));
	// lis	lr, pc@ha(0)
	GEN_LIS(ppc, rd, get_src_pc()>>16);
	set_next_dst(ppc);
	// la	lr, pc@l(lr)
	GEN_LI(ppc, rd, rd, get_src_pc());
	set_next_dst(ppc);
	
	flushRegisters();
	
#ifdef INTERPRET_JALR
	genJumpTo(mapRegister(MIPS_GET_RS(mips)), JUMPTO_REG);
#else // INTERPRET_JALR
	// TODO: jalr
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		isDelaySlot = 1;
		// Step over the already executed delay slot if we ever
		//   actually use the real LR for JAL
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_JALR
	return INTERPRETED;
#else // INTERPRET_JALR
	return CONVERT_ERROR;
#endif
}

static int SYSCALL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SYSCALL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SYSCALL
	// TODO: syscall
	return CONVERT_ERROR;
#endif
}

static int BREAK(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_BREAK
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_BREAK
	return CONVERT_ERROR;
#endif
}

static int SYNC(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SYNC
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SYNC
	return CONVERT_ERROR;
#endif
}

static int MFHI(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	// mr rd, hi
	GEN_ORI(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        mapRegister( MIPS_REG_HI ),
	        0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MTHI(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	// mr hi, rs
	GEN_ORI(ppc,
	        mapRegisterNew( MIPS_REG_HI ),
	        mapRegister( MIPS_GET_RS(mips) ),
	        0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MFLO(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	// mr rd, lo
	GEN_ORI(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        mapRegister( MIPS_REG_LO ),
	        0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MTLO(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	// mr lo, rs
	GEN_ORI(ppc,
	        mapRegisterNew( MIPS_REG_LO ),
	        mapRegister( MIPS_GET_RS(mips) ),
	        0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MULT(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_MULT
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_MULT
	// FIXME: Sign extend into MSW of hi/lo?
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't multiply if they're using r0
	if(rs && rt){
		// mullw lo, rs, rt
		GEN_MULLW(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// mulhw hi, rs, rt
		GEN_MULHW(ppc, hi, rs, rt);
		set_next_dst(ppc);
	} else {
		// li lo, 0
		GEN_LI(ppc, lo, 0, 0);
		set_next_dst(ppc);
		// li hi, 0
		GEN_LI(ppc, hi, 0, 0);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int MULTU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_MULTU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_MULTU
	// FIXME: Sign extend into MSW of hi/lo?
	int rs = MIPS_GET_RS(mips);
	int rt = MIPS_GET_RT(mips);
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't multiply if they're using r0
	if(rs && rt){
		// mullw lo, rs, rt
		GEN_MULLW(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// mulhwu hi, rs, rt
		GEN_MULHWU(ppc, hi, rs, rt);
		set_next_dst(ppc);
	} else {
		// li lo, 0
		GEN_LI(ppc, lo, 0, 0);
		set_next_dst(ppc);
		// li hi, 0
		GEN_LI(ppc, hi, 0, 0);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int DIV(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DIV
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DIV
	// This instruction computes the quotient and remainder
	//   and stores the results in lo and hi respectively
	int rs = MIPS_GET_RS(mips);
	int rt = MIPS_GET_RT(mips);
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't divide if they're using r0
	if(rs && rt){
		// divw lo, rs, rt
		GEN_DIVW(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// This is how you perform a mod in PPC
		// divw lo, rs, rt
		// NOTE: We already did that
		// mullw hi, lo, rt
		GEN_MULLW(ppc, hi, lo, rt);
		set_next_dst(ppc);
		// subf hi, hi, rs
		GEN_SUBF(ppc, hi, hi, rs);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int DIVU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DIVU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DIVU
	// This instruction computes the quotient and remainder
	//   and stores the results in lo and hi respectively
	int rs = MIPS_GET_RS(mips);
	int rt = MIPS_GET_RT(mips);
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't divide if they're using r0
	if(rs && rt){
		// divwu lo, rs, rt
		GEN_DIVWU(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// This is how you perform a mod in PPC
		// divw lo, rs, rt
		// NOTE: We already did that
		// mullw hi, lo, rt
		GEN_MULLW(ppc, hi, lo, rt);
		set_next_dst(ppc);
		// subf hi, hi, rs
		GEN_SUBF(ppc, hi, hi, rs);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int DSLLV(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else  // INTERPRET_DW
	// TODO: dsllv
	return CONVERT_ERROR;
#endif
}

static int DSRLV(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else  // INTERPRET_DW
	// TODO: dsrlv
	return CONVERT_ERROR;
#endif
}

static int DSRAV(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else  // INTERPRET_DW
	// TODO: dsrav
	return CONVERT_ERROR;
#endif
}

static int DMULT(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dmult
	return CONVERT_ERROR;
#endif
}

static int DMULTU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dmultu
	return CONVERT_ERROR;
#endif
}

static int DDIV(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: ddiv
	return CONVERT_ERROR;
#endif
}

static int DDIVU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: ddivu
	return CONVERT_ERROR;
#endif
}

static int DADDU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: daddu
	return CONVERT_ERROR;
#endif
}

static int DADD(MIPS_instr mips){
	return DADDU(mips);
}

static int DSUBU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsubu
	return CONVERT_ERROR;
#endif
}

static int DSUB(MIPS_instr mips){
	return DSUBU(mips);
}

static int DSLL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsll
	return CONVERT_ERROR;
#endif
}

static int DSRL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsrl
	return CONVERT_ERROR;
#endif
}

static int DSRA(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsra
	return CONVERT_ERROR;
#endif
}

static int DSLL32(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsll32
	return CONVERT_ERROR;
#endif
}

static int DSRL32(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsrl32
	return CONVERT_ERROR;
#endif
}

static int DSRA32(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW
	// TODO: dsra32
	return CONVERT_ERROR;
#endif
}

static int ADDU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ADD(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int ADD(MIPS_instr mips){
	return ADDU(mips);
}

static int SUBU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SUB(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SUB(MIPS_instr mips){
	return SUBU(mips);
}

static int AND(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_AND(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int OR(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_OR(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int XOR(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_XOR(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int NOR(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_NOR(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SLT(MIPS_instr mips){
	PowerPC_instr ppc;
	int shiftSrc;
	// If rt != r0: rd <- rs - rt
	if( MIPS_GET_RT(mips) ){
		int rt = mapRegister( MIPS_GET_RT(mips) );
		int rs = mapRegister( MIPS_GET_RS(mips) );
		shiftSrc = mapRegisterNew( MIPS_GET_RD(mips) );
		GEN_SUB(ppc,
		        shiftSrc,
		        rs,
		        rt);
		set_next_dst(ppc);
	} else shiftSrc = mapRegister( MIPS_GET_RS(mips) );
	// Shift the sign bit to the LSb
	GEN_SRW(ppc, mapRegisterNew( MIPS_GET_RD(mips) ), shiftSrc, 31);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SLTU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rd = mapRegisterNew( MIPS_GET_RD(mips) );
	// rd <- rs - rt
	GEN_SUB(ppc,
	        rd,
	        rs,
	        rt);
	set_next_dst(ppc);
	// Shift the sign bit to the LSb
	GEN_SRWI(ppc, rd, rd, 31);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int TEQ(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_TRAPS
	genCallInterp(mips);
	return INTERPRETED;
#else
	return CONVERT_ERROR;
#endif
}

static int (*gen_special[64])(MIPS_instr) =
{
   SLL , NI   , SRL , SRA , SLLV   , NI    , SRLV  , SRAV  ,
   JR  , JALR , NI  , NI  , SYSCALL, BREAK , NI    , SYNC  ,
   MFHI, MTHI , MFLO, MTLO, DSLLV  , NI    , DSRLV , DSRAV ,
   MULT, MULTU, DIV , DIVU, DMULT  , DMULTU, DDIV  , DDIVU ,
   ADD , ADDU , SUB , SUBU, AND    , OR    , XOR   , NOR   ,
   NI  , NI   , SLT , SLTU, DADD   , DADDU , DSUB  , DSUBU ,
   NI  , NI   , NI  , NI  , TEQ    , NI    , NI    , NI    ,
   DSLL, NI   , DSRL, DSRA, DSLL32 , NI    , DSRL32, DSRA32
};

static int SPECIAL(MIPS_instr mips){
	return gen_special[MIPS_GET_FUNC(mips)](mips);
}

// -- RegImmed Instructions --

// Since the RegImmed instructions are very similar:
//   BLTZ, BGEZ, BLTZL, BGEZL, BLZAL, BGEZAL, BLTZALL, BGEZALL
//   It's less work to handle them all in one function
static int REGIMM(MIPS_instr mips){
	PowerPC_instr  ppc;
	int which = MIPS_GET_RT(mips);
	int cond   = which & 1; // t = GE, f = LT
	int likely = which & 2;
	int link   = which & 16;
	int likely_id = -1;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 7);
	set_next_dst(ppc);
	
	if(likely){
		flushRegisters();
		
		// b[!cond] <past jumpto & delay>
		likely_id = add_jump_special(0);
		GEN_BC(ppc, likely_id, 0, 0,
		       cond ? 0xc : 0x4, 28); 
		set_next_dst(ppc);
	}
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_REGIMM
	if(likely)
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	else {
		// b[!cond] <past jumpto & delay>
		GEN_BC(ppc, JUMPTO_OFF_SIZE+delaySlot+2,
		       0, 0, cond ? 0xc : 0x4, 28);
		set_next_dst(ppc);
	}
	if(link){
		// Set LR to next instruction
		int lr = mapRegisterNew(MIPS_REG_LR);
		// lis	lr, pc@ha(0)
		GEN_LIS(ppc, lr, get_src_pc()>>16);
		set_next_dst(ppc);
		// la	lr, pc@l(lr)
		GEN_LI(ppc, lr, lr, get_src_pc());
		set_next_dst(ppc);
		
		flushRegisters();
	}
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_REGIMM
	// TODO: Link behavior if(link)
	// If we're jumping out, we need to make random comments
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		if(likely)
			set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		else {
			// b[!cond] <past jumpto & delay>
			GEN_BC(ppc, JUMPTO_OFF_SIZE+delaySlot+2,
			       0, 0, cond ? 0xc : 0x4, 28);
			set_next_dst(ppc);
		}
		if(link){
			// Set LR to next instruction
			int lr = mapRegisterNew(MIPS_REG_LR);
			// lis	lr, pc@ha(0)
			GEN_LIS(ppc, lr, get_src_pc()>>16);
			set_next_dst(ppc);
			// la	lr, pc@l(lr)
			GEN_LI(ppc, lr, lr, get_src_pc());
			set_next_dst(ppc);
			
			flushRegisters();
		}
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else if(likely)
		// TODO: set_jump_special
		;
	// The actual branch
	GEN_BC(ppc,
	       add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, j_out),
	       0, 0,
	       cond ? 0x4 : 0xc, 28);
	set_next_dst(ppc);
	// If we're jumping out, we need space to 0 r0
	if(j_out) set_next_dst(PPC_NOP);
#endif
	
	if(!likely){
		// Step over the already executed delay slot if
		//   the branch isn't taken, not necessary for
		//   a likely branch because it'll be skipped
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	}
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_REGIMM
	return INTERPRETED;
#else // INTERPRET_REGIMM
	return CONVERT_SUCCESS;
#endif
}

// -- COP0 Instructions --

/*
static int (*gen_cop0[32])(MIPS_instr) =
{
   MFC0, NI, NI, NI, MTC0, NI, NI, NI,
   NI  , NI, NI, NI, NI  , NI, NI, NI,
   TLB , NI, NI, NI, NI  , NI, NI, NI,
   NI  , NI, NI, NI, NI  , NI, NI, NI
};
*/

static int COP0(MIPS_instr mips){
#ifdef INTERPRET_COP0
	genCallInterp(mips);
	return INTERPRETED;
#else
	// TODO: COP0 instructions
	return CONVERT_ERROR;
#endif
}

// -- COP1 Instructions --

static int MFC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: mfc1
#endif
}

static int DMFC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: dmfc1
#endif
}

static int CFC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: cfc1
#endif
}

static int MTC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: mtc1
#endif
}

static int DMTC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: dmtc1
#endif
}

static int CTC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: ctc1
#endif
}

static int BC(MIPS_instr mips){
	PowerPC_instr ppc;
	int cond   = mips & 0x00010000;
	int likely = mips & 0x00020000;
	int likely_id = -1;
	// Note: we use CR bits 20-27 (CRs 5&6) to store N64 CCs
#ifdef INTERPRET_FP
	// Load the value from FCR31 and use that to set condition
	extern long FCR31;
	// mtctr r3
	GEN_MTCTR(ppc, 3);
	set_next_dst(ppc);
	// la r3, &FCR31
	GEN_LIS(ppc, 3, ((unsigned int)&FCR31)>>16);
	set_next_dst(ppc);
	GEN_LI(ppc, 3, 3, (unsigned int)&FCR31);
	set_next_dst(ppc);
	// lwz r3, 0(r3)
	GEN_LWZ(ppc, 3, 0, 3);
	set_next_dst(ppc);
	// The intention here is to shift the MSb
	//   to the bit that will be checked by the bc
	// srwi r3, r3, 20+MIPS_GET_CC(mips)
	GEN_SRWI(ppc, 3, 3, 20+MIPS_GET_CC(mips));
	set_next_dst(ppc);
	// FIXME: This destroys other CCs
	// mtcrf cr5 & cr6, 3
	ppc = NEW_PPC_INSTR();
	PPC_SET_OPCODE(ppc, PPC_OPCODE_X);
	PPC_SET_FUNC  (ppc, PPC_FUNC_MTCRF);
	PPC_SET_RD    (ppc, 3);
	ppc |= 0x06 << 12; // Set CRM to CR5
	set_next_dst(ppc);
	// mfctr r3
	GEN_MFCTR(ppc, 3);
	set_next_dst(ppc);
#endif
	
	if(likely){
		// b[!cond] <past jumpto & delay>
		likely_id = add_jump_special(0);
		GEN_BC(ppc, likely_id, 0, 0,
		       cond ? 0x4 : 0xc,
		       20+MIPS_GET_CC(mips)); 
		set_next_dst(ppc);
	}
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_BC
	if(likely)
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	else {
		// b[!cond] <past jumpto & delay> 
		GEN_BC(ppc, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0,
		       cond ? 0x4 : 0xc,
		       20+MIPS_GET_CC(mips)); 
		set_next_dst(ppc);
	}
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BC
	// If we're jumping out, we need pizza
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		if(likely)
			// Jump over the generated jump, and both delay slots
			set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		else {
			// b[!cond] <past jumpto & delay> 
			GEN_BC(ppc, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0,
			       cond ? 0x4 : 0xc,
			       20+MIPS_GET_CC(mips)); 
			set_next_dst(ppc);
		}
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else if(likely){
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, 1+2*delaySlot+1);
	
		// b[cond] <dest> 
		GEN_BC(ppc, 
		       add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, j_out),
		       0, 0,
		       cond ? 0xc : 0x4,
		       20+MIPS_GET_CC(mips));
		set_next_dst(ppc);
	}
#endif
	
	if(!likely){
		// Step over the already executed delay slot if
		//   the branch isn't taken, not necessary for
		//   a likely branch because it'll be skipped
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	}
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); isDelaySlot = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_BC
	return INTERPRETED;
#else // INTERPRET_BC
	return CONVERT_SUCCESS;
#endif
}

static int S(MIPS_instr mips){
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: single-precision FP
	return CONVERT_ERROR;
#endif
}

static int D(MIPS_instr mips){
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: double-precision FP
	return CONVERT_ERROR;
#endif
}

static int W(MIPS_instr mips){
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: integer FP
	return CONVERT_ERROR;
#endif
}

static int L(MIPS_instr mips){
#ifdef INTERPRET_FP
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: long-integer FP
	return CONVERT_ERROR;
#endif
}

static int (*gen_cop1[32])(MIPS_instr) =
{
   MFC1, DMFC1, CFC1, NI, MTC1, DMTC1, CTC1, NI,
   BC  , NI   , NI  , NI, NI  , NI   , NI  , NI,
   S   , D    , NI  , NI, W   , L    , NI  , NI,
   NI  , NI   , NI  , NI, NI  , NI   , NI  , NI
};

static int COP1(MIPS_instr mips){
	return gen_cop1[MIPS_GET_RS(mips)](mips);
}

static int (*gen_ops[64])(MIPS_instr) =
{
   SPECIAL, REGIMM, J   , JAL  , BEQ , BNE , BLEZ , BGTZ ,
   ADDI   , ADDIU , SLTI, SLTIU, ANDI, ORI , XORI , LUI  ,
   COP0   , COP1  , NI  , NI   , BEQL, BNEL, BLEZL, BGTZL,
   DADDI  , DADDIU, LDL , LDR  , NI  , NI  , NI   , NI   ,
   LB     , LH    , LWL , LW   , LBU , LHU , LWR  , LWU  ,
   SB     , SH    , SWL , SW   , SDL , SDR , SWR  , CACHE,
   LL     , LWC1  , NI  , NI   , NI  , LDC1, NI   , LD   ,
   SC     , SWC1  , NI  , NI   , NI  , SDC1, NI   , SD
};



static void genCallInterp(MIPS_instr mips){
	PowerPC_instr ppc = NEW_PPC_INSTR();
	flushRegisters();
	// Save the lr
	GEN_MFLR(ppc, 0);
	set_next_dst(ppc);
	GEN_STW(ppc, 0, 4, 1);
	set_next_dst(ppc);
	// Load the address of decodeNInterpret
	GEN_LIS(ppc, 3, ((unsigned int)decodeNInterpret)>>16);
	set_next_dst(ppc);
	GEN_LI(ppc, 3, 3, (unsigned int)decodeNInterpret);
	set_next_dst(ppc);
	// Move it to ctr for a bctr
	GEN_MTCTR(ppc, 3);
	set_next_dst(ppc);
	// Load our argument into r3 (mips)
	GEN_LIS(ppc, 3, mips>>16);
	set_next_dst(ppc);
	GEN_LI(ppc, 3, 3, mips);
	set_next_dst(ppc);
	// Load the current PC as the second arg
	GEN_LIS(ppc, 4, get_src_pc()>>16);
	set_next_dst(ppc);
	GEN_LI(ppc, 4, 4, get_src_pc());
	set_next_dst(ppc);
	// Branch to decodeNInterpret
	GEN_BCTRL(ppc);
	set_next_dst(ppc);
	// Restore the lr
	GEN_LWZ(ppc, 0, 4, 1);
	set_next_dst(ppc);
	GEN_MTLR(ppc, 0);
	set_next_dst(ppc);
	// if decodeNInterpret returned an address
	//   jumpTo it
	GEN_CMPI(ppc, 3, 0, 6);
	set_next_dst(ppc);
	GEN_BNE(ppc, 6, add_jump(-1, 0, 1), 0, 0);
	set_next_dst(ppc);
}

static void genJumpTo(unsigned int loc, unsigned int type){
	PowerPC_instr ppc = NEW_PPC_INSTR();
	
	if(type == JUMPTO_REG){
		// Load the register as the return value
		GEN_LWZ(ppc, 3, loc*8+4, 13);
		set_next_dst(ppc);
	} else {
		// Calculate the destination address
		loc <<= 2;
		if(type == JUMPTO_OFF) loc += get_src_pc();
		else loc |= get_src_pc() & 0xf0000000;
		// Load the address as the return value
		GEN_LIS(ppc, 3, loc >> 16);
		set_next_dst(ppc);
		GEN_LI(ppc, 3, 3, loc);
		set_next_dst(ppc);
	}
	
	// Branch to the jump pad
	GEN_B(ppc, add_jump(loc, 1, 1), 0, 0);
	set_next_dst(ppc);
}

static int mips_is_jump(MIPS_instr instr){
	int opcode = MIPS_GET_OPCODE(instr);
	int func   = MIPS_GET_RT    (instr);
	return (opcode == MIPS_OPCODE_J     ||
                opcode == MIPS_OPCODE_JAL   ||
                opcode == MIPS_OPCODE_BEQ   ||
                opcode == MIPS_OPCODE_BNE   ||
                opcode == MIPS_OPCODE_BLEZ  ||
                opcode == MIPS_OPCODE_BGTZ  ||
                opcode == MIPS_OPCODE_BEQL  ||
                opcode == MIPS_OPCODE_BNEL  ||
                opcode == MIPS_OPCODE_BLEZL ||
                opcode == MIPS_OPCODE_BGTZL ||
               (opcode == MIPS_OPCODE_B     &&
                 (func == MIPS_RT_BLTZ      ||
                  func == MIPS_RT_BGEZ      ||
                  func == MIPS_RT_BLTZL     ||
                  func == MIPS_RT_BGEZL     ||
                  func == MIPS_RT_BLTZAL    ||
                  func == MIPS_RT_BLTZALL   ||
                  func == MIPS_RT_BGEZALL)));
}
