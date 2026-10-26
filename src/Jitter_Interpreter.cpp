#include "Jitter.h"
#include "Jitter_Interpreter.h"

#include <cstring>
#include <memory>
#include <unordered_map>

using namespace Jitter;

#if defined(__EMSCRIPTEN__)

#include <wasm_simd128.h>
#include "Jitter_CodeGen_Wasm.h"

//Every operation below reproduces what CCodeGen_Wasm emits for it (same wasm instructions, through the
//compiler's builtins and SIMD intrinsics), so interpreted and compiled code give the same results.

bool CInterpreterProgram::GetCallSignature(const char* signature, CALL_SIGNATURE& result)
{
	static const char* const signatures[SIG_MAX] = {"vi", "iii", "jii", "viii", "viji", "iiii", "jiji", "vii", "ii", "ji", "viiii", "iiiii"};
	for(uint32 i = 0; i < SIG_MAX; i++)
	{
		if(!strcmp(signatures[i], signature))
		{
			result = static_cast<CALL_SIGNATURE>(i);
			return true;
		}
	}
	return false;
}

static bool MakeOperand(const SymbolRefPtr& symbolRef, CInterpreterProgram::OPERAND& operand, std::unordered_map<uint32, uint32>& temporaries)
{
	if(!symbolRef) return true;
	auto symbol = symbolRef->GetSymbol().get();
	operand.value = symbol->m_valueLow;
	operand.valueHi = symbol->m_valueHigh;
	switch(symbol->m_type)
	{
	case SYM_CONTEXT:
		operand.kind = CInterpreterProgram::OPERAND_CONTEXT;
		operand.size = 4;
		break;
	case SYM_CONSTANT:
	case SYM_CONSTANTPTR:
		operand.kind = CInterpreterProgram::OPERAND_CONSTANT;
		operand.size = 4;
		break;
	case SYM_CONSTANT64:
		operand.kind = CInterpreterProgram::OPERAND_CONSTANT64;
		operand.size = 8;
		break;
	case SYM_RELATIVE:
	case SYM_FP_RELATIVE32:
		operand.kind = CInterpreterProgram::OPERAND_RELATIVE;
		operand.size = 4;
		break;
	case SYM_RELATIVE64:
		operand.kind = CInterpreterProgram::OPERAND_RELATIVE;
		operand.size = 8;
		break;
	case SYM_RELATIVE128:
		operand.kind = CInterpreterProgram::OPERAND_RELATIVE;
		operand.size = 16;
		break;
	case SYM_REL_REFERENCE:
		operand.kind = CInterpreterProgram::OPERAND_RELREF;
		operand.size = 4;
		break;
	case SYM_TEMPORARY:
	case SYM_TMP_REFERENCE:
	case SYM_FP_TEMPORARY32:
	case SYM_TEMPORARY64:
	case SYM_TEMPORARY128:
	case SYM_TEMPORARY256:
	{
		operand.kind = CInterpreterProgram::OPERAND_TEMPORARY;
		operand.size = (symbol->m_type == SYM_TEMPORARY64) ? 8 : (symbol->m_type == SYM_TEMPORARY128) ? 16 : (symbol->m_type == SYM_TEMPORARY256) ? 32 : 4;
		//Temporaries are numbered by the Jitter across all types: one 32-byte slot each
		auto slot = temporaries.emplace(symbol->m_valueLow, static_cast<uint32>(temporaries.size())).first->second;
		operand.value = slot * 32;
	}
	break;
	default:
		return false;
	}
	return true;
}

std::shared_ptr<const CInterpreterProgram> CJitter::BuildInterpreterProgram() const
{
	auto program = std::make_shared<CInterpreterProgram>();
	auto& instructions = program->m_instructions;
	std::unordered_map<uint32, uint32> temporaries;
	std::unordered_map<uint32, uint32> blockStarts;
	for(const auto& basicBlock : m_basicBlocks)
	{
		blockStarts[basicBlock.id] = static_cast<uint32>(instructions.size());
		for(const auto& statement : basicBlock.statements)
		{
			CInterpreterProgram::INSTRUCTION instruction;
			instruction.op = static_cast<uint16>(statement.op);
			instruction.condition = static_cast<uint8>(statement.jmpCondition);
			if(!MakeOperand(statement.dst, instruction.dst, temporaries) ||
			   !MakeOperand(statement.src1, instruction.src1, temporaries) ||
			   !MakeOperand(statement.src2, instruction.src2, temporaries) ||
			   !MakeOperand(statement.src3, instruction.src3, temporaries))
			{
				return nullptr;
			}
			switch(statement.op)
			{
			case OP_JMP:
			case OP_CONDJMP:
				instruction.target = statement.jmpBlock;
				break;
			case OP_GOTO:
			{
				auto labelIterator = m_labels.find(statement.jmpBlock);
				if(labelIterator == std::end(m_labels)) return nullptr;
				instruction.op = OP_JMP;
				instruction.target = labelIterator->second;
			}
			break;
			case OP_CALL:
			{
				auto fctInfo = CWasmFunctionRegistry::FindFunction(statement.src1->GetSymbol()->m_valueLow);
				CInterpreterProgram::CALL_SIGNATURE signature;
				if(!fctInfo || !CInterpreterProgram::GetCallSignature(fctInfo->signature.c_str(), signature)) return nullptr;
				instruction.signature = signature;
			}
			break;
			case OP_PARAM_RET:
			case OP_CMPSELECT_P1:
			case OP_CMPSELECT_P2:
			case OP_EXTERNJMP_DYN:
			case OP_LABEL:
				return nullptr;
			default:
				break;
			}
			instructions.push_back(instruction);
		}
	}
	for(auto& instruction : instructions)
	{
		if((instruction.op == OP_JMP) || (instruction.op == OP_CONDJMP))
		{
			auto blockIterator = blockStarts.find(instruction.target);
			if(blockIterator == std::end(blockStarts)) return nullptr;
			instruction.target = blockIterator->second;
		}
	}
	program->m_temporaryBytes = static_cast<uint32>(temporaries.size()) * 32;
	return program;
}

namespace
{
	template <typename T>
	inline T LoadMem(uintptr_t address)
	{
		T value;
		memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
		return value;
	}

	template <typename T>
	inline void StoreMem(uintptr_t address, T value)
	{
		memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
	}

	inline float AsFloat(uint32 value)
	{
		float result;
		memcpy(&result, &value, 4);
		return result;
	}

	inline uint32 AsBits(float value)
	{
		uint32 result;
		memcpy(&result, &value, 4);
		return result;
	}

	inline uint32 Clz32(uint32 value)
	{
		return value ? __builtin_clz(value) : 32;
	}

	template <typename U, typename S>
	inline bool TestCondition(uint8 condition, U a, U b)
	{
		switch(condition)
		{
		case CONDITION_EQ:
			return a == b;
		case CONDITION_NE:
			return a != b;
		case CONDITION_BL:
			return a < b;
		case CONDITION_BE:
			return a <= b;
		case CONDITION_AB:
			return a > b;
		case CONDITION_AE:
			return a >= b;
		case CONDITION_LT:
			return static_cast<S>(a) < static_cast<S>(b);
		case CONDITION_LE:
			return static_cast<S>(a) <= static_cast<S>(b);
		case CONDITION_GT:
			return static_cast<S>(a) > static_cast<S>(b);
		case CONDITION_GE:
			return static_cast<S>(a) >= static_cast<S>(b);
		default:
			return false;
		}
	}

	//shuffle(a, b) taking word i from b when bit i of mask is set (MdBlendValues)
	inline v128_t Blend(v128_t a, v128_t b, uint8 mask)
	{
		alignas(16) uint32 wa[4], wb[4];
		wasm_v128_store(wa, a);
		wasm_v128_store(wb, b);
		for(uint32 i = 0; i < 4; i++)
		{
			if(mask & (1 << i)) wa[i] = wb[i];
		}
		return wasm_v128_load(wa);
	}

	inline v128_t ClampS(v128_t value)
	{
		value = wasm_i32x4_min(value, wasm_i32x4_splat(0x7F7FFFFF));
		return wasm_u32x4_min(value, wasm_i32x4_splat(static_cast<int32>(0xFF7FFFFF)));
	}
}

void CInterpreterProgram::Run(void* context) const
{
	const uintptr_t ctx = reinterpret_cast<uintptr_t>(context);
	//Temporaries (zeroed like wasm locals) live in a per-thread arena, not on the stack: big blocks (VU) have
	//tens of KB of them and threads have 64 KB stacks. Runs nest (an EE block starting a VU0 microprogram).
	constexpr size_t ARENA_SIZE = 1024 * 1024;
	thread_local std::unique_ptr<uint8[]> arenaMemory;
	thread_local uint8* arena = nullptr; //16-byte aligned
	thread_local size_t arenaTop = 0;
	if(!arena)
	{
		arenaMemory.reset(new uint8[ARENA_SIZE + 16]);
		arena = reinterpret_cast<uint8*>((reinterpret_cast<uintptr_t>(arenaMemory.get()) + 15) & ~uintptr_t(15));
	}
	const size_t temporaryBytes = (m_temporaryBytes + 15) & ~size_t(15);
	std::unique_ptr<uint8[]> overflow;
	uint8* temporaryArea = nullptr;
	if(arenaTop + temporaryBytes <= ARENA_SIZE)
	{
		temporaryArea = arena + arenaTop;
	}
	else
	{
		overflow.reset(new uint8[temporaryBytes + 16]);
		temporaryArea = reinterpret_cast<uint8*>((reinterpret_cast<uintptr_t>(overflow.get()) + 15) & ~uintptr_t(15));
	}
	const size_t savedTop = arenaTop;
	if(!overflow) arenaTop += temporaryBytes;
	struct ARENA_RESTORE
	{
		size_t& top;
		size_t value;
		~ARENA_RESTORE() { top = value; }
	} arenaRestore{arenaTop, savedTop};
	memset(temporaryArea, 0, temporaryBytes);
	const uintptr_t tmp = reinterpret_cast<uintptr_t>(temporaryArea);
	const auto& pageHandlers = CCodeGen_Wasm::GetPageAccessHandlers();

	auto address = [&](const OPERAND& operand) -> uintptr_t {
		return ((operand.kind == OPERAND_RELATIVE) ? ctx : tmp) + operand.value;
	};
	auto r32 = [&](const OPERAND& operand) -> uint32 {
		switch(operand.kind)
		{
		case OPERAND_CONTEXT:
			return static_cast<uint32>(ctx);
		case OPERAND_CONSTANT:
		case OPERAND_CONSTANT64:
			return operand.value;
		case OPERAND_RELATIVE:
		case OPERAND_RELREF:
			return LoadMem<uint32>(ctx + operand.value);
		case OPERAND_TEMPORARY:
			return LoadMem<uint32>(tmp + operand.value);
		default:
			__builtin_trap();
		}
	};
	auto r64 = [&](const OPERAND& operand) -> uint64 {
		switch(operand.kind)
		{
		case OPERAND_CONSTANT:
			return operand.value;
		case OPERAND_CONSTANT64:
			return static_cast<uint64>(operand.value) | (static_cast<uint64>(operand.valueHi) << 32);
		default:
			return LoadMem<uint64>(address(operand));
		}
	};
	auto r128 = [&](const OPERAND& operand) -> v128_t {
		return wasm_v128_load(reinterpret_cast<const void*>(address(operand)));
	};
	auto rf = [&](const OPERAND& operand) -> float { return AsFloat(r32(operand)); };
	auto w32 = [&](const OPERAND& operand, uint32 value) { StoreMem<uint32>(address(operand), value); };
	auto w64 = [&](const OPERAND& operand, uint64 value) { StoreMem<uint64>(address(operand), value); };
	auto w128 = [&](const OPERAND& operand, v128_t value) { wasm_v128_store(reinterpret_cast<void*>(address(operand)), value); };
	auto wf = [&](const OPERAND& operand, float value) { w32(operand, AsBits(value)); };

	const OPERAND* params[8];
	uint32 paramCount = 0;
	uint64 returnValue = 0;

	const INSTRUCTION* code = m_instructions.data();
	const uint32 count = static_cast<uint32>(m_instructions.size());
	uint32 pc = 0;
	while(pc < count)
	{
		const auto& i = code[pc++];
		switch(i.op)
		{
		case OP_NOP:
		case OP_FP_SETROUNDINGMODE:
			break;
		case OP_BREAK:
			__builtin_trap();
		case OP_MOV:
			switch(i.dst.size)
			{
			case 4:
				w32(i.dst, r32(i.src1));
				break;
			case 8:
				w64(i.dst, r64(i.src1));
				break;
			case 16:
				w128(i.dst, r128(i.src1));
				break;
			default:
				__builtin_trap();
			}
			break;
		case OP_ADD:
			w32(i.dst, r32(i.src1) + r32(i.src2));
			break;
		case OP_SUB:
			w32(i.dst, r32(i.src1) - r32(i.src2));
			break;
		case OP_AND:
			w32(i.dst, r32(i.src1) & r32(i.src2));
			break;
		case OP_OR:
			w32(i.dst, r32(i.src1) | r32(i.src2));
			break;
		case OP_XOR:
			w32(i.dst, r32(i.src1) ^ r32(i.src2));
			break;
		case OP_NOT:
			w32(i.dst, ~r32(i.src1));
			break;
		case OP_SLL:
			w32(i.dst, r32(i.src1) << (r32(i.src2) & 31));
			break;
		case OP_SRL:
			w32(i.dst, r32(i.src1) >> (r32(i.src2) & 31));
			break;
		case OP_SRA:
			w32(i.dst, static_cast<uint32>(static_cast<int32>(r32(i.src1)) >> (r32(i.src2) & 31)));
			break;
		case OP_LZC:
		{
			uint32 value = r32(i.src1);
			w32(i.dst, (static_cast<int32>(value) < 0) ? (Clz32(~value) - 1) : (Clz32(value) - 1));
		}
		break;
		case OP_CMP:
			w32(i.dst, TestCondition<uint32, int32>(i.condition, r32(i.src1), r32(i.src2)) ? 1 : 0);
			break;
		case OP_SELECT:
		{
			uint32 a = r32(i.src2), b = r32(i.src3);
			w32(i.dst, r32(i.src1) ? a : b);
		}
		break;
		case OP_MUL:
			w64(i.dst, static_cast<uint64>(r32(i.src1)) * static_cast<uint64>(r32(i.src2)));
			break;
		case OP_MULS:
			w64(i.dst, static_cast<uint64>(static_cast<int64>(static_cast<int32>(r32(i.src1))) * static_cast<int64>(static_cast<int32>(r32(i.src2)))));
			break;
		case OP_DIV:
		{
			uint32 a = r32(i.src1), b = r32(i.src2);
			w64(i.dst, static_cast<uint64>(a / b) | (static_cast<uint64>(a % b) << 32));
		}
		break;
		case OP_DIVS:
		{
			int32 a = static_cast<int32>(r32(i.src1)), b = static_cast<int32>(r32(i.src2));
			w64(i.dst, static_cast<uint64>(static_cast<uint32>(a / b)) | (static_cast<uint64>(static_cast<uint32>(a % b)) << 32));
		}
		break;
		case OP_RELTOREF:
			w32(i.dst, r32(i.src1) + static_cast<uint32>(ctx));
			break;
		case OP_ADDREF:
			w32(i.dst, r32(i.src1) + r32(i.src2));
			break;
		case OP_ISREFNULL:
			w32(i.dst, (r32(i.src1) == 0) ? 1 : 0);
			break;
		case OP_LOADFROMREF:
		{
			uintptr_t base = r32(i.src1);
			if(i.src2.kind != OPERAND_NONE) base += r32(i.src2) * ((i.condition == 4) ? 4 : 1);
			switch(i.dst.size)
			{
			case 4:
				w32(i.dst, LoadMem<uint32>(base));
				break;
			case 8:
				w64(i.dst, LoadMem<uint64>(base));
				break;
			case 16:
				w128(i.dst, wasm_v128_load(reinterpret_cast<const void*>(base)));
				break;
			default:
				__builtin_trap();
			}
		}
		break;
		case OP_LOAD8FROMREF:
		{
			uintptr_t base = r32(i.src1);
			if(i.src2.kind != OPERAND_NONE) base += r32(i.src2);
			w32(i.dst, LoadMem<uint8>(base));
		}
		break;
		case OP_LOAD16FROMREF:
		{
			uintptr_t base = r32(i.src1);
			if(i.src2.kind != OPERAND_NONE) base += r32(i.src2);
			w32(i.dst, LoadMem<uint16>(base));
		}
		break;
		case OP_STOREATREF:
		case OP_STORE8ATREF:
		case OP_STORE16ATREF:
		{
			uintptr_t base = r32(i.src1);
			const OPERAND* value = &i.src2;
			if(i.src3.kind != OPERAND_NONE)
			{
				base += r32(i.src2) * ((i.condition == 4) ? 4 : 1);
				value = &i.src3;
			}
			if(i.op == OP_STORE8ATREF)
			{
				StoreMem<uint8>(base, static_cast<uint8>(r32(*value)));
			}
			else if(i.op == OP_STORE16ATREF)
			{
				StoreMem<uint16>(base, static_cast<uint16>(r32(*value)));
			}
			else
			{
				switch(value->size)
				{
				case 4:
					StoreMem<uint32>(base, r32(*value));
					break;
				case 8:
					StoreMem<uint64>(base, r64(*value));
					break;
				case 16:
					wasm_v128_store(reinterpret_cast<void*>(base), r128(*value));
					break;
				default:
					__builtin_trap();
				}
			}
		}
		break;
		case OP_LOADFROMPAGE:
		{
			uint32 size = i.condition;
			uint32 pageAddress = r32(i.src2);
			uint32 table = r32(i.src1);
			uint32 entry = LoadMem<uint32>(table + ((pageAddress >> pageHandlers.pageShift) << 2));
			uintptr_t memAddress = entry + (pageAddress & ((1 << pageHandlers.pageShift) - size));
			typedef uint32 (*Load32Handler)(void*, uint32);
			typedef uint64 (*Load64Handler)(void*, uint32);
			switch(size)
			{
			case 1:
				w32(i.dst, entry ? LoadMem<uint8>(memAddress) : reinterpret_cast<Load32Handler>(pageHandlers.load8)(context, pageAddress));
				break;
			case 2:
				w32(i.dst, entry ? LoadMem<uint16>(memAddress) : reinterpret_cast<Load32Handler>(pageHandlers.load16)(context, pageAddress));
				break;
			case 4:
				w32(i.dst, entry ? LoadMem<uint32>(memAddress) : reinterpret_cast<Load32Handler>(pageHandlers.load32)(context, pageAddress));
				break;
			case 8:
				w64(i.dst, entry ? LoadMem<uint64>(memAddress) : reinterpret_cast<Load64Handler>(pageHandlers.load64)(context, pageAddress));
				break;
			case 16:
				if(entry)
				{
					w128(i.dst, wasm_v128_load(reinterpret_cast<const void*>(memAddress)));
				}
				else
				{
					alignas(16) uint32 words[4];
					for(uint32 w = 0; w < 4; w++)
					{
						words[w] = reinterpret_cast<Load32Handler>(pageHandlers.load32)(context, (pageAddress & ~0x0F) + (w * 4));
					}
					w128(i.dst, wasm_v128_load(words));
				}
				break;
			default:
				__builtin_trap();
			}
		}
		break;
		case OP_STOREATPAGE:
		{
			uint32 size = i.condition;
			uint32 pageAddress = r32(i.src2);
			uint32 table = r32(i.src1);
			uint32 entry = LoadMem<uint32>(table + ((pageAddress >> pageHandlers.pageShift) << 2));
			uintptr_t memAddress = entry + (pageAddress & ((1 << pageHandlers.pageShift) - size));
			typedef void (*Store32Handler)(void*, uint32, uint32);
			typedef void (*Store64Handler)(void*, uint64, uint32);
			switch(size)
			{
			case 1:
				if(entry)
					StoreMem<uint8>(memAddress, static_cast<uint8>(r32(i.src3)));
				else
					reinterpret_cast<Store32Handler>(pageHandlers.store8)(context, r32(i.src3), pageAddress);
				break;
			case 2:
				if(entry)
					StoreMem<uint16>(memAddress, static_cast<uint16>(r32(i.src3)));
				else
					reinterpret_cast<Store32Handler>(pageHandlers.store16)(context, r32(i.src3), pageAddress);
				break;
			case 4:
				if(entry)
					StoreMem<uint32>(memAddress, r32(i.src3));
				else
					reinterpret_cast<Store32Handler>(pageHandlers.store32)(context, r32(i.src3), pageAddress);
				break;
			case 8:
				if(entry)
					StoreMem<uint64>(memAddress, r64(i.src3));
				else
					reinterpret_cast<Store64Handler>(pageHandlers.store64)(context, r64(i.src3), pageAddress);
				break;
			case 16:
				if(entry)
				{
					wasm_v128_store(reinterpret_cast<void*>(memAddress), r128(i.src3));
				}
				else
				{
					alignas(16) uint32 words[4];
					wasm_v128_store(words, r128(i.src3));
					for(uint32 w = 0; w < 4; w++)
					{
						reinterpret_cast<Store32Handler>(pageHandlers.store32)(context, words[w], (pageAddress & ~0x0F) + (w * 4));
					}
				}
				break;
			default:
				__builtin_trap();
			}
		}
		break;

		//64 bits
		case OP_ADD64:
			w64(i.dst, r64(i.src1) + r64(i.src2));
			break;
		case OP_SUB64:
			w64(i.dst, r64(i.src1) - r64(i.src2));
			break;
		case OP_AND64:
			w64(i.dst, r64(i.src1) & r64(i.src2));
			break;
		case OP_SLL64:
			w64(i.dst, r64(i.src1) << (r32(i.src2) & 63));
			break;
		case OP_SRL64:
			w64(i.dst, r64(i.src1) >> (r32(i.src2) & 63));
			break;
		case OP_SRA64:
			w64(i.dst, static_cast<uint64>(static_cast<int64>(r64(i.src1)) >> (r32(i.src2) & 63)));
			break;
		case OP_CMP64:
			w32(i.dst, TestCondition<uint64, int64>(i.condition, r64(i.src1), r64(i.src2)) ? 1 : 0);
			break;
		case OP_MERGETO64:
			w64(i.dst, static_cast<uint64>(r32(i.src1)) | (static_cast<uint64>(r32(i.src2)) << 32));
			break;
		case OP_EXTLOW64:
			w32(i.dst, static_cast<uint32>(r64(i.src1)));
			break;
		case OP_EXTHIGH64:
			w32(i.dst, static_cast<uint32>(r64(i.src1) >> 32));
			break;

		//FPU
		case OP_FP_ADD_S:
			wf(i.dst, rf(i.src1) + rf(i.src2));
			break;
		case OP_FP_SUB_S:
			wf(i.dst, rf(i.src1) - rf(i.src2));
			break;
		case OP_FP_MUL_S:
			wf(i.dst, rf(i.src1) * rf(i.src2));
			break;
		case OP_FP_DIV_S:
			wf(i.dst, rf(i.src1) / rf(i.src2));
			break;
		case OP_FP_SQRT_S:
			wf(i.dst, __builtin_sqrtf(rf(i.src1)));
			break;
		case OP_FP_RSQRT_S:
			wf(i.dst, 1.0f / __builtin_sqrtf(rf(i.src1)));
			break;
		case OP_FP_RCPL_S:
			wf(i.dst, 1.0f / rf(i.src1));
			break;
		case OP_FP_ABS_S:
			wf(i.dst, __builtin_fabsf(rf(i.src1)));
			break;
		case OP_FP_NEG_S:
			wf(i.dst, -rf(i.src1));
			break;
		case OP_FP_MIN_S:
			wf(i.dst, wasm_f32x4_extract_lane(wasm_f32x4_min(wasm_f32x4_splat(rf(i.src1)), wasm_f32x4_splat(rf(i.src2))), 0));
			break;
		case OP_FP_MAX_S:
			wf(i.dst, wasm_f32x4_extract_lane(wasm_f32x4_max(wasm_f32x4_splat(rf(i.src1)), wasm_f32x4_splat(rf(i.src2))), 0));
			break;
		case OP_FP_CMP_S:
		{
			float a = rf(i.src1), b = rf(i.src2);
			bool result = false;
			switch(i.condition)
			{
			case CONDITION_EQ:
				result = (a == b);
				break;
			case CONDITION_BL:
				result = (a < b);
				break;
			case CONDITION_BE:
				result = (a <= b);
				break;
			case CONDITION_AB:
				result = (a > b);
				break;
			default:
				__builtin_trap();
			}
			w32(i.dst, result ? 1 : 0);
		}
		break;
		case OP_FP_CLAMP_S:
			w32(i.dst, wasm_i32x4_extract_lane(ClampS(wasm_i32x4_splat(static_cast<int32>(r32(i.src1)))), 0));
			break;
		case OP_FP_TOINT32_TRUNC_S:
			w32(i.dst, static_cast<uint32>(wasm_i32x4_extract_lane(wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_splat(rf(i.src1))), 0)));
			break;
		case OP_FP_TOSINGLE_I32:
			wf(i.dst, static_cast<float>(static_cast<int32>(r32(i.src1))));
			break;
		case OP_FP_LDCST:
			w32(i.dst, i.src1.value);
			break;

		//SIMD
		case OP_MD_ADD_B:
			w128(i.dst, wasm_i8x16_add(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADD_H:
			w128(i.dst, wasm_i16x8_add(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADD_W:
			w128(i.dst, wasm_i32x4_add(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADDSS_B:
			w128(i.dst, wasm_i8x16_add_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADDSS_H:
			w128(i.dst, wasm_i16x8_add_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADDSS_W:
		{
			v128_t x = r128(i.src1), y = r128(i.src2);
			v128_t res = wasm_i32x4_add(x, y);
			v128_t ovf = wasm_i32x4_add(wasm_i32x4_splat(0x7FFFFFFF), wasm_u32x4_shr(x, 31));
			v128_t sign = wasm_i32x4_shr(wasm_v128_or(wasm_v128_not(wasm_v128_xor(y, res)), wasm_v128_xor(y, ovf)), 31);
			w128(i.dst, wasm_v128_or(wasm_v128_and(res, sign), wasm_v128_and(ovf, wasm_v128_not(sign))));
		}
		break;
		case OP_MD_ADDUS_B:
			w128(i.dst, wasm_u8x16_add_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADDUS_H:
			w128(i.dst, wasm_u16x8_add_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ADDUS_W:
		{
			v128_t x = r128(i.src1), y = r128(i.src2);
			v128_t res = wasm_i32x4_add(x, y);
			w128(i.dst, wasm_v128_or(wasm_u32x4_lt(res, x), res));
		}
		break;
		case OP_MD_SUB_B:
			w128(i.dst, wasm_i8x16_sub(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUB_H:
			w128(i.dst, wasm_i16x8_sub(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUB_W:
			w128(i.dst, wasm_i32x4_sub(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUBSS_B:
			w128(i.dst, wasm_i8x16_sub_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUBSS_H:
			w128(i.dst, wasm_i16x8_sub_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUBSS_W:
		{
			v128_t x = r128(i.src1), y = r128(i.src2);
			v128_t res = wasm_i32x4_sub(x, y);
			v128_t ovf = wasm_i32x4_add(wasm_i32x4_splat(0x7FFFFFFF), wasm_u32x4_shr(x, 31));
			v128_t sign = wasm_i32x4_shr(wasm_v128_and(wasm_v128_xor(ovf, res), wasm_v128_xor(y, ovf)), 31);
			w128(i.dst, wasm_v128_or(wasm_v128_and(res, wasm_v128_not(sign)), wasm_v128_and(ovf, sign)));
		}
		break;
		case OP_MD_SUBUS_B:
			w128(i.dst, wasm_u8x16_sub_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUBUS_H:
			w128(i.dst, wasm_u16x8_sub_sat(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUBUS_W:
		{
			v128_t x = r128(i.src1), y = r128(i.src2);
			v128_t res = wasm_i32x4_sub(x, y);
			w128(i.dst, wasm_v128_and(wasm_u32x4_le(res, x), res));
		}
		break;
		case OP_MD_CLAMP_S:
			w128(i.dst, ClampS(r128(i.src1)));
			break;
		case OP_MD_CMPEQ_B:
			w128(i.dst, wasm_i8x16_eq(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPEQ_H:
			w128(i.dst, wasm_i16x8_eq(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPEQ_W:
			w128(i.dst, wasm_i32x4_eq(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPGT_B:
			w128(i.dst, wasm_i8x16_gt(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPGT_H:
			w128(i.dst, wasm_i16x8_gt(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPGT_W:
			w128(i.dst, wasm_i32x4_gt(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_MIN_H:
			w128(i.dst, wasm_i16x8_min(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_MIN_W:
			w128(i.dst, wasm_i32x4_min(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_MAX_H:
			w128(i.dst, wasm_i16x8_max(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_MAX_W:
			w128(i.dst, wasm_i32x4_max(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_AND:
			w128(i.dst, wasm_v128_and(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_OR:
			w128(i.dst, wasm_v128_or(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_XOR:
			w128(i.dst, wasm_v128_xor(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_NOT:
			w128(i.dst, wasm_v128_not(r128(i.src1)));
			break;
		case OP_MD_BITSELECT:
			w128(i.dst, wasm_v128_bitselect(r128(i.src1), r128(i.src2), r128(i.src3)));
			break;
		case OP_MD_SLLH:
			w128(i.dst, wasm_i16x8_shl(r128(i.src1), r32(i.src2)));
			break;
		case OP_MD_SLLW:
			w128(i.dst, wasm_i32x4_shl(r128(i.src1), r32(i.src2)));
			break;
		case OP_MD_SRLH:
			w128(i.dst, wasm_u16x8_shr(r128(i.src1), r32(i.src2)));
			break;
		case OP_MD_SRLW:
			w128(i.dst, wasm_u32x4_shr(r128(i.src1), r32(i.src2)));
			break;
		case OP_MD_SRAH:
			w128(i.dst, wasm_i16x8_shr(r128(i.src1), r32(i.src2)));
			break;
		case OP_MD_SRAW:
			w128(i.dst, wasm_i32x4_shr(r128(i.src1), r32(i.src2)));
			break;
		case OP_MD_SRL256:
		{
			alignas(16) uint8 bytes[32];
			uintptr_t source = address(i.src1);
			wasm_v128_store(bytes, wasm_v128_load(reinterpret_cast<const void*>(source)));
			wasm_v128_store(bytes + 16, wasm_v128_load(reinterpret_cast<const void*>(source + 16)));
			uint32 shift = (r32(i.src2) & 0x7F) / 8;
			alignas(16) uint8 result[16];
			for(uint32 b = 0; b < 16; b++)
			{
				result[b] = bytes[b + shift];
			}
			w128(i.dst, wasm_v128_load(result));
		}
		break;
		case OP_MERGETO256:
		{
			v128_t lo = r128(i.src1), hi = r128(i.src2);
			uintptr_t target = address(i.dst);
			wasm_v128_store(reinterpret_cast<void*>(target), lo);
			wasm_v128_store(reinterpret_cast<void*>(target + 16), hi);
		}
		break;
		case OP_MD_MAKECLIP:
		{
			v128_t x = r128(i.src1);
			v128_t gt = wasm_f32x4_gt(x, r128(i.src2));
			v128_t lt = wasm_f32x4_lt(x, r128(i.src3));
			v128_t merged = wasm_i8x16_shuffle(gt, lt, 0x00, 0x02, 0x10, 0x12, 0x04, 0x06, 0x14, 0x16, 0x08, 0x0A, 0x18, 0x1A, 0x0C, 0x0E, 0x1C, 0x1E);
			w32(i.dst, wasm_i16x8_bitmask(merged) & 0x3F);
		}
		break;
		case OP_MD_MAKESZ:
		{
			v128_t x = r128(i.src1);
			v128_t sign = wasm_i32x4_shr(x, 31);
			v128_t zero = wasm_f32x4_eq(x, wasm_i32x4_splat(0));
			v128_t merged = wasm_i8x16_shuffle(sign, zero, 0x1E, 0x1C, 0x1A, 0x18, 0x16, 0x14, 0x12, 0x10, 0x0E, 0x0C, 0x0A, 0x08, 0x06, 0x04, 0x02, 0x00);
			w32(i.dst, wasm_i16x8_bitmask(merged));
		}
		break;
		case OP_MD_TOSINGLE_I32:
			w128(i.dst, wasm_f32x4_convert_i32x4(r128(i.src1)));
			break;
		case OP_MD_TOINT32_TRUNC_S:
			w128(i.dst, wasm_i32x4_trunc_sat_f32x4(r128(i.src1)));
			break;
		case OP_MD_ADD_S:
			w128(i.dst, wasm_f32x4_add(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_SUB_S:
			w128(i.dst, wasm_f32x4_sub(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_MUL_S:
			w128(i.dst, wasm_f32x4_mul(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_DIV_S:
			w128(i.dst, wasm_f32x4_div(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_ABS_S:
			w128(i.dst, wasm_f32x4_abs(r128(i.src1)));
			break;
		case OP_MD_NEG_S:
			w128(i.dst, wasm_f32x4_neg(r128(i.src1)));
			break;
		case OP_MD_MIN_S:
			w128(i.dst, wasm_f32x4_min(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_MAX_S:
			w128(i.dst, wasm_f32x4_max(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPLT_S:
			w128(i.dst, wasm_f32x4_lt(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_CMPGT_S:
			w128(i.dst, wasm_f32x4_gt(r128(i.src1), r128(i.src2)));
			break;
		case OP_MD_EXPAND_W:
			if(i.src2.kind != OPERAND_NONE)
			{
				alignas(16) uint32 words[4];
				wasm_v128_store(words, r128(i.src1));
				w128(i.dst, wasm_i32x4_splat(static_cast<int32>(words[i.src2.value & 3])));
			}
			else
			{
				w128(i.dst, wasm_i32x4_splat(static_cast<int32>(r32(i.src1))));
			}
			break;
		case OP_MD_UNPACK_LOWER_BH:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23));
			break;
		case OP_MD_UNPACK_LOWER_HW:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23));
			break;
		case OP_MD_UNPACK_LOWER_WD:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23));
			break;
		case OP_MD_UNPACK_UPPER_BH:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31));
			break;
		case OP_MD_UNPACK_UPPER_HW:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31));
			break;
		case OP_MD_UNPACK_UPPER_WD:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31));
			break;
		case OP_MD_PACK_HB:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30));
			break;
		case OP_MD_PACK_WH:
			w128(i.dst, wasm_i8x16_shuffle(r128(i.src2), r128(i.src1), 0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29));
			break;
		case OP_MD_MOV_MASKED:
			w128(i.dst, Blend(r128(i.src1), r128(i.src2), i.condition));
			break;
		case OP_MD_LOADFROMREF_MASKED:
		{
			uintptr_t base = r32(i.src1) + r32(i.src2);
			v128_t blendWith = r128(i.src3);
			w128(i.dst, Blend(blendWith, wasm_v128_load(reinterpret_cast<const void*>(base)), i.condition));
		}
		break;
		case OP_MD_STOREATREF_MASKED:
		{
			uintptr_t base = r32(i.src1) + r32(i.src2);
			v128_t current = wasm_v128_load(reinterpret_cast<const void*>(base));
			wasm_v128_store(reinterpret_cast<void*>(base), Blend(current, r128(i.src3), i.condition));
		}
		break;

		//Calls and flow control
		case OP_PARAM:
			if(paramCount == 8) __builtin_trap();
			params[paramCount++] = &i.src1;
			break;
		case OP_CALL:
		{
			//The last OP_PARAM is the first argument
			uint32 argumentCount = i.src2.value;
			if(argumentCount > paramCount) __builtin_trap();
			const OPERAND** args = params + paramCount - argumentCount;
			auto arg = [&](uint32 index) -> const OPERAND& { return *args[argumentCount - 1 - index]; };
			uintptr_t function = i.src1.value;
			switch(i.signature)
			{
			case SIG_VI:
				reinterpret_cast<void (*)(uint32)>(function)(r32(arg(0)));
				break;
			case SIG_VII:
				reinterpret_cast<void (*)(uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)));
				break;
			case SIG_II:
				returnValue = reinterpret_cast<uint32 (*)(uint32)>(function)(r32(arg(0)));
				break;
			case SIG_JI:
				returnValue = reinterpret_cast<uint64 (*)(uint32)>(function)(r32(arg(0)));
				break;
			case SIG_III:
				returnValue = reinterpret_cast<uint32 (*)(uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)));
				break;
			case SIG_JII:
				returnValue = reinterpret_cast<uint64 (*)(uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)));
				break;
			case SIG_VIII:
				reinterpret_cast<void (*)(uint32, uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)), r32(arg(2)));
				break;
			case SIG_VIJI:
				reinterpret_cast<void (*)(uint32, uint64, uint32)>(function)(r32(arg(0)), r64(arg(1)), r32(arg(2)));
				break;
			case SIG_IIII:
				returnValue = reinterpret_cast<uint32 (*)(uint32, uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)), r32(arg(2)));
				break;
			case SIG_JIJI:
				returnValue = reinterpret_cast<uint64 (*)(uint32, uint64, uint32)>(function)(r32(arg(0)), r64(arg(1)), r32(arg(2)));
				break;
			case SIG_VIIII:
				reinterpret_cast<void (*)(uint32, uint32, uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)), r32(arg(2)), r32(arg(3)));
				break;
			case SIG_IIIII:
				returnValue = reinterpret_cast<uint32 (*)(uint32, uint32, uint32, uint32)>(function)(r32(arg(0)), r32(arg(1)), r32(arg(2)), r32(arg(3)));
				break;
			default:
				__builtin_trap();
			}
			paramCount -= argumentCount;
		}
		break;
		case OP_RETVAL:
			if(i.dst.size == 8)
				w64(i.dst, returnValue);
			else
				w32(i.dst, static_cast<uint32>(returnValue));
			break;
		case OP_EXTERNJMP:
			reinterpret_cast<void (*)(void*)>(static_cast<uintptr_t>(i.src1.value))(context);
			break;
		case OP_EXTERNJMP_IDX:
		{
			uint32 functionIndex = r32(i.src1);
			if(functionIndex != 0)
			{
				reinterpret_cast<void (*)(void*)>(static_cast<uintptr_t>(functionIndex))(context);
				return;
			}
		}
		break;
		case OP_JMP:
			pc = i.target;
			break;
		case OP_CONDJMP:
			if(TestCondition<uint32, int32>(i.condition, r32(i.src1), r32(i.src2))) pc = i.target;
			break;
		default:
			__builtin_trap();
		}
	}
}

#else

std::shared_ptr<const CInterpreterProgram> CJitter::BuildInterpreterProgram() const
{
	return nullptr;
}

bool CInterpreterProgram::GetCallSignature(const char*, CALL_SIGNATURE&)
{
	return false;
}

void CInterpreterProgram::Run(void*) const
{
}

#endif
