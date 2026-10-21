#include "Jitter_CodeGen_Wasm.h"

#include <stdexcept>
#include <set>
#include <algorithm>

#include "WasmDefs.h"
#include "WasmModuleBuilder.h"

using namespace Jitter;

#include "Jitter_CodeGen_Wasm_LoadStore.h"

template <uint32 op>
void CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	m_functionStream.Write8(op);

	CommitSymbol(dst);
}

template <bool isSigned>
void CCodeGen_Wasm::Emit_Mul_Tmp64AnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	assert(dst->m_type == SYM_TEMPORARY64);

	uint32 localIdx = GetTemporaryLocation(dst);

	PrepareSymbolUse(src1);
	m_functionStream.Write8(isSigned ? Wasm::INST_I64_EXTEND_I32_S : Wasm::INST_I64_EXTEND_I32_U);

	PrepareSymbolUse(src2);
	m_functionStream.Write8(isSigned ? Wasm::INST_I64_EXTEND_I32_S : Wasm::INST_I64_EXTEND_I32_U);

	m_functionStream.Write8(Wasm::INST_I64_MUL);

	m_functionStream.Write8(Wasm::INST_LOCAL_SET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

template <bool isSigned>
void CCodeGen_Wasm::Emit_Div_Tmp64AnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	assert(dst->m_type == SYM_TEMPORARY64);

	uint32 localIdx = GetTemporaryLocation(dst);

	//Compute dividend
	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	m_functionStream.Write8(isSigned ? Wasm::INST_I32_DIV_S : Wasm::INST_I32_DIV_U);
	m_functionStream.Write8(Wasm::INST_I64_EXTEND_I32_U);

	//Compute remainder
	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	m_functionStream.Write8(isSigned ? Wasm::INST_I32_REM_S : Wasm::INST_I32_REM_U);
	m_functionStream.Write8(Wasm::INST_I64_EXTEND_I32_U);

	m_functionStream.Write8(Wasm::INST_I64_CONST);
	CWasmModuleBuilder::WriteSLeb128(m_functionStream, 32);

	m_functionStream.Write8(Wasm::INST_I64_SHL);

	//Combine
	m_functionStream.Write8(Wasm::INST_I64_OR);

	m_functionStream.Write8(Wasm::INST_LOCAL_SET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

// clang-format off
CCodeGen_Wasm::CONSTMATCHER CCodeGen_Wasm::g_constMatchers[] =
{
	{ OP_NOP,            MATCH_NIL,            MATCH_NIL,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Nop                                    },
	{ OP_BREAK,          MATCH_NIL,            MATCH_NIL,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Break                                  },

	{ OP_MOV,            MATCH_VARIABLE,       MATCH_ANY,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Mov_VarAny                             },

	{ OP_RELTOREF,       MATCH_VAR_REF,        MATCH_CONSTANT,       MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_RelToRef_VarCst                        },

	{ OP_ADDREF,         MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_AddRef_AnyAnyAny                       },

	{ OP_ISREFNULL,      MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_IsRefNull_VarVar                       },

	{ OP_LOADFROMREF,    MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_LoadFromRef_VarVar                     },
	{ OP_LOADFROMREF,    MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_ANY32,         MATCH_NIL,      &CCodeGen_Wasm::Emit_LoadFromRef_VarVarAny                  },

	// Reusing the same generators for refs since they're 32-bit values
	{ OP_LOADFROMREF,    MATCH_VAR_REF,        MATCH_VAR_REF,        MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_LoadFromRef_VarVar                     },
	{ OP_LOADFROMREF,    MATCH_VAR_REF,        MATCH_VAR_REF,        MATCH_ANY32,         MATCH_NIL,      &CCodeGen_Wasm::Emit_LoadFromRef_VarVarAny                  },

	{ OP_LOAD8FROMREF,   MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_LoadFromRef_MemVar<Wasm::INST_I32_LOAD8_U, 0>     },
	{ OP_LOAD8FROMREF,   MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_ANY32,         MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_LoadFromRef_MemVarAny<Wasm::INST_I32_LOAD8_U, 0>  },
	{ OP_LOAD16FROMREF,  MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_LoadFromRef_MemVar<Wasm::INST_I32_LOAD16_U, 1>    },
	{ OP_LOAD16FROMREF,  MATCH_VARIABLE,       MATCH_VAR_REF,        MATCH_ANY32,         MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_LoadFromRef_MemVarAny<Wasm::INST_I32_LOAD16_U, 1> },

	{ OP_STOREATREF,     MATCH_NIL,            MATCH_VAR_REF,        MATCH_ANY32,         MATCH_NIL,      &CCodeGen_Wasm::Emit_StoreAtRef_VarAny                      },
	{ OP_STOREATREF,     MATCH_NIL,            MATCH_VAR_REF,        MATCH_ANY32,         MATCH_ANY32,    &CCodeGen_Wasm::Emit_StoreAtRef_VarAnyAny                   },

	{ OP_STORE8ATREF,    MATCH_NIL,            MATCH_VAR_REF,        MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_StoreAtRef_VarAny<Wasm::INST_I32_STORE8, 0>     },
	{ OP_STORE8ATREF,    MATCH_NIL,            MATCH_VAR_REF,        MATCH_ANY,           MATCH_ANY32,    &CCodeGen_Wasm::Emit_Generic_StoreAtRef_VarAnyAny<Wasm::INST_I32_STORE8, 0>  },
	{ OP_STORE16ATREF,   MATCH_NIL,            MATCH_VAR_REF,        MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_StoreAtRef_VarAny<Wasm::INST_I32_STORE16, 1>    },
	{ OP_STORE16ATREF,   MATCH_NIL,            MATCH_VAR_REF,        MATCH_ANY,           MATCH_ANY32,    &CCodeGen_Wasm::Emit_Generic_StoreAtRef_VarAnyAny<Wasm::INST_I32_STORE16, 1> },

	{ OP_PARAM,          MATCH_NIL,            MATCH_CONTEXT,        MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Param_Ctx                              },
	{ OP_PARAM,          MATCH_NIL,            MATCH_ANY,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Param_Any                              },

	{ OP_CALL,           MATCH_NIL,            MATCH_CONSTANTPTR,    MATCH_CONSTANT,      MATCH_NIL,      &CCodeGen_Wasm::Emit_Call                                   },
	{ OP_RETVAL,         MATCH_TEMPORARY,      MATCH_NIL,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_RetVal_Tmp                             },

	{ OP_EXTERNJMP,      MATCH_NIL,            MATCH_CONSTANTPTR,    MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_ExternJmp                              },
	{ OP_EXTERNJMP_IDX,  MATCH_NIL,            MATCH_ANY32,          MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_ExternJmpIdx                           },

	{ OP_JMP,            MATCH_NIL,            MATCH_NIL,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Jmp                                    },

	{ OP_CONDJMP,        MATCH_NIL,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_CondJmp_AnyAny                         },

	{ OP_CMP,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Cmp_AnyAnyAny                          },

	{ OP_SELECT,         MATCH_VARIABLE,       MATCH_VARIABLE,       MATCH_ANY,           MATCH_ANY,      &CCodeGen_Wasm::Emit_Select_VarVarAnyAny                    },

	{ OP_SLL,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_SHL>   },
	{ OP_SRL,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_SHR_U> },
	{ OP_SRA,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_SHR_S> },

	{ OP_NOT,            MATCH_ANY,            MATCH_ANY,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Not_AnyAny                             },
	{ OP_LZC,            MATCH_ANY,            MATCH_ANY,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Lzc_AnyAny                             },

	{ OP_AND,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_AND> },
	{ OP_OR,             MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_OR>  },
	{ OP_XOR,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_XOR> },

	{ OP_ADD,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_ADD> },
	{ OP_SUB,            MATCH_ANY,            MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Generic_Binary_MemAnyAny<Wasm::INST_I32_SUB> },

	{ OP_DIV,            MATCH_TEMPORARY64,    MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Div_Tmp64AnyAny<false>                 },
	{ OP_DIVS,           MATCH_TEMPORARY64,    MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Div_Tmp64AnyAny<true>                  },

	{ OP_MUL,            MATCH_TEMPORARY64,    MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Mul_Tmp64AnyAny<false>                 },
	{ OP_MULS,           MATCH_TEMPORARY64,    MATCH_ANY,            MATCH_ANY,           MATCH_NIL,      &CCodeGen_Wasm::Emit_Mul_Tmp64AnyAny<true>                  },

	{ OP_EXTLOW64,       MATCH_VARIABLE,       MATCH_MEMORY64,       MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_ExtLow64VarMem64                       },
	{ OP_EXTHIGH64,      MATCH_VARIABLE,       MATCH_MEMORY64,       MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::Emit_ExtHigh64VarMem64                      },

	{ OP_LOADFROMPAGE,   MATCH_ANY,            MATCH_REL_REF,        MATCH_ANY32,         MATCH_NIL,      &CCodeGen_Wasm::Emit_LoadFromPage                           },
	{ OP_STOREATPAGE,    MATCH_NIL,            MATCH_REL_REF,        MATCH_ANY32,         MATCH_ANY,      &CCodeGen_Wasm::Emit_StoreAtPage                            },

	{ OP_LABEL,          MATCH_NIL,            MATCH_NIL,            MATCH_NIL,           MATCH_NIL,      &CCodeGen_Wasm::MarkLabel                                   },

	{ OP_MOV,            MATCH_NIL,            MATCH_NIL,            MATCH_NIL,           MATCH_NIL,      nullptr                                                     },
};
// clang-format on

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
// clang-format off
EM_JS_DEPS(WasmRegisterExternFunction, "$convertJsFunctionToWasm,$getWasmTableEntry");
EM_JS(int, RegisterExternFunction, (const char* functionName, const char* functionSig, uintptr_t functionPtr), {
	let fctName = UTF8ToString(functionName);
	let fctSig = UTF8ToString(functionSig);
	//The function's own table entry is a wasm function: generated code calls it without going
	//through JavaScript (the export below is a JS wrapper around the same function)
	let tableFct = getWasmTableEntry(functionPtr);
	let fct = Module[fctName];
	if(fct === undefined && !tableFct)
	{
		out(`Warning: Could not find function '${fctName}' (missing export?).`);
	}
	if(Module.codeGenImportTable === undefined) {
		out("Creating import table...");
		Module.codeGenImportTable = new WebAssembly.Table({
			element: 'anyfunc',
			initial: 32
		});
		Module.codeGenImportTableNextIndex = 0;
	}
	//TODO: Check how we can use the export from the WASM module directly
	//instead of having to wrap the wrapper
	let wrappedFct = tableFct ? tableFct : convertJsFunctionToWasm(fct, fctSig);
	let fctId = Module.codeGenImportTableNextIndex++;
	Module.codeGenImportTable.set(fctId, wrappedFct);
	out(`Registered function '${fctName}(${fctSig})' = > id = ${fctId}.`);
	return fctId;
});
// clang-format on

std::map<uintptr_t, CWasmFunctionRegistry::WASM_FUNCTION_INFO> CWasmFunctionRegistry::m_functions;

void CWasmFunctionRegistry::RegisterFunction(uintptr_t functionPtr, const char* functionName, const char* functionSig)
{
	{
		auto fctIterator = m_functions.find(functionPtr);
		assert(fctIterator == m_functions.end());
	}
	WASM_FUNCTION_INFO functionInfo;
	functionInfo.id = RegisterExternFunction(functionName, functionSig, functionPtr);
	functionInfo.signature = functionSig;
	m_functions.insert(std::make_pair(functionPtr, functionInfo));
}

const CWasmFunctionRegistry::WASM_FUNCTION_INFO* CWasmFunctionRegistry::FindFunction(uintptr_t functionPtr)
{
	auto fctIterator = m_functions.find(functionPtr);
	if(fctIterator == std::end(m_functions)) return nullptr;
	return &fctIterator->second;
}

#else

void CWasmFunctionRegistry::RegisterFunction(uintptr_t functionPtr, const char* functionName, const char* functionSig)
{
}

const CWasmFunctionRegistry::WASM_FUNCTION_INFO* CWasmFunctionRegistry::FindFunction(uintptr_t functionPtr)
{
	static const WASM_FUNCTION_INFO fctInfo = {0, "v"};
	return &fctInfo;
}

#endif

CCodeGen_Wasm::CCodeGen_Wasm()
{
	const auto copyMatchers =
	    [this](const CONSTMATCHER* constMatchers) {
		    for(auto* constMatcher = constMatchers; constMatcher->emitter != nullptr; constMatcher++)
		    {
			    MATCHER matcher;
			    matcher.op = constMatcher->op;
			    matcher.dstType = constMatcher->dstType;
			    matcher.src1Type = constMatcher->src1Type;
			    matcher.src2Type = constMatcher->src2Type;
			    matcher.src3Type = constMatcher->src3Type;
			    matcher.emitter = std::bind(constMatcher->emitter, this, std::placeholders::_1);
			    m_matchers.insert(MatcherMapType::value_type(matcher.op, matcher));
		    }
	    };

	copyMatchers(g_constMatchers);
	copyMatchers(g_64ConstMatchers);
	copyMatchers(g_fpuConstMatchers);
	copyMatchers(g_mdConstMatchers);
}

void CCodeGen_Wasm::GenerateCode(const StatementList& statements, unsigned int stackSize)
{
	CWasmModuleBuilder moduleBuilder;

	assert((stackSize & 0x3) == 0);

	m_functionStream.ResetBuffer();
	m_signatures.clear();
	m_labelFlows.clear();
	m_temporaryLocations.clear();
	m_localI32Count = 0;
	m_localI64Count = 0;
	m_localF32Count = 0;
	m_localV128Count = 0;
	m_isInsideBlock = false;
	m_isInsideLoop = false;
	m_currentBlockDepth = 0;
	m_loopBlock = -1;
	m_usesBlockTable = false;
	m_promotedLocations.clear();
	m_promotedAccesses.clear();
	m_promotedDirty.clear();
	m_relativeOffsets.clear();
	m_branchHintOffsets.clear();

	BuildLabelFlows(statements);
	PrepareSignatures(moduleBuilder, statements);
	{
		uint32 typeCount = 0;
		for(const auto& signature : m_signatures)
		{
			typeCount = std::max(typeCount, signature.second + 1);
		}
		for(uint32 i = 0; i < typeCount; i++)
		{
			moduleBuilder.AddFunctionType(GetCanonicalFunctionType(i));
		}
		moduleBuilder.SetFunctionTypeIndex(m_signatures["vi"]);
	}
	PrepareLocalVars(statements);
	if(m_contextPromotion)
	{
		PreparePromotion(statements);
	}
	EmitPromotedLoadAll();

	for(auto statementIterator = statements.begin(); statementIterator != statements.end(); statementIterator++)
	{
		const auto& statement = *statementIterator;
		auto nextIterator = std::next(statementIterator);
		if((nextIterator != statements.end()) && TryEmitStorePair64(statement, *nextIterator))
		{
			statementIterator = nextIterator;
			continue;
		}
		bool found = false;
		auto begin = m_matchers.lower_bound(statement.op);
		auto end = m_matchers.upper_bound(statement.op);

		for(auto matchIterator(begin); matchIterator != end; matchIterator++)
		{
			const auto& matcher(matchIterator->second);
			if(!SymbolMatches(matcher.dstType, statement.dst)) continue;
			if(!SymbolMatches(matcher.src1Type, statement.src1)) continue;
			if(!SymbolMatches(matcher.src2Type, statement.src2)) continue;
			if(!SymbolMatches(matcher.src3Type, statement.src3)) continue;
			matcher.emitter(statement);
			found = true;
			break;
		}
		assert(found);
		if(!found)
		{
			throw std::runtime_error("No suitable emitter found for statement.");
		}
	}

	//Terminate current block
	assert(m_isInsideBlock);
	m_functionStream.Write8(Wasm::INST_END);

	//Terminate loop
	if(m_isInsideLoop)
	{
		m_functionStream.Write8(Wasm::INST_END);
	}

	EmitPromotedStoreDirty();

	m_functionStream.Write8(Wasm::INST_END);
	if(!m_relativeOffsets.empty()) throw std::runtime_error("Relative address without its load/store");

	CWasmModuleBuilder::FUNCTION function;
	function.code = CWasmModuleBuilder::FunctionCode(m_functionStream.GetBuffer(), m_functionStream.GetBuffer() + m_functionStream.GetSize());
	function.localI32Count = m_localI32Count;
	function.localI64Count = m_localI64Count;
	function.localF32Count = m_localF32Count;
	function.localV128Count = m_localV128Count;
	function.name = std::move(m_functionName);
	if(m_branchHints) function.branchHints = m_branchHintOffsets;
	m_functionName.clear();

	moduleBuilder.SetImportsBlockTable(m_usesBlockTable);
	moduleBuilder.AddFunction(std::move(function));
	moduleBuilder.WriteModule(*m_stream);

	assert(m_params.empty());
}

//Two 32-bit stores that write both halves of one 64-bit context value (a sign-extended 32-bit
//result: lo = x, hi = x >> 31, or two constants) become one 64-bit store. Besides saving a store,
//a later 64-bit load of that value (64-bit compares in branches, DADDU, ...) can then be forwarded
//from the store buffer; a 64-bit load that spans two separate 32-bit stores stalls until they retire.
bool CCodeGen_Wasm::TryEmitStorePair64(const STATEMENT& first, const STATEMENT& second)
{
	auto isRelative = [](const SymbolRefPtr& ref) {
		return ref && (ref->GetSymbol()->m_type == SYM_RELATIVE);
	};
	auto isConstant = [](const SymbolRefPtr& ref) {
		return ref && (ref->GetSymbol()->m_type == SYM_CONSTANT);
	};
	if(!isRelative(first.dst) || !isRelative(second.dst)) return false;
	auto firstDst = first.dst->GetSymbol().get();
	auto secondDst = second.dst->GetSymbol().get();
	if(IsPromoted(firstDst) || IsPromoted(secondDst)) return false;

	//Two constants, any order
	if((first.op == OP_MOV) && (second.op == OP_MOV) && isConstant(first.src1) && isConstant(second.src1))
	{
		const STATEMENT* lo = nullptr;
		const STATEMENT* hi = nullptr;
		if(secondDst->m_valueLow == firstDst->m_valueLow + 4)
		{
			lo = &first;
			hi = &second;
		}
		else if(firstDst->m_valueLow == secondDst->m_valueLow + 4)
		{
			lo = &second;
			hi = &first;
		}
		else
		{
			return false;
		}
		uint64 value = static_cast<uint64>(lo->src1->GetSymbol()->m_valueLow) |
		               (static_cast<uint64>(hi->src1->GetSymbol()->m_valueLow) << 32);
		PushRelativeAddress(lo->dst->GetSymbol().get());
		m_functionStream.Write8(Wasm::INST_I64_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, static_cast<int64>(value));
		m_functionStream.Write8(Wasm::INST_I64_STORE);
		WriteRelativeMemArg(0x03);
		return true;
	}

	//hi = x >> 31 (arithmetic) and lo = x, any order
	auto isSignOf = [&](const STATEMENT& statement) {
		return (statement.op == OP_SRA) && isConstant(statement.src2) && (statement.src2->GetSymbol()->m_valueLow == 31);
	};
	const STATEMENT* sra = nullptr;
	const STATEMENT* mov = nullptr;
	if(isSignOf(first) && (second.op == OP_MOV))
	{
		sra = &first;
		mov = &second;
	}
	else if((first.op == OP_MOV) && isSignOf(second))
	{
		mov = &first;
		sra = &second;
	}
	else
	{
		return false;
	}
	auto loDst = mov->dst->GetSymbol().get();
	auto hiDst = sra->dst->GetSymbol().get();
	if(hiDst->m_valueLow != loDst->m_valueLow + 4) return false;
	auto value = mov->src1->GetSymbol().get();
	if(!sra->src1->GetSymbol()->Equals(value)) return false;
	if((value->m_type != SYM_TEMPORARY) && (value->m_type != SYM_RELATIVE)) return false;
	//If the sign store comes first and its destination is the source, the low store would see the new value
	if((sra == &first) && value->Equals(hiDst)) return false;
	PushRelativeAddress(loDst);
	PrepareSymbolUse(value);
	m_functionStream.Write8(Wasm::INST_I64_EXTEND_I32_S);
	m_functionStream.Write8(Wasm::INST_I64_STORE);
	WriteRelativeMemArg(0x03);
	return true;
}

CCodeGen_Wasm::PAGE_ACCESS_HANDLERS CCodeGen_Wasm::m_pageAccessHandlers;

void CCodeGen_Wasm::SetPageAccessHandlers(const PAGE_ACCESS_HANDLERS& handlers)
{
	m_pageAccessHandlers = handlers;
}

const CCodeGen_Wasm::PAGE_ACCESS_HANDLERS& CCodeGen_Wasm::GetPageAccessHandlers()
{
	return m_pageAccessHandlers;
}

bool CCodeGen_Wasm::SupportsPageAccess() const
{
	return m_pageAccessHandlers.load32 != 0;
}

//address -> m_pageAddressLocal, page table entry -> m_pageEntryLocal (left on the stack too)
void CCodeGen_Wasm::EmitPageEntry(const STATEMENT& statement)
{
	PrepareSymbolUse(statement.src2->GetSymbol().get());
	EmitLocalSet(m_pageAddressLocal);
	PrepareSymbolUse(statement.src1->GetSymbol().get());
	EmitLocalGet(m_pageAddressLocal);
	m_functionStream.Write8(Wasm::INST_I32_CONST);
	CWasmModuleBuilder::WriteSLeb128(m_functionStream, m_pageAccessHandlers.pageShift);
	m_functionStream.Write8(Wasm::INST_I32_SHR_U);
	m_functionStream.Write8(Wasm::INST_I32_CONST);
	m_functionStream.Write8(2);
	m_functionStream.Write8(Wasm::INST_I32_SHL);
	m_functionStream.Write8(Wasm::INST_I32_ADD);
	m_functionStream.Write8(Wasm::INST_I32_LOAD);
	m_functionStream.Write8(0x02);
	m_functionStream.Write8(0x00);
	m_functionStream.Write8(0x22); //local.tee
	CWasmModuleBuilder::WriteULeb128(m_functionStream, m_pageEntryLocal);
}

//Operands (context, ...) already pushed
void CCodeGen_Wasm::EmitHandlerCall(uintptr_t handler)
{
	auto fctInfo = CWasmFunctionRegistry::FindFunction(handler);
	assert(fctInfo);
	auto sigIdxIterator = m_signatures.find(fctInfo->signature);
	assert(sigIdxIterator != std::end(m_signatures));
	m_functionStream.Write8(Wasm::INST_I32_CONST);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, fctInfo->id);
	m_functionStream.Write8(Wasm::INST_CALL_INDIRECT);
	m_functionStream.Write8(sigIdxIterator->second);
	m_functionStream.Write8(0x00);
}

//dst = entry ? load(entry + (address & (pageSize - size))) : handler(context, address)
void CCodeGen_Wasm::Emit_LoadFromPage(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	uint32 size = static_cast<uint32>(statement.jmpCondition);
	const auto& h = m_pageAccessHandlers;

	PrepareSymbolDef(dst);
	EmitPageEntry(statement);
	m_branchHintOffsets.push_back({static_cast<uint32>(m_functionStream.GetSize()), static_cast<uint8>(1)});
	m_functionStream.Write8(Wasm::INST_IF);
	m_functionStream.Write8((size == 16) ? Wasm::TYPE_V128 : (size == 8) ? Wasm::TYPE_I64 : Wasm::TYPE_I32);
	{
		EmitLocalGet(m_pageEntryLocal);
		EmitLocalGet(m_pageAddressLocal);
		m_functionStream.Write8(Wasm::INST_I32_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, (1 << h.pageShift) - size);
		m_functionStream.Write8(Wasm::INST_I32_AND);
		m_functionStream.Write8(Wasm::INST_I32_ADD);
		switch(size)
		{
		case 1:
			m_functionStream.Write8(Wasm::INST_I32_LOAD8_U);
			m_functionStream.Write8(0x00);
			break;
		case 2:
			m_functionStream.Write8(Wasm::INST_I32_LOAD16_U);
			m_functionStream.Write8(0x01);
			break;
		case 4:
			m_functionStream.Write8(Wasm::INST_I32_LOAD);
			m_functionStream.Write8(0x02);
			break;
		case 8:
			m_functionStream.Write8(Wasm::INST_I64_LOAD);
			m_functionStream.Write8(0x03);
			break;
		case 16:
			m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
			m_functionStream.Write8(Wasm::INST_V128_LOAD);
			m_functionStream.Write8(0x04);
			break;
		default:
			assert(false);
			break;
		}
		m_functionStream.Write8(0x00); //offset
	}
	m_functionStream.Write8(Wasm::INST_ELSE);
	{
		//The handler gets the context: it must see our values and we must see its changes
		EmitPromotedStoreDirty();
		if(size == 16)
		{
			//Four words (no 128-bit returns), quadword aligned
			for(uint32 i = 0; i < 4; i++)
			{
				PushContext();
				EmitLocalGet(m_pageAddressLocal);
				m_functionStream.Write8(Wasm::INST_I32_CONST);
				CWasmModuleBuilder::WriteSLeb128(m_functionStream, static_cast<int32>(~0x0F));
				m_functionStream.Write8(Wasm::INST_I32_AND);
				m_functionStream.Write8(Wasm::INST_I32_CONST);
				CWasmModuleBuilder::WriteSLeb128(m_functionStream, i * 4);
				m_functionStream.Write8(Wasm::INST_I32_ADD);
				EmitHandlerCall(h.load32);
				m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
				if(i == 0)
				{
					m_functionStream.Write8(0x11); //i32x4.splat
				}
				else
				{
					m_functionStream.Write8(Wasm::INST_I32x4_REPLACE_LANE);
					m_functionStream.Write8(i);
				}
			}
		}
		else
		{
			PushContext();
			EmitLocalGet(m_pageAddressLocal);
			EmitHandlerCall((size == 1) ? h.load8 : (size == 2) ? h.load16 : (size == 8) ? h.load64 : h.load32);
		}
		EmitPromotedLoadAll();
	}
	m_functionStream.Write8(Wasm::INST_END);
	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_StoreAtPage(const STATEMENT& statement)
{
	auto value = statement.src3->GetSymbol().get();
	uint32 size = static_cast<uint32>(statement.jmpCondition);
	const auto& h = m_pageAccessHandlers;

	EmitPageEntry(statement);
	m_branchHintOffsets.push_back({static_cast<uint32>(m_functionStream.GetSize()), static_cast<uint8>(1)});
	m_functionStream.Write8(Wasm::INST_IF);
	m_functionStream.Write8(Wasm::BLOCK_TYPE_VOID);
	{
		EmitLocalGet(m_pageEntryLocal);
		EmitLocalGet(m_pageAddressLocal);
		m_functionStream.Write8(Wasm::INST_I32_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, (1 << h.pageShift) - size);
		m_functionStream.Write8(Wasm::INST_I32_AND);
		m_functionStream.Write8(Wasm::INST_I32_ADD);
		PrepareSymbolUse(value);
		switch(size)
		{
		case 1:
			m_functionStream.Write8(Wasm::INST_I32_STORE8);
			m_functionStream.Write8(0x00);
			break;
		case 2:
			m_functionStream.Write8(Wasm::INST_I32_STORE16);
			m_functionStream.Write8(0x01);
			break;
		case 4:
			m_functionStream.Write8(Wasm::INST_I32_STORE);
			m_functionStream.Write8(0x02);
			break;
		case 8:
			m_functionStream.Write8(Wasm::INST_I64_STORE);
			m_functionStream.Write8(0x03);
			break;
		case 16:
			m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
			m_functionStream.Write8(Wasm::INST_V128_STORE);
			m_functionStream.Write8(0x04);
			break;
		default:
			assert(false);
			break;
		}
		m_functionStream.Write8(0x00); //offset
	}
	m_functionStream.Write8(Wasm::INST_ELSE);
	{
		EmitPromotedStoreDirty();
		if(size == 16)
		{
			for(uint32 i = 0; i < 4; i++)
			{
				PushContext();
				PrepareSymbolUse(value);
				m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
				m_functionStream.Write8(Wasm::INST_I32x4_EXTRACT_LANE);
				m_functionStream.Write8(i);
				EmitLocalGet(m_pageAddressLocal);
				m_functionStream.Write8(Wasm::INST_I32_CONST);
				CWasmModuleBuilder::WriteSLeb128(m_functionStream, static_cast<int32>(~0x0F));
				m_functionStream.Write8(Wasm::INST_I32_AND);
				m_functionStream.Write8(Wasm::INST_I32_CONST);
				CWasmModuleBuilder::WriteSLeb128(m_functionStream, i * 4);
				m_functionStream.Write8(Wasm::INST_I32_ADD);
				EmitHandlerCall(h.store32);
			}
		}
		else
		{
			PushContext();
			PrepareSymbolUse(value);
			EmitLocalGet(m_pageAddressLocal);
			EmitHandlerCall((size == 1) ? h.store8 : (size == 2) ? h.store16 : (size == 8) ? h.store64 : h.store32);
		}
		EmitPromotedLoadAll();
	}
	m_functionStream.Write8(Wasm::INST_END);
}

void CCodeGen_Wasm::SetStream(Framework::CStream* stream)
{
	m_stream = stream;
}

void CCodeGen_Wasm::RegisterExternalSymbols(CObjectFile*) const
{
}

unsigned int CCodeGen_Wasm::GetAvailableRegisterCount() const
{
	return 0;
}

unsigned int CCodeGen_Wasm::GetAvailableMdRegisterCount() const
{
	return 0;
}

bool CCodeGen_Wasm::Has128BitsCallOperands() const
{
	return false;
}

bool CCodeGen_Wasm::CanHold128BitsReturnValueInRegisters() const
{
	return false;
}

bool CCodeGen_Wasm::SupportsExternalJumps() const
{
	return false;
}

bool CCodeGen_Wasm::SupportsCmpSelect() const
{
	return false;
}

uint32 CCodeGen_Wasm::GetPointerSize() const
{
	return 4;
}

void CCodeGen_Wasm::BuildLabelFlows(const StatementList& statements)
{
	//Patterns
	//-----------

	//If/EndIf
	//-------
	//OP_CONDJMP y
	//OP_LABEL x
	//...
	//OP_LABEL y
	//...

	//If/Else/EndIf
	//-------------
	//OP_CONDJMP y
	//OP_LABEL x
	//...
	//OP_JMP z
	//OP_LABEL y
	//...
	//OP_LABEL z
	//...

	//Loop
	//----
	//OP_LABEL x
	//...
	//OP_CONDJMP z
	//OP_LABEL y
	//...
	//OP_JMP x (jump up)
	//OP_LABEL z
	//...

	std::set<uint32> visitedLabels;
	for(auto outerStatementIterator = statements.begin();
	    outerStatementIterator != statements.end(); outerStatementIterator++)
	{
		const auto& outerStatement = *outerStatementIterator;
		if(outerStatement.op == OP_LABEL)
		{
			visitedLabels.insert(outerStatement.jmpBlock);
		}
		else if((outerStatement.op == OP_JMP) && visitedLabels.find(outerStatement.jmpBlock) != std::end(visitedLabels))
		{
			//Jumping to a label that we've already seen (jumping up)
			//Note: We only support one loop block
			assert(m_loopBlock == -1);
			m_loopBlock = outerStatement.jmpBlock;
		}
		else if(outerStatement.op == OP_CONDJMP)
		{
			//Find the target label
			auto innerStatementIterator = outerStatementIterator;

			//Next statement should be a label (start of if block)
			innerStatementIterator++;
			assert(innerStatementIterator->op == OP_LABEL);
			{
				FRAMEWORK_MAYBE_UNUSED auto insertResult = m_labelFlows.insert(std::make_pair(innerStatementIterator->jmpBlock, LABEL_FLOW_IF));
				assert(insertResult.second);
			}

			bool foundEnd = false;
			for(; (innerStatementIterator != statements.end()) && !foundEnd; innerStatementIterator++)
			{
				const auto& innerStatement = *innerStatementIterator;
				if((innerStatement.op == OP_LABEL) &&
				   (innerStatement.jmpBlock == outerStatement.jmpBlock))
				{
					foundEnd = true;
					auto prevStatementIterator = innerStatementIterator;
					prevStatementIterator--;
					const auto& prevStatement = *prevStatementIterator;
					//Check that we're jumping down to some other block
					if((prevStatement.op == OP_JMP) && (visitedLabels.find(prevStatement.jmpBlock) == std::end(visitedLabels)))
					{
						//We have a If/Else/EndIf situation
						{
							FRAMEWORK_MAYBE_UNUSED auto insertResult = m_labelFlows.insert(std::make_pair(innerStatementIterator->jmpBlock, LABEL_FLOW_ELSE));
							assert(insertResult.second);
						}
						{
							FRAMEWORK_MAYBE_UNUSED auto insertResult = m_labelFlows.insert(std::make_pair(prevStatementIterator->jmpBlock, LABEL_FLOW_ENDIF));
							assert(insertResult.second);
						}
					}
					else
					{
						//We have a If/EndIf situation
						{
							FRAMEWORK_MAYBE_UNUSED auto insertResult = m_labelFlows.insert(std::make_pair(innerStatementIterator->jmpBlock, LABEL_FLOW_ENDIF));
							assert(insertResult.second);
						}
					}
				}
			}
			assert(foundEnd);
		}
	}

	if(m_loopBlock != -1)
	{
		FRAMEWORK_MAYBE_UNUSED auto insertResult = m_labelFlows.insert(std::make_pair(m_loopBlock, LABEL_FLOW_LOOP));
		assert(insertResult.second);
	}

	//Look for unbound labels and build blocks
	for(const auto& statement : statements)
	{
		if(statement.op != OP_LABEL) continue;
		auto labelFlowIterator = m_labelFlows.find(statement.jmpBlock);
		if(labelFlowIterator != std::end(m_labelFlows)) continue;
		m_labelFlows.insert(std::make_pair(statement.jmpBlock, LABEL_FLOW_BLOCK));
	}

#ifdef _DEBUG
	//Validate jmps to other blocks: we can only jump to the next block
	for(auto outerStatementIterator = statements.begin();
	    outerStatementIterator != statements.end(); outerStatementIterator++)
	{
		const auto& outerStatement = *outerStatementIterator;
		if(outerStatement.op != OP_JMP) continue;

		//Check that this jmp targets a block (not part of a If structure)
		{
			auto labelFlowIterator = m_labelFlows.find(outerStatement.jmpBlock);
			assert(labelFlowIterator != std::end(m_labelFlows));
			if(labelFlowIterator->second != LABEL_FLOW_BLOCK) continue;
		}

		//Check that the next label we encounter is our target label
		for(auto innerStatementIterator = outerStatementIterator;
		    innerStatementIterator != statements.end(); innerStatementIterator++)
		{
			const auto& innerStatement = *innerStatementIterator;
			if(innerStatement.op != OP_LABEL) continue;

			//Check that label is a new block
			auto labelFlowIterator = m_labelFlows.find(innerStatement.jmpBlock);
			assert(labelFlowIterator != std::end(m_labelFlows));
			if(labelFlowIterator->second != LABEL_FLOW_BLOCK) continue;

			//This is the next block, check that our jmp targets this.
			assert(innerStatement.jmpBlock == outerStatement.jmpBlock);
			break;
		}
	}
#endif
}

void CCodeGen_Wasm::PrepareSignatures(CWasmModuleBuilder& moduleBuilder, const StatementList& statements)
{
	//Register this function's signature
	RegisterSignature(moduleBuilder, "vi");

	for(const auto& statement : statements)
	{
		if((statement.op == OP_LOADFROMPAGE) || (statement.op == OP_STOREATPAGE))
		{
			bool isLoad = (statement.op == OP_LOADFROMPAGE);
			uint32 size = static_cast<uint32>(statement.jmpCondition);
			const auto& h = m_pageAccessHandlers;
			uintptr_t handler = isLoad ? ((size == 1) ? h.load8 : (size == 2) ? h.load16 : (size == 8) ? h.load64 : h.load32)
			                           : ((size == 1) ? h.store8 : (size == 2) ? h.store16 : (size == 8) ? h.store64 : h.store32);
			auto fctInfo = CWasmFunctionRegistry::FindFunction(handler);
			assert(fctInfo);
			if(m_signatures.find(fctInfo->signature) == std::end(m_signatures))
			{
				RegisterSignature(moduleBuilder, fctInfo->signature);
			}
			continue;
		}
		if(statement.op != OP_CALL) continue;

		auto src1 = statement.src1->GetSymbol().get();
		assert(src1->m_type == SYM_CONSTANTPTR);

		auto fctInfo = CWasmFunctionRegistry::FindFunction(src1->m_valueLow);
		assert(fctInfo);

		assert(!fctInfo->signature.empty());

		auto signatureIterator = m_signatures.find(fctInfo->signature);
		if(signatureIterator == std::end(m_signatures))
		{
			RegisterSignature(moduleBuilder, fctInfo->signature);
		}
	}
}

//Signatures are "<result><params>" over v (void, result only), i (i32) and j (i64), at most 4 params.
//Every module numbers them the same way (by param count, then result, then param types), so function
//bodies can be moved between modules unchanged (see CWasmModuleBuilder::BuildBatchModule).
uint32 CCodeGen_Wasm::GetCanonicalSignatureIndex(const std::string& signature)
{
	uint32 paramCount = static_cast<uint32>(signature.size()) - 1;
	if(signature.empty() || (paramCount > 4))
	{
		throw std::runtime_error("Unsupported signature.");
	}
	uint32 base = 3 * ((1 << paramCount) - 1);
	uint32 result = 0;
	switch(signature[0])
	{
	case 'v':
		result = 0;
		break;
	case 'i':
		result = 1;
		break;
	case 'j':
		result = 2;
		break;
	default:
		throw std::runtime_error("Unsupported signature.");
	}
	uint32 params = 0;
	for(uint32 i = 1; i < signature.size(); i++)
	{
		if((signature[i] != 'i') && (signature[i] != 'j'))
		{
			throw std::runtime_error("Unsupported signature.");
		}
		params = (params << 1) | ((signature[i] == 'j') ? 1 : 0);
	}
	return base + (result << paramCount) + params;
}

CWasmModuleBuilder::FUNCTION_TYPE CCodeGen_Wasm::GetCanonicalFunctionType(uint32 index)
{
	uint32 paramCount = 0;
	while(index >= 3 * ((2 << paramCount) - 1))
	{
		paramCount++;
	}
	index -= 3 * ((1 << paramCount) - 1);
	CWasmModuleBuilder::FUNCTION_TYPE functionType;
	uint32 result = index >> paramCount;
	if(result == 1) functionType.results.push_back(Wasm::TYPE_I32);
	if(result == 2) functionType.results.push_back(Wasm::TYPE_I64);
	for(uint32 i = 0; i < paramCount; i++)
	{
		bool isI64 = (index >> (paramCount - 1 - i)) & 1;
		functionType.params.push_back(isI64 ? Wasm::TYPE_I64 : Wasm::TYPE_I32);
	}
	return functionType;
}

void CCodeGen_Wasm::RegisterSignature(CWasmModuleBuilder&, std::string signature)
{
	m_signatures.insert(std::make_pair(signature, GetCanonicalSignatureIndex(signature)));
}

void CCodeGen_Wasm::PrepareLocalVars(const StatementList& statements)
{
	bool usesPageAccess = false;
	for(const auto& statement : statements)
	{
		usesPageAccess |= (statement.op == OP_LOADFROMPAGE) || (statement.op == OP_STOREATPAGE);
		statement.VisitOperands(
		    [this](const SymbolRefPtr& symbolRef, bool isDef) {
			    auto symbol = symbolRef->GetSymbol();
			    if(!symbol->IsTemporary()) return;
			    auto temporaryInstance = std::make_pair(symbol->m_type, symbol->m_stackLocation);
			    if(m_temporaryLocations.find(temporaryInstance) != std::end(m_temporaryLocations)) return;
			    switch(symbol->m_type)
			    {
			    case SYM_TEMPORARY:
			    case SYM_TMP_REFERENCE:
				    m_temporaryLocations[temporaryInstance] = m_localI32Count;
				    m_localI32Count++;
				    break;
			    case SYM_TEMPORARY64:
				    m_temporaryLocations[temporaryInstance] = m_localI64Count;
				    m_localI64Count++;
				    break;
			    case SYM_FP_TEMPORARY32:
				    m_temporaryLocations[temporaryInstance] = m_localF32Count;
				    m_localF32Count++;
				    break;
			    case SYM_TEMPORARY128:
				    m_temporaryLocations[temporaryInstance] = m_localV128Count;
				    m_localV128Count++;
				    break;
			    case SYM_TEMPORARY256:
				    //Allocate 2 v128 for a 256-bit temp
				    m_temporaryLocations[temporaryInstance] = m_localV128Count;
				    m_localV128Count += 2;
				    break;
			    default:
				    assert(false);
				    break;
			    }
		    });
	}
	if(usesPageAccess)
	{
		//Scratch i32 locals (absolute indexes: the first local is the context parameter)
		m_pageAddressLocal = 1 + m_localI32Count++;
		m_pageEntryLocal = 1 + m_localI32Count++;
	}
}

uint32 CCodeGen_Wasm::GetTemporaryLocation(CSymbol* symbol) const
{
	assert(symbol->IsTemporary());
	auto temporaryLocationIterator = m_temporaryLocations.find(std::make_pair(symbol->m_type, symbol->m_stackLocation));
	assert(temporaryLocationIterator != std::end(m_temporaryLocations));
	uint32 temporaryLocation = temporaryLocationIterator->second;

	//First local is the function's parameter
	uint32 localIdx = 0;

	switch(symbol->m_type)
	{
	case SYM_TEMPORARY:
	case SYM_TMP_REFERENCE:
		localIdx = temporaryLocation + 1;
		break;
	case SYM_TEMPORARY64:
		localIdx = temporaryLocation + m_localI32Count + 1;
		break;
	case SYM_FP_TEMPORARY32:
		localIdx = temporaryLocation + m_localI32Count + m_localI64Count + 1;
		break;
	case SYM_TEMPORARY128:
	case SYM_TEMPORARY256:
		localIdx = temporaryLocation + m_localI32Count + m_localI64Count + m_localF32Count + 1;
		break;
	default:
		assert(false);
		break;
	}

	return localIdx;
}

void CCodeGen_Wasm::PushContext()
{
	//Context is the first param
	m_functionStream.Write8(Wasm::INST_LOCAL_GET);
	m_functionStream.Write8(0x00);
}

void CCodeGen_Wasm::PushRelativeAddress(CSymbol* symbol)
{
	assert(
	    (symbol->m_type == SYM_RELATIVE) ||
	    (symbol->m_type == SYM_RELATIVE64) ||
	    (symbol->m_type == SYM_FP_RELATIVE32) ||
	    (symbol->m_type == SYM_RELATIVE128));

	//Only the context goes on the stack: the offset becomes the memarg offset of the load/store that uses
	//the address (WriteRelativeMemArg), so engines don't see (and CSE) an add per context field
	PushContext();
	m_relativeOffsets.push_back(symbol->m_valueLow);
}

void CCodeGen_Wasm::WriteRelativeMemArg(uint8 alignment)
{
	if(m_relativeOffsets.empty()) throw std::runtime_error("WriteRelativeMemArg: no pending relative address");
	m_functionStream.Write8(alignment);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, m_relativeOffsets.back());
	m_relativeOffsets.pop_back();
}

void CCodeGen_Wasm::PushRelative(CSymbol* symbol)
{
	if(IsPromoted(symbol))
	{
		PushPromoted(symbol);
		return;
	}
	PushRelativeAddress(symbol);

	m_functionStream.Write8(Wasm::INST_I32_LOAD);
	WriteRelativeMemArg(0x02);
}

void CCodeGen_Wasm::PushTemporary(CSymbol* symbol)
{
	uint32 localIdx = GetTemporaryLocation(symbol);

	m_functionStream.Write8(Wasm::INST_LOCAL_GET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

void CCodeGen_Wasm::PullTemporary(CSymbol* symbol)
{
	assert(symbol->m_type == SYM_TEMPORARY);

	uint32 localIdx = GetTemporaryLocation(symbol);

	m_functionStream.Write8(Wasm::INST_LOCAL_SET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

void CCodeGen_Wasm::PushRelativeRefAddress(CSymbol* symbol)
{
	assert(symbol->m_type == SYM_REL_REFERENCE);

	PushContext();
	m_relativeOffsets.push_back(symbol->m_valueLow);
}

void CCodeGen_Wasm::PushRelativeRef(CSymbol* symbol)
{
	PushRelativeRefAddress(symbol);

	m_functionStream.Write8(Wasm::INST_I32_LOAD);
	WriteRelativeMemArg(0x02);
}

void CCodeGen_Wasm::PushTemporaryRef(CSymbol* symbol)
{
	assert(symbol->m_type == SYM_TMP_REFERENCE);

	uint32 localIdx = GetTemporaryLocation(symbol);

	m_functionStream.Write8(Wasm::INST_LOCAL_GET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

void CCodeGen_Wasm::PullTemporaryRef(CSymbol* symbol)
{
	assert(symbol->m_type == SYM_TMP_REFERENCE);

	uint32 localIdx = GetTemporaryLocation(symbol);

	m_functionStream.Write8(Wasm::INST_LOCAL_SET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

void CCodeGen_Wasm::PrepareSymbolUse(CSymbol* symbol)
{
	switch(symbol->m_type)
	{
	case SYM_RELATIVE:
		PushRelative(symbol);
		break;
	case SYM_REL_REFERENCE:
		PushRelativeRef(symbol);
		break;
	case SYM_TEMPORARY:
		PushTemporary(symbol);
		break;
	case SYM_TMP_REFERENCE:
		PushTemporaryRef(symbol);
		break;
	case SYM_CONSTANT:
		m_functionStream.Write8(Wasm::INST_I32_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, static_cast<int32>(symbol->m_valueLow));
		break;
	case SYM_RELATIVE64:
		PushRelative64(symbol);
		break;
	case SYM_TEMPORARY64:
		PushTemporary64(symbol);
		break;
	case SYM_CONSTANT64:
		m_functionStream.Write8(Wasm::INST_I64_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, symbol->GetConstant64());
		break;
	case SYM_FP_RELATIVE32:
		PushRelativeFp32(symbol);
		break;
	case SYM_FP_TEMPORARY32:
		PushTemporaryFp32(symbol);
		break;
	case SYM_RELATIVE128:
		PushRelative128(symbol);
		break;
	case SYM_TEMPORARY128:
		PushTemporary128(symbol);
		break;
	default:
		assert(false);
		break;
	}
}

void CCodeGen_Wasm::PrepareSymbolDef(CSymbol* symbol)
{
	if(IsPromoted(symbol)) return;
	switch(symbol->m_type)
	{
	case SYM_RELATIVE:
	case SYM_RELATIVE64:
	case SYM_RELATIVE128:
	case SYM_FP_RELATIVE32:
		PushRelativeAddress(symbol);
		break;
	case SYM_TEMPORARY:
	case SYM_TEMPORARY64:
	case SYM_TEMPORARY128:
	case SYM_TMP_REFERENCE:
	case SYM_FP_TEMPORARY32:
		break;
	default:
		assert(false);
		break;
	}
}

void CCodeGen_Wasm::CommitSymbol(CSymbol* symbol)
{
	if(IsPromoted(symbol))
	{
		CommitPromoted(symbol);
		return;
	}
	switch(symbol->m_type)
	{
	case SYM_RELATIVE:
		m_functionStream.Write8(Wasm::INST_I32_STORE);
		WriteRelativeMemArg(0x02);
		break;
	case SYM_TEMPORARY:
		PullTemporary(symbol);
		break;
	case SYM_TMP_REFERENCE:
		PullTemporaryRef(symbol);
		break;
	case SYM_RELATIVE64:
		m_functionStream.Write8(Wasm::INST_I64_STORE);
		WriteRelativeMemArg(0x03);
		break;
	case SYM_TEMPORARY64:
		PullTemporary64(symbol);
		break;
	case SYM_FP_RELATIVE32:
		m_functionStream.Write8(Wasm::INST_F32_STORE);
		WriteRelativeMemArg(0x02);
		break;
	case SYM_FP_TEMPORARY32:
		PullTemporaryFp32(symbol);
		break;
	case SYM_RELATIVE128:
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		m_functionStream.Write8(Wasm::INST_V128_STORE);
		WriteRelativeMemArg(0x04);
		break;
	case SYM_TEMPORARY128:
		PullTemporary128(symbol);
		break;
	default:
		assert(false);
		break;
	}
}

void CCodeGen_Wasm::MarkLabel(const STATEMENT& statement)
{
	auto labelFlowIterator = m_labelFlows.find(statement.jmpBlock);
	assert(labelFlowIterator != std::end(m_labelFlows));
	switch(labelFlowIterator->second)
	{
	case LABEL_FLOW_BLOCK:
		assert(m_currentBlockDepth == 0);
		if(m_isInsideBlock)
		{
			m_functionStream.Write8(Wasm::INST_END);
		}
		m_isInsideBlock = true;
		m_functionStream.Write8(Wasm::INST_BLOCK);
		m_functionStream.Write8(Wasm::BLOCK_TYPE_VOID);
		break;
	case LABEL_FLOW_IF:
		m_functionStream.Write8(Wasm::INST_IF);
		m_functionStream.Write8(Wasm::BLOCK_TYPE_VOID);
		m_currentBlockDepth++;
		break;
	case LABEL_FLOW_ELSE:
		m_functionStream.Write8(Wasm::INST_ELSE);
		break;
	case LABEL_FLOW_ENDIF:
		m_functionStream.Write8(Wasm::INST_END);
		assert(m_currentBlockDepth != 0);
		m_currentBlockDepth--;
		break;
	case LABEL_FLOW_LOOP:
		assert(m_currentBlockDepth == 0);
		if(m_isInsideBlock)
		{
			m_functionStream.Write8(Wasm::INST_END);
		}
		m_isInsideBlock = true;
		m_isInsideLoop = true;
		m_functionStream.Write8(Wasm::INST_LOOP);
		m_functionStream.Write8(Wasm::BLOCK_TYPE_VOID);
		m_functionStream.Write8(Wasm::INST_BLOCK);
		m_functionStream.Write8(Wasm::BLOCK_TYPE_VOID);
		break;
	default:
		assert(false);
		break;
	}
}

void CCodeGen_Wasm::Emit_Nop(const STATEMENT& statement)
{
}

void CCodeGen_Wasm::Emit_Break(const STATEMENT& statement)
{
	m_functionStream.Write8(Wasm::INST_UNREACHABLE);
}

void CCodeGen_Wasm::Emit_Mov_VarAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_RelToRef_VarCst(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);
	PushContext();

	m_functionStream.Write8(Wasm::INST_I32_ADD);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_AddRef_AnyAnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	m_functionStream.Write8(Wasm::INST_I32_ADD);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_IsRefNull_VarVar(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);

	m_functionStream.Write8(Wasm::INST_I32_EQZ);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_LoadFromRef_VarVar(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);

	m_functionStream.Write8(Wasm::INST_I32_LOAD);
	m_functionStream.Write8(0x02);
	m_functionStream.Write8(0x00);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_LoadFromRef_VarVarAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();
	uint8 scale = static_cast<uint8>(statement.jmpCondition);

	assert((scale == 1) || (scale == 4));

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	switch(scale)
	{
	case 1:
		//Nothing to do
		break;
	case 4:
		m_functionStream.Write8(Wasm::INST_I32_CONST);
		m_functionStream.Write8(2);
		m_functionStream.Write8(Wasm::INST_I32_SHL);
		break;
	default:
		assert(false);
		break;
	}

	m_functionStream.Write8(Wasm::INST_I32_ADD);

	m_functionStream.Write8(Wasm::INST_I32_LOAD);
	m_functionStream.Write8(0x02);
	m_functionStream.Write8(0x00);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_StoreAtRef_VarAny(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	m_functionStream.Write8(Wasm::INST_I32_STORE);
	m_functionStream.Write8(0x02);
	m_functionStream.Write8(0x00);
}

void CCodeGen_Wasm::Emit_StoreAtRef_VarAnyAny(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();
	auto src3 = statement.src3->GetSymbol().get();
	uint8 scale = static_cast<uint8>(statement.jmpCondition);

	assert((scale == 1) || (scale == 4));

	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	switch(scale)
	{
	case 1:
		//Nothing to do
		break;
	case 4:
		m_functionStream.Write8(Wasm::INST_I32_CONST);
		m_functionStream.Write8(2);
		m_functionStream.Write8(Wasm::INST_I32_SHL);
		break;
	default:
		assert(false);
		break;
	}

	m_functionStream.Write8(Wasm::INST_I32_ADD);

	PrepareSymbolUse(src3);

	m_functionStream.Write8(Wasm::INST_I32_STORE);
	m_functionStream.Write8(0x02);
	m_functionStream.Write8(0x00);
}

void CCodeGen_Wasm::Emit_Param_Ctx(const STATEMENT& statement)
{
	FRAMEWORK_MAYBE_UNUSED auto src1 = statement.src1->GetSymbol().get();
	assert(src1->m_type == SYM_CONTEXT);

	m_params.push([this]() { PushContext(); });
}

void CCodeGen_Wasm::Emit_Param_Any(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	m_params.push([this, src1]() { PrepareSymbolUse(src1); });
}

void CCodeGen_Wasm::Emit_Call(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	assert(src1->m_type == SYM_CONSTANTPTR);
	assert(src2->m_type == SYM_CONSTANT);

	//The callee gets the context: it must see our values and we must see its changes
	EmitPromotedStoreDirty();

	unsigned int paramCount = src2->m_valueLow;
	for(unsigned int i = 0; i < paramCount; i++)
	{
		auto paramFct = m_params.top();
		paramFct();
		m_params.pop();
	}

	auto fctInfo = CWasmFunctionRegistry::FindFunction(src1->m_valueLow);
	auto sigIdxIterator = m_signatures.find(fctInfo->signature);
	assert(sigIdxIterator != std::end(m_signatures));
	auto sigIdx = sigIdxIterator->second;

	m_functionStream.Write8(Wasm::INST_I32_CONST);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, fctInfo->id);

	m_functionStream.Write8(Wasm::INST_CALL_INDIRECT);
	m_functionStream.Write8(sigIdx); //Signature index
	m_functionStream.Write8(0x00);   //Table index

	EmitPromotedLoadAll();
}

void CCodeGen_Wasm::Emit_RetVal_Tmp(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	PullTemporary(dst);
}

void CCodeGen_Wasm::Emit_ExternJmp(const STATEMENT& statement)
{
	//Not really supported, but can be used with some caveats
	//Implemented as a simple indirect call for now (which works fine if the caller returns immediately).
	//This could be implemented using tail calls which doesn't seem to be widely supported.
	//Maybe we could emit a return after the call and be done with it? (stack overflow problems maybe)

	auto src1 = statement.src1->GetSymbol().get();

	assert(src1->m_type == SYM_CONSTANTPTR);

	EmitPromotedStoreDirty();

	PushContext();

	auto fctInfo = CWasmFunctionRegistry::FindFunction(src1->m_valueLow);
	auto sigIdxIterator = m_signatures.find(fctInfo->signature);
	assert(sigIdxIterator != std::end(m_signatures));
	auto sigIdx = sigIdxIterator->second;

	m_functionStream.Write8(Wasm::INST_I32_CONST);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, fctInfo->id);

	m_functionStream.Write8(Wasm::INST_CALL_INDIRECT);
	m_functionStream.Write8(sigIdx); //Signature index
	m_functionStream.Write8(0x00);   //Table index

	EmitPromotedLoadAll();
}

void CCodeGen_Wasm::Emit_ExternJmpIdx(const STATEMENT& statement)
{
	//if(idx != 0) return_call_indirect idx(context) through table 1 (env.blockTable, the main module's function table)
	auto src1 = statement.src1->GetSymbol().get();

	EmitPromotedStoreDirty();

	PrepareSymbolUse(src1);
	m_functionStream.Write8(Wasm::INST_IF);
	m_functionStream.Write8(Wasm::BLOCK_TYPE_VOID);

	PushContext();
	PrepareSymbolUse(src1);

	auto sigIdxIterator = m_signatures.find("vi");
	assert(sigIdxIterator != std::end(m_signatures));
	m_functionStream.Write8(Wasm::INST_RETURN_CALL_INDIRECT);
	m_functionStream.Write8(sigIdxIterator->second); //Signature index
	m_functionStream.Write8(0x01);                   //Table index

	m_functionStream.Write8(Wasm::INST_END);

	m_usesBlockTable = true;
}

void CCodeGen_Wasm::Emit_Jmp(const STATEMENT& statement)
{
	auto labelFlowIterator = m_labelFlows.find(statement.jmpBlock);
	assert(labelFlowIterator != std::end(m_labelFlows));
	if(labelFlowIterator->second == LABEL_FLOW_BLOCK)
	{
		//This can only be used to exit the current block
		//Validated in BuildLabelFlows
		assert(m_isInsideBlock);
		m_functionStream.Write8(Wasm::INST_BR);
		m_functionStream.Write8(m_currentBlockDepth);
	}
	else if(labelFlowIterator->second == LABEL_FLOW_LOOP)
	{
		assert(m_isInsideBlock);
		assert(m_isInsideLoop);
		m_functionStream.Write8(Wasm::INST_BR);
		m_functionStream.Write8(m_currentBlockDepth + 1);
	}
}

void CCodeGen_Wasm::Emit_CondJmp_AnyAny(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	//We use the reverse condition because jmpCondition is the condition to skip
	switch(statement.jmpCondition)
	{
	case CONDITION_EQ:
		m_functionStream.Write8(Wasm::INST_I32_NE);
		break;
	case CONDITION_NE:
		m_functionStream.Write8(Wasm::INST_I32_EQ);
		break;
	case CONDITION_BL:
		m_functionStream.Write8(Wasm::INST_I32_GE_U);
		break;
	case CONDITION_BE:
		m_functionStream.Write8(Wasm::INST_I32_GT_U);
		break;
	case CONDITION_AB:
		m_functionStream.Write8(Wasm::INST_I32_LE_U);
		break;
	case CONDITION_AE:
		m_functionStream.Write8(Wasm::INST_I32_LT_U);
		break;
	case CONDITION_LT:
		m_functionStream.Write8(Wasm::INST_I32_GE_S);
		break;
	case CONDITION_LE:
		m_functionStream.Write8(Wasm::INST_I32_GT_S);
		break;
	case CONDITION_GT:
		m_functionStream.Write8(Wasm::INST_I32_LE_S);
		break;
	case CONDITION_GE:
		m_functionStream.Write8(Wasm::INST_I32_LT_S);
		break;
	default:
		assert(false);
		break;
	}
}

void CCodeGen_Wasm::Emit_Cmp_AnyAnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);
	PrepareSymbolUse(src2);

	switch(statement.jmpCondition)
	{
	case CONDITION_EQ:
		m_functionStream.Write8(Wasm::INST_I32_EQ);
		break;
	case CONDITION_NE:
		m_functionStream.Write8(Wasm::INST_I32_NE);
		break;
	case CONDITION_BL:
		m_functionStream.Write8(Wasm::INST_I32_LT_U);
		break;
	case CONDITION_BE:
		m_functionStream.Write8(Wasm::INST_I32_LE_U);
		break;
	case CONDITION_AB:
		m_functionStream.Write8(Wasm::INST_I32_GT_U);
		break;
	case CONDITION_AE:
		m_functionStream.Write8(Wasm::INST_I32_GE_U);
		break;
	case CONDITION_LT:
		m_functionStream.Write8(Wasm::INST_I32_LT_S);
		break;
	case CONDITION_LE:
		m_functionStream.Write8(Wasm::INST_I32_LE_S);
		break;
	case CONDITION_GT:
		m_functionStream.Write8(Wasm::INST_I32_GT_S);
		break;
	case CONDITION_GE:
		m_functionStream.Write8(Wasm::INST_I32_GE_S);
		break;
	default:
		assert(false);
		break;
	}

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_Select_VarVarAnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();
	auto src3 = statement.src3->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src2);
	PrepareSymbolUse(src3);
	PrepareSymbolUse(src1);

	m_functionStream.Write8(Wasm::INST_SELECT);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_Not_AnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);
	PrepareSymbolUse(src1);

	m_functionStream.Write8(Wasm::INST_I32_CONST);
	CWasmModuleBuilder::WriteSLeb128(m_functionStream, -1);

	m_functionStream.Write8(Wasm::INST_I32_XOR);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_Lzc_AnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);

	//Check if MSB is 0 or 1
	PrepareSymbolUse(src1);

	m_functionStream.Write8(Wasm::INST_I32_CONST);
	CWasmModuleBuilder::WriteSLeb128(m_functionStream, 0);

	m_functionStream.Write8(Wasm::INST_I32_LT_S);

	m_functionStream.Write8(Wasm::INST_IF);
	m_functionStream.Write8(Wasm::TYPE_I32);
	{
		//First bit is 1
		PrepareSymbolUse(src1);
		m_functionStream.Write8(Wasm::INST_I32_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, -1);

		m_functionStream.Write8(Wasm::INST_I32_XOR);

		m_functionStream.Write8(Wasm::INST_I32_CLZ);

		m_functionStream.Write8(Wasm::INST_I32_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, 1);

		m_functionStream.Write8(Wasm::INST_I32_SUB);
	}
	m_functionStream.Write8(Wasm::INST_ELSE);
	{
		//First bit is 0
		PrepareSymbolUse(src1);
		m_functionStream.Write8(Wasm::INST_I32_CLZ);

		m_functionStream.Write8(Wasm::INST_I32_CONST);
		CWasmModuleBuilder::WriteSLeb128(m_functionStream, 1);

		m_functionStream.Write8(Wasm::INST_I32_SUB);
	}
	m_functionStream.Write8(Wasm::INST_END);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_ExtLow64VarMem64(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);

	uint32 localIdx = GetTemporaryLocation(src1);

	m_functionStream.Write8(Wasm::INST_LOCAL_GET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);

	m_functionStream.Write8(Wasm::INST_I32_WRAP_I64);

	CommitSymbol(dst);
}

void CCodeGen_Wasm::Emit_ExtHigh64VarMem64(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	PrepareSymbolDef(dst);

	uint32 localIdx = GetTemporaryLocation(src1);

	m_functionStream.Write8(Wasm::INST_LOCAL_GET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);

	m_functionStream.Write8(Wasm::INST_I64_CONST);
	CWasmModuleBuilder::WriteSLeb128(m_functionStream, 32);

	m_functionStream.Write8(Wasm::INST_I64_SHR_U);

	m_functionStream.Write8(Wasm::INST_I32_WRAP_I64);

	CommitSymbol(dst);
}

std::vector<std::pair<uint32, uint32>> CCodeGen_Wasm::m_contextArrays;

void CCodeGen_Wasm::SetContextPromotion(bool enabled)
{
	m_contextPromotion = enabled;
}

bool CCodeGen_Wasm::m_branchHints = true;

void CCodeGen_Wasm::SetBranchHints(bool enabled)
{
	m_branchHints = enabled;
}

void CCodeGen_Wasm::SetFunctionName(std::string name)
{
	m_functionName = std::move(name);
}

void CCodeGen_Wasm::RegisterContextArray(uint32 offset, uint32 size)
{
	m_contextArrays.push_back(std::make_pair(offset, size));
}

void CCodeGen_Wasm::PreparePromotion(const StatementList& statements)
{
	struct ACCESS
	{
		uint32 start;
		uint32 end;
	};
	std::map<PromotedKey, ACCESS> accesses;
	std::set<PromotedKey> excluded;
	std::set<PromotedKey> written;
	std::vector<std::pair<uint32, uint32>> usedArrays;

	for(const auto& statement : statements)
	{
		if(statement.op == OP_RELTOREF)
		{
			uint32 offset = statement.src1->GetSymbol()->m_valueLow;
			auto arrayIterator = std::find_if(m_contextArrays.begin(), m_contextArrays.end(),
			                                  [offset](const auto& range) { return (offset >= range.first) && (offset < range.first + range.second); });
			if(arrayIterator == m_contextArrays.end())
			{
				//Unknown extent: anything could be reached through this reference
				return;
			}
			usedArrays.push_back(*arrayIterator);
		}
		statement.VisitOperands(
		    [&](const SymbolRefPtr& symbolRef, bool isDef) {
			    auto symbol = symbolRef->GetSymbol();
			    if(!symbol->IsRelative()) return;
			    PromotedKey key(symbol->m_type, symbol->m_valueLow);
			    accesses[key] = ACCESS{symbol->m_valueLow, symbol->m_valueLow + symbol->GetSize()};
			    if(isDef) written.insert(key);
			    if((symbol->m_type == SYM_REL_REFERENCE) || (statement.op == OP_PARAM_RET))
			    {
				    excluded.insert(key);
			    }
		    });
	}

	//Containers: every 128-bit symbol, and scalar symbols that are not inside a 128-bit one.
	//Smaller accesses inside a container become lanes (or a bitcast for f32 over i32).
	auto isInside128 = [&](const ACCESS& access) {
		for(const auto& other : accesses)
		{
			if(other.first.first != SYM_RELATIVE128) continue;
			if((access.start < other.second.end) && (other.second.start < access.end)) return true;
		}
		return false;
	};
	std::map<PromotedKey, ACCESS> containers;
	for(const auto& accessPair : accesses)
	{
		const auto& key = accessPair.first;
		switch(key.first)
		{
		case SYM_RELATIVE128:
			containers[key] = accessPair.second;
			break;
		case SYM_RELATIVE:
		case SYM_RELATIVE64:
			if(!isInside128(accessPair.second)) containers[key] = accessPair.second;
			break;
		case SYM_FP_RELATIVE32:
			//f32 container only when no i32 symbol is at the same place (else it maps to it)
			if(!isInside128(accessPair.second) && !accesses.count(PromotedKey(SYM_RELATIVE, key.second)))
			{
				containers[key] = accessPair.second;
			}
			break;
		default:
			break;
		}
	}

	for(const auto& containerPair : containers)
	{
		const auto& containerKey = containerPair.first;
		const auto& range = containerPair.second;
		if(excluded.count(containerKey)) continue;
		bool promotable = true;
		std::map<PromotedKey, PROMOTED_ACCESS> members;
		for(const auto& accessPair : accesses)
		{
			const auto& key = accessPair.first;
			const auto& access = accessPair.second;
			if(!((access.start < range.end) && (range.start < access.end))) continue;
			if(excluded.count(key) || (access.start < range.start) || (access.end > range.end))
			{
				promotable = false;
				break;
			}
			PROMOTED_ACCESS member;
			member.container = containerKey;
			if(key == containerKey)
			{
				member.kind = PROMOTED_WHOLE;
			}
			else if(containerKey.first == SYM_RELATIVE128)
			{
				uint32 size = access.end - access.start;
				uint32 offset = access.start - range.start;
				if((key.first == SYM_REL_REFERENCE) || ((offset % size) != 0))
				{
					promotable = false;
					break;
				}
				member.kind = PROMOTED_LANE;
				member.lane = offset / size;
			}
			else if((containerKey.first == SYM_RELATIVE) && (key.first == SYM_FP_RELATIVE32) && (access.start == range.start))
			{
				member.kind = PROMOTED_BITCAST;
			}
			else
			{
				promotable = false;
				break;
			}
			members[key] = member;
		}
		if(!promotable) continue;
		for(const auto& arrayRange : usedArrays)
		{
			if((range.start < arrayRange.first + arrayRange.second) && (arrayRange.first < range.end))
			{
				promotable = false;
				break;
			}
		}
		if(!promotable) continue;

		uint32 groupIndex = 0;
		switch(containerKey.first)
		{
		case SYM_RELATIVE:
			groupIndex = m_localI32Count++;
			break;
		case SYM_RELATIVE64:
			groupIndex = m_localI64Count++;
			break;
		case SYM_FP_RELATIVE32:
			groupIndex = m_localF32Count++;
			break;
		case SYM_RELATIVE128:
			groupIndex = m_localV128Count++;
			break;
		default:
			assert(false);
			break;
		}
		m_promotedLocations[containerKey] = groupIndex;
		for(const auto& member : members)
		{
			m_promotedAccesses[member.first] = member.second;
			if(written.count(member.first)) m_promotedDirty.insert(containerKey);
		}
	}

	if(!m_promotedLocations.empty())
	{
		//Scratch locals for lane writes (group-relative for now, made absolute below)
		m_scratchI32Local = m_localI32Count++;
		m_scratchI64Local = m_localI64Count++;
		m_scratchF32Local = m_localF32Count++;
		m_scratchI32Local = m_scratchI32Local + 1;
		m_scratchI64Local = m_scratchI64Local + m_localI32Count + 1;
		m_scratchF32Local = m_scratchF32Local + m_localI32Count + m_localI64Count + 1;
	}
}

bool CCodeGen_Wasm::IsPromoted(const CSymbol* symbol) const
{
	if(m_promotedAccesses.empty()) return false;
	return m_promotedAccesses.count(PromotedKey(symbol->m_type, symbol->m_valueLow)) != 0;
}

const CCodeGen_Wasm::PROMOTED_ACCESS& CCodeGen_Wasm::GetPromotedAccess(const CSymbol* symbol) const
{
	auto accessIterator = m_promotedAccesses.find(PromotedKey(symbol->m_type, symbol->m_valueLow));
	assert(accessIterator != std::end(m_promotedAccesses));
	return accessIterator->second;
}

uint32 CCodeGen_Wasm::GetPromotedLocal(const PromotedKey& key) const
{
	auto locationIterator = m_promotedLocations.find(key);
	assert(locationIterator != std::end(m_promotedLocations));
	uint32 groupIndex = locationIterator->second;
	//Same layout as GetTemporaryLocation (first local is the function's parameter)
	switch(key.first)
	{
	case SYM_RELATIVE:
		return groupIndex + 1;
	case SYM_RELATIVE64:
		return groupIndex + m_localI32Count + 1;
	case SYM_FP_RELATIVE32:
		return groupIndex + m_localI32Count + m_localI64Count + 1;
	case SYM_RELATIVE128:
		return groupIndex + m_localI32Count + m_localI64Count + m_localF32Count + 1;
	default:
		assert(false);
		return 0;
	}
}

void CCodeGen_Wasm::EmitLocalGet(uint32 localIdx)
{
	m_functionStream.Write8(Wasm::INST_LOCAL_GET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

void CCodeGen_Wasm::EmitLocalSet(uint32 localIdx)
{
	m_functionStream.Write8(Wasm::INST_LOCAL_SET);
	CWasmModuleBuilder::WriteULeb128(m_functionStream, localIdx);
}

static uint8 GetLaneOpcode(SYM_TYPE type, bool replace)
{
	switch(type)
	{
	case SYM_RELATIVE:
		return replace ? Wasm::INST_I32x4_REPLACE_LANE : Wasm::INST_I32x4_EXTRACT_LANE;
	case SYM_FP_RELATIVE32:
		return replace ? Wasm::INST_F32x4_REPLACE_LANE : Wasm::INST_F32x4_EXTRACT_LANE;
	case SYM_RELATIVE64:
		return replace ? Wasm::INST_I64x2_REPLACE_LANE : Wasm::INST_I64x2_EXTRACT_LANE;
	default:
		assert(false);
		return 0;
	}
}

void CCodeGen_Wasm::PushPromoted(const CSymbol* symbol)
{
	const auto& access = GetPromotedAccess(symbol);
	EmitLocalGet(GetPromotedLocal(access.container));
	switch(access.kind)
	{
	case PROMOTED_WHOLE:
		break;
	case PROMOTED_LANE:
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		CWasmModuleBuilder::WriteULeb128(m_functionStream, GetLaneOpcode(symbol->m_type, false));
		m_functionStream.Write8(static_cast<uint8>(access.lane));
		break;
	case PROMOTED_BITCAST:
		m_functionStream.Write8(Wasm::INST_F32_REINTERPRET_I32);
		break;
	}
}

void CCodeGen_Wasm::CommitPromoted(const CSymbol* symbol)
{
	//Value to store is on the stack
	const auto& access = GetPromotedAccess(symbol);
	uint32 containerLocal = GetPromotedLocal(access.container);
	switch(access.kind)
	{
	case PROMOTED_WHOLE:
		EmitLocalSet(containerLocal);
		break;
	case PROMOTED_LANE:
	{
		uint32 scratch = (symbol->m_type == SYM_RELATIVE) ? m_scratchI32Local : (symbol->m_type == SYM_RELATIVE64) ? m_scratchI64Local : m_scratchF32Local;
		EmitLocalSet(scratch);
		EmitLocalGet(containerLocal);
		EmitLocalGet(scratch);
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		CWasmModuleBuilder::WriteULeb128(m_functionStream, GetLaneOpcode(symbol->m_type, true));
		m_functionStream.Write8(static_cast<uint8>(access.lane));
		EmitLocalSet(containerLocal);
	}
	break;
	case PROMOTED_BITCAST:
		m_functionStream.Write8(Wasm::INST_I32_REINTERPRET_F32);
		EmitLocalSet(containerLocal);
		break;
	}
}

void CCodeGen_Wasm::PushPromotedBits(const CSymbol* symbol)
{
	//Pushes the raw bits of a promoted f32 symbol as an i32
	assert(symbol->m_type == SYM_FP_RELATIVE32);
	const auto& access = GetPromotedAccess(symbol);
	EmitLocalGet(GetPromotedLocal(access.container));
	switch(access.kind)
	{
	case PROMOTED_WHOLE:
		m_functionStream.Write8(Wasm::INST_I32_REINTERPRET_F32);
		break;
	case PROMOTED_LANE:
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		CWasmModuleBuilder::WriteULeb128(m_functionStream, Wasm::INST_I32x4_EXTRACT_LANE);
		m_functionStream.Write8(static_cast<uint8>(access.lane));
		break;
	case PROMOTED_BITCAST:
		break;
	}
}

void CCodeGen_Wasm::CommitPromotedBits(const CSymbol* symbol)
{
	//Stores raw bits (i32 on the stack) into a promoted f32 symbol
	assert(symbol->m_type == SYM_FP_RELATIVE32);
	const auto& access = GetPromotedAccess(symbol);
	uint32 containerLocal = GetPromotedLocal(access.container);
	switch(access.kind)
	{
	case PROMOTED_WHOLE:
		m_functionStream.Write8(Wasm::INST_F32_REINTERPRET_I32);
		EmitLocalSet(containerLocal);
		break;
	case PROMOTED_LANE:
		EmitLocalSet(m_scratchI32Local);
		EmitLocalGet(containerLocal);
		EmitLocalGet(m_scratchI32Local);
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		CWasmModuleBuilder::WriteULeb128(m_functionStream, Wasm::INST_I32x4_REPLACE_LANE);
		m_functionStream.Write8(static_cast<uint8>(access.lane));
		EmitLocalSet(containerLocal);
		break;
	case PROMOTED_BITCAST:
		EmitLocalSet(containerLocal);
		break;
	}
}

void CCodeGen_Wasm::EmitPromotedLoad(const PromotedKey& key)
{
	PushContext();
	//Use the memarg offset: no address arithmetic needed
	switch(key.first)
	{
	case SYM_RELATIVE:
		m_functionStream.Write8(Wasm::INST_I32_LOAD);
		m_functionStream.Write8(0x02);
		break;
	case SYM_RELATIVE64:
		m_functionStream.Write8(Wasm::INST_I64_LOAD);
		m_functionStream.Write8(0x03);
		break;
	case SYM_FP_RELATIVE32:
		m_functionStream.Write8(Wasm::INST_F32_LOAD);
		m_functionStream.Write8(0x02);
		break;
	case SYM_RELATIVE128:
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		m_functionStream.Write8(Wasm::INST_V128_LOAD);
		m_functionStream.Write8(0x04);
		break;
	default:
		assert(false);
		break;
	}
	CWasmModuleBuilder::WriteULeb128(m_functionStream, key.second);
	EmitLocalSet(GetPromotedLocal(key));
}

void CCodeGen_Wasm::EmitPromotedStore(const PromotedKey& key)
{
	PushContext();
	EmitLocalGet(GetPromotedLocal(key));
	switch(key.first)
	{
	case SYM_RELATIVE:
		m_functionStream.Write8(Wasm::INST_I32_STORE);
		m_functionStream.Write8(0x02);
		break;
	case SYM_RELATIVE64:
		m_functionStream.Write8(Wasm::INST_I64_STORE);
		m_functionStream.Write8(0x03);
		break;
	case SYM_FP_RELATIVE32:
		m_functionStream.Write8(Wasm::INST_F32_STORE);
		m_functionStream.Write8(0x02);
		break;
	case SYM_RELATIVE128:
		m_functionStream.Write8(Wasm::INST_PREFIX_SIMD);
		m_functionStream.Write8(Wasm::INST_V128_STORE);
		m_functionStream.Write8(0x04);
		break;
	default:
		assert(false);
		break;
	}
	CWasmModuleBuilder::WriteULeb128(m_functionStream, key.second);
}

void CCodeGen_Wasm::EmitPromotedLoadAll()
{
	for(const auto& locationPair : m_promotedLocations)
	{
		EmitPromotedLoad(locationPair.first);
	}
}

void CCodeGen_Wasm::EmitPromotedStoreDirty()
{
	for(const auto& key : m_promotedDirty)
	{
		EmitPromotedStore(key);
	}
}
